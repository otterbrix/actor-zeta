#pragma once

#include <actor-zeta/base/address.hpp>
#include <actor-zeta/detail/memory_resource.hpp>
#include <actor-zeta/detail/pmr/polymorphic_allocator.hpp>
#include <actor-zeta/detail/future_state.hpp>
#include <actor-zeta/future.hpp>
#include <actor-zeta/make_message.hpp>

namespace actor_zeta { namespace base {

    /// @brief Actor mixin - common functionality for all actor types
    /// Provides: id_t, address(), placement operators, enqueue_sync_impl()
    /// Used by: cooperative_actor, supervisor, and other actor types
    template<typename Derived>
    class actor_mixin {
    public:
        // ===== Actor ID type =====
        class id_t final {
        public:
            id_t() = delete;

            explicit id_t(actor_mixin* impl) noexcept
                : impl_{impl} {
            }

            bool operator==(id_t const& other) const noexcept {
                return impl_ == other.impl_;
            }

            bool operator!=(id_t const& other) const noexcept {
                return impl_ != other.impl_;
            }

            bool operator<(id_t const& other) const noexcept {
                return impl_ < other.impl_;
            }

            bool operator>(id_t const& other) const noexcept {
                return other.impl_ < impl_;
            }

            bool operator<=(id_t const& other) const noexcept {
                return !(*this > other);
            }

            bool operator>=(id_t const& other) const noexcept {
                return !(*this < other);
            }

            template<typename charT, class traitsT>
            friend std::basic_ostream<charT, traitsT>&
            operator<<(std::basic_ostream<charT, traitsT>& os, id_t const& other) {
                if (nullptr != other.impl_) {
                    return os << other.impl_;
                }
                return os << "{not-valid}";
            }

            explicit operator bool() const noexcept {
                return nullptr != impl_;
            }

            bool operator!() const noexcept {
                return nullptr == impl_;
            }

        private:
            actor_mixin* impl_{nullptr};
        };

        // ===== Placement new/delete operators =====
        // CRITICAL: These control actor allocation - only placement new with tag is allowed
        struct placement_tag {};
        constexpr static placement_tag placement = {};

        static void* operator new(size_t, void* ptr, placement_tag) noexcept {
            return ptr;
        }

        static void operator delete(void*, void*, placement_tag) noexcept {}

        static void operator delete(void* ptr) noexcept {
            // Empty - actor managed by unique_ptr with custom deleter
            // Actual deallocation done by pmr::deleter_t
            (void)ptr;
        }

        // Explicitly deleted operators to prevent incorrect usage
        static void* operator new(size_t, void* ptr) = delete;
        static void* operator new(size_t) = delete;
        static void* operator new[](size_t) = delete;
        static void operator delete[](void*) = delete;

        // ===== Deleted copy and move operations =====
        // CRITICAL: Actors must not be copied or moved - only unique_ptr ownership
        actor_mixin(const actor_mixin&) = delete;
        actor_mixin& operator=(const actor_mixin&) = delete;
        actor_mixin(actor_mixin&&) = delete;
        actor_mixin& operator=(actor_mixin&&) = delete;

        /// @brief Get address of this actor
        address_t address() noexcept {
            return address_t(static_cast<Derived*>(this));
        }

        /// @brief Get unique ID of this actor (based on pointer)
        id_t id() const {
            return id_t(const_cast<actor_mixin*>(this));
        }

        /// @brief Get polymorphic allocator for type T
        /// @note Uses CRTP to call derived class's resource() method
        template<class T>
        pmr::polymorphic_allocator<T> allocator() const noexcept {
            return {static_cast<const Derived*>(this)->resource()};
        }

        /// @brief Helper for supervisors - simplifies enqueue_impl<R, Args...>() implementation
        /// @note Supervisor calls behavior() synchronously, but returns async future (already ready)
        /// @note NEW API: Creates message in receiver's resource, calls behavior, returns ready future
        /// @note Usage: return enqueue_sync_impl<R>(sender, cmd, [this](auto* msg) { behavior(msg); }, std::forward<Args>(args)...);
        /// @note Uses CRTP to call derived class's resource() method
        template<typename R, typename BehaviorFunc, typename... Args>
        unique_future<R> enqueue_sync_impl(
            base::address_t sender,
            mailbox::message_id cmd,
            BehaviorFunc&& behavior_func,
            Args&&... args
        ) {
            auto* derived = static_cast<Derived*>(this);
            auto* res = derived->resource();

            // Create message in receiver's resource (avoid cross-arena migration)
            auto msg = detail::make_message(
                res,
                std::move(sender),
                cmd,
                std::forward<Args>(args)...
            );

            // Allocate future_state<R> for result with refcount=2 (future + supervisor code)
            void* mem = res->allocate(sizeof(detail::future_state<R>), alignof(detail::future_state<R>));
            auto* slot = new (mem) detail::future_state<R>(res);
            msg->set_result_slot(slot);

            // Call behavior function with message pointer (synchronous execution)
            behavior_func(msg.get());

            // Slot is now ready (behavior already executed)
            // Return async future (already in ready state, no waiting needed)
            return unique_future<R>{slot, false};  // needs_scheduling=false (sync execution, no mailbox)
        }

    protected:
        actor_mixin() noexcept = default;
        ~actor_mixin() = default;
    };

}} // namespace actor_zeta::base
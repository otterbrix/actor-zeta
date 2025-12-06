#pragma once

#include <actor-zeta/actor/address.hpp>
#include <actor-zeta/detail/future_state.hpp>
#include <memory_resource>
#include <actor-zeta/future.hpp>
#include <actor-zeta/make_message.hpp>

namespace actor_zeta { namespace actor {

    /// @brief Actor mixin - common functionality for all actor types
    /// Provides: id_t, address(), placement operators, enqueue_sync_impl()
    /// Used by: cooperative_actor, supervisor, and other actor types
    template<typename Derived>
    class actor_mixin {
    public:
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

        struct placement_tag {};
        constexpr static placement_tag placement = {};

        static void* operator new(size_t, void* ptr, placement_tag) noexcept {
            return ptr;
        }

        static void operator delete(void*, void*, placement_tag) noexcept {}

        static void operator delete(void* ptr) noexcept { (void)ptr; }

        static void* operator new(size_t, void* ptr) = delete;
        static void* operator new(size_t) = delete;
        static void* operator new[](size_t) = delete;
        static void operator delete[](void*) = delete;

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
        template<class T>
        std::pmr::polymorphic_allocator<T> allocator() const noexcept {
            return {static_cast<const Derived*>(this)->resource()};
        }

        /// @brief Sync enqueue for supervisors - calls behavior synchronously, returns ready future
        template<typename R, typename BehaviorFunc, typename... Args>
        unique_future<R> enqueue_sync_impl(
            actor::address_t sender,
            mailbox::message_id cmd,
            BehaviorFunc&& behavior_func,
            Args&&... args
        ) {
            auto* derived = static_cast<Derived*>(this);
            auto* res = derived->resource();

            // Create message + promise in one call (cleaner API)
            auto [msg, future] = detail::make_message_with_result<R>(
                res,
                std::move(sender),
                cmd,
                std::forward<Args>(args)...
            );

            // Call behavior function with message pointer (synchronous execution)
            behavior_func(msg.get());

            // Slot is now ready (behavior already executed)
            return std::move(future);
        }

    protected:
        actor_mixin() noexcept = default;
        ~actor_mixin() = default;
    };

}} // namespace actor_zeta::base
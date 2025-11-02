#pragma once

#include <string>
#include <utility>
#include <cassert>

#include "forwards.hpp"
#include <actor-zeta/detail/memory_resource.hpp>
#include <actor-zeta/detail/ref_counted.hpp>
#include <actor-zeta/detail/queue/enqueue_result.hpp>
#include <actor-zeta/detail/future_state.hpp>
#include <actor-zeta/mailbox/message.hpp>
#include <actor-zeta/future.hpp>

namespace actor_zeta {

namespace base {
    ///
    /// @brief Abstract concept of an actor
    ///

    class actor_abstract_t : public ref_counted {
    public:
        struct placement_tag {};
        constexpr static placement_tag placement = {};

        static void* operator new(size_t, void* ptr, placement_tag) noexcept {
            return ptr;
        }

        static void operator delete(void*, void*, placement_tag) noexcept {}

        static void operator delete(void* ptr) noexcept;

        static void* operator new(size_t, void* ptr) = delete;
        static void* operator new(size_t) = delete;
        static void* operator new[](size_t) = delete;
        static void operator delete[](void*) = delete;

        virtual ~actor_abstract_t();

        class id_t final {
        public:
            id_t() = delete;

            explicit id_t(actor_abstract_t* impl) noexcept
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
            actor_abstract_t* impl_{nullptr};
        };

        /// @brief Default future type - async unique_future
        /// @note Supervisors now use async futures (same as cooperative actors)
        template<typename T>
        using unique_future = actor_zeta::unique_future<T>;

        address_t address() noexcept;
        id_t id() const;
        pmr::memory_resource* resource() const noexcept;

        template<class T>
        pmr::polymorphic_allocator<T> allocator() const noexcept {
            return {resource_};
        }

    protected:
        explicit actor_abstract_t(pmr::memory_resource* resource);

        actor_abstract_t() = delete;
        actor_abstract_t(const actor_abstract_t&) = delete;
        actor_abstract_t& operator=(const actor_abstract_t&) = delete;
        actor_abstract_t(actor_abstract_t&&) = delete;
        actor_abstract_t& operator=(actor_abstract_t&&) = delete;

        /// @brief Helper для supervisors - упрощает реализацию enqueue_impl<R>()
        /// @note Supervisor вызывает behavior() синхронно, но возвращает async future (уже готовый)
        /// @note Supervisor просто вызывает: return enqueue_sync_impl<R>(std::move(msg), [this](auto* msg) { behavior(msg); });
        template<typename R, typename BehaviorFunc>
        unique_future<R> enqueue_sync_impl(mailbox::message_ptr msg, BehaviorFunc&& behavior_func) {
            // Allocate future_state<R> for result with refcount=2 (future + supervisor code)
            void* mem = resource_->allocate(sizeof(detail::future_state<R>), alignof(detail::future_state<R>));
            auto* slot = new (mem) detail::future_state<R>(resource_);
            msg->set_result_slot(slot);

            // Call behavior function with message pointer (synchronous execution)
            behavior_func(msg.get());

            // Slot is now ready (behavior already executed)
            // Return async future (already in ready state, no waiting needed)
            return unique_future<R>{slot, false};  // needs_scheduling=false (sync execution, no mailbox)
        }

    private:
        actor_zeta::pmr::memory_resource* resource_;
    };

}} // namespace actor_zeta::base

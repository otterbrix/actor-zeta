#pragma once

#include <string>
#include <utility>
#include <cassert>

#include "forwards.hpp"
#include <actor-zeta/detail/memory_resource.hpp>
#include <actor-zeta/detail/ref_counted.hpp>
#include <actor-zeta/detail/queue/enqueue_result.hpp>
#include <actor-zeta/detail/slot_refcount.hpp>
#include <actor-zeta/mailbox/message.hpp>

namespace actor_zeta {

    /// @brief Synchronous future для supervisor - результат доступен сразу
    /// @note Используется для supervisor методов, которые выполняются синхронно
    /// @note Ошибки обрабатываются через assert (fail-fast)
    template<typename T>
    class sync_future {
        T value_;

    public:
        explicit sync_future(T value)
            : value_(std::move(value)) {}

        // Move-only
        sync_future(sync_future&&) = default;
        sync_future& operator=(sync_future&&) = default;
        sync_future(const sync_future&) = delete;
        sync_future& operator=(const sync_future&) = delete;

        /// @brief Получить результат (destructive read)
        T get() {
            return std::move(value_);
        }

        /// @brief Всегда готово (sync execution)
        bool is_ready() const {
            return true;
        }

        /// @brief Всегда ok (ошибки через assert)
        mailbox::slot_error_code error() const {
            return mailbox::slot_error_code::ok;
        }
    };

    /// @brief Специализация для void
    template<>
    class sync_future<void> {
    public:
        explicit sync_future() = default;

        // Move-only
        sync_future(sync_future&&) = default;
        sync_future& operator=(sync_future&&) = default;
        sync_future(const sync_future&) = delete;
        sync_future& operator=(const sync_future&) = delete;

        void get() {
            // Nothing to return
        }

        bool is_ready() const {
            return true;
        }

        mailbox::slot_error_code error() const {
            return mailbox::slot_error_code::ok;
        }
    };

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

        /// @brief Default future type for supervisors - sync_future
        /// @note Наследники могут переопределить этот typedef (например, cooperative_actor использует async_future)
        template<typename T>
        using unique_future = actor_zeta::sync_future<T>;

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

        /// @brief Helper для supervisors - упрощает реализацию enqueue_impl<R>() (REFCOUNT MODEL)
        /// @note Supervisor просто вызывает: return enqueue_sync_impl<R>(std::move(msg), [this](auto* msg) { behavior(msg); });
        template<typename R, typename BehaviorFunc>
        unique_future<R> enqueue_sync_impl(mailbox::message_ptr msg, BehaviorFunc&& behavior_func) {
            // Allocate slot for result with refcount=2 (future + supervisor code)
            void* mem = resource_->allocate(sizeof(detail::slot_refcount), alignof(detail::slot_refcount));
            auto* slot = new (mem) detail::slot_refcount(resource_);
            msg->set_result_slot(slot);

            // Call behavior function with message pointer
            behavior_func(msg.get());

            // Extract result from slot
            if constexpr (std::is_void_v<R>) {
                // Release supervisor's reference (refcount: 2 -> 1)
                slot->release();
                // Return sync_future (will release the other reference)
                return sync_future<void>{};
            } else {
                R result = slot->result().template get<R>(0);
                // Release supervisor's reference (refcount: 2 -> 1)
                slot->release();
                // Return sync_future with result (will release the other reference)
                return sync_future<R>{std::move(result)};
            }
        }

    private:
        actor_zeta::pmr::memory_resource* resource_;
    };

}} // namespace actor_zeta::base

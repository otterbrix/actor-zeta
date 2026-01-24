#pragma once

#include <cassert>
#include <memory_resource>
#include <utility>

#include <actor-zeta/config.hpp>
#include <actor-zeta/detail/coroutine.hpp>
#include <actor-zeta/detail/coro_frame_header.hpp>
#include <actor-zeta/detail/type_traits.hpp>

namespace actor_zeta {

    // Forward declaration
    template<typename T>
    class unique_future;

    namespace detail {
        template<typename T>
        concept has_resource_method_behavior = requires(T* ptr) {
            { ptr->resource() } -> std::convertible_to<std::pmr::memory_resource*>;
        };
    } // namespace detail

    /// @brief Coroutine type for behavior() method
    /// Framework stores ONE coroutine per actor.
    /// behavior() returns coroutine, framework does: current_behavior_ = self()->behavior(msg)
    struct behavior_t {
        struct promise_type {
            std::pmr::memory_resource* resource_ = nullptr;

            behavior_t get_return_object() noexcept {
                return behavior_t{detail::coroutine_handle<promise_type>::from_promise(*this)};
            }

            // immediate start (no initial suspend)
            detail::suspend_never initial_suspend() noexcept { return {}; }

            // suspend at end (coroutine doesn't self-destroy, ~behavior_t() calls destroy())
            detail::suspend_always final_suspend() noexcept { return {}; }

            void return_void() noexcept {}

            void unhandled_exception() noexcept {
                assert(false && "unhandled_exception() should never be called (-fno-exceptions)");
                std::terminate();
            }

            // await_transform for unique_future<T>
            template<typename T>
            auto await_transform(unique_future<T>&& f) noexcept {
                return typename unique_future<T>::awaiter{f.internal_state()};
            }

            // await_transform for pair<bool, unique_future<T>> from send()
            template<typename T>
            auto await_transform(std::pair<bool, unique_future<T>>&& p) noexcept;

            // === PMR allocation (same pattern as unique_future) ===

            // Default constructor (should not be used in practice)
            promise_type() noexcept
                : resource_(nullptr) {}

            // Constructor extracting resource from first argument (this* of Actor)
            template<typename First, typename... Args>
            promise_type(First&& first, Args&&...) noexcept
                : resource_(extract_resource_or_null(std::forward<First>(first))) {}

            template<typename... Args>
            static void* operator new(std::size_t size, const Args&... args) {
                auto* res = extract_resource_or_abort(args...);
                return detail::allocate_coro_frame(res, size);
            }

            template<typename... Args>
            static void operator delete(void* ptr, std::size_t size, const Args&...) noexcept {
                detail::deallocate_coro_frame(ptr, size);
            }

            static void operator delete(void* ptr, std::size_t size) noexcept {
                detail::deallocate_coro_frame(ptr, size);
            }

            static void operator delete(void* ptr) noexcept {
                detail::deallocate_coro_frame_unsized(ptr);
            }

        private:
            template<typename U>
            static std::pmr::memory_resource* try_get_resource(U* ptr) noexcept {
                if constexpr (detail::has_resource_method_behavior<U>) {
                    return ptr->resource();
                } else {
                    return nullptr;
                }
            }

            template<typename U>
            static std::pmr::memory_resource* extract_resource_impl(U&& arg) noexcept {
                using decayed = std::decay_t<U>;
                if constexpr (std::is_pointer_v<decayed>) {
                    using ptr_type = std::remove_reference_t<U>;
                    return try_get_resource(static_cast<ptr_type>(arg));
                } else {
                    return try_get_resource(&arg);
                }
            }

            static std::pmr::memory_resource* extract_resource_impl(std::pmr::memory_resource* res) noexcept {
                return res;
            }

            static std::pmr::memory_resource* extract_resource_from_args() noexcept {
                return nullptr;
            }

            template<typename First, typename... Rest>
            static std::pmr::memory_resource* extract_resource_from_args(First&& first, Rest&&... rest) noexcept {
                auto res = extract_resource_impl(std::forward<First>(first));
                if (res != nullptr)
                    return res;
                if constexpr (sizeof...(Rest) > 0) {
                    return extract_resource_from_args(std::forward<Rest>(rest)...);
                }
                return nullptr;
            }

            template<typename First, typename... Rest>
            static std::pmr::memory_resource* extract_resource_or_null(First&& first, Rest&&... rest) noexcept {
                return extract_resource_from_args(std::forward<First>(first), std::forward<Rest>(rest)...);
            }

            [[noreturn]] static std::pmr::memory_resource* extract_resource_or_abort() noexcept {
                assert(false && "Coroutine must be defined inline (GCC doesn't pass 'this' for out-of-line methods)");
                std::abort();
            }

            template<typename First, typename... Rest>
            RETURNS_NONNULL static std::pmr::memory_resource* extract_resource_or_abort(First&& first, Rest&&... rest) noexcept {
                auto* res = extract_resource_from_args(std::forward<First>(first), std::forward<Rest>(rest)...);
                assert(res != nullptr && "Coroutine must be actor member function with resource() method");
                if (!res) {
                    std::abort();
                }
                return res;
            }
        };

        detail::coroutine_handle<promise_type> handle_;

        behavior_t() noexcept
            : handle_{} {}

        explicit behavior_t(detail::coroutine_handle<promise_type> h) noexcept
            : handle_(h) {}

        behavior_t(behavior_t&& o) noexcept
            : handle_(std::exchange(o.handle_, {})) {}

        behavior_t& operator=(behavior_t&& o) noexcept {
            if (this != &o) {
                if (handle_) {
                    handle_.destroy();
                }
                handle_ = std::exchange(o.handle_, {});
            }
            return *this;
        }

        ~behavior_t() {
            if (handle_) {
                handle_.destroy();
            }
        }

        behavior_t(const behavior_t&) = delete;
        behavior_t& operator=(const behavior_t&) = delete;

        [[nodiscard]] bool done() const noexcept {
            return !handle_ || handle_.done();
        }

        explicit operator bool() const noexcept {
            return handle_ != nullptr;
        }
    };

    // Deferred implementation of await_transform for pair (needs unique_future definition)
    template<typename T>
    auto behavior_t::promise_type::await_transform(std::pair<bool, unique_future<T>>&& p) noexcept {
        struct pair_awaiter {
            bool needs_sched_;
            typename unique_future<T>::awaiter inner_;

            bool await_ready() const noexcept { return inner_.await_ready(); }

            auto await_suspend(detail::coroutine_handle<> h) noexcept {
                return inner_.await_suspend(h);
            }

            auto await_resume() {
                if constexpr (std::is_void_v<T>) {
                    inner_.await_resume();
                    return needs_sched_;
                } else {
                    return std::make_pair(needs_sched_, inner_.await_resume());
                }
            }
        };
        return pair_awaiter{p.first, typename unique_future<T>::awaiter{p.second.internal_state()}};
    }

} // namespace actor_zeta

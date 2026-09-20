#pragma once

#include <cassert>
#include <memory_resource>
#include <utility>

#include <actor-zeta/config.hpp>
#include <actor-zeta/detail/coroutine.hpp>
#include <actor-zeta/detail/coro_frame_header.hpp>
#include <actor-zeta/detail/future_awaiters.hpp>
#include <actor-zeta/detail/state_flags.hpp>
#include <actor-zeta/detail/type_traits.hpp>

namespace actor_zeta {

    // Forward declaration
    template<typename T>
    class unique_future;

    /// @brief Coroutine type for behavior() method
    /// Framework stores ONE coroutine per actor.
    /// behavior() returns coroutine, framework does: current_behavior_ = self()->behavior(msg)
    struct behavior_t {
        // promise_type inherits the shared awaiter machinery (await_transform overloads +
        // lock-free CAS + awaited-chain propagation) from detail::future_awaiter_mixin
        // (future_awaiters.hpp), the same one unique_future uses.
        // behavior_t is always the chain ROOT (no outer promise).
        struct promise_type : detail::future_awaiter_mixin<promise_type> {
            std::pmr::memory_resource* resource_ = nullptr;

            behavior_t get_return_object() noexcept {
                return behavior_t{detail::coroutine_handle<promise_type>::from_promise(*this)};
            }

            // immediate start (no initial suspend)
            detail::suspend_never initial_suspend() noexcept { return {}; }

            // Q7: always stay suspended at final_suspend; ~behavior_t() destroys the frame.
            // Safe unconditionally because the producer never calls cont.resume() (Q6), so
            // the frame is never running on another thread.
            auto final_suspend() noexcept {
                struct final_awaiter {
                    bool await_ready() const noexcept { return false; }
                    void await_suspend(detail::coroutine_handle<promise_type>) const noexcept {
                        // Stay suspended, ~behavior_t() will destroy
                    }
                    void await_resume() const noexcept {}
                };
                return final_awaiter{};
            }

            void return_void() noexcept {}

            void unhandled_exception() noexcept {
                assert(false && "unhandled_exception() should never be called (-fno-exceptions)");
                std::terminate();
            }

            // await_transform overloads for the actor-zeta awaitables (unique_future<T>&&,
            // pair<bool, unique_future<T>>&&) are inherited from
            // detail::future_awaiter_mixin<promise_type>. There is NO generic foreign-awaitable
            // passthrough anywhere: a foreign (e.g. Asio) awaiter would resume the coroutine
            // off its scheduler thread (UAF / threading hazard). External loops integrate by
            // polling our unique_future instead — see the note in future_awaiters.hpp.

            // === PMR allocation (same pattern as unique_future) ===

            // Default constructor (should not be used in practice)
            promise_type() noexcept
                : resource_(nullptr) {}

            // Constructor extracting resource from first argument (this* of Actor)
            template<typename First, typename... Args>
            promise_type(First&& first, Args&&...) noexcept
                : resource_(extract_resource_or_null(std::forward<std::remove_reference_t<First>>(first))) {}

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
                if constexpr (detail::has_resource_method<U>) {
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

            // The SAME predicate the runtime path above uses, lifted so a static_assert
            // and extract_resource_impl() can never disagree about a type.
            template<typename U>
            static constexpr bool supplies_resource() noexcept {
                using decayed = std::decay_t<U>;
                if constexpr (std::is_same_v<decayed, std::pmr::memory_resource*>) {
                    return true;
                } else if constexpr (std::is_pointer_v<decayed>) {
                    return detail::has_resource_method<
                        std::remove_pointer_t<std::remove_reference_t<U>>>;
                } else {
                    return detail::has_resource_method<std::remove_reference_t<U>>;
                }
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

            // Zero arguments means there is nothing that could carry a memory resource.
            // A property of the coroutine's SIGNATURE, so it is decided at compile time.
            template<typename Dependent = promise_type>
            [[noreturn]] static std::pmr::memory_resource* extract_resource_or_abort() noexcept {
                static_assert(sizeof(Dependent) == 0,
                              "behavior() must be an actor member function defined inline "
                              "(GCC does not pass 'this' for out-of-line methods)");
                std::abort();
            }

            template<typename First, typename... Rest>
            RETURNS_NONNULL static std::pmr::memory_resource* extract_resource_or_abort(First&& first, Rest&&... rest) noexcept {
                // Whether ANY argument can supply a resource is a property of the types.
                // The pointer merely being null at run time is not -- that stays an assert.
                static_assert((supplies_resource<First>() || ... || supplies_resource<Rest>()),
                              "no argument of behavior() can supply a memory resource -- "
                              "it must be an actor member function so `this` is in the pack");
                auto* res = extract_resource_from_args(std::forward<First>(first), std::forward<Rest>(rest)...);
                assert(res != nullptr && "resource() returned null");
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

        // Q7: always destroy. Safe because the producer never calls cont.resume() (Q6), so
        // the coroutine is either done or suspended, never running on another thread.
        behavior_t& operator=(behavior_t&& o) noexcept {
            if (this != &o) {
                if (handle_) {
                    handle_.destroy();
                }
                handle_ = std::exchange(o.handle_, {});
            }
            return *this;
        }

        // Q7: always destroy — see operator=.
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

        /// @brief Check if behavior is suspended on co_await (waiting for async result)
        [[nodiscard]] bool is_busy() const noexcept {
            return handle_ && !handle_.done() && handle_.promise().awaited_flags_ != nullptr;
        }

        /// @brief Q8: Check if awaited future is ready (promise_released)
        [[nodiscard]] bool is_awaited_ready() const noexcept {
            if (!handle_ || handle_.done()) {
                return false;
            }
            auto* flags = handle_.promise().awaited_flags_;
            if (!flags) {
                return false;
            }
            return flags->load(std::memory_order_acquire) & detail::state_flags::promise_released;
        }

        /// @brief Take the deepest awaited continuation for resuming
        /// @return The continuation handle, or null if none
        [[nodiscard]] detail::coroutine_handle<> take_awaited_continuation() noexcept {
            if (!handle_ || handle_.done()) {
                return nullptr;
            }
            auto* cont_ptr = handle_.promise().awaited_continuation_;
            if (!cont_ptr) {
                return nullptr;
            }
            return cont_ptr->exchange(nullptr, std::memory_order_acq_rel);
        }
    };

    // NOTE: The await_transform overloads for unique_future<T>&& and
    // std::pair<bool, unique_future<T>>&& are inherited from
    // detail::future_awaiter_mixin<promise_type> (future_awaiters.hpp). They are templates
    // instantiated at the co_await site, which is what lets this header get away with only
    // a forward declaration of unique_future — no out-of-line definition is needed here.

} // namespace actor_zeta

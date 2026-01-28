#pragma once

#include <cassert>
#include <memory_resource>
#include <utility>

#include <actor-zeta/config.hpp>
#include <actor-zeta/detail/coroutine.hpp>
#include <actor-zeta/detail/coro_frame_header.hpp>
#include <actor-zeta/detail/state_flags.hpp>
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
            // Track deepest awaited future for spinning mechanism (propagated through chain)
            std::atomic<std::uint8_t>* awaited_flags_ = nullptr;
            std::atomic<detail::coroutine_handle<>>* awaited_continuation_ = nullptr;
            // Track where our awaited state was propagated to (outer promise's pointers)
            // behavior_t is usually the root, so this is often nullptr
            std::atomic<std::uint8_t>** propagated_to_flags_ = nullptr;
            std::atomic<detail::coroutine_handle<>>** propagated_to_cont_ = nullptr;

            behavior_t get_return_object() noexcept {
                return behavior_t{detail::coroutine_handle<promise_type>::from_promise(*this)};
            }

            // immediate start (no initial suspend)
            detail::suspend_never initial_suspend() noexcept { return {}; }

            // Q7: Always stay suspended at final_suspend, ~behavior_t() will destroy
            // No more detached_ logic - with (no cont.resume()), destroy is always safe
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

            // await_transform for unique_future<T>
            // CRITICAL: The awaiter must OWN the future to prevent premature destruction.
            // If we just extract the state pointer, the temporary unique_future is destroyed
            // right after await_transform returns, setting future_released flag.
            template<typename T>
            auto await_transform(unique_future<T>&& future) noexcept {
                // Propagate deepest awaited state for spinning mechanism
                propagate_awaited_state(future);

                struct owning_awaiter {
                    unique_future<T> owned_;
                    std::atomic<std::uint8_t>** flags_ptr_;
                    std::atomic<detail::coroutine_handle<>>** cont_ptr_;
                    std::atomic<std::uint8_t>** outer_flags_ptr_;
                    std::atomic<detail::coroutine_handle<>>** outer_cont_ptr_;

                    bool await_ready() const noexcept {
                        return owned_.internal_state()->has_result();
                    }

                    detail::coroutine_handle<> await_suspend(detail::coroutine_handle<> h) noexcept {
                        auto* state = owned_.internal_state();
                        detail::coroutine_handle<> expected = nullptr;
                        if (state->continuation_.compare_exchange_strong(
                                expected, h,
                                std::memory_order_acq_rel,
                                std::memory_order_acquire)) {
                            if (state->flags_.load(std::memory_order_acquire)
                                    & detail::state_flags::result_set) {
                                auto cont = state->continuation_.exchange(
                                    nullptr, std::memory_order_acquire);
                                if (cont) {
                                    return cont;
                                }
                                return detail::noop_coroutine();
                            }
                            return detail::noop_coroutine();
                        } else {
                            assert(false && "double co_await on unique_future is undefined behavior");
                            return h;
                        }
                    }

                    auto await_resume() {
                        // Clear awaited state since we're done waiting
                        if (flags_ptr_) *flags_ptr_ = nullptr;
                        if (cont_ptr_) *cont_ptr_ = nullptr;
                        // Also clear outer promise's copy (if any) BEFORE state is freed
                        if (outer_flags_ptr_) *outer_flags_ptr_ = nullptr;
                        if (outer_cont_ptr_) *outer_cont_ptr_ = nullptr;

                        auto* state = owned_.internal_state();
                        assert(!state->has_error() && "future completed with error");
                        if constexpr (std::is_void_v<T>) {
                            state->take_value();
                        } else {
                            return state->take_value();
                        }
                    }
                };
                return owning_awaiter{std::move(future), &awaited_flags_, &awaited_continuation_,
                                       propagated_to_flags_, propagated_to_cont_};
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
            // Propagate deepest awaited state from inner coroutine (for spinning mechanism)
            template<typename T>
            void propagate_awaited_state(unique_future<T>& future) noexcept {
                // If result is already set, coroutine may have been destroyed via final_suspend.
                // No need to track awaited state — await_ready() will return true.
                if (future.internal_state()->has_result()) {
                    awaited_flags_ = nullptr;
                    awaited_continuation_ = nullptr;
                    // Also update outer promise if we propagated to it
                    update_propagated_outer();
                    return;
                }

                auto inner_handle = future.coroutine_handle();
                if (inner_handle) {
                    // Method coroutine — check if it has deeper awaited state
                    auto& inner_promise = inner_handle.promise();

                    // Set up back-reference for direct field updates
                    inner_promise.propagated_to_flags_ = &awaited_flags_;
                    inner_promise.propagated_to_cont_ = &awaited_continuation_;

                    // behavior_t IS the root. Set up type-erased outer promise pointer
                    // so inner can call our update_propagated_outer() recursively.
                    inner_promise.outer_promise_raw_ = this;
                    inner_promise.outer_update_fn_ = &call_update_propagated_outer;

                    if (inner_promise.awaited_flags_) {
                        // Inner coroutine is waiting for something deeper — propagate
                        awaited_flags_ = inner_promise.awaited_flags_;
                        awaited_continuation_ = inner_promise.awaited_continuation_;
                    } else {
                        // Inner coroutine not waiting — this future is the deepest level
                        awaited_flags_ = &future.internal_state()->flags_;
                        awaited_continuation_ = &future.internal_state()->continuation_;
                    }
                } else {
                    // Cross-actor future (from promise.get_future()) — this is the deepest level
                    awaited_flags_ = &future.internal_state()->flags_;
                    awaited_continuation_ = &future.internal_state()->continuation_;
                }

                // Update outer promise with our new awaited state
                update_propagated_outer();
            }

            // Static helper to call update_propagated_outer() on this promise type
            static void call_update_propagated_outer(void* promise) noexcept {
                static_cast<promise_type*>(promise)->update_propagated_outer();
            }

            // Update outer promise's copy of our awaited state
            void update_propagated_outer() noexcept {
                if (propagated_to_flags_) {
                    *propagated_to_flags_ = awaited_flags_;
                }
                if (propagated_to_cont_) {
                    *propagated_to_cont_ = awaited_continuation_;
                }
                // behavior_t IS the root, so no outer to propagate to
            }

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

        // Q7: Always destroy - with Q6 (no cont.resume()), destroy is always safe
        // because coroutine is either done or suspended (never running on another thread)
        behavior_t& operator=(behavior_t&& o) noexcept {
            if (this != &o) {
                if (handle_) {
                    handle_.destroy();  // Always safe - no race with cont.resume()
                }
                handle_ = std::exchange(o.handle_, {});
            }
            return *this;
        }

        // Q7: Always destroy - safe because producer doesn't call cont.resume()
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

        /// @brief Resume the coroutine (call from resume_impl when awaited future is ready)
        void resume() noexcept {
            assert(handle_ && !handle_.done() && "resume() on invalid or done behavior");
            handle_.resume();
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

    // Deferred implementation of await_transform for pair (needs unique_future definition)
    // CRITICAL: Must own the future to prevent premature destruction (see owning_awaiter above)
    template<typename T>
    auto behavior_t::promise_type::await_transform(std::pair<bool, unique_future<T>>&& p) noexcept {
        // Propagate deepest awaited state for spinning mechanism
        propagate_awaited_state(p.second);

        struct owning_pair_awaiter {
            bool needs_sched_;
            unique_future<T> owned_;
            std::atomic<std::uint8_t>** flags_ptr_;
            std::atomic<detail::coroutine_handle<>>** cont_ptr_;
            std::atomic<std::uint8_t>** outer_flags_ptr_;
            std::atomic<detail::coroutine_handle<>>** outer_cont_ptr_;

            bool await_ready() const noexcept {
                return owned_.internal_state()->has_result();
            }

            detail::coroutine_handle<> await_suspend(detail::coroutine_handle<> h) noexcept {
                auto* state = owned_.internal_state();
                detail::coroutine_handle<> expected = nullptr;
                if (state->continuation_.compare_exchange_strong(
                        expected, h,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    if (state->flags_.load(std::memory_order_acquire)
                            & detail::state_flags::result_set) {
                        auto cont = state->continuation_.exchange(
                            nullptr, std::memory_order_acquire);
                        if (cont) {
                            return cont;
                        }
                        return detail::noop_coroutine();
                    }
                    return detail::noop_coroutine();
                } else {
                    assert(false && "double co_await on unique_future is undefined behavior");
                    return h;
                }
            }

            auto await_resume() {
                // Clear awaited state since we're done waiting
                if (flags_ptr_) *flags_ptr_ = nullptr;
                if (cont_ptr_) *cont_ptr_ = nullptr;
                // Also clear outer promise's copy (if any) BEFORE state is freed
                if (outer_flags_ptr_) *outer_flags_ptr_ = nullptr;
                if (outer_cont_ptr_) *outer_cont_ptr_ = nullptr;

                auto* state = owned_.internal_state();
                assert(!state->has_error() && "future completed with error");
                if constexpr (std::is_void_v<T>) {
                    state->take_value();
                    return needs_sched_;
                } else {
                    return std::make_pair(needs_sched_, state->take_value());
                }
            }
        };
        return owning_pair_awaiter{p.first, std::move(p.second), &awaited_flags_, &awaited_continuation_, propagated_to_flags_, propagated_to_cont_};
    }

} // namespace actor_zeta

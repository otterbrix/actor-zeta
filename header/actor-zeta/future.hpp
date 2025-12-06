#pragma once

#include <cassert>
#include <new>
#include <chrono>
#include <thread>
#include <type_traits>
#include <utility>
#include <memory_resource>

#include <actor-zeta/config.hpp>
#include <actor-zeta/detail/future_state.hpp>
#include <actor-zeta/detail/intrusive_ptr.hpp>
#include <actor-zeta/detail/rtt.hpp>
#include <actor-zeta/detail/coroutine.hpp>
#include <actor-zeta/detail/coro_frame_header.hpp>

namespace actor_zeta {

    template<typename T>
    class unique_future;

    // internal_construct_tag is defined in mailbox/message.hpp to avoid ODR violation
    // It's a simple empty struct used as a tag for internal constructors

    /// @brief Unified promise<T> - works for both void and non-void types
    /// Uses SFINAE to provide appropriate set_value() overloads
    template<typename T>
    class promise final {
    private:
        static constexpr bool is_void_type = std::is_void_v<T>;
        using state_type = detail::future_state<T>;

    public:
        promise() = delete;
        promise(const promise&) = delete;
        promise& operator=(const promise&) = delete;

        /// @brief Create promise that owns its state
        /// @param res Memory resource for state allocation
        explicit promise(std::pmr::memory_resource* res)
            : state_(nullptr)
            , resource_(res) {
            assert(res && "promise constructed with null resource");
            void* mem = resource_->allocate(sizeof(state_type), alignof(state_type));
            state_ = new (mem) state_type(resource_);
            // state_ starts with refcount=1, promise owns it
        }

        /// @brief Internal constructor - wraps existing state (adds refcount)
        /// @note Used by message::get_result_promise<T>() to create promise view
        /// @note Convention: don't use internal_construct_tag in user code
        explicit promise(internal_construct_tag, state_type* existing_state, std::pmr::memory_resource* res) noexcept
            : state_(existing_state)
            , resource_(res) {
            if (state_) {
                state_->add_ref();
            }
        }

        promise(promise&& other) noexcept
            : state_(other.state_)
            , resource_(other.resource_) {
            other.state_ = nullptr;
        }

        promise& operator=(promise&& other) noexcept {
            if (this != &other) {
                release();
                state_ = other.state_;
                resource_ = other.resource_;
                other.state_ = nullptr;
            }
            return *this;
        }

        ~promise() noexcept {
            release();
        }

        /// @brief Get future associated with this promise
        /// @return unique_future that shares state with this promise
        /// @note Can be called multiple times (each future adds reference)
        [[nodiscard]] unique_future<T> get_future() noexcept;

        /// @brief Set value (non-void types) - perfect forwarding
        /// @note Uses forwarding reference to handle both rvalue and lvalue
        /// @note Accepts any type convertible to T (e.g., const char* → std::string)
        template<typename U, std::enable_if_t<!std::is_void_v<T> && std::is_constructible_v<T, U&&>, int> = 0>
        void set_value(U&& value) {
            assert(state_ && "set_value() on moved-from promise");
            if (state_->is_cancelled()) {
                assert(false && "set_value() on orphaned/cancelled promise");
                return;
            }
            state_->set_value(T(std::forward<U>(value)));
        }

        /// @brief Set value (void type) - no arguments
        template<typename U = T, std::enable_if_t<std::is_void_v<U>, int> = 0>
        void set_value() {
            assert(state_ && "set_value() on moved-from promise");
            if (state_->is_cancelled()) {
                assert(false && "set_value() on orphaned/cancelled promise");
                return;
            }
            state_->set_ready();
        }

        /// @brief Check if promise has valid state
        [[nodiscard]] bool valid() const noexcept {
            return state_ != nullptr;
        }

        /// @brief Get memory resource
        [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
            return resource_;
        }

        /// @brief Internal: get state as base pointer (for library use only)
        /// @note detail:: in return type signals "internal use only"
        [[nodiscard]] detail::future_state_base* internal_state_base() const noexcept {
            return static_cast<detail::future_state_base*>(state_);
        }

    private:
        void release() noexcept {
            if (state_) {
                intrusive_ptr_release(state_);
                state_ = nullptr;
            }
        }

        state_type* state_;
        std::pmr::memory_resource* resource_;
    };

    /// @brief Unified unique_future<T> - works for both void and non-void types
    /// For T=void: get() returns void
    /// For T!=void: get() returns T
    /// All value storage delegated to future_state<T>::result_storage<T>
    template<typename T>
    class unique_future final {
    private:
        static constexpr bool is_void_type = std::is_void_v<T>;
        // For void: use future_state_base to allow storing any future_state<U>* without UB
        // For non-void: use future_state<T> for typed access
        using state_type = std::conditional_t<is_void_type, detail::future_state_base, detail::future_state<T>>;
        // For coroutine allocation: always use concrete type (can't allocate abstract base)
        using coroutine_state_type = std::conditional_t<is_void_type, detail::future_state<void>, detail::future_state<T>>;

    public:
        unique_future(const unique_future&) = delete;
        unique_future& operator=(const unique_future&) = delete;

        /// @brief Construct from promise - THE ONLY PUBLIC CONSTRUCTOR
        /// All unique_future creation goes through promise for clean API
        explicit unique_future(promise<T>& p)
            : state_(static_cast<state_type*>(p.internal_state_base()), true)
            , resource_(p.resource())
            , needs_scheduling_(false) {
        }

        unique_future(unique_future&& other) noexcept
            : state_(std::move(other.state_))
            , resource_(other.resource_)
            , needs_scheduling_(other.needs_scheduling_) {
            other.needs_scheduling_ = false;
        }

        /// @brief Converting constructor from unique_future<U> to unique_future<void>
        /// @note SAFE: Stores as future_state_base* (common base class) - NO UB!
        /// @note Result delivery happens via result_slot mechanism, not through behavior() return
        /// @warning The typed result is DISCARDED - this is intentional for behavior() pattern
        template<typename U, std::enable_if_t<is_void_type && !std::is_void_v<U>, int> = 0>
        unique_future(unique_future<U>&& other) noexcept
            : state_()  // Default init first
            , resource_(other.resource())  // Get resource while other.state_ still valid
            , needs_scheduling_(other.needs_scheduling()) {
            // Release state AFTER getting resource (other.state_ becomes nullptr)
            state_ = intrusive_ptr<state_type>(
                static_cast<state_type*>(other.internal_release_state()), false);
            other.set_needs_scheduling(false);
        }

        unique_future& operator=(unique_future&& other) noexcept {
            if (this != &other) {
                // Cancel current if pending
                if (state_ && !state_->is_ready()) {
                    state_->set_state(detail::future_state_enum::cancelled);
                }

                state_ = std::move(other.state_);
                resource_ = other.resource_;
                needs_scheduling_ = other.needs_scheduling_;
                other.needs_scheduling_ = false;
            }
            return *this;
        }

        ~unique_future() noexcept = default;

        // get() for non-void types - returns T
        template<typename U = T>
        [[nodiscard]] std::enable_if_t<!std::is_void_v<U>, U> get() && {
            assert(state_ && "get() on invalid future");
            wait_for_ready();
            U result = state_->take_value();
            state_ = nullptr;
            return result;
        }

        // get() for void type - returns void
        template<typename U = T>
        std::enable_if_t<std::is_void_v<U>, void> get() && {
            assert(state_ && "get() on invalid future");
            wait_for_ready();
            state_ = nullptr;
        }

        // Deleted lvalue get()
        template<typename U = T>
        std::enable_if_t<!std::is_void_v<U>, U> get() & = delete;
        template<typename U = T>
        std::enable_if_t<std::is_void_v<U>, void> get() & = delete;

        /// @brief Check if result is available (ready or consumed)
        [[nodiscard]] bool available() const noexcept {
            return state_ && state_->is_ready();
        }

        /// @brief Check if future failed (error or cancelled)
        [[nodiscard]] bool failed() const noexcept {
            return state_ && state_->is_failed();
        }

        /// @brief Check if future has valid state
        [[nodiscard]] bool valid() const noexcept {
            return state_ != nullptr;
        }

        /// @brief Explicitly ignore a ready future
        /// Use this to acknowledge that a future result is intentionally discarded
        void ignore() noexcept {
            state_ = nullptr;
        }

        [[nodiscard]] bool needs_scheduling() const noexcept { return needs_scheduling_; }
        void set_needs_scheduling(bool value) noexcept { needs_scheduling_ = value; }

        /// @brief Get memory resource
        [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
            assert(state_ && "memory_resource() called on invalid future");
            return resource_;
        }

        /// @brief Internal: release state and return raw pointer
        /// @note Takes ownership - caller must manage lifetime
        /// @note Used by converting constructor unique_future<void>(unique_future<U>&&)
        [[nodiscard]] state_type* internal_release_state() noexcept {
            return state_.detach();
        }

        void cancel() noexcept {
            if (state_) {
                state_->set_state(detail::future_state_enum::cancelled);
            }
        }

        [[nodiscard]] bool is_cancelled() const noexcept {
            return state_ && state_->is_cancelled();
        }

        /// @brief Forward result to target promise (non-void types)
        /// @note Always sets up forwarding chain (handles both ready and pending)
        /// @note Uses set_forward_target which handles ready states internally
        template<typename U = T, std::enable_if_t<!std::is_void_v<U>, int> = 0>
        void forward_to(promise<T>& target) {
            if (!state_) return;
            auto* target_state = static_cast<detail::future_state<T>*>(target.internal_state_base());
            state_->set_forward_target(target_state);
        }

        /// @brief Forward completion to target promise (void type)
        /// @note Always sets up forwarding chain (handles both ready and pending)
        template<typename U = T, std::enable_if_t<std::is_void_v<U>, int> = 0>
        void forward_to(promise<void>& target) {
            if (!state_) return;
            auto* target_state = static_cast<detail::future_state<void>*>(target.internal_state_base());
            state_->set_forward_target_void(target_state);
        }

        // =========================================================================
        // Nested awaiter for co_await support (has access to private members)
        // =========================================================================
        struct awaiter_type {
            unique_future<T>& future_;
            detail::future_state_base* promise_state_;

            explicit awaiter_type(unique_future<T>& f, detail::future_state_base* prom_state = nullptr) noexcept
                : future_(f)
                , promise_state_(prom_state) {}

            [[nodiscard]] bool await_ready() const noexcept {
                return future_.available();
            }

            std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
                if (future_.available()) {
                    return caller;
                }

                auto* state = future_.get_state_internal();
                if (!state) {
                    return caller;
                }

                state->set_coroutine(caller);
                if (promise_state_) {
                    promise_state_->set_awaiting_on(state);
                }

                if (future_.available()) {
                    return caller;
                }

                return std::noop_coroutine();
            }

            auto await_resume() {
                if (promise_state_) {
                    promise_state_->clear_awaiting_on();
                }
                if constexpr (std::is_void_v<T>) {
                    std::move(future_).get();
                } else {
                    return std::move(future_).get();
                }
            }
        };

        // Coroutine promise_type - uses inheritance to separate return_value/return_void
    private:
        // Internal accessor (private, used by nested awaiter_type)
        [[nodiscard]] state_type* get_state_internal() const noexcept {
            return state_.get();
        }

        // Internal constructor for coroutine promise_type (accessible from nested class)
        unique_future(state_type* state, std::pmr::memory_resource* res, bool add_ref) noexcept
            : state_(state, add_ref)
            , resource_(res)
            , needs_scheduling_(false) {
        }

        // Base class with common promise functionality
        struct promise_type_base {
            using value_type = T;

            unique_future<T> get_return_object() {
                assert(resource_ != nullptr &&
                       "Coroutine must be actor member function with resource() method");

                // Use coroutine_state_type for allocation (concrete type, not abstract base)
                void* mem = resource_->allocate(sizeof(coroutine_state_type), alignof(coroutine_state_type));
                state_ = new (mem) coroutine_state_type(resource_);

                auto handle = std::coroutine_handle<struct promise_type>::from_promise(static_cast<struct promise_type&>(*this));
                state_->set_coroutine_owning(handle);  // This state owns the coroutine

                // For void: coroutine_state_type* (future_state<void>*) → state_type* (future_state_base*)
                // This is legal: derived* → base*
                // Use private constructor: add_ref=false (adopt existing refcount)
                return unique_future<T>(static_cast<state_type*>(state_), resource_, false);
            }

            detail::suspend_never initial_suspend() noexcept { return {}; }

            /// @brief Final awaiter with symmetric transfer support
            /// When coroutine completes, directly resumes awaiting coroutine (if any)
            /// This avoids scheduler roundtrip for chained coroutines
            auto final_suspend() noexcept {
                struct final_awaiter {
                    coroutine_state_type* state_;

                    bool await_ready() noexcept { return false; }

                    /// @brief Symmetric transfer at coroutine completion
                    /// @return Continuation handle if someone is waiting, else noop
                    std::coroutine_handle<> await_suspend(
                        std::coroutine_handle<promise_type> /*h*/
                    ) noexcept {
                        // If there's a waiting coroutine, resume it directly
                        if (auto cont = state_->take_continuation()) {
                            return cont;  // Symmetric transfer!
                        }
                        return std::noop_coroutine();  // No waiter - stay suspended
                    }

                    void await_resume() noexcept {}
                };
                return final_awaiter{this->state_};
            }

            template<typename U>
            auto await_transform(unique_future<U>&& future) noexcept {
                // state_ is coroutine_state_type* which derives from future_state_base
                // Use nested awaiter_type which has access to private members
                return typename unique_future<U>::awaiter_type{future, static_cast<detail::future_state_base*>(state_)};
            }

            void unhandled_exception() noexcept {
                assert(false && "unhandled_exception() should never be called (-fno-exceptions)");
            }

            promise_type_base() noexcept : resource_(nullptr), state_(nullptr) {}

            template<typename First, typename... Args>
            promise_type_base(First&& first, Args&&... args) noexcept
                : resource_(extract_resource_from_args(std::forward<First>(first), std::forward<Args>(args)...))
                , state_(nullptr) {}

            ~promise_type_base() noexcept = default;

        protected:
            template<typename U>
            static auto try_get_resource(U* ptr, int) noexcept -> decltype(ptr->resource()) {
                return ptr->resource();
            }

            template<typename U>
            static std::pmr::memory_resource* try_get_resource(U*, ...) noexcept {
                return nullptr;
            }

            template<typename U>
            static std::pmr::memory_resource* extract_impl_dispatch(U&& arg, std::true_type) noexcept {
                using ptr_type = std::remove_reference_t<U>;
                return try_get_resource(static_cast<ptr_type>(arg), 0);
            }

            template<typename U>
            static std::pmr::memory_resource* extract_impl_dispatch(U&& arg, std::false_type) noexcept {
                return try_get_resource(&arg, 0);
            }

            template<typename U>
            static std::pmr::memory_resource* extract_resource_impl(U&& arg) noexcept {
                using decayed = std::decay_t<U>;
                return extract_impl_dispatch(std::forward<U>(arg), std::is_pointer<decayed>{});
            }

            // Direct overload for std::pmr::memory_resource* - enables lambda-coroutines
            static std::pmr::memory_resource* extract_resource_impl(std::pmr::memory_resource* res) noexcept {
                return res;
            }

            // Base case: no arguments left
            static std::pmr::memory_resource* extract_resource_from_args() noexcept {
                return nullptr;
            }

            // Recursive search through all arguments for memory_resource
            // This is needed for lambda-coroutines with captures, where the closure
            // object is passed as the first argument to the promise constructor,
            // and the actual parameters (including std::pmr::memory_resource*) follow after.
            template<typename First, typename... Rest>
            static std::pmr::memory_resource* extract_resource_from_args(First&& first, Rest&&... rest) noexcept {
                auto res = extract_resource_impl(std::forward<First>(first));
                if (res != nullptr) return res;
                if constexpr (sizeof...(Rest) > 0) {
                    return extract_resource_from_args(std::forward<Rest>(rest)...);
                }
                return nullptr;
            }

            std::pmr::memory_resource* resource_;
            coroutine_state_type* state_;  // Use concrete type for coroutine operations
        };

        // Non-void: has return_value
        template<typename U, bool IsVoid>
        struct promise_type_return : promise_type_base {
            using promise_type_base::promise_type_base;

            void return_value(U&& value) noexcept {
                assert(this->state_ && "return_value() with null state");
                this->state_->set_value(std::forward<U>(value));
            }

            void return_value(const U& value) noexcept {
                assert(this->state_ && "return_value() with null state");
                this->state_->set_value(value);
            }

            void return_value(unique_future<U>&& ready_future) noexcept {
                assert(this->state_ && "return_value() with null state");
                assert(ready_future.valid() && "return_value() with invalid future");
                U val = std::move(ready_future).get();
                this->state_->set_value(std::move(val));
            }
        };

        // Void specialization: has return_void
        template<typename U>
        struct promise_type_return<U, true> : promise_type_base {
            using promise_type_base::promise_type_base;

            void return_void() noexcept {
                assert(this->state_ && "return_void() with null state");
                this->state_->set_ready();
            }
        };

    public:
        // Final promise_type selects correct base
        struct promise_type : promise_type_return<T, is_void_type> {
            using promise_type_return<T, is_void_type>::promise_type_return;

            // =========================================================================
            // Custom coroutine frame allocation using actor's memory_resource
            // =========================================================================

            /// @brief Allocate coroutine frame using actor's memory_resource
            /// @note Compiler passes coroutine arguments to operator new
            /// @note First argument is typically 'this' pointer (actor*)
            /// @note We pass arguments without std::forward to avoid GCC 11 ambiguity with collapsed references
            template<typename First, typename... Args>
            static void* operator new(std::size_t size, First&& first, Args&&... args) {
                // Don't use std::forward - just pass as lvalues to avoid GCC 11 ambiguity
                auto* res = promise_type_base::extract_resource_from_args(first, args...);
                return detail::allocate_coro_frame(res, size);
            }

            /// @brief Matching placement delete (called if promise constructor throws)
            /// @note Required for exception safety, though we compile with -fno-exceptions
            template<typename First, typename... Args>
            static void operator delete(void* ptr, std::size_t size, First&&, Args&&...) noexcept {
                detail::deallocate_coro_frame(ptr, size);
            }

            /// @brief Regular sized delete (called when coroutine is destroyed)
            /// @note Uses header to recover memory_resource pointer
            static void operator delete(void* ptr, std::size_t size) noexcept {
                detail::deallocate_coro_frame(ptr, size);
            }

            /// @brief Fallback for compilers that don't pass size (GCC)
            /// @note Frame size is recovered from header stored during allocation
            static void operator delete(void* ptr) noexcept {
                detail::deallocate_coro_frame_unsized(ptr);
            }
        };

    private:
        void wait_for_ready() {
            // Helper to check cancellation and handle error
            auto check_cancelled = [this]() -> bool {
                if (state_->is_cancelled()) {
                    state_ = nullptr;
                    assert(false && "get() on cancelled future!");
                    if constexpr (!is_void_type) std::terminate();
                    return true;
                }
                return false;
            };

            int spin_count = 0;
            constexpr int fast_spin_limit = 10;
            constexpr int yield_limit = 100;

            // Fast spin phase (no syscall)
            while (!state_->is_ready() && spin_count < fast_spin_limit) {
                if (check_cancelled()) return;
                ++spin_count;
            }

            // Yield phase
            while (!state_->is_ready() && spin_count < yield_limit) {
                if (check_cancelled()) return;
                std::this_thread::yield();
                ++spin_count;
            }

#if HAVE_ATOMIC_WAIT
            // Wait until truly ready (not just state changed)
            // Must handle transient states: pending → setting → ready
            while (!state_->is_ready()) {
                if (check_cancelled()) return;
                auto current = state_->state();
                // Wait on pending OR setting states (both are transient)
                if (current == detail::future_state_enum::pending ||
                    current == detail::future_state_enum::setting) {
                    state_->wait(current);  // Returns when state changes
                }
            }
#else
            // Backoff phase
            auto backoff = std::chrono::microseconds(1);
            constexpr auto max_backoff = std::chrono::microseconds(100);

            while (!state_->is_ready()) {
                if (check_cancelled()) return;
                std::this_thread::sleep_for(backoff);
                if (backoff < max_backoff) backoff *= 2;
            }
#endif
        }

        // Member variables - simplified after removing inline storage
        intrusive_ptr<state_type> state_;
        std::pmr::memory_resource* resource_;
        bool needs_scheduling_;
    };

    /// @brief Create ready future via promise (clean API)
    /// @note Uses promise internally for consistency
    template<typename T>
    unique_future<T> make_ready_future(std::pmr::memory_resource* resource, T&& value) {
        promise<T> p(resource);
        p.set_value(std::forward<T>(value));
        return p.get_future();
    }

    template<typename T>
    unique_future<T> make_ready_future(std::pmr::memory_resource* resource, const T& value) {
        promise<T> p(resource);
        p.set_value(value);
        return p.get_future();
    }

    /// @brief Create ready void future via promise (clean API)
    inline unique_future<void> make_ready_future_void(std::pmr::memory_resource* resource) {
        promise<void> p(resource);
        p.set_value();
        return p.get_future();
    }

    /// @brief Create error future via promise
    template<typename T>
    unique_future<T> make_error_future(std::pmr::memory_resource* resource) {
        promise<T> p(resource);
        p.internal_state_base()->set_state(detail::future_state_enum::error);
        return p.get_future();
    }

    template<typename T>
    unique_future<T> promise<T>::get_future() noexcept {
        assert(state_ && "get_future() on moved-from promise");
        // Use public constructor: unique_future(promise<T>&)
        return unique_future<T>(*this);
    }

}
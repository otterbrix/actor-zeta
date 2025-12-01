#pragma once

#include <actor-zeta/config.hpp>
#include <actor-zeta/detail/future_state.hpp>
#include <actor-zeta/detail/memory_resource.hpp>
#include <actor-zeta/detail/intrusive_ptr.hpp>
#include <actor-zeta/detail/monostate.hpp>
#include <actor-zeta/detail/rtt.hpp>

#include <cassert>
#include <chrono>
#include <thread>
#include <type_traits>
#include <utility>

namespace actor_zeta {

    // Forward declaration for awaiter (used by promise::await_transform)
    template<typename T>
    struct awaiter;


    // Forward declaration
    template<typename T>
    class unique_future;

    template<typename T>
    class promise final {
    public:
        promise() = delete;
        promise(const promise&) = delete;
        promise& operator=(const promise&) = delete;

        /// @brief Create promise that owns its state (Seastar-style)
        /// @param res Memory resource for state allocation
        explicit promise(pmr::memory_resource* res)
            : state_(nullptr)
            , resource_(res)
            , owns_state_(true) {
            assert(res && "promise constructed with null resource");
            void* mem = resource_->allocate(sizeof(detail::future_state<T>),
                                            alignof(detail::future_state<T>));
            state_ = new (mem) detail::future_state<T>(resource_);
            // state_ starts with refcount=1, promise owns it
        }

        promise(promise&& other) noexcept
            : state_(other.state_)
            , resource_(other.resource_)
            , owns_state_(other.owns_state_) {
            other.state_ = nullptr;
            other.owns_state_ = false;
        }

        promise& operator=(promise&& other) noexcept {
            if (this != &other) {
                release();
                state_ = other.state_;
                resource_ = other.resource_;
                owns_state_ = other.owns_state_;
                other.state_ = nullptr;
                other.owns_state_ = false;
            }
            return *this;
        }

        ~promise() noexcept {
            release();
        }

        /// @brief Get future associated with this promise (Seastar-style)
        /// @return unique_future that shares state with this promise
        /// @note Can be called multiple times (each future adds reference)
        [[nodiscard]] unique_future<T> get_future() noexcept;

        void set_value(T&& value) {
            assert(state_ && "set_value() on moved-from promise");

            if (state_->is_cancelled()) {
                assert(false && "set_value() on orphaned/cancelled promise");
                return;
            }

            // Use set_value() for ZERO ALLOCATION in-place storage
            state_->set_value(std::forward<T>(value));
        }

        void set_value(const T& value) {
            assert(state_ && "set_value() on moved-from promise");

            if (state_->is_cancelled()) {
                assert(false && "set_value() on orphaned/cancelled promise");
                return;
            }

            // Use set_value() for ZERO ALLOCATION in-place storage
            state_->set_value(value);
        }

        /// @brief Check if promise has valid state - Seastar API
        [[nodiscard]] bool valid() const noexcept {
            return state_ != nullptr;
        }

        [[nodiscard]] detail::future_state<T>* state() const noexcept {
            return state_;
        }

        [[nodiscard]] pmr::memory_resource* resource() const noexcept {
            return resource_;
        }

        /// @brief Wrap existing state (non-owning) - for result propagation
        /// @param state Existing future state to wrap
        /// @param res Memory resource (required)
        /// @note State must outlive this promise. Caller responsible for lifetime.
        static promise wrap(detail::future_state<T>* state, pmr::memory_resource* res) noexcept {
            assert(res && "wrap() requires memory_resource");
            assert(state && "wrap() requires valid state");
            return promise(state, res, false);  // non-owning
        }

    private:
        // Private constructor for wrap() - non-owning
        promise(detail::future_state<T>* state, pmr::memory_resource* res, bool owns) noexcept
            : state_(state)
            , resource_(res)
            , owns_state_(owns) {}

        void release() noexcept {
            if (owns_state_ && state_) {
                intrusive_ptr_release(state_);
            }
            state_ = nullptr;
        }

        detail::future_state<T>* state_;
        pmr::memory_resource* resource_;
        bool owns_state_;
    };

    template<>
    class promise<void> final {
    public:
        promise() = delete;
        promise(const promise&) = delete;
        promise& operator=(const promise&) = delete;

        /// @brief Create promise that owns its state (Seastar-style)
        /// @param res Memory resource for state allocation
        explicit promise(pmr::memory_resource* res)
            : state_(nullptr)
            , resource_(res)
            , owns_state_(true) {
            assert(res && "promise<void> constructed with null resource");
            void* mem = resource_->allocate(sizeof(detail::future_state<void>),
                                            alignof(detail::future_state<void>));
            state_ = new (mem) detail::future_state<void>(resource_);
            // state_ starts with refcount=1, promise owns it
        }

        promise(promise&& other) noexcept
            : state_(other.state_)
            , resource_(other.resource_)
            , owns_state_(other.owns_state_) {
            other.state_ = nullptr;
            other.owns_state_ = false;
        }

        promise& operator=(promise&& other) noexcept {
            if (this != &other) {
                release();
                state_ = other.state_;
                resource_ = other.resource_;
                owns_state_ = other.owns_state_;
                other.state_ = nullptr;
                other.owns_state_ = false;
            }
            return *this;
        }

        ~promise() noexcept {
            release();
        }

        /// @brief Get future associated with this promise (Seastar-style)
        /// @return unique_future that shares state with this promise
        /// @note Can be called multiple times (each future adds reference)
        [[nodiscard]] unique_future<void> get_future() noexcept;

        void set_value() {
            assert(state_ && "set_value() on moved-from promise<void>");

            if (state_->is_cancelled()) {
                assert(false && "set_value() on orphaned/cancelled promise");
                return;
            }

            // Use set_ready() for void - no RTT needed
            state_->set_ready();
        }

        /// @brief Check if promise has valid state - Seastar API
        [[nodiscard]] bool valid() const noexcept {
            return state_ != nullptr;
        }

        [[nodiscard]] detail::future_state<void>* state() const noexcept {
            return state_;
        }

        [[nodiscard]] pmr::memory_resource* resource() const noexcept {
            return resource_;
        }

        /// @brief Wrap existing state (non-owning) - for result propagation
        /// @param state Existing future state to wrap
        /// @param res Memory resource (required)
        /// @note State must outlive this promise. Caller responsible for lifetime.
        static promise wrap(detail::future_state<void>* state, pmr::memory_resource* res) noexcept {
            assert(res && "wrap() requires memory_resource");
            assert(state && "wrap() requires valid state");
            return promise(state, res, false);  // non-owning
        }

    private:
        // Private constructor for wrap() - non-owning
        promise(detail::future_state<void>* state, pmr::memory_resource* res, bool owns) noexcept
            : state_(state)
            , resource_(res)
            , owns_state_(owns) {}

        void release() noexcept {
            if (owns_state_ && state_) {
                intrusive_ptr_release(state_);
            }
            state_ = nullptr;
        }

        detail::future_state<void>* state_;
        pmr::memory_resource* resource_;
        bool owns_state_;
    };

    // Tag type for adopting an existing reference (no add_ref() in constructor)
    struct adopt_ref_t {};
    inline constexpr adopt_ref_t adopt_ref{};

    // Tag type for creating ready future with inline value (zero allocation)
    struct ready_future_marker_t {};
    inline constexpr ready_future_marker_t ready_future_marker{};

    // ============================================================================
    // Inline storage threshold: only store values inline if sizeof(T) <= threshold
    // Larger types use regular state allocation to avoid bloating unique_future size
    // ============================================================================
    inline constexpr std::size_t inline_storage_threshold = sizeof(void*) * 4;  // 32 bytes on 64-bit

    template<typename T>
    inline constexpr bool use_inline_storage_v =
        !std::is_void_v<T> &&
        sizeof(T) <= inline_storage_threshold &&
        std::is_nothrow_move_constructible_v<T>;

    /// @brief Unified unique_future<T> - works for both void and non-void types (Seastar-style)
    /// Uses monostate internally for void, but API remains unchanged.
    /// For T=void: get() returns void, is_ready_void_ enables zero-allocation fast path
    /// For T!=void: get() returns T, supports both async (state_) and inline (inline_value_) storage
    /// Fast Ready Future Factory: make_ready_future() uses inline storage (ZERO ALLOCATION!)
    template<typename T>
    class unique_future final {
    private:
        // Type traits for unified void/non-void handling
        static constexpr bool is_void_type = std::is_void_v<T>;
        using stored_type = detail::future_stored_type_t<T>;
        // For void: use future_state_base to allow storing any future_state<U>* without UB
        // For non-void: use future_state<T> for typed access
        using state_type = std::conditional_t<is_void_type, detail::future_state_base, detail::future_state<T>>;
        // For coroutine allocation: always use concrete type (can't allocate abstract base)
        using coroutine_state_type = std::conditional_t<is_void_type, detail::future_state<void>, detail::future_state<T>>;

        // ========================================================================
        // Union storage for non-void: either async state OR inline value
        // ========================================================================
        template<typename U, bool IsVoid>
        union storage_union {
            // Default: uninitialized (caller MUST placement-new the correct member)
            storage_union() noexcept {}
            // Destructor: manual (caller MUST call correct destructor based on has_inline_value_)
            ~storage_union() {}

            intrusive_ptr<detail::future_state<U>> state_;
            detail::result_storage<U> inline_value_;
        };

        // Void specialization: no inline storage needed (is_ready_void_ handles this)
        template<typename U>
        union storage_union<U, true> {
            storage_union() noexcept {}
            ~storage_union() {}

            intrusive_ptr<detail::future_state_base> state_;
        };

        // Helper to construct state_ member
        void construct_state(state_type* ptr, bool add_ref = true) noexcept {
            if constexpr (is_void_type) {
                new (&storage_.state_) intrusive_ptr<state_type>(ptr, add_ref);
            } else {
                new (&storage_.state_) intrusive_ptr<state_type>(ptr, add_ref);
            }
        }

        // Helper to construct inline_value_ member (non-void only)
        template<typename... Args>
        void construct_inline_value(Args&&... args) noexcept {
            static_assert(!is_void_type, "Cannot construct inline_value for void type");
            new (&storage_.inline_value_) detail::result_storage<T>();
            storage_.inline_value_.emplace(std::forward<Args>(args)...);
        }

        // Helper to destroy active union member
        void destroy_storage() noexcept {
            if constexpr (is_void_type) {
                storage_.state_.~intrusive_ptr();
            } else {
                if (has_inline_value_) {
                    storage_.inline_value_.~result_storage();
                } else {
                    storage_.state_.~intrusive_ptr();
                }
            }
        }

    public:
        explicit unique_future(pmr::memory_resource* /*res*/) noexcept
            : needs_scheduling_(false)
            , is_ready_void_(false)
            , has_inline_value_(false) {
            construct_state(nullptr, false);
        }

        explicit unique_future(state_type* state, bool needs_sched = false) noexcept
            : needs_scheduling_(needs_sched)
            , is_ready_void_(false)
            , has_inline_value_(false) {
            construct_state(state, true);  // add_ref = true
        }

        // Constructor for adopting an existing reference (no add_ref())
        unique_future(adopt_ref_t, state_type* state, bool needs_sched = false) noexcept
            : needs_scheduling_(needs_sched)
            , is_ready_void_(false)
            , has_inline_value_(false) {
            construct_state(state, false);  // add_ref = false
        }

        // ========================================================================
        // ZERO ALLOCATION: Ready future with inline value (non-void only)
        // ========================================================================
        template<typename U = T, std::enable_if_t<!std::is_void_v<U>, int> = 0>
        unique_future(ready_future_marker_t, U&& value) noexcept
            : needs_scheduling_(false)
            , is_ready_void_(false)
            , has_inline_value_(true) {
            construct_inline_value(std::forward<U>(value));
        }

        template<typename U = T, std::enable_if_t<!std::is_void_v<U>, int> = 0>
        unique_future(ready_future_marker_t, const U& value) noexcept
            : needs_scheduling_(false)
            , is_ready_void_(false)
            , has_inline_value_(true) {
            construct_inline_value(value);
        }

        // Static factory for ready void future (zero allocation) - only enabled for void
        template<typename U = T, std::enable_if_t<std::is_void_v<U>, int> = 0>
        static unique_future make_ready() noexcept {
            unique_future f(nullptr, false);
            f.is_ready_void_ = true;
            return f;
        }

        unique_future(const unique_future&) = delete;
        unique_future& operator=(const unique_future&) = delete;

        unique_future(unique_future&& other) noexcept
            : needs_scheduling_(other.needs_scheduling_)
            , is_ready_void_(other.is_ready_void_)
            , has_inline_value_(other.has_inline_value_) {
            if constexpr (is_void_type) {
                // Void: only state_ path
                new (&storage_.state_) intrusive_ptr<state_type>(std::move(other.storage_.state_));
            } else {
                // Non-void: check which union member is active
                if (has_inline_value_) {
                    new (&storage_.inline_value_) detail::result_storage<T>(
                        std::move(other.storage_.inline_value_));
                    // CRITICAL: Switch source to state_ mode for safe destruction
                    // After move, source's inline_value_ is moved-from. Destroy it and
                    // construct empty state_ so destructor doesn't access invalid union member.
                    other.storage_.inline_value_.~result_storage();
                    new (&other.storage_.state_) intrusive_ptr<state_type>(nullptr);
                } else {
                    new (&storage_.state_) intrusive_ptr<state_type>(std::move(other.storage_.state_));
                }
            }
            other.needs_scheduling_ = false;
            other.is_ready_void_ = false;
            other.has_inline_value_ = false;
        }

        /// @brief Converting constructor from unique_future<U> to unique_future<void>
        /// @note SAFE: Stores as future_state_base* (common base class) - NO UB!
        /// @note Result delivery happens via result_slot mechanism, not through behavior() return
        /// @warning The typed result is DISCARDED - this is intentional for behavior() pattern
        template<typename U, std::enable_if_t<is_void_type && !std::is_void_v<U>, int> = 0>
        unique_future(unique_future<U>&& other) noexcept
            : needs_scheduling_(other.needs_scheduling())
            , is_ready_void_(other.has_inline_value())  // Inline ready → is_ready_void_
            , has_inline_value_(false) {
            if (other.has_inline_value()) {
                // Source has inline value → convert to is_ready_void_ = true
                // Discard the actual value (behavior() pattern doesn't use it)
                construct_state(nullptr, false);
            } else {
                // Get the typed state and store as base pointer
                auto* typed_state = other.release_state();
                if (typed_state) {
                    // static_cast: future_state<U>* → future_state_base* (derived to base)
                    construct_state(static_cast<state_type*>(typed_state), false);
                } else {
                    construct_state(nullptr, false);
                }
            }
            other.set_needs_scheduling(false);
        }

        unique_future& operator=(unique_future&& other) noexcept {
            if (this != &other) {
                // Cancel current if pending
                if constexpr (is_void_type) {
                    if (!is_ready_void_ && storage_.state_ && !storage_.state_->is_ready()) {
                        storage_.state_->set_state(detail::future_state_enum::cancelled);
                    }
                } else {
                    if (!has_inline_value_ && storage_.state_ && !storage_.state_->is_ready()) {
                        storage_.state_->set_state(detail::future_state_enum::cancelled);
                    }
                }

                // Destroy current storage
                destroy_storage();

                // Move from other
                needs_scheduling_ = other.needs_scheduling_;
                is_ready_void_ = other.is_ready_void_;
                has_inline_value_ = other.has_inline_value_;

                if constexpr (is_void_type) {
                    new (&storage_.state_) intrusive_ptr<state_type>(std::move(other.storage_.state_));
                } else {
                    if (has_inline_value_) {
                        new (&storage_.inline_value_) detail::result_storage<T>(
                            std::move(other.storage_.inline_value_));
                        // CRITICAL: Switch source to state_ mode for safe destruction
                        other.storage_.inline_value_.~result_storage();
                        new (&other.storage_.state_) intrusive_ptr<state_type>(nullptr);
                    } else {
                        new (&storage_.state_) intrusive_ptr<state_type>(std::move(other.storage_.state_));
                    }
                }

                other.needs_scheduling_ = false;
                other.is_ready_void_ = false;
                other.has_inline_value_ = false;
            }
            return *this;
        }

        ~unique_future() noexcept {
            destroy_storage();
        }

        // get() for non-void types - returns T
        template<typename U = T>
        [[nodiscard]] std::enable_if_t<!std::is_void_v<U>, U> get() && {
            // Fast path: inline value (ZERO ALLOCATION path)
            if (has_inline_value_) {
                U result = storage_.inline_value_.take();
                // Switch to state_ mode for safe destruction
                storage_.inline_value_.~result_storage();
                new (&storage_.state_) intrusive_ptr<state_type>(nullptr);
                has_inline_value_ = false;
                return result;
            }

            // Async path: state-based
            assert(storage_.state_ && "get() on invalid future");
            wait_for_ready();

            // take_value() now returns T directly (in-place storage)
            U result = storage_.state_->take_value();
            storage_.state_ = nullptr;
            return result;
        }

        // get() for void type - returns void
        template<typename U = T>
        std::enable_if_t<std::is_void_v<U>, void> get() && {
            if (is_ready_void_) {
                return;
            }
            assert(storage_.state_ && "get() on invalid future");
            wait_for_ready();
            storage_.state_ = nullptr;
        }

        // Deleted lvalue get()
        template<typename U = T>
        std::enable_if_t<!std::is_void_v<U>, U> get() & = delete;
        template<typename U = T>
        std::enable_if_t<std::is_void_v<U>, void> get() & = delete;

        /// @brief Check if result is available (ready or consumed) - Seastar API
        [[nodiscard]] bool available() const noexcept {
            if constexpr (is_void_type) {
                return is_ready_void_ || (storage_.state_ && storage_.state_->is_ready());
            } else {
                return has_inline_value_ || (storage_.state_ && storage_.state_->is_ready());
            }
        }

        /// @brief Check if future failed (error or cancelled) - Seastar API
        [[nodiscard]] bool failed() const noexcept {
            if constexpr (is_void_type) {
                if (is_ready_void_) return false;
                return storage_.state_ && storage_.state_->is_failed();
            } else {
                if (has_inline_value_) return false;
                return storage_.state_ && storage_.state_->is_failed();
            }
        }

        /// @brief Check if future has valid state - Seastar API
        [[nodiscard]] bool valid() const noexcept {
            if constexpr (is_void_type) {
                return is_ready_void_ || (storage_.state_ != nullptr);
            } else {
                return has_inline_value_ || (storage_.state_ != nullptr);
            }
        }

        /// @brief Explicitly ignore a ready future - Seastar API
        /// Use this to acknowledge that a future result is intentionally discarded
        void ignore() noexcept {
            if constexpr (is_void_type) {
                is_ready_void_ = false;
                storage_.state_ = nullptr;
            } else {
                if (has_inline_value_) {
                    storage_.inline_value_.~result_storage();
                    has_inline_value_ = false;
                    // Re-init state_ to valid empty state
                    new (&storage_.state_) intrusive_ptr<state_type>(nullptr);
                } else {
                    storage_.state_ = nullptr;
                }
            }
        }

        [[nodiscard]] bool needs_scheduling() const noexcept { return needs_scheduling_; }
        void set_needs_scheduling(bool value) noexcept { needs_scheduling_ = value; }

        [[nodiscard]] bool has_inline_value() const noexcept { return has_inline_value_; }

        void cancel() noexcept {
            if constexpr (!is_void_type) {
                if (has_inline_value_) return;  // Inline value can't be cancelled
            }
            if (storage_.state_) {
                storage_.state_->set_state(detail::future_state_enum::cancelled);
            }
        }

        [[nodiscard]] bool is_cancelled() const noexcept {
            if constexpr (!is_void_type) {
                if (has_inline_value_) return false;
            }
            return storage_.state_ && storage_.state_->is_cancelled();
        }

        [[nodiscard]] bool has_coroutine() const noexcept {
            if constexpr (!is_void_type) {
                if (has_inline_value_) return false;
            }
            return storage_.state_ && storage_.state_->has_coroutine();
        }

        [[nodiscard]] bool coroutine_done() const noexcept {
            if constexpr (!is_void_type) {
                if (has_inline_value_) return false;
            }
            return storage_.state_ && storage_.state_->coroutine_done();
        }

        void resume() noexcept {
            if constexpr (!is_void_type) {
                if (has_inline_value_) return;
            }
            if (storage_.state_) {
                storage_.state_->resume_coroutine();
            }
        }

        [[nodiscard]] bool awaiting_ready() const noexcept {
            if constexpr (!is_void_type) {
                if (has_inline_value_) return false;
            }
            if (!storage_.state_) return false;
            auto* awaiting = storage_.state_->get_awaiting_on();
            return awaiting && awaiting->is_ready();
        }

        [[nodiscard]] state_type* get_state() const noexcept {
            if constexpr (!is_void_type) {
                if (has_inline_value_) return nullptr;
            }
            return storage_.state_.get();
        }

        [[nodiscard]] state_type* release_state() noexcept {
            if constexpr (!is_void_type) {
                if (has_inline_value_) return nullptr;
            }
            return storage_.state_.detach();
        }

        template<typename U = T, std::enable_if_t<!std::is_void_v<U>, int> = 0>
        [[nodiscard]] pmr::memory_resource* memory_resource() const noexcept {
            assert(!has_inline_value_ && storage_.state_ && "memory_resource() called on inline future");
            return storage_.state_->memory_resource();
        }

        // forward_to - only for non-void (Seastar API)
        template<typename U = T, std::enable_if_t<!std::is_void_v<U>, int> = 0>
        void forward_to(promise<T>& target) {
            if (has_inline_value_) {
                T result = storage_.inline_value_.take();
                // Switch to state_ mode for safe destruction
                storage_.inline_value_.~result_storage();
                new (&storage_.state_) intrusive_ptr<state_type>(nullptr);
                has_inline_value_ = false;
                target.set_value(std::move(result));
                return;
            }
            if (!storage_.state_) return;
            if (storage_.state_->is_ready()) {
                target.set_value(std::move(*this).get());
            }
        }

        // ============================================================================
        // Coroutine promise_type - uses inheritance to separate return_value/return_void
        // SFINAE doesn't work for coroutine promise types - compiler sees both methods
        // ============================================================================
    private:
        // Base class with common promise functionality
        struct promise_type_base {
            using value_type = T;

            unique_future<T> get_return_object() {
                assert(resource_ != nullptr &&
                       "Coroutine must be actor member function with resource() method");

                // Use coroutine_state_type for allocation (concrete type, not abstract base)
                void* mem = resource_->allocate(sizeof(coroutine_state_type), alignof(coroutine_state_type));
                state_ = new (mem) coroutine_state_type(resource_);

                auto handle = std::coroutine_handle<struct promise_type>::from_promise(
                    static_cast<struct promise_type&>(*this));
                state_->set_coroutine(handle);

                // For void: coroutine_state_type* (future_state<void>*) → state_type* (future_state_base*)
                // This is legal: derived* → base*
                return unique_future<T>(adopt_ref, static_cast<state_type*>(state_), false);
            }

            detail::suspend_never initial_suspend() noexcept { return {}; }
            detail::suspend_always final_suspend() noexcept { return {}; }

            template<typename U>
            auto await_transform(unique_future<U>&& future) noexcept {
                // state_ is coroutine_state_type* which derives from future_state_base
                return awaiter<U>{std::move(future), static_cast<detail::future_state_base*>(state_)};
            }

            void unhandled_exception() noexcept {
                assert(false && "unhandled_exception() should never be called (-fno-exceptions)");
            }

            promise_type_base() noexcept : resource_(nullptr), state_(nullptr) {}

            template<typename First, typename... Args>
            promise_type_base(First&& first, Args&&...) noexcept
                : resource_(extract_resource_impl(first))
                , state_(nullptr) {}

            ~promise_type_base() noexcept = default;

        protected:
            template<typename U>
            static auto try_get_resource(U* ptr, int) noexcept -> decltype(ptr->resource()) {
                return ptr->resource();
            }

            template<typename U>
            static pmr::memory_resource* try_get_resource(U*, ...) noexcept {
                return nullptr;
            }

            template<typename U>
            static pmr::memory_resource* extract_impl_dispatch(U&& arg, std::true_type) noexcept {
                using ptr_type = std::remove_reference_t<U>;
                return try_get_resource(static_cast<ptr_type>(arg), 0);
            }

            template<typename U>
            static pmr::memory_resource* extract_impl_dispatch(U&& arg, std::false_type) noexcept {
                return try_get_resource(&arg, 0);
            }

            template<typename U>
            static pmr::memory_resource* extract_resource_impl(U&& arg) noexcept {
                using decayed = std::decay_t<U>;
                return extract_impl_dispatch(std::forward<U>(arg), std::is_pointer<decayed>{});
            }

            pmr::memory_resource* resource_;
            coroutine_state_type* state_;  // Use concrete type for coroutine operations
        };

        // Non-void: has return_value
        template<typename U, bool IsVoid>
        struct promise_type_return : promise_type_base {
            using promise_type_base::promise_type_base;

            void return_value(U&& value) noexcept {
                assert(this->state_ && "return_value() with null state");
                // Use set_value() for ZERO ALLOCATION in-place storage
                this->state_->set_value(std::forward<U>(value));
            }

            void return_value(const U& value) noexcept {
                assert(this->state_ && "return_value() with null state");
                // Use set_value() for ZERO ALLOCATION in-place storage
                this->state_->set_value(value);
            }

            void return_value(unique_future<U>&& ready_future) noexcept {
                assert(this->state_ && "return_value() with null state");
                assert(ready_future.valid() && "return_value() with invalid future");
                U val = std::move(ready_future).get();
                // Use set_value() for ZERO ALLOCATION in-place storage
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
        };

    private:
        void wait_for_ready() {
            int spin_count = 0;
            constexpr int fast_spin_limit = 10;
            constexpr int yield_limit = 100;

            while (!storage_.state_->is_ready() && spin_count < fast_spin_limit) {
                if (storage_.state_->is_cancelled()) {
                    storage_.state_ = nullptr;
                    assert(false && "get() on cancelled future!");
                    if constexpr (!is_void_type) std::terminate();
                    return;
                }
                ++spin_count;
            }

            while (!storage_.state_->is_ready() && spin_count < yield_limit) {
                if (storage_.state_->is_cancelled()) {
                    storage_.state_ = nullptr;
                    assert(false && "get() on cancelled future!");
                    if constexpr (!is_void_type) std::terminate();
                    return;
                }
                std::this_thread::yield();
                ++spin_count;
            }

#if HAVE_ATOMIC_WAIT
            // Wait until truly ready (not just state changed)
            // Must handle transient states: pending → setting → ready
            while (!storage_.state_->is_ready()) {
                if (storage_.state_->is_cancelled()) {
                    storage_.state_ = nullptr;
                    assert(false && "get() on cancelled future!");
                    if constexpr (!is_void_type) std::terminate();
                    return;
                }
                auto current = storage_.state_->state();
                // Wait on pending OR setting states (both are transient)
                if (current == detail::future_state_enum::pending ||
                    current == detail::future_state_enum::setting) {
                    storage_.state_->wait(current);  // Returns when state changes
                }
                // Re-check is_ready() in while condition
            }
#else
            auto backoff = std::chrono::microseconds(1);
            constexpr auto max_backoff = std::chrono::microseconds(100);

            while (!storage_.state_->is_ready()) {
                if (storage_.state_->is_cancelled()) {
                    storage_.state_ = nullptr;
                    assert(false && "get() on cancelled future!");
                    if constexpr (!is_void_type) std::terminate();
                    return;
                }
                std::this_thread::sleep_for(backoff);
                if (backoff < max_backoff) backoff *= 2;
            }
#endif
        }

        // Member variables - storage union MUST be first for proper initialization
        storage_union<T, is_void_type> storage_;
        bool needs_scheduling_;
        bool is_ready_void_;        // Only used when T=void for zero-allocation fast path
        bool has_inline_value_;     // Only used when T!=void for inline value storage
    };

    /// @brief Create ready future with inline value storage (ZERO ALLOCATION!)
    /// @param resource Memory resource (unused for inline storage, kept for API compatibility)
    /// @param value Value to store inline in the future
    /// @return Ready future with inline storage
    /// @note This is the optimized path - no future_state allocation!
    template<typename T>
    unique_future<T> make_ready_future(pmr::memory_resource* /*resource*/, T&& value) {
        // ZERO ALLOCATION: value stored inline in unique_future
        return unique_future<T>(ready_future_marker, std::forward<T>(value));
    }

    template<typename T>
    unique_future<T> make_ready_future(pmr::memory_resource* /*resource*/, const T& value) {
        // ZERO ALLOCATION: value stored inline in unique_future
        return unique_future<T>(ready_future_marker, value);
    }

    inline unique_future<void> make_ready_future_void(pmr::memory_resource* /*resource*/) {
        // Zero allocation - just return ready void future
        return unique_future<void>::make_ready();
    }

    template<typename T>
    unique_future<T> make_error_future(pmr::memory_resource* resource) {
        void* mem = resource->allocate(sizeof(detail::future_state<T>),
                                       alignof(detail::future_state<T>));
        auto* state = new (mem) detail::future_state<T>(resource);  // refcount = 1
        state->set_state(detail::future_state_enum::error);

        // Use adopt_ref - no message to release, future adopts initial refcount
        return unique_future<T>(adopt_ref, state, false);
    }

    // ============================================================================
    // promise<T>::get_future() implementation (Seastar-style)
    // ============================================================================

    template<typename T>
    unique_future<T> promise<T>::get_future() noexcept {
        assert(state_ && "get_future() on moved-from promise");
        // Future shares state with promise - add reference
        // Promise keeps its reference, future adds its own
        return unique_future<T>(state_, false);  // Calls add_ref() via intrusive_ptr
    }

    inline unique_future<void> promise<void>::get_future() noexcept {
        assert(state_ && "get_future() on moved-from promise<void>");
        // Future shares state with promise - add reference
        return unique_future<void>(state_, false);  // Calls add_ref() via intrusive_ptr
    }

}

#include <actor-zeta/detail/coroutine.hpp>

namespace actor_zeta {

    // ============================================================================
    // Awaiter with continuation support (used by promise::await_transform)
    // ============================================================================
    //
    // NOTE: This is the ONLY awaiter used in this library. The co_await operator
    // is ONLY available inside actor coroutines via await_transform().
    // External co_await is intentionally NOT supported to enforce thread affinity.
    // ============================================================================
    template<typename T>
    struct awaiter {
        unique_future<T> future_;
        detail::future_state_base* promise_state_;  // Promise state for continuation

        explicit awaiter(unique_future<T>&& f, detail::future_state_base* prom_state = nullptr) noexcept
            : future_(std::move(f))
            , promise_state_(prom_state) {}

        [[nodiscard]] bool await_ready() const noexcept {
            return future_.available();
        }

        bool await_suspend(std::coroutine_handle<> handle) noexcept {
            if (future_.available()) {
                return false;
            }

            auto* state = future_.get_state();
            if (!state) {
                return false;
            }

            // Store coroutine handle in awaited future's state
            // User code will call state->resume_coroutine() when future is ready
            state->set_coroutine(handle);

            // Register awaited future in promise_state for user polling
            // User code can access via: method_future.get_state()->get_awaiting_on()
            if (promise_state_) {
                promise_state_->set_awaiting_on(state);
            }

            // Re-check after set_coroutine() to close race window
            if (future_.available()) {
                return false;  // Don't suspend - another thread set result
            }

            return true;
        }

        T await_resume() {
            return std::move(future_).get();
        }
    };

    /// @brief Specialization of awaiter for void (Seastar-style unified handling)
    /// Uses monostate internally but returns void from await_resume()
    template<>
    struct awaiter<void> {
        unique_future<void> future_;
        detail::future_state_base* promise_state_;

        explicit awaiter(unique_future<void>&& f, detail::future_state_base* prom_state = nullptr) noexcept
            : future_(std::move(f))
            , promise_state_(prom_state) {}

        [[nodiscard]] bool await_ready() const noexcept {
            return future_.available();
        }

        bool await_suspend(std::coroutine_handle<> handle) noexcept {
            if (future_.available()) {
                return false;
            }

            auto* state = future_.get_state();
            if (!state) {
                return false;
            }

            state->set_coroutine(handle);

            if (promise_state_) {
                promise_state_->set_awaiting_on(state);
            }

            if (future_.available()) {
                return false;
            }

            return true;
        }

        void await_resume() {
            std::move(future_).get();  // void return
        }
    };

}
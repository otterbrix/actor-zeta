#pragma once

#include <actor-zeta/detail/future_state.hpp>
#include <actor-zeta/detail/memory_resource.hpp>
#include <actor-zeta/detail/intrusive_ptr.hpp>
#include <actor-zeta/detail/rtt.hpp>

#include <cassert>
#include <chrono>
#include <thread>
#include <utility>
#include <iostream>

namespace actor_zeta {

#if HAVE_STD_COROUTINES
    template<typename T>
    struct future_awaiter;
#endif

    template<typename T>
    class promise final {
    public:
        promise() = delete;
        promise(const promise&) = delete;
        promise& operator=(const promise&) = delete;

        explicit promise(detail::future_state_base* slot, pmr::memory_resource* res) noexcept
            : slot_(slot)
            , resource_(res) {
            assert(slot_ && "promise constructed with null slot");
        }

        promise(promise&& other) noexcept
            : slot_(other.slot_)
            , resource_(other.resource_) {
            other.slot_ = nullptr;
        }

        promise& operator=(promise&& other) noexcept {
            if (this != &other) {
                slot_ = other.slot_;
                resource_ = other.resource_;
                other.slot_ = nullptr;
            }
            return *this;
        }

        ~promise() noexcept = default;

        void set_value(T&& value) {
            assert(slot_ && "set_value() on moved-from promise");
            slot_->set_result_rtt(detail::rtt(resource_, std::forward<T>(value)));
        }

        void set_value(const T& value) {
            assert(slot_ && "set_value() on moved-from promise");
            slot_->set_result_rtt(detail::rtt(resource_, value));
        }

        [[nodiscard]] bool is_valid() const noexcept {
            return slot_ != nullptr;
        }

        [[nodiscard]] detail::future_state_base* slot() const noexcept {
            return slot_;
        }

    private:
        detail::future_state_base* slot_;
        pmr::memory_resource* resource_;
    };

    template<>
    class promise<void> final {
    public:
        promise() = delete;
        promise(const promise&) = delete;
        promise& operator=(const promise&) = delete;

        explicit promise(detail::future_state_base* slot, pmr::memory_resource* res) noexcept
            : slot_(slot), resource_(res) {
            assert(slot_ && "promise<void> constructed with null slot");
        }

        promise(promise&& other) noexcept
            : slot_(other.slot_), resource_(other.resource_) {
            other.slot_ = nullptr;
        }

        promise& operator=(promise&& other) noexcept {
            if (this != &other) {
                slot_ = other.slot_;
                resource_ = other.resource_;
                other.slot_ = nullptr;
            }
            return *this;
        }

        ~promise() noexcept = default;

        void set_value() {
            assert(slot_ && "set_value() on moved-from promise<void>");
            slot_->set_result_rtt(detail::rtt(resource_, int{0}));
        }

        [[nodiscard]] bool is_valid() const noexcept {
            return slot_ != nullptr;
        }

        [[nodiscard]] detail::future_state_base* slot() const noexcept {
            return slot_;
        }

    private:
        detail::future_state_base* slot_;
        pmr::memory_resource* resource_;
    };

    template<typename T>
    class unique_future final {
    public:
        explicit unique_future(pmr::memory_resource* /*res*/) noexcept
            : mode_(storage_mode::invalid)
            , needs_scheduling_(false) {
            storage_.state_ = nullptr;
        }

        unique_future(const unique_future&) = delete;
        unique_future& operator=(const unique_future&) = delete;

        explicit unique_future(detail::future_state<T>* state, bool needs_sched = false) noexcept
            : mode_(storage_mode::state)
            , needs_scheduling_(needs_sched) {
            storage_.state_ = state;
        }

        unique_future(T&& value) noexcept(std::is_nothrow_move_constructible<T>::value)
            : mode_(storage_mode::immediate)
            , needs_scheduling_(false) {
            new (&storage_.value_) T(std::forward<T>(value));
        }

        unique_future(const T& value) noexcept(std::is_nothrow_copy_constructible<T>::value)
            : mode_(storage_mode::immediate)
            , needs_scheduling_(false) {
            new (&storage_.value_) T(value);
        }

        unique_future(unique_future&& other) noexcept
            : mode_(other.mode_)
            , needs_scheduling_(other.needs_scheduling_) {
            if (mode_ == storage_mode::state) {
                storage_.state_ = other.storage_.state_;
                other.storage_.state_ = nullptr;
            } else if (mode_ == storage_mode::immediate) {
                new (&storage_.value_) T(std::move(other.storage_.value_));
                other.storage_.value_.~T();
            }

            other.mode_ = storage_mode::invalid;
            other.needs_scheduling_ = false;
        }

        unique_future& operator=(unique_future&& other) noexcept {
            if (this != &other) {
                if (mode_ == storage_mode::state && storage_.state_) {
                    if (!storage_.state_->is_ready()) {
                        storage_.state_->set_state(detail::future_state_enum::cancelled);
                    }
                    storage_.state_->release();
                } else if (mode_ == storage_mode::immediate) {
                    storage_.value_.~T();
                }

                mode_ = other.mode_;
                needs_scheduling_ = other.needs_scheduling_;

                if (mode_ == storage_mode::state) {
                    storage_.state_ = other.storage_.state_;
                    other.storage_.state_ = nullptr;
                } else if (mode_ == storage_mode::immediate) {
                    new (&storage_.value_) T(std::move(other.storage_.value_));
                    other.storage_.value_.~T();
                }

                other.mode_ = storage_mode::invalid;
                other.needs_scheduling_ = false;
            }
            return *this;
        }

        ~unique_future() noexcept {
            if (mode_ == storage_mode::state && storage_.state_) {
                storage_.state_->release();
            } else if (mode_ == storage_mode::immediate) {
                storage_.value_.~T();
            }
        }

        T get() && {
            if (mode_ == storage_mode::immediate) {
                T result = std::move(storage_.value_);
                storage_.value_.~T();
                mode_ = storage_mode::invalid;
                return result;
            }

            assert(mode_ == storage_mode::state && "get() on invalid future");
            assert(storage_.state_ && "get() with null state");

            int spin_count = 0;
            constexpr int fast_spin_limit = 10;
            constexpr int yield_limit = 100;

            while (!storage_.state_->is_ready() && spin_count < fast_spin_limit) {
                if (storage_.state_->is_cancelled()) {
                    storage_.state_->release();
                    storage_.state_ = nullptr;
                    mode_ = storage_mode::invalid;
                    assert(false && "get() on cancelled future!");
                    return T{};
                }
                ++spin_count;
            }

            while (!storage_.state_->is_ready() && spin_count < yield_limit) {
                if (storage_.state_->is_cancelled()) {
                    storage_.state_->release();
                    storage_.state_ = nullptr;
                    mode_ = storage_mode::invalid;
                    assert(false && "get() on cancelled future!");
                    return T{};
                }
                std::this_thread::yield();
                ++spin_count;
            }

            auto backoff = std::chrono::microseconds(1);
            constexpr auto max_backoff = std::chrono::microseconds(100);

            while (!storage_.state_->is_ready()) {
                if (storage_.state_->is_cancelled()) {
                    storage_.state_->release();
                    storage_.state_ = nullptr;
                    mode_ = storage_mode::invalid;
                    assert(false && "get() on cancelled future!");
                    return T{};
                }

                std::this_thread::sleep_for(backoff);
                if (backoff < max_backoff) {
                    backoff *= 2;
                }
            }

            std::atomic_signal_fence(std::memory_order_acq_rel);

            T result = storage_.state_->result().template get<T>(0);

            storage_.state_->release();
            storage_.state_ = nullptr;
            mode_ = storage_mode::invalid;

            return result;
        }

        T get() & = delete;

        [[nodiscard]] bool is_ready() const noexcept {
            if (mode_ == storage_mode::immediate) {
                return true;
            } else if (mode_ == storage_mode::state) {
                return storage_.state_ && storage_.state_->is_ready();
            }
            return false;
        }

        [[nodiscard]] bool valid() const noexcept {
            return mode_ != storage_mode::invalid;
        }

        [[nodiscard]] bool needs_scheduling() const noexcept {
            return needs_scheduling_;
        }

        void cancel() noexcept {
            if (mode_ == storage_mode::state && storage_.state_) {
                storage_.state_->set_state(detail::future_state_enum::cancelled);
            }
        }

        [[nodiscard]] bool is_cancelled() const noexcept {
            return mode_ == storage_mode::state && storage_.state_ && storage_.state_->is_cancelled();
        }

        [[nodiscard]] bool is_immediate() const noexcept {
            return mode_ == storage_mode::immediate;
        }

        [[nodiscard]] bool is_state() const noexcept {
            return mode_ == storage_mode::state;
        }

        T take_immediate_value() && {
            assert(mode_ == storage_mode::immediate && "take_immediate_value() on non-immediate future");
            T value = std::move(storage_.value_);
            storage_.value_.~T();
            mode_ = storage_mode::invalid;
            return value;
        }

        detail::future_state<T>* take_state() && {
            assert(mode_ == storage_mode::state && "take_state() on non-state future");
            auto* state = storage_.state_;
            storage_.state_ = nullptr;
            mode_ = storage_mode::invalid;
            return state;
        }

        [[nodiscard]] pmr::memory_resource* memory_resource() const noexcept {
            if (mode_ == storage_mode::state && storage_.state_) {
                return storage_.state_->memory_resource();
            }
            return pmr::get_default_resource();
        }

#if HAVE_STD_COROUTINES
        template<typename F>
        auto then(F&& callback) -> unique_future<
            typename std::conditional_t<
                type_traits::is_unique_future_v<typename std::invoke_result<F, T>::type>,
                typename type_traits::is_unique_future<typename std::invoke_result<F, T>::type>::value_type,
                typename std::invoke_result<F, T>::type
            >
        > {
            using R = typename std::invoke_result<F, T>::type;

            using unwrapped_result_t = typename std::conditional_t<
                type_traits::is_unique_future_v<R>,
                typename type_traits::is_unique_future<R>::value_type,
                R
            >;

            pmr::memory_resource* resource = nullptr;
            if (mode_ == storage_mode::state && storage_.state_) {
                resource = storage_.state_->memory_resource();
            } else {
                resource = pmr::get_default_resource();
            }

            if (mode_ == storage_mode::immediate) {
                T value = std::move(storage_.value_);
                storage_.value_.~T();
                mode_ = storage_mode::invalid;

                if constexpr (std::is_void_v<R>) {
                    callback(std::move(value));
                    return unique_future<unwrapped_result_t>(resource);
                } else if constexpr (type_traits::is_unique_future_v<R>) {
                    R inner_future = callback(std::move(value));
                    return inner_future;
                } else {
                    R result = callback(std::move(value));
                    return unique_future<unwrapped_result_t>(std::move(result));
                }
            }

            assert(mode_ == storage_mode::state && "then() on invalid future");
            assert(storage_.state_ && "then() with null state");

            void* mem = resource->allocate(sizeof(detail::future_state<unwrapped_result_t>),
                                          alignof(detail::future_state<unwrapped_result_t>));
            auto* new_state = new (mem) detail::future_state<unwrapped_result_t>(resource);
            new_state->add_ref();

            auto* current_state = storage_.state_;
            current_state->add_ref();

            current_state->add_continuation(detail::unique_function<void()>(resource, [current_state, new_state, callback = std::forward<F>(callback), resource]() mutable {
                assert(current_state->is_ready() && "Continuation called before future ready");

                T value = current_state->result().template get<T>(0);

                if constexpr (std::is_void_v<R>) {
                    callback(std::move(value));
                    new_state->set_ready();
                } else if constexpr (type_traits::is_unique_future_v<R>) {
                    R inner_future = callback(std::move(value));

                    if (inner_future.is_immediate()) {
                        unwrapped_result_t inner_value = std::move(inner_future).take_immediate_value();
                        detail::rtt result_rtt(resource, std::move(inner_value));
                        new_state->set_result(std::move(result_rtt));
                    } else if (inner_future.is_state()) {
                        auto* inner_state = std::move(inner_future).take_state();
                        inner_state->add_ref();

                        inner_state->add_continuation(detail::unique_function<void()>(resource, [inner_state, new_state, resource]() mutable {
                            assert(inner_state->is_ready() && "Inner continuation called before ready");

                            unwrapped_result_t final_value = inner_state->result().template get<unwrapped_result_t>(0);
                            detail::rtt final_rtt(resource, std::move(final_value));
                            new_state->set_result(std::move(final_rtt));

                            inner_state->release();
                        }));

                        inner_state->release();
                    } else {
                        new_state->set_state(detail::future_state_enum::cancelled);
                    }
                } else {
                    R result = callback(std::move(value));
                    detail::rtt result_rtt(resource, std::move(result));
                    new_state->set_result(std::move(result_rtt));
                }

                current_state->release();
            }));

            storage_.state_ = nullptr;
            mode_ = storage_mode::invalid;

            return unique_future<unwrapped_result_t>(new_state, false);
        }
#endif

#if HAVE_STD_COROUTINES
        struct promise_type {
            using value_type = T;

            unique_future<T> get_return_object() {

                void* mem = resource_->allocate(sizeof(detail::future_state<T>),
                                                alignof(detail::future_state<T>));
                state_ = new (mem) detail::future_state<T>(resource_);

                auto handle = detail::coroutine_handle<promise_type>::from_promise(*this);
                state_->set_coroutine(handle);

                return unique_future<T>(state_, false);
            }

            detail::suspend_never initial_suspend() noexcept {
                return {};
            }

            detail::suspend_always final_suspend() noexcept {
                return {};
            }

            void return_value(T&& value) noexcept {
                assert(state_ && "return_value() with null state");
                detail::rtt result(resource_, std::forward<T>(value));
                state_->set_result(std::move(result));
            }

            void return_value(const T& value) noexcept {
                assert(state_ && "return_value() with null state");
                detail::rtt result(resource_, value);
                state_->set_result(std::move(result));
            }

            void unhandled_exception() noexcept {
                assert(false && "unhandled_exception() should never be called (-fno-exceptions)");
            }

            promise_type() noexcept
                : resource_(pmr::get_default_resource())
                , state_(nullptr) {
            }

            template<typename... Args>
            promise_type(Args&&...) noexcept
                : resource_(pmr::get_default_resource())
                , state_(nullptr) {
            }

            ~promise_type() noexcept = default;

        private:
            pmr::memory_resource* resource_;
            detail::future_state<T>* state_;
        };
#endif

    private:
        enum class storage_mode : uint8_t {
            invalid,
            state,
            immediate
        };

        union storage {
            detail::future_state<T>* state_;
            T value_;

            storage() noexcept : state_(nullptr) {}
            ~storage() noexcept {}
        } storage_;

        storage_mode mode_;
        bool needs_scheduling_;
    };

    template<>
    class unique_future<void> final {
    public:
        explicit unique_future(pmr::memory_resource* /*res*/) noexcept
            : state_(nullptr)
            , needs_scheduling_(false) {
        }

        explicit unique_future(detail::future_state<void>* state, bool needs_sched = false) noexcept
            : state_(state)
            , needs_scheduling_(needs_sched) {
        }

        unique_future(const unique_future&) = delete;
        unique_future& operator=(const unique_future&) = delete;

        unique_future(unique_future&& other) noexcept
            : state_(other.state_)
            , needs_scheduling_(other.needs_scheduling_) {
            other.state_ = nullptr;
            other.needs_scheduling_ = false;
        }

        unique_future& operator=(unique_future&& other) noexcept {
            if (this != &other) {
                if (state_ && !state_->is_ready()) {
                    state_->set_state(detail::future_state_enum::cancelled);
                }

                if (state_) {
                    state_->release();
                }

                state_ = other.state_;
                needs_scheduling_ = other.needs_scheduling_;

                other.state_ = nullptr;
                other.needs_scheduling_ = false;
            }
            return *this;
        }

        ~unique_future() noexcept {
            if (state_) {
                state_->release();
            }
        }

        void get() && {
            assert(state_ && "get() on invalid future");

            int spin_count = 0;
            constexpr int fast_spin_limit = 10;
            constexpr int yield_limit = 100;

            while (!state_->is_ready() && spin_count < fast_spin_limit) {
                if (state_->is_cancelled()) {
                    state_->release();
                    state_ = nullptr;
                    assert(false && "get() on cancelled future!");
                    return;
                }
                ++spin_count;
            }

            while (!state_->is_ready() && spin_count < yield_limit) {
                if (state_->is_cancelled()) {
                    state_->release();
                    state_ = nullptr;
                    assert(false && "get() on cancelled future!");
                    return;
                }
                std::this_thread::yield();
                ++spin_count;
            }

            auto backoff = std::chrono::microseconds(1);
            constexpr auto max_backoff = std::chrono::microseconds(100);

            while (!state_->is_ready()) {
                if (state_->is_cancelled()) {
                    state_->release();
                    state_ = nullptr;
                    assert(false && "get() on cancelled future!");
                    return;
                }

                std::this_thread::sleep_for(backoff);
                if (backoff < max_backoff) {
                    backoff *= 2;
                }
            }

            state_->release();
            state_ = nullptr;
        }

        void get() & = delete;

        [[nodiscard]] bool is_ready() const noexcept {
            return state_ && state_->is_ready();
        }

        [[nodiscard]] bool valid() const noexcept {
            return state_ != nullptr;
        }

        [[nodiscard]] bool needs_scheduling() const noexcept {
            return needs_scheduling_;
        }

        void cancel() noexcept {
            if (state_) {
                state_->set_state(detail::future_state_enum::cancelled);
            }
        }

        [[nodiscard]] bool is_cancelled() const noexcept {
            return state_ && state_->is_cancelled();
        }

        [[nodiscard]] detail::future_state<void>* get_state() const noexcept {
            return state_;
        }

#if HAVE_STD_COROUTINES
        template<typename F>
        auto then(F&& callback) -> unique_future<typename std::invoke_result<F>::type> {
            using R = typename std::invoke_result<F>::type;

            pmr::memory_resource* resource = nullptr;
            if (state_) {
                resource = state_->memory_resource();
            } else {
                resource = pmr::get_default_resource();
            }

            if (!state_) {
                return unique_future<R>(resource);
            }

            void* mem = resource->allocate(sizeof(detail::future_state<R>), alignof(detail::future_state<R>));
            auto* new_state = new (mem) detail::future_state<R>(resource);
            new_state->add_ref();

            auto* current_state = state_;
            current_state->add_ref();

            current_state->add_continuation(detail::unique_function<void()>(resource, [current_state, new_state, callback = std::forward<F>(callback), resource]() mutable {
                assert(current_state->is_ready() && "Continuation called before future ready");

                if constexpr (std::is_void_v<R>) {
                    callback();
                    new_state->set_ready();
                } else {
                    R result = callback();
                    detail::rtt result_rtt(resource, std::move(result));
                    new_state->set_result(std::move(result_rtt));
                }

                current_state->release();
            }));

            state_ = nullptr;

            return unique_future<R>(new_state, false);
        }
#endif

#if HAVE_STD_COROUTINES
        struct promise_type {
            using value_type = void;

            unique_future<void> get_return_object() {
                void* mem = resource_->allocate(sizeof(detail::future_state<void>),
                                                alignof(detail::future_state<void>));
                state_ = new (mem) detail::future_state<void>(resource_);

                auto handle = detail::coroutine_handle<promise_type>::from_promise(*this);
                state_->set_coroutine(handle);

                return unique_future<void>(state_, false);
            }

            detail::suspend_never initial_suspend() noexcept { return {}; }

            detail::suspend_always final_suspend() noexcept { return {}; }

            void return_void() noexcept {
                assert(state_ && "return_void() with null state");
                state_->set_ready();
            }

            void unhandled_exception() noexcept {
                assert(false && "unhandled_exception() should never be called (-fno-exceptions)");
            }

            explicit promise_type(pmr::memory_resource* res = pmr::get_default_resource()) noexcept
                : resource_(res)
                , state_(nullptr) {}

        private:
            pmr::memory_resource* resource_;
            detail::future_state<void>* state_;
        };
#endif

    private:
        detail::future_state<void>* state_;
        bool needs_scheduling_;
    };

}

#include <actor-zeta/detail/coroutine.hpp>

#if HAVE_STD_COROUTINES

namespace actor_zeta {

    template<typename T>
    struct future_awaiter {
        detail::future_state<T>* state_;
        pmr::memory_resource* resource_;

        explicit future_awaiter(detail::future_state<T>* s, pmr::memory_resource* res) noexcept
            : state_(s), resource_(res) {
        }

        future_awaiter(const future_awaiter&) = default;
        future_awaiter(future_awaiter&&) noexcept = default;
        future_awaiter& operator=(const future_awaiter&) = default;
        future_awaiter& operator=(future_awaiter&&) = default;

        ~future_awaiter() noexcept {
        }

        [[nodiscard]] bool await_ready() const noexcept {

            if (!state_) {
                return true;
            }

            bool ready = state_->is_ready();
            return ready;
        }

        bool await_suspend(std::coroutine_handle<> handle) noexcept {

            if (!state_) {
                return false;
            }

            if (state_->is_ready()) {
                return false;
            }

            state_->add_continuation(detail::unique_function<void()>(resource_, [handle]() mutable {
                handle.resume();
            }));

            return true;
        }

        T await_resume() {

            assert(state_ && "await_resume() with null state");
            assert(state_->is_ready() && "await_resume() called but state not ready!");

            T result = state_->result().template get<T>(0);

            state_->release();
            state_ = nullptr;

            return result;
        }
    };

    template<>
    struct future_awaiter<void> {
        unique_future<void>& future_;

        explicit future_awaiter(unique_future<void>& f) noexcept : future_(f) {}

        [[nodiscard]] bool await_ready() const noexcept {
            return future_.is_ready();
        }

        bool await_suspend(std::coroutine_handle<> handle) noexcept {
            if (future_.is_ready()) {
                return false;
            }

            auto* state = future_.get_state();
            if (!state) {
                return false;
            }

            auto* resource = state->memory_resource();
            state->add_continuation(detail::unique_function<void()>(resource, [handle]() mutable {
                handle.resume();
            }));

            return true;
        }

        void await_resume() {
            assert(future_.valid() && "await_resume() on invalid future");
            std::move(future_).get();
        }
    };

    template<typename T>
    auto operator co_await(unique_future<T>&& f) noexcept {

        if (f.is_immediate()) {
            return future_awaiter<T>{nullptr, pmr::get_default_resource()};
        }

        auto* resource = f.memory_resource();

        auto* state = std::move(f).take_state();


        return future_awaiter<T>{state, resource};
    }

    template<typename T>
    auto operator co_await(unique_future<T>& f) noexcept {

        if (f.is_immediate()) {
            return future_awaiter<T>{nullptr, pmr::get_default_resource()};
        }

        auto* resource = f.memory_resource();

        auto* state = std::move(f).take_state();


        return future_awaiter<T>{state, resource};
    }

}

#endif
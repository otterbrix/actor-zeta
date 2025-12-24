#pragma once

#include <cassert>
#include <chrono>
#include <concepts>
#include <memory_resource>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>

#include <actor-zeta/config.hpp>
#include <actor-zeta/detail/coro_frame_header.hpp>
#include <actor-zeta/detail/coroutine.hpp>
#include <actor-zeta/detail/future_state.hpp>
#include <actor-zeta/detail/intrusive_ptr.hpp>

namespace actor_zeta {

    // Forward declarations for generator support
    template<typename T> class generator;
    namespace detail {
        template<typename T> class generator_state;
        template<typename T> struct next_awaiter;
    }

    namespace detail {
        template<typename T>
        concept has_resource_method = requires(T* ptr) {
            { ptr->resource() } -> std::convertible_to<std::pmr::memory_resource*>;
        };
    } // namespace detail

    template<typename T>
    class unique_future;

    template<typename T>
    class promise;

    template<typename T>
    class promise final {
    private:
        static constexpr bool is_void_type = std::is_void_v<T>;
        using state_type = detail::future_state<T>;

    public:
        promise() = delete;
        promise(const promise&) = delete;
        promise& operator=(const promise&) = delete;

        explicit promise(std::pmr::memory_resource* res)
            : state_(nullptr)
            , resource_(res) {
            assert(res && "promise constructed with null resource");
            void* mem = resource_->allocate(sizeof(state_type), alignof(state_type));
            state_ = new (mem) state_type(resource_);
        }

        explicit promise(type_traits::internal_construct_tag, state_type* existing_state, std::pmr::memory_resource* res) noexcept
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

        [[nodiscard]] unique_future<T> get_future() noexcept;

        template<typename U>
            requires(!std::is_void_v<T> && std::is_constructible_v<T, U&&>)
        void set_value(U&& value) {
            assert(state_ && "set_value() on moved-from promise");
            if (state_->is_cancelled()) {
                assert(false && "set_value() on orphaned/cancelled promise");
                return;
            }
            state_->set_value(T(std::forward<U>(value)));
        }

        void set_value()
            requires(std::is_void_v<T>)
        {
            assert(state_ && "set_value() on moved-from promise");
            if (state_->is_cancelled()) {
                assert(false && "set_value() on orphaned/cancelled promise");
                return;
            }
            state_->set_ready();
        }

        void error(std::error_code ec) {
            assert(state_ && "error() on moved-from promise");
            state_->error(ec);
        }

        [[nodiscard]] bool valid() const noexcept {
            return state_ != nullptr;
        }

        [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
            return resource_;
        }

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

    template<typename T>
    class unique_future final {
    private:
        static constexpr bool is_void_type = std::is_void_v<T>;
        using state_type = std::conditional_t<is_void_type, detail::future_state_base, detail::future_state<T>>;
        using coroutine_state_type = std::conditional_t<is_void_type, detail::future_state<void>, detail::future_state<T>>;

    public:
        unique_future(const unique_future&) = delete;
        unique_future& operator=(const unique_future&) = delete;

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

        template<typename U>
            requires(is_void_type && !std::is_void_v<U>)
        unique_future(unique_future<U>&& other) noexcept
            : state_()
            , resource_(other.resource())
            , needs_scheduling_(other.needs_scheduling()) {
            state_ = intrusive_ptr<state_type>(
                static_cast<state_type*>(other.internal_release_state()), false);
            other.set_needs_scheduling(false);
        }

        unique_future& operator=(unique_future&& other) noexcept {
            if (this != &other) {
                if (state_ && state_->is_pending()) {
                    state_->cancelled();
                }

                state_ = std::move(other.state_);
                resource_ = other.resource_;
                needs_scheduling_ = other.needs_scheduling_;
                other.needs_scheduling_ = false;
            }
            return *this;
        }

        ~unique_future() noexcept = default;

        [[nodiscard]] T get() &&
            requires(!std::is_void_v<T>)
        {
            assert(state_ && "get() on invalid future");
            assert(available() && "get() requires ready future - use co_await or check available() first!");
            if (state_->is_failed()) {
                state_ = nullptr;
                assert(false && "get() on failed/cancelled future!");
                std::terminate();
            }
            T result = state_->take_value();
            state_ = nullptr;
            return result;
        }

        void get() &&
            requires(std::is_void_v<T>)
        {
            assert(state_ && "get() on invalid future");
            assert(available() && "get() requires ready future - use co_await or check available() first!");
            if (state_->is_failed()) {
                state_ = nullptr;
                assert(false && "get() on failed/cancelled future!");
                std::terminate();
            }
            state_ = nullptr;
        }

        T get() & requires(!std::is_void_v<T>) = delete;
        void get() & requires(std::is_void_v<T>) = delete;

        [[nodiscard]] bool available() const noexcept {
            return state_ && state_->is_ready();
        }

        [[nodiscard]] bool failed() const noexcept {
            return state_ && state_->is_failed();
        }

        [[nodiscard]] std::error_code error() const noexcept {
            return state_ ? state_->error() : std::error_code{};
        }

        [[nodiscard]] bool valid() const noexcept {
            return state_ != nullptr;
        }

        void ignore() noexcept {
            state_ = nullptr;
        }

        [[nodiscard]] bool needs_scheduling() const noexcept { return needs_scheduling_; }
        void set_needs_scheduling(bool value) noexcept { needs_scheduling_ = value; }

        [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
            assert(state_ && "memory_resource() called on invalid future");
            return resource_;
        }

        [[nodiscard]] state_type* internal_release_state() noexcept {
            return state_.detach();
        }

        [[nodiscard]] state_type* internal_state() const noexcept {
            return state_.get();
        }

        void cancel() noexcept {
            if (state_) {
                state_->cancelled();
            }
        }

        [[nodiscard]] bool is_cancelled() const noexcept {
            return state_ && state_->is_cancelled();
        }

        void forward_to(promise<T>& target)
            requires(!std::is_void_v<T>)
        {
            if (!state_)
                return;
            auto* target_state = static_cast<detail::future_state<T>*>(target.internal_state_base());
            state_->set_forward_target(target_state);
        }

        void forward_to(promise<void>& target)
            requires(std::is_void_v<T>)
        {
            if (!state_)
                return;
            auto* target_state = static_cast<detail::future_state<void>*>(target.internal_state_base());
            state_->set_forward_target_void(target_state);
        }

        struct awaiter_type {
            unique_future<T>& future_;
            detail::future_state_base* promise_state_;

            explicit awaiter_type(unique_future<T>& f, detail::future_state_base* prom_state = nullptr) noexcept
                : future_(f)
                , promise_state_(prom_state) {}

            [[nodiscard]] bool await_ready() const noexcept {
                return future_.available();
            }

            detail::coroutine_handle<> await_suspend(detail::coroutine_handle<> caller) noexcept {
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

                return detail::noop_coroutine();
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

    private:
        [[nodiscard]] state_type* get_state_internal() const noexcept {
            return state_.get();
        }

        unique_future(state_type* state, std::pmr::memory_resource* res, bool add_ref) noexcept
            : state_(state, add_ref)
            , resource_(res)
            , needs_scheduling_(false) {
        }

        struct promise_type_base {
            using value_type = T;

            unique_future<T> get_return_object() {
                assert(resource_ != nullptr &&
                       "Coroutine must be actor member function with resource() method");

                void* mem = resource_->allocate(sizeof(coroutine_state_type), alignof(coroutine_state_type));
                state_ = new (mem) coroutine_state_type(resource_);

                auto handle = detail::coroutine_handle<struct promise_type>::from_promise(static_cast<struct promise_type&>(*this));
                state_->set_coroutine_owning(handle);

                return unique_future<T>(static_cast<state_type*>(state_), resource_, false);
            }

            detail::suspend_never initial_suspend() noexcept { return {}; }

            auto final_suspend() noexcept {
                struct final_awaiter {
                    coroutine_state_type* state_;

                    bool await_ready() noexcept { return false; }

                    detail::coroutine_handle<> await_suspend(
                        detail::coroutine_handle<promise_type> /*h*/
                        ) noexcept {
                        if (auto cont = state_->take_continuation()) {
                            return cont;
                        }
                        return detail::noop_coroutine();
                    }

                    void await_resume() noexcept {}
                };
                return final_awaiter{this->state_};
            }

            template<typename U>
            auto await_transform(unique_future<U>&& future) noexcept {
                return typename unique_future<U>::awaiter_type{future, static_cast<detail::future_state_base*>(state_)};
            }
            
            template<typename U>
            auto await_transform(generator<U>& gen) noexcept {
                return detail::next_awaiter<U>{gen.internal_state()};
            }

            void unhandled_exception() noexcept {
                assert(false && "unhandled_exception() should never be called (-fno-exceptions)");
            }

            promise_type_base() noexcept
                : resource_(nullptr)
                , state_(nullptr) {}

            template<typename First, typename... Args>
            promise_type_base(First&& first, Args&&... args) noexcept
                : resource_(extract_resource_from_args(std::forward<First>(first), std::forward<Args>(args)...))
                , state_(nullptr) {}

            ~promise_type_base() noexcept = default;

        protected:
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

            std::pmr::memory_resource* resource_;
            coroutine_state_type* state_;
        };

        struct promise_type_non_void : promise_type_base {
            using promise_type_base::promise_type_base;

            void return_value(T&& value) noexcept {
                assert(this->state_ && "return_value() with null state");
                this->state_->set_value(std::forward<T>(value));
            }

            void return_value(const T& value) noexcept {
                assert(this->state_ && "return_value() with null state");
                this->state_->set_value(value);
            }

            void return_value(unique_future<T>&& ready_future) noexcept {
                assert(this->state_ && "return_value() with null state");
                assert(ready_future.valid() && "return_value() with invalid future");
                assert(ready_future.available() && "return_value() requires READY future - use co_await first!");
                T val = std::move(ready_future).get();
                this->state_->set_value(std::move(val));
            }

            void return_value(std::error_code ec) noexcept {
                assert(this->state_ && "return_value(error_code) with null state");
                this->state_->error(ec);
            }
        };

        struct promise_type_void : promise_type_base {
            using promise_type_base::promise_type_base;

            void return_void() noexcept {
                assert(this->state_ && "return_void() with null state");
                this->state_->set_ready();
            }
        };

        using promise_type_selected = std::conditional_t<is_void_type, promise_type_void, promise_type_non_void>;

    public:
        struct promise_type : promise_type_selected {
            using promise_type_selected::promise_type_selected;

            template<typename... Args>
            static void* operator new(std::size_t size, const Args&... args) {
                auto* res = promise_type_base::extract_resource_from_args(args...);
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
        };

    private:
        intrusive_ptr<state_type> state_;
        std::pmr::memory_resource* resource_;
        bool needs_scheduling_;
    };

    template<typename T>
    unique_future<T> promise<T>::get_future() noexcept {
        assert(state_ && "get_future() on moved-from promise");
        return unique_future<T>(*this);
    }

    template<typename T>
    auto operator co_await(unique_future<T>&& f) noexcept {
        return typename unique_future<T>::awaiter_type{f, nullptr};
    }

    template<typename T>
    [[nodiscard]] unique_future<T> make_error(std::pmr::memory_resource* res, std::error_code ec) {
        promise<T> p(res);
        p.error(ec);
        return p.get_future();
    }

} // namespace actor_zeta
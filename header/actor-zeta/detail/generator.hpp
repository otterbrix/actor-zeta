#pragma once

#include <atomic>
#include <cassert>
#include <coroutine>
#include <cstdint>
#include <memory_resource>
#include <utility>

#include <system_error>

#include <actor-zeta/detail/future_state.hpp>
#include <actor-zeta/detail/type_traits.hpp>

namespace actor_zeta {

/// Error marker for generator streaming
struct stream_error {
    std::error_code ec;

    explicit stream_error(std::error_code e) noexcept
        : ec(e) {}
};

template<typename T>
class generator;

namespace detail {

namespace generator_states {
    constexpr uint8_t created   = 0;  // Producer not yet started
    constexpr uint8_t suspended = 1;  // Producer suspended at co_yield, value available
    constexpr uint8_t exhausted = 2;  // Producer finished (co_return)
    constexpr uint8_t cancelled = 3;  // Consumer cancelled
    constexpr uint8_t detached  = 4;  // Consumer detached (won't consume more)
}

template<typename T>
class generator_state;

template<typename T>
struct yield_awaiter;

template<typename T>
struct next_awaiter;

template<typename T>
struct final_awaiter;

template<typename T>
struct generator_promise_type;

template<typename T>
class generator_state final : public future_state_base {
public:
    explicit generator_state(std::pmr::memory_resource* res) noexcept
        : future_state_base(res)
        , value_ptr_(nullptr)
        , linked_state_(nullptr)
        , sync_(static_cast<uint8_t>(sync_state::idle)) {
        store_state_raw(generator_states::created);
    }

    ~generator_state() noexcept override = default;

    [[nodiscard]] bool is_ready() const noexcept {
        return load_state_raw() == generator_states::suspended;
    }

    [[nodiscard]] bool is_terminal() const noexcept {
        auto s = load_state_raw();
        return s == generator_states::exhausted ||
               s == generator_states::cancelled ||
               s == generator_states::detached;
    }

    [[nodiscard]] bool is_exhausted() const noexcept {
        return load_state_raw() == generator_states::exhausted;
    }

    [[nodiscard]] bool is_cancelled() const noexcept {
        return load_state_raw() == generator_states::cancelled;
    }

    [[nodiscard]] bool is_detached() const noexcept {
        return load_state_raw() == generator_states::detached;
    }

    [[nodiscard]] bool has_error() const noexcept {
        return static_cast<bool>(error_code_);
    }

    [[nodiscard]] std::error_code error() const noexcept {
        return error_code_;
    }

    [[nodiscard]] bool is_created() const noexcept {
        return load_state_raw() == generator_states::created;
    }

    void set_suspended(void* value_ptr) noexcept {
        value_ptr_ = value_ptr;
        store_state_raw(generator_states::suspended);
    }

    void set_exhausted() noexcept {
        store_state_raw(generator_states::exhausted);
    }

    void set_cancelled() noexcept {
        store_state_raw(generator_states::cancelled);
    }

    void set_detached() noexcept {
        store_state_raw(generator_states::detached);
    }

    bool set_error(std::error_code ec) noexcept {
        error_code_ = ec;
        set_exhausted();
        return true;
    }

    void reset_to_created() noexcept {
        value_ptr_ = nullptr;
        store_state_raw(generator_states::created);
    }

    [[nodiscard]] T& current() noexcept {
        assert(load_state_raw() == generator_states::suspended && "current() requires suspended state");
        assert(value_ptr_ != nullptr && "value_ptr_ is null");
        return *static_cast<T*>(value_ptr_);
    }

    [[nodiscard]] const T& current() const noexcept {
        assert(load_state_raw() == generator_states::suspended && "current() requires suspended state");
        assert(value_ptr_ != nullptr && "value_ptr_ is null");
        return *static_cast<const T*>(value_ptr_);
    }

    void link_to(generator_state<T>* external) noexcept {
        linked_state_ = external;
        if (external) {
            external->add_ref();
        }
    }

    [[nodiscard]] generator_state<T>* linked_state() const noexcept {
        return linked_state_;
    }

    void unlink() noexcept {
        if (linked_state_) {
            linked_state_->release();
            linked_state_ = nullptr;
        }
    }

    void set_producer_handle(std::coroutine_handle<> h) noexcept {
        set_coroutine_owning(h);
    }

    [[nodiscard]] std::coroutine_handle<> take_producer_handle() noexcept {
        auto h = owning_coro_handle_;
        return h;
    }

    void set_consumer_handle(std::coroutine_handle<> h) noexcept {
        set_coroutine(h);
    }

    [[nodiscard]] std::coroutine_handle<> take_consumer_handle() noexcept {
        return take_continuation();
    }

    [[nodiscard]] bool try_set_producer_waiting() noexcept {
        uint8_t expected = static_cast<uint8_t>(sync_state::idle);
        return sync_.compare_exchange_strong(
            expected,
            static_cast<uint8_t>(sync_state::producer_waiting),
            std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    [[nodiscard]] bool try_set_consumer_waiting() noexcept {
        uint8_t expected = static_cast<uint8_t>(sync_state::idle);
        return sync_.compare_exchange_strong(
            expected,
            static_cast<uint8_t>(sync_state::consumer_waiting),
            std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    void reset_sync() noexcept {
        sync_.store(static_cast<uint8_t>(sync_state::idle), std::memory_order_release);
    }

    [[nodiscard]] bool is_producer_waiting() const noexcept {
        return sync_.load(std::memory_order_acquire) == static_cast<uint8_t>(sync_state::producer_waiting);
    }

    [[nodiscard]] bool is_consumer_waiting() const noexcept {
        return sync_.load(std::memory_order_acquire) == static_cast<uint8_t>(sync_state::consumer_waiting);
    }

    bool set_forward_target_void(future_state_base*) noexcept override {
        return false;
    }

protected:
    void destroy() noexcept override {
        unlink();
        this->~generator_state();
        resource_->deallocate(this, sizeof(generator_state<T>), alignof(generator_state<T>));
    }

private:
    enum class sync_state : uint8_t {
        idle = 0,
        producer_waiting = 1,
        consumer_waiting = 2,
    };

    void* value_ptr_;
    generator_state<T>* linked_state_;
    std::atomic<uint8_t> sync_;
    std::error_code error_code_;
};

template<typename T>
struct yield_awaiter {
    generator_state<T>* state_;
    void* value_ptr_;

    bool await_ready() noexcept {
        return state_->is_cancelled() || state_->is_detached();
    }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> producer) noexcept {
        if (state_->is_cancelled() || state_->is_detached()) {
            state_->set_exhausted();
            return producer;
        }

        state_->set_suspended(value_ptr_);
        state_->set_producer_handle(producer);

        auto* linked = state_->linked_state();
        if (linked) {
            linked->set_suspended(value_ptr_);
            linked->set_producer_handle(producer);

            if (linked->try_set_producer_waiting()) {
                return std::noop_coroutine();
            }

            linked->reset_sync();
            auto consumer = linked->take_consumer_handle();
            if (consumer && !consumer.done()) {
                return consumer;
            }
        }

        return std::noop_coroutine();
    }

    void await_resume() noexcept {}
};

template<typename T>
struct next_awaiter {
    generator_state<T>* state_;

    bool await_ready() noexcept {
        return state_->is_ready() || state_->is_terminal();
    }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> consumer) noexcept {
        state_->set_consumer_handle(consumer);

        if (state_->is_created()) {
            auto producer = state_->take_producer_handle();
            if (producer && !producer.done()) {
                state_->try_set_consumer_waiting();
                return producer;
            }
        }

        if (state_->try_set_consumer_waiting()) {
            if (state_->is_ready()) {
                state_->reset_sync();
                return consumer;
            }
            return std::noop_coroutine();
        }

        state_->reset_sync();

        if (state_->is_ready()) {
            return consumer;
        }

        return consumer;
    }

    [[nodiscard]] bool await_resume() noexcept {
        return state_->is_ready() && !state_->is_terminal();
    }
};

template<typename T>
struct final_awaiter {
    generator_state<T>* state_;

    bool await_ready() noexcept {
        return false;
    }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<>) noexcept {
        state_->set_exhausted();

        auto* linked = state_->linked_state();
        if (linked) {
            linked->set_exhausted();

            if (linked->is_consumer_waiting()) {
                linked->reset_sync();
                auto consumer = linked->take_consumer_handle();
                if (consumer && !consumer.done()) {
                    return consumer;
                }
            }
        }

        return std::noop_coroutine();
    }

    void await_resume() noexcept {}
};

/// Awaiter for co_await unique_future<U> inside generator<T>
template<typename GenT, typename FutT>
struct generator_future_awaiter {
    unique_future<FutT>& future_;
    generator_state<GenT>* gen_state_;

    explicit generator_future_awaiter(
        unique_future<FutT>& f,
        generator_state<GenT>* state) noexcept
        : future_(f)
        , gen_state_(state) {}

    [[nodiscard]] bool await_ready() const noexcept {
        return future_.available();
    }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
        // Store generator coroutine for resumption
        gen_state_->set_producer_handle(caller);

        // Set continuation on future's state
        auto* future_state = future_.internal_state();
        if (future_state) {
            future_state->set_coroutine(caller);
        }

        // Check if became ready during setup (race condition)
        if (future_.available()) {
            return caller;
        }

        return std::noop_coroutine();
    }

    auto await_resume() {
        if constexpr (std::is_void_v<FutT>) {
            return;
        } else {
            return std::move(future_).get();
        }
    }
};

template<typename T>
struct generator_promise_type {
    using handle_type = std::coroutine_handle<generator_promise_type>;

    std::pmr::memory_resource* resource_ = nullptr;
    generator_state<T>* state_ = nullptr;

    generator_promise_type() noexcept
        : resource_(nullptr)
        , state_(nullptr) {
        assert(false && "generator can only be created as actor method");
    }

    template<typename First, typename... Args>
    generator_promise_type(First&& first, Args&&...) noexcept
        : resource_(extract_resource_from_args(std::forward<First>(first)))
        , state_(nullptr) {
        assert(resource_ != nullptr && "generator requires actor with resource()");
    }

    generator<T> get_return_object() noexcept;

    std::suspend_always initial_suspend() noexcept {
        return {};
    }

    final_awaiter<T> final_suspend() noexcept {
        return final_awaiter<T>{state_};
    }

    yield_awaiter<T> yield_value(T& value) noexcept {
        return yield_awaiter<T>{state_, std::addressof(value)};
    }

    yield_awaiter<T> yield_value(T&& value) noexcept {
        return yield_awaiter<T>{state_, std::addressof(value)};
    }

    yield_awaiter<T> yield_value(stream_error err) noexcept {
        if (state_) {
            state_->set_error(err.ec);
        }
        return yield_awaiter<T>{state_, nullptr};
    }

    // Support co_await unique_future<U> inside generator
    template<typename U>
    auto await_transform(unique_future<U>&& future) noexcept {
        return generator_future_awaiter<T, U>{future, state_};
    }

    template<typename U>
    auto await_transform(unique_future<U>& future) noexcept {
        return generator_future_awaiter<T, U>{future, state_};
    }

    // Support co_await generator<U> inside generator (for forwarding)
    template<typename U>
    auto await_transform(generator<U>& gen) noexcept {
        return next_awaiter<U>{gen.internal_state()};
    }

    void return_void() noexcept {
        if (state_) {
            state_->set_exhausted();
        }
    }

    void unhandled_exception() noexcept {
        std::terminate();
    }

    template<typename... Args>
    static void* operator new(std::size_t size, const Args&... args) {
        auto* res = extract_resource_from_args(args...);
        if (!res) {
            res = std::pmr::get_default_resource();
        }

        void* ptr = res->allocate(size + sizeof(std::pmr::memory_resource*),
                                   alignof(std::max_align_t));
        auto** res_ptr = static_cast<std::pmr::memory_resource**>(ptr);
        *res_ptr = res;
        return static_cast<char*>(ptr) + sizeof(std::pmr::memory_resource*);
    }

    static void operator delete(void* ptr, std::size_t size) noexcept {
        auto* real_ptr = static_cast<char*>(ptr) - sizeof(std::pmr::memory_resource*);
        auto* resource = *reinterpret_cast<std::pmr::memory_resource**>(real_ptr);
        resource->deallocate(real_ptr, size + sizeof(std::pmr::memory_resource*),
                             alignof(std::max_align_t));
    }

private:

    template<typename U>
    static std::pmr::memory_resource* try_get_resource(U* ptr) noexcept {
        if constexpr (requires { ptr->resource(); }) {
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
};

} // namespace detail


template<typename T>
class generator {
public:
    using value_type = T;
    using promise_type = detail::generator_promise_type<T>;

private:
    detail::generator_state<T>* state_;

public:

    generator() noexcept : state_(nullptr) {}

    explicit generator(detail::generator_state<T>* state) noexcept
        : state_(state) {
        if (state_) {
            state_->add_ref();
        }
    }

    generator(generator&& other) noexcept
        : state_(std::exchange(other.state_, nullptr)) {}

    generator& operator=(generator&& other) noexcept {
        if (this != &other) {
            if (state_) {
                state_->release();
            }
            state_ = std::exchange(other.state_, nullptr);
        }
        return *this;
    }

    generator(const generator&) = delete;
    generator& operator=(const generator&) = delete;

    ~generator() {
        if (state_) {
            if (!state_->is_terminal()) {
                state_->set_cancelled();
            }
            state_->release();
        }
    }

    [[nodiscard]] detail::next_awaiter<T> operator co_await() noexcept {
        assert(state_ != nullptr && "co_await on moved-from generator");
        return detail::next_awaiter<T>{state_};
    }

    [[nodiscard]] T& current() noexcept {
        assert(state_ != nullptr && "current() on moved-from generator");
        return state_->current();
    }

    [[nodiscard]] const T& current() const noexcept {
        assert(state_ != nullptr && "current() on moved-from generator");
        return state_->current();
    }

    [[nodiscard]] bool exhausted() const noexcept {
        return state_ == nullptr || state_->is_exhausted();
    }

    [[nodiscard]] bool is_cancelled() const noexcept {
        return state_ != nullptr && state_->is_cancelled();
    }

    [[nodiscard]] bool has_error() const noexcept {
        return state_ && state_->has_error();
    }

    [[nodiscard]] std::error_code error() const noexcept {
        return state_ ? state_->error() : std::error_code{};
    }

    [[nodiscard]] bool is_safe_to_destroy() const noexcept {
        return state_ == nullptr || state_->is_terminal();
    }

    [[nodiscard]] bool valid() const noexcept {
        return state_ != nullptr;
    }

    void cancel() noexcept {
        if (state_ && !state_->is_terminal()) {
            state_->set_cancelled();
        }
    }

    void detach() noexcept {
        if (state_) {
            if (!state_->is_terminal()) {
                state_->set_detached();
            }
            state_->release();
            state_ = nullptr;
        }
    }

    void link_to(detail::generator_state<T>* external) noexcept {
        if (state_) {
            state_->link_to(external);
        }
    }

    [[nodiscard]] detail::generator_state<T>* linked_state() const noexcept {
        return state_ ? state_->linked_state() : nullptr;
    }

    [[nodiscard]] detail::generator_state<T>* internal_state() const noexcept {
        return state_;
    }
};

namespace detail {

template<typename T>
generator<T> generator_promise_type<T>::get_return_object() noexcept {
    if (!resource_) {
        resource_ = std::pmr::get_default_resource();
    }

    void* mem = resource_->allocate(sizeof(generator_state<T>), alignof(generator_state<T>));
    state_ = new (mem) generator_state<T>(resource_);

    state_->set_coroutine_owning(handle_type::from_promise(*this));

    return generator<T>(state_);
}

} // namespace detail

} // namespace actor_zeta
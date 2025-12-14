#pragma once

#include <chrono>
#include <thread>

#include <actor-zeta/actor/actor_mixin.hpp>
#include <actor-zeta/config.hpp>
#include <actor-zeta/detail/future.hpp>
#include <actor-zeta/detail/future_state.hpp>
#include <actor-zeta/detail/memory.hpp>
#include <actor-zeta/detail/queue/enqueue_result.hpp>
#include <actor-zeta/scheduler/resumable.hpp>

namespace actor_zeta { namespace actor {

    enum class actor_state : uint8_t {
        idle = 0b000,
        scheduled = 0b001,
        running = 0b010,
        running_scheduled = 0b011,
        idle_destroying = 0b100,
        scheduled_destroying = 0b101,
        running_destroying = 0b110,
        running_scheduled_destroying = 0b111
    };

    constexpr bool is_scheduled(actor_state s) noexcept {
        return (static_cast<uint8_t>(s) & 0b001) != 0;
    }

    constexpr bool is_running(actor_state s) noexcept {
        return (static_cast<uint8_t>(s) & 0b010) != 0;
    }

    constexpr bool is_destroying(actor_state s) noexcept {
        return (static_cast<uint8_t>(s) & 0b100) != 0;
    }

    constexpr actor_state set_scheduled(actor_state s, bool value) noexcept {
        auto bits = static_cast<uint8_t>(s);
        if (value) {
            bits |= 0b001;
        } else {
            bits &= ~0b001;
        }
        return static_cast<actor_state>(bits);
    }

    constexpr actor_state set_running(actor_state s, bool value) noexcept {
        auto bits = static_cast<uint8_t>(s);
        if (value) {
            bits |= 0b010;
        } else {
            bits &= ~0b010;
        }
        return static_cast<actor_state>(bits);
    }

    constexpr actor_state set_destroying(actor_state s) noexcept {
        return static_cast<actor_state>(static_cast<uint8_t>(s) | 0b100);
    }

    constexpr actor_state make_state(bool scheduled, bool running, bool destroying) noexcept {
        uint8_t bits = 0;
        if (scheduled)
            bits |= 0b001;
        if (running)
            bits |= 0b010;
        if (destroying)
            bits |= 0b100;
        return static_cast<actor_state>(bits);
    }

    template<class Target>
    Target* check_ptr(Target* ptr) {
        assert(ptr);
        return ptr;
    }

    /// @brief Maximum CAS retry attempts before declaring livelock
    inline constexpr int kMaxCasAttempts = 1000;

    /// @brief Exponential backoff for CAS loops
    /// @param attempt Current retry attempt (0-based)
    /// @note Phase 1 (0-3):   Pure spin (fast path, ~10-40 CPU cycles)
    /// @note Phase 2 (4-9):   yield() - let other threads run
    /// @note Phase 3 (10+):   sleep(2^(attempt-10) us), capped at 1ms
    inline void exponential_backoff(int attempt) noexcept {
        constexpr int kSpinPhaseEnd = 4;
        constexpr int kYieldPhaseEnd = 10;
        constexpr int kMaxSleepMicroseconds = 1000;

        if (attempt < kSpinPhaseEnd) {
            // Pure spin - fastest for short contention
        } else if (attempt < kYieldPhaseEnd) {
            std::this_thread::yield();
        } else {
            auto sleep_us = std::min(1 << (attempt - kYieldPhaseEnd), kMaxSleepMicroseconds);
            std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
        }
    }

    template<class Actor, class MailBox>
    class cooperative_actor
        : public actor_mixin<Actor> {
    private:
        static constexpr bool check_dispatch_traits_exists() {
            using dispatch_traits_check = typename Actor::dispatch_traits;
            (void) sizeof(dispatch_traits_check);
            return true;
        }

    public:
        using typename actor_mixin<Actor>::id_t;
        using typename actor_mixin<Actor>::placement_tag;
        using actor_mixin<Actor>::placement;

        using unique_actor = std::unique_ptr<cooperative_actor<Actor, MailBox>, pmr::deleter_t>;

        template<typename T>
        using promise = actor_zeta::promise<T>;

        template<typename T>
        using unique_future = actor_zeta::unique_future<T>;

        template<typename R, typename... Args>
        [[nodiscard("Check needs_scheduling() and call schedule() if needed")]]
        unique_future<R> enqueue_impl(
            actor::address_t sender,
            mailbox::message_id cmd,
            Args&&... args) {
            // Create message + promise in one call (cleaner API)
            auto [msg, future] = detail::make_message<R>(
                this->resource(),
                std::move(sender),
                cmd,
                std::forward<Args>(args)...);

            // Save result_promise before msg is moved (for error handling)
            auto result_promise = msg->template get_result_promise<R>();

            if (is_destroying(state_.load(std::memory_order_acquire))) {
                result_promise.error(std::make_error_code(std::errc::operation_canceled));
                return std::move(future);
            }

            auto result = mailbox().push_back(std::move(msg));
            bool needs_sched = false;

            switch (result) {
                case detail::enqueue_result::unblocked_reader: {
                    auto current = state_.load(std::memory_order_acquire);
                    actor_state desired;

                    int cas_attempts = 0;

                    while (true) {
                        exponential_backoff(cas_attempts);
                        ++cas_attempts;

#ifndef NDEBUG
                        assert(cas_attempts < kMaxCasAttempts && "enqueue_impl: CAS loop - possible livelock!");
#else
                        if (cas_attempts >= kMaxCasAttempts) {
                            std::terminate();
                        }
#endif

                        if (is_running(current) || is_destroying(current)) {
                            needs_sched = false;
                            break;
                        }

                        if (is_scheduled(current)) {
                            needs_sched = false;
                            break;
                        }

                        assert((current == actor_state::idle || current == actor_state::idle_destroying) &&
                               "enqueue_impl: expected idle or idle_destroying!");

                        desired = set_scheduled(current, true);
                        assert(desired == actor_state::scheduled && "enqueue_impl: invalid desired state!");

                        if (state_.compare_exchange_weak(current, desired,
                                                         std::memory_order_acq_rel,
                                                         std::memory_order_acquire)) {
                            needs_sched = true;
                            break;
                        }
                    }
                    break;
                }

                case detail::enqueue_result::success:
                    needs_sched = false;
                    break;

                case detail::enqueue_result::queue_closed:
                    result_promise.error(std::make_error_code(std::errc::broken_pipe));
                    needs_sched = false;
                    break;

                default:
                    assert(false && "enqueue_result: unreachable");
                    needs_sched = false;
                    break;
            }

            future.set_needs_scheduling(needs_sched);
            return std::move(future);
        }

        scheduler::resume_info resume(size_t max_throughput) noexcept {
            assert(max_throughput > 0 && "max_throughput must be greater than 0");

            struct resume_guard {
                std::atomic<actor_state>& state_ref_;
                bool keep_scheduled_;

                explicit resume_guard(std::atomic<actor_state>& state)
                    : state_ref_(state)
                    , keep_scheduled_(false) {
                    auto current = state_ref_.load(std::memory_order_acquire);
                    actor_state desired;

                    int cas_attempts = 0;

                    while (true) {
                        exponential_backoff(cas_attempts);
                        ++cas_attempts;

#ifndef NDEBUG
                        assert(cas_attempts < kMaxCasAttempts && "resume_guard: CAS loop - possible livelock!");
#else
                        if (cas_attempts >= kMaxCasAttempts) {
                            std::terminate();
                        }
#endif

                        if (is_running(current)) {
#ifndef NDEBUG
                            assert(false && "Concurrent resume() detected!");
#else
                            std::terminate();
#endif
                        }

#ifndef NDEBUG
                        assert((current == actor_state::idle ||
                                current == actor_state::scheduled ||
                                current == actor_state::idle_destroying ||
                                current == actor_state::scheduled_destroying) &&
                               "resume_guard: unexpected entry state!");
#endif

                        desired = set_running(current, true);

#ifndef NDEBUG
                        assert((desired == actor_state::running ||
                                desired == actor_state::running_scheduled ||
                                desired == actor_state::running_destroying ||
                                desired == actor_state::running_scheduled_destroying) &&
                               "resume_guard: invalid desired state!");
#endif

                        if (state_ref_.compare_exchange_weak(current, desired,
                                                             std::memory_order_acq_rel,
                                                             std::memory_order_acquire)) {
                            break;
                        }
                    }
                }

                ~resume_guard() {
                    auto current = state_ref_.load(std::memory_order_acquire);
                    actor_state desired;

                    int cas_attempts = 0;

                    while (true) {
                        exponential_backoff(cas_attempts);
                        ++cas_attempts;

#ifndef NDEBUG
                        assert(cas_attempts < kMaxCasAttempts && "resume_guard destructor: CAS loop!");
#else
                        if (cas_attempts >= kMaxCasAttempts) {
                            std::terminate();
                        }
#endif

                        assert(is_running(current) && "resume_guard: not running!");
                        desired = set_running(current, false);

                        if (!keep_scheduled_) {
                            desired = set_scheduled(desired, false);
                        }

                        if (state_ref_.compare_exchange_weak(current, desired,
                                                             std::memory_order_acq_rel,
                                                             std::memory_order_acquire)) {
                            break;
                        }
                    }
                }

                void keep_scheduled() {
                    keep_scheduled_ = true;
                }
            };
            resume_guard guard(state_);

            auto finalize = [&guard](scheduler::resume_result result, size_t handled, bool keep_scheduled) -> scheduler::resume_info {
                if (keep_scheduled) {
                    guard.keep_scheduled();
                }
                return scheduler::resume_info(result, handled);
            };

            auto check_race_window = [this]() -> bool {
                return !mailbox().blocked() && !mailbox().empty();
            };

            size_t handled = 0;

            if (is_destroying(state_.load(std::memory_order_acquire))) {
                return finalize(scheduler::resume_result::done, 0, false);
            }

            if (mailbox().closed()) {
                return finalize(scheduler::resume_result::done, 0, false);
            }

            if (mailbox().blocked()) {
                return finalize(scheduler::resume_result::awaiting, 0, false);
            }

            if (mailbox().empty()) {
                auto result = mailbox().try_block()
                                  ? scheduler::resume_result::awaiting
                                  : scheduler::resume_result::resume;
                bool keep_scheduled = (result == scheduler::resume_result::awaiting && check_race_window());
                if (keep_scheduled) {
                    result = scheduler::resume_result::resume;
                }
                return finalize(result, 0, keep_scheduled);
            }

            while (handled < max_throughput) {
                if (is_destroying(state_.load(std::memory_order_acquire))) {
                    return finalize(scheduler::resume_result::done, handled, false);
                }

                if (mailbox().closed()) {
                    return finalize(scheduler::resume_result::done, handled, false);
                }

                if (mailbox().blocked()) {
                    return finalize(scheduler::resume_result::awaiting, handled, false);
                }

                const size_t before = handled;

                auto msg = mailbox().pop_front();
                if (msg) {
                    struct message_guard {
                        cooperative_actor* actor_;
                        mailbox::message_ptr message_;
                        mailbox::message* prev_message_;

                        message_guard(cooperative_actor* actor, mailbox::message_ptr msg) noexcept
                            : actor_(actor)
                            , message_(std::move(msg))
                            , prev_message_(actor->current_message_) {
                            actor_->current_message_ = message_.get();
                        }

                        ~message_guard() noexcept {
                            // Refcount managed by promise/future - message_guard only restores state
                            actor_->current_message_ = prev_message_;
                        }

                        mailbox::message* get() const noexcept { return message_.get(); }
                    };

                    message_guard guard(this, std::move(msg));

                    if (!is_destroying(state_.load(std::memory_order_acquire))) {
                        self()->behavior(guard.get());
                    }

                    ++handled;
                }

                if (handled == before) {
                    if (mailbox().closed()) {
                        return finalize(scheduler::resume_result::done, handled, false);
                    }
                    auto result = mailbox().try_block()
                                      ? scheduler::resume_result::awaiting
                                      : scheduler::resume_result::resume;
                    bool keep_scheduled = (result == scheduler::resume_result::awaiting && check_race_window());
                    if (keep_scheduled) {
                        result = scheduler::resume_result::resume;
                    }
                    return finalize(result, handled, keep_scheduled);
                }
            }

            if (mailbox().closed()) {
                return finalize(scheduler::resume_result::done, handled, false);
            }

            auto result = mailbox().try_block()
                              ? scheduler::resume_result::awaiting
                              : scheduler::resume_result::resume;
            bool keep_scheduled = (result == scheduler::resume_result::awaiting && check_race_window());
            if (keep_scheduled) {
                result = scheduler::resume_result::resume;
            }
            return finalize(result, handled, keep_scheduled);
        }

        std::pmr::memory_resource* resource() const noexcept {
#ifndef NDEBUG
            assert(magic_ == kMagicAlive && "Use-after-free: resource() called on destroyed actor!");
#endif
            return resource_;
        }

        cooperative_actor() = delete;

        ~cooperative_actor() {
#ifndef NDEBUG
            assert(magic_ == kMagicAlive && "Double-delete!");
#endif
            auto current = state_.load(std::memory_order_acquire);

            while (!is_destroying(current)) {
                auto desired = set_destroying(current);
                if (state_.compare_exchange_weak(current, desired,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
                    break;
                }
            }

            wait_for_resume_to_complete("Destructor");

            current = state_.load(std::memory_order_acquire);
            while (true) {
                assert(!is_running(current) && "Destructor: still running!");
                auto desired = make_state(false, false, true);
                if (state_.compare_exchange_weak(current, desired,
                                                 std::memory_order_release,
                                                 std::memory_order_acquire)) {
                    break;
                }
            }
        }

    protected:
        explicit cooperative_actor(std::pmr::memory_resource* in_resource)
            : actor_mixin<Actor>()
            , shutdown_guard_(this) // Initialize FIRST (destroyed LAST!)
            , resource_(check_ptr(in_resource))
            , current_message_(nullptr)
            , mailbox_()
#ifndef NDEBUG
            , magic_(kMagicAlive)
#endif
        {
            static_assert(check_dispatch_traits_exists(),
                          "Actor must define nested 'struct dispatch_traits { using methods = type_list<...>; }'");
            mailbox().try_block();
        }

    private:
        void begin_shutdown() noexcept {
            auto current = state_.load(std::memory_order_acquire);
            while (!is_destroying(current)) {
                auto desired = set_destroying(current);
                if (state_.compare_exchange_weak(current, desired,
                                                 std::memory_order_release,
                                                 std::memory_order_acquire)) {
                    break;
                }
            }

            wait_for_resume_to_complete("begin_shutdown");
        }

        struct shutdown_guard_t {
            cooperative_actor* self_;

            explicit shutdown_guard_t(cooperative_actor* self) noexcept
                : self_(self) {}

            ~shutdown_guard_t() noexcept {
                self_->begin_shutdown();
            }

            shutdown_guard_t(const shutdown_guard_t&) = delete;
            shutdown_guard_t& operator=(const shutdown_guard_t&) = delete;
            shutdown_guard_t(shutdown_guard_t&&) = delete;
            shutdown_guard_t& operator=(shutdown_guard_t&&) = delete;
        };

        void wait_for_resume_to_complete(const char* context) noexcept {
            std::atomic_thread_fence(std::memory_order_seq_cst);

            auto start_time = std::chrono::steady_clock::now();
#ifndef NDEBUG
            constexpr auto timeout = std::chrono::seconds(5);
#else
            constexpr auto timeout = std::chrono::seconds(30);
#endif

            int wait_attempts = 0;
            while (is_running(state_.load(std::memory_order_acquire))) {
                exponential_backoff(wait_attempts);
                ++wait_attempts;

                auto elapsed = std::chrono::steady_clock::now() - start_time;
                if (elapsed > timeout) {
#ifndef NDEBUG
                    (void) context;
                    assert(false && "wait_for_resume_to_complete: timeout!");
#else
                    (void) context;
                    std::terminate();
#endif
                }
            }

            std::atomic_thread_fence(std::memory_order_seq_cst);
        }

        mailbox::message* current_message() noexcept { return current_message_; }

        inline const Actor* self() const noexcept {
            return static_cast<const Actor*>(this);
        }

        inline Actor* self() noexcept {
            return static_cast<Actor*>(this);
        }

        MailBox& mailbox() noexcept {
            return mailbox_;
        }

        shutdown_guard_t shutdown_guard_;

        std::pmr::memory_resource* resource_;
        mailbox::message* current_message_;
        MailBox mailbox_;
        std::atomic<actor_state> state_{actor_state::idle};

#ifndef NDEBUG
        static constexpr uint32_t kMagicAlive = 0xFEEDFACE;

        uint32_t magic_;
#endif
    };

}} // namespace actor_zeta::actor

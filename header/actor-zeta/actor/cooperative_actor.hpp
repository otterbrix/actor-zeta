#pragma once

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include <actor-zeta/actor/actor_mixin.hpp>
#include <actor-zeta/detail/behavior_t.hpp>
#include <actor-zeta/detail/forwards.hpp>
#include <actor-zeta/detail/memory.hpp>
#include <actor-zeta/scheduler/resumable.hpp>

namespace actor_zeta { namespace actor {

    // Three flags in the low bits, in-flight senders counted above them. One word, not
    // two atomics: two would let a sender and the destructor each miss the other's write
    // short of seq_cst fences on both sides; one word has one modification order. uint32_t:
    // in a uint8_t the 32nd sender would carry out of the word and read as zero senders.
    enum class actor_state : uint32_t {
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
        return (static_cast<uint32_t>(s) & 0b001) != 0;
    }

    constexpr bool is_running(actor_state s) noexcept {
        return (static_cast<uint32_t>(s) & 0b010) != 0;
    }

    constexpr bool is_destroying(actor_state s) noexcept {
        return (static_cast<uint32_t>(s) & 0b100) != 0;
    }

    constexpr actor_state set_scheduled(actor_state s, bool value) noexcept {
        auto bits = static_cast<uint32_t>(s);
        if (value) {
            bits |= 0b001;
        } else {
            bits &= ~static_cast<uint32_t>(0b001);
        }
        return static_cast<actor_state>(bits);
    }

    constexpr actor_state set_running(actor_state s, bool value) noexcept {
        auto bits = static_cast<uint32_t>(s);
        if (value) {
            bits |= 0b010;
        } else {
            bits &= ~static_cast<uint32_t>(0b010);
        }
        return static_cast<actor_state>(bits);
    }

    constexpr actor_state set_destroying(actor_state s) noexcept {
        return static_cast<actor_state>(static_cast<uint32_t>(s) | 0b100);
    }

    // A sender stays registered until its last touch of the actor; the destructor waits the count out.
    inline constexpr uint32_t kSenderStep = 0b1000;

    constexpr uint32_t sender_count(actor_state s) noexcept {
        return static_cast<uint32_t>(s) / kSenderStep;
    }

    // The flags alone, for comparisons that must not see the count.
    constexpr actor_state flags_of(actor_state s) noexcept {
        return static_cast<actor_state>(static_cast<uint32_t>(s) % kSenderStep);
    }

    constexpr actor_state add_sender(actor_state s) noexcept {
        return static_cast<actor_state>(static_cast<uint32_t>(s) + kSenderStep);
    }

    constexpr actor_state sub_sender(actor_state s) noexcept {
        return static_cast<actor_state>(static_cast<uint32_t>(s) - kSenderStep);
    }

    // Rebuilds the word from scratch: only valid where the sender count is known to be zero.
    constexpr actor_state make_state(bool scheduled, bool running, bool destroying) noexcept {
        uint32_t bits = 0;
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

    inline constexpr int kMaxCasAttempts = 1000;

    inline void exponential_backoff(int attempt) noexcept {
        constexpr int kSpinPhaseEnd = 4;
        constexpr int kYieldPhaseEnd = 10;
        constexpr int kMaxSleepMicroseconds = 1000;

        if (attempt < kSpinPhaseEnd) {
        } else if (attempt < kYieldPhaseEnd) {
            std::this_thread::yield();
        } else {
            // Cap the EXPONENT: `1 << 31` is INT_MIN and past it the shift is UB. Reachable via wait_for_activity_to_drain().
            constexpr int kMaxShift = 10;
            const int exponent = attempt - kYieldPhaseEnd;
            const int computed = exponent >= kMaxShift ? kMaxSleepMicroseconds : (1 << exponent);
            const auto sleep_us = computed < kMaxSleepMicroseconds ? computed : kMaxSleepMicroseconds;
            std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
        }
    }

    // Backoff plus the bound for every CAS loop here. Not a retry budget: the flags
    // only ever move forward, so a thousand failures on one word is a broken state
    // machine, not contention -- hence abort, in release too, where silent refusal is worst.
    inline void cas_attempt(int& attempts, const char* context) noexcept {
        exponential_backoff(attempts);
        if (++attempts >= kMaxCasAttempts) {
            std::fprintf(stderr,
                         "actor-zeta: %s spun %d times on one compare-exchange.\n"
                         "  The flags in the state word only ever move forward, so this is a\n"
                         "  broken state machine rather than contention -- something is writing\n"
                         "  the word outside the transitions in this file.\n",
                         context, attempts);
            std::abort();
        }
    }

    template<class Actor, class MailBox>
    class cooperative_actor
        : public actor_mixin<Actor> {
    private:
        static constexpr bool check_dispatch_traits_exists() {
            using dispatch_traits_check = typename Actor::dispatch_traits;
            detail::ignore_unused(sizeof(dispatch_traits_check));
            return true;
        }

    public:
        using is_cooperative_actor_type = void;

        using typename actor_mixin<Actor>::id_t;
        using typename actor_mixin<Actor>::placement_tag;
        using actor_mixin<Actor>::placement;

        using unique_actor = std::unique_ptr<cooperative_actor<Actor, MailBox>, pmr::deleter_t>;

        template<typename T>
        using promise = actor_zeta::promise<T>;

        template<typename T>
        using unique_future = actor_zeta::unique_future<T>;

        [[nodiscard]]
        std::pair<bool, detail::enqueue_result> enqueue_impl(mailbox::message_ptr msg) {
            // Read `destroying` and register as a sender in ONE RMW: in the word's order
            // either our registration precedes the destructor's drain (it waits for us) or
            // `destroying` precedes us (we never touch the mailbox; it may be gone the instant
            // we return). A bare check could not make it wait for a sender already in push_back.
            {
                auto current = state_.load(std::memory_order_acquire);
                int cas_attempts = 0;

                while (true) {
                    cas_attempt(cas_attempts, "enqueue_impl");
                    if (is_destroying(current)) {
                        return {false, detail::enqueue_result::queue_closed};
                    }

                    assert(sender_count(current) < (1u << 29) - 1 && "enqueue_impl: sender count overflow!");

                    if (state_.compare_exchange_weak(current, add_sender(current),
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
                        break;
                    }
                }
            }

            auto result = mailbox().push_back(std::move(msg));

            // Deregister and claim `scheduled` in one RMW: between two, we would be
            // uncounted but unclaimed, and the destructor may proceed in that gap.
            const bool needs_sched = leave_and_maybe_schedule(
                "enqueue_impl", result == detail::enqueue_result::unblocked_reader);

            return {needs_sched, result};
        }

        // The verdict is an obligation: on `resume` the caller must re-enqueue this actor.
        // Nothing else will -- a completing future is flag-only, and send() only says needs_sched when blocked.
        [[nodiscard]] scheduler::resume_info resume(size_t max_throughput) noexcept {
            assert(max_throughput > 0 && "max_throughput must be greater than 0");

            auto try_acquire_running = [this]() -> bool {
                auto current = state_.load(std::memory_order_acquire);
                actor_state desired;
                int cas_attempts = 0;

                while (true) {
                    cas_attempt(cas_attempts, "try_acquire_running");

                    if (is_running(current)) {
                        desired = set_scheduled(current, true);
                        if (state_.compare_exchange_weak(current, desired,
                                                         std::memory_order_acq_rel,
                                                         std::memory_order_acquire)) {
                            return false;  // Not acquired, but marked for re-run
                        }
                        continue;
                    }

#ifndef NDEBUG
                    // flags_of: a live sender count would make every enumerator compare unequal.
                    assert((flags_of(current) == actor_state::idle ||
                            flags_of(current) == actor_state::scheduled ||
                            flags_of(current) == actor_state::idle_destroying ||
                            flags_of(current) == actor_state::scheduled_destroying) &&
                           "try_acquire_running: unexpected state!");
#endif

                    desired = set_scheduled(set_running(current, true), false);

                    if (state_.compare_exchange_weak(current, desired,
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
                        return true;
                    }
                }
            };

            if (!try_acquire_running()) {
                // `awaiting`, not `done`: the `running` holder has the scheduled bit now and
                // returns `resume` itself; `done` would let a driver retire a contended actor.
                return scheduler::resume_info(scheduler::resume_result::awaiting, 0);
            }

            struct resume_guard {
                std::atomic<actor_state>& state_ref_;
                bool keep_scheduled_;
                bool* scheduled_while_running_;

                explicit resume_guard(std::atomic<actor_state>& state, bool* scheduled_while_running)
                    : state_ref_(state)
                    , keep_scheduled_(false)
                    , scheduled_while_running_(scheduled_while_running) {
                }

                ~resume_guard() {
                    auto current = state_ref_.load(std::memory_order_acquire);
                    actor_state desired;
                    int cas_attempts = 0;

                    *scheduled_while_running_ = is_scheduled(current);

                    while (true) {
                        cas_attempt(cas_attempts, "~resume_guard");

                        assert(is_running(current) && "resume_guard: not running!");
                        desired = set_running(current, false);

                        desired = set_scheduled(desired, keep_scheduled_ || is_scheduled(current));

                        if (state_ref_.compare_exchange_weak(current, desired,
                                                             std::memory_order_acq_rel,
                                                             std::memory_order_acquire)) {
                            break;
                        }
                        *scheduled_while_running_ = *scheduled_while_running_ || is_scheduled(current);
                    }
                }

                void keep_scheduled() { keep_scheduled_ = true; }
            };

            auto check_race_window = [this]() -> bool {
                return !mailbox().blocked() && !mailbox().empty();
            };

            scheduler::resume_info result_info;
            bool scheduled_while_running = false;

            {
                resume_guard guard(state_, &scheduled_while_running);

                auto finalize = [&guard](scheduler::resume_result result, size_t handled, bool keep_sched) -> scheduler::resume_info {
                    if (keep_sched) {
                        guard.keep_scheduled();
                    }
                    return scheduler::resume_info(result, handled);
                };

                result_info = resume_impl(max_throughput, finalize, check_race_window);
            }

            if (result_info.result == scheduler::resume_result::awaiting && scheduled_while_running) {
                result_info.result = scheduler::resume_result::resume;
            }

            return result_info;
        }

    private:
        template<typename Finalize, typename CheckRaceWindow>
        scheduler::resume_info resume_impl(size_t max_throughput, Finalize& finalize, CheckRaceWindow& check_race_window) noexcept {
            size_t handled = 0;

            if (is_destroying(state_.load(std::memory_order_acquire))) {
                return finalize(scheduler::resume_result::done, 0, false);
            }

            if (mailbox().closed()) {
                return finalize(scheduler::resume_result::done, 0, false);
            }

            // Un-park on the way in -- the counterpart of park()'s try_block(). blocked()
            // means "no job exists and the next producer owes the scheduling"; under
            // `running` neither holds (we drain; scheduled_while_running catches a late
            // arrival), and left blocked a concurrent send() would be handed needs_sched
            // for a running actor -- a second job node. Reached when something other than
            // send() drives the actor. Needs `running`: without it, clearing the tag strands
            // the next message (needs_sched == false, nobody committed to draining).
            assert(is_running(state_.load(std::memory_order_acquire)) &&
                   "resume_impl: un-parking without holding `running`");
            mailbox().try_unblock();

            // Drain a suspended behavior BEFORE the park() below; the order is the point. A
            // behavior on a co_await must report `resume`, never `awaiting` -- awaiting pairs
            // with keep_scheduled = false, so nothing would ever wake it. Readiness is flag-only
            // (release_promise() touches neither mailbox nor scheduler): the mailbox cannot speak for it.
            if (current_behavior_.is_busy()) {
                if (current_behavior_.is_awaited_ready()) {
                    auto cont = current_behavior_.take_awaited_continuation();
                    if (cont) {
                        cont.resume();
                    }
                }
                // Re-check: not ready, or re-suspended inside cont.resume(); falling through would strand it.
                if (current_behavior_.is_busy()) {
                    return finalize(scheduler::resume_result::resume, 0, true);
                }
            }

            if (mailbox().blocked()) {
                return finalize(scheduler::resume_result::awaiting, 0, false);
            }

            if (mailbox().empty()) {
                return park(finalize, check_race_window, 0);
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

                // Same drain as at entry: ready, unwind; not ready, stay scheduled.
                if (current_behavior_.is_busy()) {
                    if (current_behavior_.is_awaited_ready()) {
                        auto cont = current_behavior_.take_awaited_continuation();
                        if (cont) {
                            cont.resume();
                        }
                    } else {
                        return finalize(scheduler::resume_result::resume, handled, true);
                    }
                }

                if (current_behavior_.is_busy()) {
                    return finalize(scheduler::resume_result::resume, handled, true);
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
                            actor_->current_message_ = prev_message_;
                        }

                        mailbox::message* get() const noexcept { return message_.get(); }
                    };

                    message_guard msg_guard(this, std::move(msg));

                    if (!is_destroying(state_.load(std::memory_order_acquire))) {
                        current_behavior_ = self()->behavior(msg_guard.get());
                    }

                    ++handled;
                }

                if (handled == before) {
                    if (mailbox().closed()) {
                        return finalize(scheduler::resume_result::done, handled, false);
                    }
                    return park(finalize, check_race_window, handled);
                }
            }

            if (mailbox().closed()) {
                return finalize(scheduler::resume_result::done, handled, false);
            }

            if (current_behavior_.is_busy()) {
                return finalize(scheduler::resume_result::resume, handled, true);
            }

            return park(finalize, check_race_window, handled);
        }

        // Park on an empty inbox. NEVER returns (awaiting, keep_scheduled = true):
        // ~resume_guard would set `scheduled` with no job in any queue, and
        // leave_and_maybe_schedule() never claims a set bit -- needs_sched false forever;
        // the verdict becomes `resume` instead. check_race_window() runs only after
        // try_block() succeeded; its `!blocked() &&` keeps empty() away from a blocked inbox.
        template<typename Finalize, typename CheckRaceWindow>
        scheduler::resume_info park(Finalize& finalize,
                                    CheckRaceWindow& check_race_window,
                                    size_t handled) noexcept {
            // A live behavior here is the lost wakeup the drain ordering prevents; every
            // caller is dominated by is_busy(). done(): no behavior, or one at final_suspend.
            assert(current_behavior_.done() && "park() with a live behavior -- lost wakeup");

            auto result = mailbox().try_block()
                              ? scheduler::resume_result::awaiting
                              : scheduler::resume_result::resume;
            bool keep_scheduled = (result == scheduler::resume_result::resume) ||
                                  (result == scheduler::resume_result::awaiting && check_race_window());
            if (keep_scheduled && result == scheduler::resume_result::awaiting) {
                result = scheduler::resume_result::resume;
            }
            return finalize(result, handled, keep_scheduled);
        }

    public:
        std::pmr::memory_resource* resource() const noexcept {
#ifndef NDEBUG
            assert(magic_ == kMagicAlive && "Use-after-free!");
#endif
            return resource_;
        }

        cooperative_actor() = delete;

        ~cooperative_actor() {
#ifndef NDEBUG
            assert(magic_ == kMagicAlive && "Double-delete!");
#endif
            auto current = state_.load(std::memory_order_acquire);
            int cas_attempts = 0;

            while (!is_destroying(current)) {
                cas_attempt(cas_attempts, "~cooperative_actor (publish destroying)");
                auto desired = set_destroying(current);
                if (state_.compare_exchange_weak(current, desired,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
                    break;
                }
            }

            wait_for_activity_to_drain();

            current = state_.load(std::memory_order_acquire);
            cas_attempts = 0;
            while (true) {
                cas_attempt(cas_attempts, "~cooperative_actor (terminal state)");
                assert(!is_running(current) && "Destructor: still running!");
                // make_state() would erase a sender count; the drain above guarantees none is left.
                assert(sender_count(current) == 0 && "Destructor: sender still in flight!");
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
            , resource_(check_ptr(in_resource))
            , current_message_(nullptr)
            , mailbox_()
#ifndef NDEBUG
            , magic_(kMagicAlive)
#endif
        {
            static_assert(check_dispatch_traits_exists(),
                          "Actor must define nested 'dispatch_traits'");
            mailbox().try_block();
        }

    private:
        // Deregister and, if we unblocked the mailbox, claim `scheduled` in the same RMW;
        // returns the obligation to enqueue. Branches, not early exits: always deregister.
        [[nodiscard]] bool leave_and_maybe_schedule(const char* context, bool unblocked_us) noexcept {
            auto current = state_.load(std::memory_order_acquire);
            int cas_attempts = 0;

            while (true) {
                cas_attempt(cas_attempts, context);
                assert(sender_count(current) > 0 && "leave_and_maybe_schedule: not registered!");

                auto desired = sub_sender(current);
                bool owes_enqueue = false;

                // Never schedule a dying actor or claim a held bit. A RUNNING actor discharges
                // the obligation itself via scheduled_while_running -- read from the pre-CAS value.
                if (unblocked_us && !is_destroying(current) && !is_scheduled(current)) {
                    desired = set_scheduled(desired, true);
                    owes_enqueue = !is_running(current);
                }

                if (state_.compare_exchange_weak(current, desired,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
                    return owes_enqueue;
                }
            }
        }

        // Waits out the `running` holder and any sender past enqueue_impl's gate.
        // `destroying` must already be published, or a sender can slip in behind us.
        void wait_for_activity_to_drain() noexcept {

            auto start_time = std::chrono::steady_clock::now();
#ifndef NDEBUG
            constexpr auto timeout = std::chrono::seconds(5);
#else
            constexpr auto timeout = std::chrono::seconds(30);
#endif

            int wait_attempts = 0;
            auto busy = [this]() noexcept {
                const auto current = state_.load(std::memory_order_acquire);
                return is_running(current) || sender_count(current) != 0;
            };

            while (busy()) {
                exponential_backoff(wait_attempts);
                ++wait_attempts;

                auto elapsed = std::chrono::steady_clock::now() - start_time;
                if (elapsed > timeout) {
#ifndef NDEBUG
                    assert(false && "wait_for_activity_to_drain: timeout!");
#else
                    std::terminate();
#endif
                }
            }
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

        std::pmr::memory_resource* resource_;
        mailbox::message* current_message_;
        MailBox mailbox_;
        // ONE coroutine per actor, deliberately unreachable from outside: an accessor
        // would be a public way to advance the chain off the thread holding `running`.
        behavior_t current_behavior_;
        std::atomic<actor_state> state_{actor_state::idle};

#ifndef NDEBUG
        static constexpr uint32_t kMagicAlive = 0xFEEDFACE;
        uint32_t magic_;
#endif
    };

}} // namespace actor_zeta::actor
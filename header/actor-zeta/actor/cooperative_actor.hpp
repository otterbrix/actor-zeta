#pragma once

#include <chrono>
#include <thread>

#include <actor-zeta/actor/actor_mixin.hpp>
#include <actor-zeta/detail/behavior_t.hpp>
#include <actor-zeta/detail/forwards.hpp>
#include <actor-zeta/detail/memory.hpp>
#include <actor-zeta/scheduler/resumable.hpp>

namespace actor_zeta { namespace actor {

    // Three flags in the low bits, a count of in-flight senders above them.
    //
    // The count lives HERE, in the same word as `destroying`, rather than in an
    // atomic of its own. With two words a sender and the destructor can each miss
    // the other -- the destructor reads the count before the increment is visible
    // while the sender reads the state before `destroying` is -- and closing that
    // needs a seq_cst fence on both sides. One word has one modification order, so
    // either the sender's registration precedes the destructor's read or it does
    // not, and no fence is required to decide.
    //
    // uint32_t rather than uint8_t: five spare bits cap the count at 31, and the
    // 32nd registration would carry into a bit that does not exist, leaving the
    // count reading zero with senders still inside.
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

    // A sender is registered from before it reads `destroying` until after its last
    // touch of the actor. The destructor waits the count out.
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

    // Builds a word with NO senders registered. Only valid where the count is
    // known to be zero -- it rebuilds from scratch and would otherwise erase it.
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
            // Cap the EXPONENT, not just the result. The sleep already saturates at
            // attempt 20 (1 << 10 = 1024 > kMaxSleepMicroseconds), and shifting past
            // that is not merely pointless: at attempt 41 `1 << 31` is INT_MIN, which
            // compares below the cap and yields a negative duration that does not sleep
            // at all, and from attempt 42 the exponent reaches 32 -- undefined
            // behaviour on a 32-bit int.
            //
            // Reachable, not theoretical: wait_for_activity_to_drain() spins here for
            // as long as a resume or an in-flight sender takes, so anything over ~22ms
            // gets there.
            constexpr int kMaxShift = 10;
            const int exponent = attempt - kYieldPhaseEnd;
            const int computed = exponent >= kMaxShift ? kMaxSleepMicroseconds : (1 << exponent);
            const auto sleep_us = computed < kMaxSleepMicroseconds ? computed : kMaxSleepMicroseconds;
            std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
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
        /// Marker type to identify cooperative actors (for concept detection)
        using is_cooperative_actor_type = void;

        using typename actor_mixin<Actor>::id_t;
        using typename actor_mixin<Actor>::placement_tag;
        using actor_mixin<Actor>::placement;

        using unique_actor = std::unique_ptr<cooperative_actor<Actor, MailBox>, pmr::deleter_t>;

        template<typename T>
        using promise = actor_zeta::promise<T>;

        template<typename T>
        using unique_future = actor_zeta::unique_future<T>;

        /// Type-erased enqueue for address_t polymorphism.
        [[nodiscard]]
        std::pair<bool, detail::enqueue_result> enqueue_impl(mailbox::message_ptr msg) {
            // Read `destroying` and register as a sender in ONE read-modify-write on
            // state_. The destructor publishes `destroying` into the same word and then
            // waits for the count to drain, so the two sides cannot miss each other: in
            // that word's modification order either our registration lands first, and
            // the destructor waits for us, or `destroying` does, and we never touch the
            // mailbox at all.
            //
            // Checking `destroying` without registering -- which is what this used to do
            // -- says "do not start" but never "wait for the ones that already started".
            // A sender is not `running`, so nothing else covered it, and the destructor
            // could free the mailbox with a sender inside push_back.
            {
                auto current = state_.load(std::memory_order_acquire);
                int cas_attempts = 0;

                while (true) {
                    exponential_backoff(cas_attempts);
                    ++cas_attempts;
#ifndef NDEBUG
                    assert(cas_attempts < kMaxCasAttempts && "enqueue_impl: CAS livelock!");
#else
                    if (cas_attempts >= kMaxCasAttempts) {
                        std::terminate();
                    }
#endif
                    if (is_destroying(current)) {
                        // Only loads touched *this, so the actor may already be gone the
                        // instant we return.
                        return {false, detail::enqueue_result::queue_closed};
                    }

                    // 29 bits, i.e. half a billion threads inside one enqueue_impl at
                    // once. Unreachable, but a wrap would read back as zero with the
                    // flags intact and let the destructor proceed, so say it out loud.
                    assert(sender_count(current) < (1u << 29) - 1 && "enqueue_impl: sender count overflow!");

                    if (state_.compare_exchange_weak(current, add_sender(current),
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
                        break;
                    }
                }
            }

            auto result = mailbox().push_back(std::move(msg));

            // Deregistering and claiming the scheduled bit are the same step: two
            // read-modify-writes would leave a gap in which we are no longer counted and
            // have not claimed yet, and the destructor is entitled to proceed in it.
            const bool needs_sched = leave_and_maybe_schedule(
                "enqueue_impl", result == detail::enqueue_result::unblocked_reader);

            return {needs_sched, result};
        }

        // The verdict is an obligation, not a status: `resume` means the caller must
        // put this actor back in a run queue. Nothing else will -- a future completing
        // is flag-only, so it neither pushes to the mailbox nor re-enqueues the actor,
        // and send() only reports needs_sched when the inbox was blocked. A driver
        // that drops it strands the actor.
        [[nodiscard]] scheduler::resume_info resume(size_t max_throughput) noexcept {
            assert(max_throughput > 0 && "max_throughput must be greater than 0");

            // Try to acquire running state. If actor is already running,
            // mark it as scheduled so it will re-run, and return early.
            auto try_acquire_running = [this]() -> bool {
                auto current = state_.load(std::memory_order_acquire);
                actor_state desired;
                int cas_attempts = 0;

                while (true) {
                    exponential_backoff(cas_attempts);
                    ++cas_attempts;

#ifndef NDEBUG
                    assert(cas_attempts < kMaxCasAttempts && "try_acquire_running: CAS livelock!");
#else
                    if (cas_attempts >= kMaxCasAttempts) {
                        std::terminate();
                    }
#endif

                    // If already running, mark as scheduled so the running actor will re-run
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
                    // flags_of: the enumerators name flag combinations, and a live
                    // sender count would make every one of them compare unequal.
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
                // `awaiting`, not `done`. Another thread holds `running`; try_acquire
                // just set the scheduled bit for it, and that thread will discharge the
                // obligation by returning `resume`. So the verdict this caller owes is
                // "drop your node, the wakeup belongs to somebody else" -- which is what
                // awaiting means, and what the worker does with it.
                //
                // `done` means finished. The worker reacts to it by calling
                // policy_.after_completion(), a no-op only in the unprofiled policy, and
                // any driver that treats `done` as terminal -- reasonably -- retires an
                // actor that is merely contended.
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
                        exponential_backoff(cas_attempts);
                        ++cas_attempts;

#ifndef NDEBUG
                        assert(cas_attempts < kMaxCasAttempts && "~resume_guard: CAS livelock!");
#else
                        if (cas_attempts >= kMaxCasAttempts) {
                            std::terminate();
                        }
#endif

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

            // Un-park on the way in -- the symmetric half of park()'s try_block().
            //
            // blocked() means "no job exists and the next producer owes the scheduling".
            // Neither half is true here: we hold `running`, so a message arriving now is
            // drained by this instance, and one landing after the drain is picked up by
            // scheduled_while_running. Left blocked, a concurrent send() takes the
            // unblocked_reader branch and is handed needs_sched for an actor that is
            // already running -- a second job node that try_acquire_running must absorb.
            //
            // Reached whenever something other than a send drives the actor: a bare
            // scheduler->enqueue(), or a manual resume() loop. The CAS fails harmlessly
            // when the inbox was not blocked, which is the ordinary case.
            mailbox().try_unblock();

            // Q6 before the park() below, and the order is the point: a behavior
            // suspended on a co_await must report `resume`, never `awaiting`. Awaiting
            // pairs with keep_scheduled = false, so ~resume_guard would leave no job in
            // any queue and nothing would ever wake it. Readiness here is flag-based --
            // release_promise() touches neither the mailbox nor the scheduler -- so the
            // mailbox cannot speak for the behavior.
            //
            // The re-check after cont.resume() is the load-bearing half: the awaited
            // future may not have been ready, or the coroutine may have re-suspended on
            // its next co_await inside the resume.
            if (current_behavior_.is_busy()) {
                if (current_behavior_.is_awaited_ready()) {
                    auto cont = current_behavior_.take_awaited_continuation();
                    if (cont) {
                        cont.resume();
                    }
                }
                // Re-check: the await may not have been ready, or the coroutine
                // re-suspended on its next co_await inside cont.resume(). Falling through to
                // the blocked-return / try_block below would re-strand a live behavior.
                // The hoist above recovers an already-parked behavior; this re-check is what
                // keeps a live one from being parked in the first place.
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

                // Q6 (see resume_impl entry above): if the awaited future is ready,
                // unwind via symmetric transfer; otherwise spin.
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

            // Keep a behavior suspended on co_await scheduled.
            if (current_behavior_.is_busy()) {
                return finalize(scheduler::resume_result::resume, handled, true);
            }

            return park(finalize, check_race_window, handled);
        }

        // Park the actor on an empty inbox.
        //
        // NEVER returns (awaiting, keep_scheduled = true): ~resume_guard would set the
        // `scheduled` bit with no job in any queue, and try_schedule_after_enqueue()
        // refuses to schedule an actor whose bit is already set -- so needs_sched would
        // read false forever and the actor would be stranded. When keep_scheduled is
        // required the verdict is downgraded to `resume`, which puts a real job back in
        // the queue.
        //
        // check_race_window() is only reached when try_block() succeeded, i.e. with the
        // inbox blocked; its own `!mailbox().blocked() &&` short-circuit is what keeps
        // the unconditional mailbox().empty() away from a blocked inbox.
        template<typename Finalize, typename CheckRaceWindow>
        scheduler::resume_info park(Finalize& finalize,
                                    CheckRaceWindow& check_race_window,
                                    size_t handled) noexcept {
            // Parking a LIVE behavior is the lost wakeup this whole ordering exists to
            // prevent: awaiting pairs with keep_scheduled = false, so the actor would
            // leave with no job in any queue and a continuation nobody will ever drain.
            // Every caller is dominated by an is_busy() check that returns `resume`
            // first -- this makes that argument a checked invariant instead of a
            // property of the control flow that a future edit could quietly break.
            //
            // done() is "no behavior, or one parked at final_suspend", i.e. exactly the
            // complement of the state in question.
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
                exponential_backoff(cas_attempts);
                ++cas_attempts;
#ifndef NDEBUG
                assert(cas_attempts < kMaxCasAttempts && "~cooperative_actor: CAS livelock (1)!");
#else
                if (cas_attempts >= kMaxCasAttempts) {
                    std::terminate();
                }
#endif
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
                exponential_backoff(cas_attempts);
                ++cas_attempts;
#ifndef NDEBUG
                assert(cas_attempts < kMaxCasAttempts && "~cooperative_actor: CAS livelock (2)!");
#else
                if (cas_attempts >= kMaxCasAttempts) {
                    std::terminate();
                }
#endif
                assert(!is_running(current) && "Destructor: still running!");
                // make_state() rebuilds the word, so it would erase a sender count.
                // wait_for_activity_to_drain() above guarantees there is none left.
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
            , shutdown_guard_(this)
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
        // Leave (drop this sender's registration) and, when the mailbox reports that we
        // are the producer who unblocked it, claim the `scheduled` bit in the same
        // read-modify-write. Returns the caller's obligation to enqueue a job.
        //
        // The two early exits that used to skip the CAS are branch conditions now: a
        // registered sender must always deregister, whatever the answer about the bit.
        [[nodiscard]] bool leave_and_maybe_schedule(const char* context, bool unblocked_us) noexcept {
            auto current = state_.load(std::memory_order_acquire);
            int cas_attempts = 0;

            while (true) {
                exponential_backoff(cas_attempts);
                ++cas_attempts;

#ifndef NDEBUG
                assert(cas_attempts < kMaxCasAttempts && context);
#else
                detail::ignore_unused(context);
                if (cas_attempts >= kMaxCasAttempts) {
                    std::terminate();
                }
#endif
                assert(sender_count(current) > 0 && "leave_and_maybe_schedule: not registered!");

                auto desired = sub_sender(current);
                bool owes_enqueue = false;

                // Do not schedule a dying actor, and do not claim a bit somebody already
                // holds. A RUNNING actor discharges its own obligation instead, through
                // scheduled_while_running -- which is why the answer is read from the
                // pre-CAS value.
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

        void begin_shutdown() noexcept {
            auto current = state_.load(std::memory_order_acquire);
            int cas_attempts = 0;

            while (!is_destroying(current)) {
                exponential_backoff(cas_attempts);
                ++cas_attempts;
#ifndef NDEBUG
                assert(cas_attempts < kMaxCasAttempts && "begin_shutdown: CAS livelock!");
#else
                if (cas_attempts >= kMaxCasAttempts) {
                    std::terminate();
                }
#endif
                auto desired = set_destroying(current);
                if (state_.compare_exchange_weak(current, desired,
                                                 std::memory_order_release,
                                                 std::memory_order_acquire)) {
                    break;
                }
            }

            wait_for_activity_to_drain();
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

        // Waits out everything that can still touch this actor's members: the thread
        // holding `running`, and any sender already past enqueue_impl's gate. Callers
        // must have published `destroying` first, or a sender can slip in behind us.
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

        shutdown_guard_t shutdown_guard_;

        std::pmr::memory_resource* resource_;
        mailbox::message* current_message_;
        MailBox mailbox_;
        behavior_t current_behavior_;  // ONE coroutine per actor for behavior()
        std::atomic<actor_state> state_{actor_state::idle};

#ifndef NDEBUG
        static constexpr uint32_t kMagicAlive = 0xFEEDFACE;
        uint32_t magic_;
#endif
    };

}} // namespace actor_zeta::actor
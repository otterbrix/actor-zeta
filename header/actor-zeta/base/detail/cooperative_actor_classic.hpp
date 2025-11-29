#pragma once

#include "traits_actor.hpp"
#include <actor-zeta/base/actor_mixin.hpp>
#include <actor-zeta/base/forwards.hpp>
#include <actor-zeta/detail/memory.hpp>
#include <actor-zeta/detail/type_traits.hpp>
#include <actor-zeta/detail/future_state.hpp>
#include <actor-zeta/future.hpp>
#include <actor-zeta/make_message.hpp>
#include <actor-zeta/scheduler/resumable.hpp>
#include <actor-zeta/detail/ignore_unused.hpp>
#include <actor-zeta/detail/queue/enqueue_result.hpp>

#include <actor-zeta/mailbox/mailbox.hpp>
#include <actor-zeta/mailbox/default_mailbox.hpp>

#include <thread>
#include <chrono>

namespace actor_zeta { namespace base {

    enum class actor_state : uint8_t {
        idle                         = 0b000,
        scheduled                    = 0b001,
        running                      = 0b010,
        running_scheduled            = 0b011,
        idle_destroying              = 0b100,
        scheduled_destroying         = 0b101,
        running_destroying           = 0b110,
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
        if (scheduled) bits |= 0b001;
        if (running) bits |= 0b010;
        if (destroying) bits |= 0b100;
        return static_cast<actor_state>(bits);
    }

    template<class Target>
    Target* check_ptr(Target* ptr) {
        assert(ptr);
        return ptr;
    }

    /// @brief Helper function for exponential backoff in CAS loops and wait loops
    /// @param attempt Current attempt number (0-based)
    /// Implements progressive backoff strategy:
    /// - Attempts 1-4: spin (no yield/sleep) - for low contention
    /// - Attempts 5-10: yield CPU - for medium contention
    /// - Attempts 11+: exponential sleep (1us -> 2us -> 4us -> ... -> 1ms cap) - for high contention
    inline void exponential_backoff(int attempt) noexcept {
        if (attempt >= 4) {
            if (attempt < 10) {
                std::this_thread::yield();
            } else {
                auto sleep_us = std::min(1 << (attempt - 10), 1000);  // Cap at 1ms
                std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
            }
        }
    }

    template<class Actor, class MailBox>
    class cooperative_actor<Actor, MailBox, actor_type::classic>
        : public actor_mixin<Actor> {
    private:
        static constexpr bool check_dispatch_traits_exists() {
            using dispatch_traits_check = typename Actor::dispatch_traits;
            (void)sizeof(dispatch_traits_check);
            return true;
        }

    public:
        using typename actor_mixin<Actor>::id_t;
        using typename actor_mixin<Actor>::placement_tag;
        using actor_mixin<Actor>::placement;

        using mailbox_t = MailBox;
        using unique_actor = std::unique_ptr<cooperative_actor<Actor, MailBox, actor_type::classic>, pmr::deleter_t>;

        template<typename T>
        using promise = actor_zeta::promise<T>;

        template<typename T>
        using unique_future = actor_zeta::unique_future<T>;

        template<typename R, typename... Args>
        [[nodiscard("Check needs_scheduling() and call schedule() if needed")]]
        unique_future<R> enqueue_impl(
            base::address_t sender,
            mailbox::message_id cmd,
            Args&&... args
        ) {
            // Create message in RECEIVER'S memory (this->resource())
            // This avoids cross-arena migration issues with RTT
            auto msg = detail::make_message(
                this->resource(),           // Receiver's memory resource
                std::move(sender),
                cmd,
                std::forward<Args>(args)...
            );

            // Use PMR make_counted - cleaner, exception-safe allocation
            auto state = pmr::make_counted<detail::future_state<R>>(resource());  // refcount = 1

            // set_result_slot accepts intrusive_ptr by const ref, calls add_ref() internally
            msg->set_result_slot(state);  // refcount = 2 (local + message)

            // ============================================================================
            // REFCOUNT OWNERSHIP MODEL
            // ============================================================================
            //
            // 1. make_counted creates intrusive_ptr with refcount = 1 (local 'state')
            // 2. set_result_slot(state) copies intrusive_ptr -> add_ref() -> refcount = 2
            //    Owners: local 'state' (+1), message slot_ (+1)
            // 3. adopt_ref + detach() transfers ownership without add_ref():
            //    - state.detach() releases local ownership WITHOUT decrementing refcount
            //    - adopt_ref tells unique_future to adopt the +1 reference (don't add_ref)
            //    - refcount stays at 2: message (+1), future (+1 adopted)
            // 4. When actor processes message:
            //    - Message destroyed -> slot_->release() -> refcount: 2 -> 1
            //    - Future still owns +1 reference
            // 5. When caller consumes future:
            //    - future.get() / ~unique_future() -> slot_->release() -> refcount: 1 -> 0
            //    - future_state deleted
            //
            // CRITICAL: adopt_ref prevents race where actor destroys message before
            // future constructor finishes. Without adopt_ref, we'd need add_ref() which
            // could race with message destruction -> use-after-free!
            // ============================================================================

            // NOTE: This is_destroying() check is racy by design (TOCTOU - Time-Of-Check-Time-Of-Use).
            // If actor starts destroying AFTER this check but BEFORE push_back(), the message will
            // be enqueued but not processed. This is acceptable:
            // - resume() has a second is_destroying() check that prevents behavior() execution
            // - Minimal waste (one message allocation + enqueue)
            // - Early check here prevents unnecessary work in the common case
            if (is_destroying(state_.load(std::memory_order_acquire))) {
                state->set_state(detail::future_state_enum::error);
                return unique_future<R>(adopt_ref, state.detach(), false);
            }

            auto result = mailbox().push_back(std::move(msg));

            // DO NOT release here! Actor may process message immediately
            // and destroy state before we construct the future below

            bool needs_sched = false;

            switch (result) {
                case detail::enqueue_result::unblocked_reader: {
                    auto current = state_.load(std::memory_order_acquire);
                    actor_state desired;

                    int cas_attempts = 0;
                    constexpr int MAX_CAS_ATTEMPTS = 1000;

                    while (true) {
                        exponential_backoff(cas_attempts);
                        ++cas_attempts;

#ifndef NDEBUG
                        // Debug: assert on livelock
                        assert(cas_attempts < MAX_CAS_ATTEMPTS && "enqueue_impl: CAS loop - possible livelock/race condition!");
#else
                        // Release: detect livelock and abort to prevent infinite loop
                        if (cas_attempts >= MAX_CAS_ATTEMPTS) {
                            // Livelock detected - should never happen in correct code
                            // Safer to abort than hang forever
                            std::terminate();
                        }
#endif

                        // ISSUE #2 FIX: Transient destroying states (scheduled_destroying, running_scheduled_destroying)
                        // can appear when actor is being destroyed while scheduled/running.
                        // These are valid transient states - just exit without scheduling.
                        if (is_running(current) || is_destroying(current)) {
                            needs_sched = false;
                            break;
                        }

                        if (is_scheduled(current)) {
                            needs_sched = false;
                            break;
                        }

                        // Valid states for scheduling: idle or idle_destroying (rare but possible)
                        // NOTE: idle_destroying is allowed here - it's a transient state during shutdown.
                        // If destructor sets destroying bit AFTER we checked but BEFORE CAS:
                        //   - We set scheduled bit -> actor becomes scheduled_destroying
                        //   - Scheduler will call resume(), which checks is_destroying() and returns done
                        //   - Messages won't be processed (check at line 478 prevents behavior() call)
                        //   - Minimal waste (one unnecessary schedule call), but correct behavior
                        assert((current == actor_state::idle || current == actor_state::idle_destroying) &&
                               "enqueue_impl: expected idle or idle_destroying state for scheduling!");

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
                    state->set_state(detail::future_state_enum::error);
                    needs_sched = false;
                    break;

                default:
                    assert(false && "enqueue_result: unreachable");
                    needs_sched = false;
                    break;
            }

            // Use adopt_ref to transfer ownership without add_ref() race
            // Actor may have already processed message and released its ref!
            // Refcount stays at 2: message +1 (may be destroyed), future adopts local ref +1
            return unique_future<R>(adopt_ref, state.detach(), needs_sched);
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
                    constexpr int MAX_CAS_ATTEMPTS = 1000;

                    while (true) {
                        exponential_backoff(cas_attempts);
                        ++cas_attempts;

#ifndef NDEBUG
                        // Debug: assert on livelock
                        assert(cas_attempts < MAX_CAS_ATTEMPTS && "resume_guard constructor: CAS loop - possible livelock!");
#else
                        // Release: detect livelock and abort to prevent infinite loop
                        if (cas_attempts >= MAX_CAS_ATTEMPTS) {
                            // Livelock detected - should never happen in correct code
                            // Safer to abort than hang forever
                            std::terminate();
                        }
#endif

                        // ISSUE #3 FIX: Transient destroying states can appear when:
                        // - scheduled_destroying: actor was scheduled when destructor started
                        // - running_scheduled_destroying: should be impossible (caught by is_running check)
                        // These are valid transient states during shutdown.

                        if (is_running(current)) {
#ifndef NDEBUG
                            assert(false && "Concurrent resume() detected - scheduler BUG!");
#else
                            // CRITICAL: Concurrent resume() means scheduler bug!
                            // Cannot safely continue - behavior() will be called concurrently!
                            // Safer to abort with diagnostics than corrupt user data.
                            std::terminate();
#endif
                        }

#ifndef NDEBUG
                        // Valid entry states: idle, scheduled, or any *_destroying variant (except running variants)
                        assert((current == actor_state::idle ||
                                current == actor_state::scheduled ||
                                current == actor_state::idle_destroying ||
                                current == actor_state::scheduled_destroying) &&
                               "resume_guard: unexpected state at entry!");
#endif

                        desired = set_running(current, true);

#ifndef NDEBUG
                        // Valid states after setting running bit:
                        // - running (from idle)
                        // - running_scheduled (from scheduled)
                        // - running_destroying (from idle_destroying)
                        // - running_scheduled_destroying (from scheduled_destroying)
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
                    constexpr int MAX_CAS_ATTEMPTS = 1000;

                    while (true) {
                        exponential_backoff(cas_attempts);
                        ++cas_attempts;

#ifndef NDEBUG
                        // Debug: assert on livelock
                        assert(cas_attempts < MAX_CAS_ATTEMPTS && "resume_guard destructor: CAS loop - possible livelock!");
#else
                        // Release: detect livelock and abort to prevent infinite loop
                        if (cas_attempts >= MAX_CAS_ATTEMPTS) {
                            // Livelock detected - should never happen in correct code
                            // Safer to abort than hang forever
                            std::terminate();
                        }
#endif

                        assert(is_running(current) && "resume_guard: not running!");

                        desired = set_running(current, false);

                        // ============================================================================
                        // KEEP_SCHEDULED MECHANISM (Race Window Handling)
                        // ============================================================================
                        //
                        // If keep_scheduled_ = true, we leave the scheduled bit set when leaving resume().
                        // This happens when:
                        // 1. mailbox.empty() returns true
                        // 2. mailbox.try_block() fails (message arrived during race window)
                        // 3. check_race_window() confirms message exists
                        //
                        // WHY KEEP SCHEDULED BIT:
                        // - Prevents actor from going to idle state when message is pending
                        // - Scheduler will call resume() again immediately
                        // - Avoids unnecessary state transition: running -> idle -> scheduled -> running
                        // - Performance optimization: saves CAS operations and scheduler overhead
                        //
                        // STATE TRANSITIONS:
                        // - Without keep_scheduled: running_scheduled -> idle (loses scheduled bit)
                        // - With keep_scheduled:    running_scheduled -> scheduled (keeps bit)
                        //
                        // CORRECTNESS: Safe because check_race_window() already verified message exists
                        // ============================================================================
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

            // ============================================================================
            // RACE WINDOW DETECTION (Best-Effort, Non-Atomic)
            // ============================================================================
            //
            // This lambda checks if mailbox has pending messages after try_block() failed.
            // If try_block() fails (returns false), it means a message arrived AFTER empty()
            // check but BEFORE try_block() call - this is the "race window".
            //
            // WHY NON-ATOMIC CHECK IS OK:
            // - This is a best-effort optimization to reduce latency
            // - False negatives are acceptable:
            //   * If we miss a message here, it will be processed on next resume()
            //   * Worst case: actor goes to awaiting state, sender re-schedules it
            //   * No correctness issue, just slightly higher latency
            // - False positives are impossible with current mailbox implementation:
            //   * blocked() and empty() are both atomic operations
            //   * If check returns true, message definitely exists
            //
            // ALTERNATIVE (rejected): Add atomic mailbox::has_pending() method
            // - Would require additional atomic operations in hot path
            // - Negligible benefit - current approach works correctly
            // ============================================================================
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
                // Cooperative cancellation: check is_destroying() on each loop iteration
                // This enables fast shutdown without waiting for max_throughput to be reached
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
                            // ============================================================================
                            // REFCOUNT OWNERSHIP - NO MANIPULATION NEEDED
                            // ============================================================================
                            //
                            // message_guard does NOT touch refcount at all! Here's why it works:
                            //
                            // 1. enqueue_impl creates future_state with refcount = 2:
                            //    - Message slot_ holds +1 reference (via set_result_slot)
                            //    - Future adopted +1 reference (via adopt_ref + detach)
                            // 2. When ~message() runs (here in ~message_guard):
                            //    - slot_->release() decrements refcount: 2 -> 1
                            //    - Future still owns +1 reference
                            // 3. When caller consumes future:
                            //    - future.get() / ~unique_future() -> release() -> refcount: 1 -> 0
                            //    - future_state deleted
                            //
                            // CRITICAL: adopt_ref in enqueue_impl prevents race!
                            // Without adopt_ref, we'd need: msg->result_slot()->add_ref() HERE
                            // But that would race with message destruction -> use-after-free!
                            // ============================================================================
                            actor_->current_message_ = prev_message_;
                        }

                        mailbox::message* get() const noexcept { return message_.get(); }
                    };

                    message_guard guard(this, std::move(msg));

                    if (!is_destroying(state_.load(std::memory_order_acquire))) {
                        auto behavior_future = self()->behavior(guard.get());

                        // The behavior_future (unique_future<void>) is useful for:
                        // 1. Coroutine management - tracking suspended coroutines
                        // 2. Complex async algorithms inside actors
                        //
                        // TODO: Add coroutine tracking here if needed
                        if (behavior_future.get_state() != nullptr) {
                            // Future state is kept alive by behavior_future until it goes out of scope
                        }
                    }

                    ++handled;
                }

                // ============================================================================
                // NO-PROGRESS DETECTION (Mailbox Empty During Processing)
                // ============================================================================
                //
                // If handled == before, it means pop_front() returned nullptr (no message).
                // This can happen in these scenarios:
                //
                // 1. Mailbox became empty during processing:
                //    - We checked !empty() earlier, but actor processed all messages since then
                //    - This is normal during high-throughput bursts
                //
                // 2. Race window during blocked state:
                //    - Mailbox was temporarily blocked (awaiting external event)
                //    - Message arrived AFTER empty() check but BEFORE try_block()
                //
                // HANDLING:
                // - Try to block mailbox (suspend actor until next message arrives)
                // - If try_block() fails (race window detected), check_race_window() verifies
                // - If message exists, keep_scheduled ensures actor resumes immediately
                // - Otherwise, actor goes to awaiting state (scheduler removes from queue)
                //
                // CORRECTNESS: This is the ONLY way to handle mailbox emptying during processing
                // without busy-waiting or missing messages.
                // ============================================================================
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

        pmr::memory_resource* resource() const noexcept {
#ifndef NDEBUG
            assert(magic_ == kMagicAlive && "Use-after-free: resource() called on destroyed actor!");
#endif
            return resource_;
        }

        cooperative_actor()= delete;

        ~cooperative_actor() {
#ifndef NDEBUG
            assert(magic_ == kMagicAlive && "Double-delete: destructor called on already destroyed actor!");
#endif
            // ════════════════════════════════════════════════════════════════════════════
            // NOTE: shutdown_guard_ destructor was called BEFORE we entered here!
            // ════════════════════════════════════════════════════════════════════════════
            //
            // C++ destruction order guarantees:
            //   1. ~shutdown_guard_t() executed    ← Called begin_shutdown(), set destroying bit
            //   2. NOW WE ARE HERE                 ← Entering ~cooperative_actor()
            //   3. ~derived_members will execute   ← After this destructor completes
            //
            // Therefore:
            //   - destroying bit is ALREADY set (by shutdown_guard)
            //   - resume() is ALREADY complete (waited by shutdown_guard)
            //   - wait_for_resume_to_complete() below will return instantly (~1μs)
            // ════════════════════════════════════════════════════════════════════════════
            auto current = state_.load(std::memory_order_acquire);

            // NOTE: Assert removed - shutdown_guard_t handles begin_shutdown() automatically
            // No need to verify here, as fallback code below handles both cases:
            // 1. Normal case: shutdown_guard already set destroying bit (loop skipped)
            // 2. Edge case: If not set (shouldn't happen), set it now

            // Set destroying bit if not already set (idempotent)
            while (!is_destroying(current)) {
                auto desired = set_destroying(current);
                if (state_.compare_exchange_weak(current, desired,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
                    break;
                }
            }

            // Wait for resume() to complete (instant if shutdown_guard already waited)
            // ISSUE #6 FIX: Use extracted method instead of duplicated code
            wait_for_resume_to_complete("Destructor");

            current = state_.load(std::memory_order_acquire);
            while (true) {
                assert(!is_running(current) && "Destructor: still running after wait!");
                auto desired = make_state(false, false, true);
                if (state_.compare_exchange_weak(current, desired,
                                                 std::memory_order_release,
                                                 std::memory_order_acquire)) {
                    break;
                }
            }

            // NOTE: Do NOT set magic_ = kMagicDead here!
            // Reason: TSAN data race - other threads may still be reading magic_ in resource()
            // The destroying bit in state_ already prevents new operations.
            // If someone tries to use the actor after destruction, the magic check will fail
            // when the memory is reused or freed.
        }

    protected:
        explicit cooperative_actor(pmr::memory_resource* in_resource)
            : actor_mixin<Actor>()
            , shutdown_guard_(this)       // Initialize FIRST (destroyed LAST!)
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
        // ════════════════════════════════════════════════════════════════════════════
        // begin_shutdown() - PRIVATE (called only by shutdown_guard_t destructor)
        // ════════════════════════════════════════════════════════════════════════════
        //
        // NOTE: This method is now PRIVATE instead of protected.
        //
        // REASON: With shutdown_guard_t providing automatic safety, there is NO need
        // for derived classes to call begin_shutdown() manually.
        //
        // BEFORE (without shutdown_guard):
        //   - begin_shutdown() was protected
        //   - Derived classes MUST call it in destructor to prevent race condition
        //   - Easy to forget → footgun!
        //
        // NOW (with shutdown_guard):
        //   - begin_shutdown() is private
        //   - shutdown_guard_t calls it automatically before destructor
        //   - Derived classes CANNOT call it (unnecessary and potentially harmful)
        //
        // BENEFIT: Eliminates possibility of incorrect manual shutdown logic.
        // shutdown_guard_t is the ONLY authorized caller.
        // ════════════════════════════════════════════════════════════════════════════
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

            // ISSUE #6 FIX: Use extracted method instead of duplicated code
            wait_for_resume_to_complete("begin_shutdown");
        }
        // ════════════════════════════════════════════════════════════════════════════
        // SHUTDOWN GUARD - Automatic race condition protection
        // ════════════════════════════════════════════════════════════════════════════
        //
        // PURPOSE: Ensure resume() completes BEFORE derived class members are destroyed.
        //
        // PROBLEM WITHOUT GUARD:
        //   Thread 1 (Destructor): ~cooperative_actor() (base) → ~derived_members (behavior_t)
        //   Thread 2 (Worker):     resume() → behavior() → reads behavior_t members ← RACE!
        //
        // SOLUTION:
        //   shutdown_guard_t destructor is called BEFORE base class destructor
        //   (C++ destruction order: members destroyed before base classes).
        //   Guard calls begin_shutdown() which waits for resume() to complete.
        //
        // DESTRUCTION ORDER:
        //   1. ~shutdown_guard_t()          ← Calls begin_shutdown(), waits for resume()
        //   2. ~cooperative_actor() (base)  ← wait_for_resume() instant (already done)
        //   3. ~DerivedActor members        ← SAFE! resume() guaranteed not running
        //
        // OVERHEAD: sizeof(void*) = 8 bytes per actor (stores 'this' pointer only)
        //
        // PATTERN CONSISTENCY:
        //   - resume_guard: Local RAII for running bit (acquire/release pattern)
        //   - message_guard: Local RAII for current_message (save/restore pattern)
        //   - shutdown_guard_t: Member RAII for destruction order (early cleanup pattern)
        //
        // NOTE: This makes begin_shutdown() call in derived destructors OPTIONAL.
        //       Users get automatic safety, but can still call begin_shutdown()
        //       explicitly for custom cleanup logic between shutdown and destruction.
        // ════════════════════════════════════════════════════════════════════════════
        struct shutdown_guard_t {
            cooperative_actor* self_;

            explicit shutdown_guard_t(cooperative_actor* self) noexcept
                : self_(self) {}

            ~shutdown_guard_t() noexcept {
                // CRITICAL: Called BEFORE base class destructor!
                // Ensures resume() completes before any derived members are destroyed.
                //
                // With cooperative cancellation (COOPERATIVE_CANCELLATION.md):
                //   - begin_shutdown() returns in ~1-10ms (fast!)
                //   - Derived destructor can then safely destroy behavior_t members
                //
                // Without cooperative cancellation (current code):
                //   - May wait up to 30 seconds (if resume() processing many messages)
                //   - But still prevents race condition (correctness > performance)
                self_->begin_shutdown();
            }

            // Non-copyable, non-movable (guard must stay with owner)
            shutdown_guard_t(const shutdown_guard_t&) = delete;
            shutdown_guard_t& operator=(const shutdown_guard_t&) = delete;
            shutdown_guard_t(shutdown_guard_t&&) = delete;
            shutdown_guard_t& operator=(shutdown_guard_t&&) = delete;
        };

        /// @brief Wait for resume() to complete (used by destructor and begin_shutdown)
        /// @param context Error message context (e.g., "Destructor", "begin_shutdown()")
        /// ISSUE #6 FIX: Extracted common wait-for-resume logic to eliminate duplication
        void wait_for_resume_to_complete(const char* context) noexcept {
            std::atomic_thread_fence(std::memory_order_seq_cst);

            // ISSUE #5 FIX: Use timer instead of counter (more reliable across different CPUs)
            // Now enabled in BOTH debug and release builds for safety
            auto start_time = std::chrono::steady_clock::now();

#ifndef NDEBUG
            constexpr auto timeout = std::chrono::seconds(5);  // Shorter timeout for debug
#else
            constexpr auto timeout = std::chrono::seconds(30);  // Longer timeout for release
#endif

            int wait_attempts = 0;

            // P1 OPTIMIZATION: Use acquire instead of seq_cst (sufficient for read-only check)
            while (is_running(state_.load(std::memory_order_acquire))) {
                exponential_backoff(wait_attempts);
                ++wait_attempts;

                auto elapsed = std::chrono::steady_clock::now() - start_time;
                if (elapsed > timeout) {
#ifndef NDEBUG
                    // Debug: assert with message
                    (void)context;  // Suppress unused warning
                    assert(false && "wait_for_resume_to_complete: timeout - possible deadlock!");
#else
                    // Release: terminate to prevent infinite hang
                    // Deadlock detected - safer to abort than hang forever
                    // Context is lost (no exceptions), but at least we don't hang
                    (void)context;
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

        mailbox_t& mailbox() noexcept {
            return mailbox_;
        }

        // ════════════════════════════════════════════════════════════════════════════
        // MEMBER VARIABLES - Destruction order matters!
        // ════════════════════════════════════════════════════════════════════════════
        //
        // C++ destruction order: Members destroyed in REVERSE declaration order.
        // Declaration order:  shutdown_guard_, resource_, current_message_, mailbox_, state_
        // Destruction order:  state_, mailbox_, current_message_, resource_, shutdown_guard_
        //
        // CRITICAL: shutdown_guard_ declared FIRST → destroyed LAST → before base destructor!
        // This ensures begin_shutdown() is called before ~cooperative_actor() runs.
        // ════════════════════════════════════════════════════════════════════════════
        shutdown_guard_t shutdown_guard_;  // FIRST member → destroyed LAST! (initialized in constructor)

        pmr::memory_resource* resource_;
        mailbox::message* current_message_;
        mailbox_t mailbox_;
        std::atomic<actor_state> state_{actor_state::idle};

#ifndef NDEBUG
        // Magic values for use-after-free detection (same pattern as future_state)
        static constexpr uint32_t kMagicAlive = 0xFEEDFACE;

        // NOTE: kMagicDead is intentionally NOT used in destructor to avoid TSAN data race.
        // Other threads may still be reading magic_ in resource() when destructor runs.
        // The destroying bit in state_ already prevents new operations, making kMagicDead
        // redundant. Reserved for potential future use (e.g., delayed cleanup patterns).
        static constexpr uint32_t kMagicDead = 0xDEADC0DE;

        uint32_t magic_;
#endif
    };

}}
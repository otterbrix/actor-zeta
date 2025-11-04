#pragma once

#include "traits_actor.hpp"
#include <actor-zeta/base/actor_mixin.hpp>
#include <actor-zeta/base/behavior.hpp>
#include <actor-zeta/base/forwards.hpp>
#include <actor-zeta/detail/memory.hpp>
#include <actor-zeta/detail/type_traits.hpp>
#include <actor-zeta/detail/future_state.hpp>
#include <actor-zeta/future.hpp>
#include <actor-zeta/scheduler/resumable.hpp>
#include <actor-zeta/detail/ignore_unused.hpp>
#include <actor-zeta/detail/queue/enqueue_result.hpp>

#include <actor-zeta/mailbox/mailbox.hpp>
#include <actor-zeta/mailbox/default_mailbox.hpp>

#include <thread>
#include <chrono>

namespace actor_zeta { namespace base {

    /// @brief Actor execution state - combines scheduled/running/destroying flags
    /// Uses bit flags for efficient atomic operations
    /// Bit 0: scheduled - actor is in scheduler queue
    /// Bit 1: running - resume() is currently executing
    /// Bit 2: destroying - actor is being destroyed
    enum class actor_state : uint8_t {
        idle                         = 0b000,  // Not scheduled, not running
        scheduled                    = 0b001,  // In scheduler queue
        running                      = 0b010,  // Executing resume()
        running_scheduled            = 0b011,  // Running + needs reschedule
        idle_destroying              = 0b100,  // Being destroyed
        scheduled_destroying         = 0b101,  // INVALID: can't schedule destroying actor
        running_destroying           = 0b110,  // Destroying while running
        running_scheduled_destroying = 0b111   // INVALID: can't reschedule destroying actor
    };

    /// @brief Check if actor is scheduled (in queue)
    constexpr bool is_scheduled(actor_state s) noexcept {
        return (static_cast<uint8_t>(s) & 0b001) != 0;
    }

    /// @brief Check if actor is running (executing resume())
    constexpr bool is_running(actor_state s) noexcept {
        return (static_cast<uint8_t>(s) & 0b010) != 0;
    }

    /// @brief Check if actor is being destroyed
    constexpr bool is_destroying(actor_state s) noexcept {
        return (static_cast<uint8_t>(s) & 0b100) != 0;
    }

    /// @brief Set/clear scheduled bit
    constexpr actor_state set_scheduled(actor_state s, bool value) noexcept {
        auto bits = static_cast<uint8_t>(s);
        if (value) {
            bits |= 0b001;
        } else {
            bits &= ~0b001;
        }
        return static_cast<actor_state>(bits);
    }

    /// @brief Set/clear running bit
    constexpr actor_state set_running(actor_state s, bool value) noexcept {
        auto bits = static_cast<uint8_t>(s);
        if (value) {
            bits |= 0b010;
        } else {
            bits &= ~0b010;
        }
        return static_cast<actor_state>(bits);
    }

    /// @brief Set destroying bit (always sets, never clears)
    constexpr actor_state set_destroying(actor_state s) noexcept {
        return static_cast<actor_state>(static_cast<uint8_t>(s) | 0b100);
    }

    /// @brief Construct state from individual flags
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

    template<class Actor, class MailBox>
    class cooperative_actor<Actor, MailBox, actor_type::classic>
        : public actor_mixin<Actor> {
    private:
        static constexpr bool check_dispatch_traits_exists() {
            using dispatch_traits_check = typename Actor::dispatch_traits;
            (void)sizeof(dispatch_traits_check); // Suppress unused warning
            return true;
        }

    public:
        // Import types from actor_mixin
        using typename actor_mixin<Actor>::id_t;
        using typename actor_mixin<Actor>::placement_tag;
        using actor_mixin<Actor>::placement;

        using mailbox_t = MailBox;
        using unique_actor = std::unique_ptr<cooperative_actor<Actor, MailBox, actor_type::classic>, pmr::deleter_t>;

        /// @brief Use actor_zeta::promise and actor_zeta::unique_future (defined in future.hpp)
        template<typename T>
        using promise = actor_zeta::promise<T>;

        template<typename T>
        using unique_future = actor_zeta::unique_future<T>;

        /// @brief Enqueue message and return future
        template<typename R>
        unique_future<R> enqueue_impl(mailbox::message_ptr msg) {
            assert(msg.get() != nullptr);

            // Allocate future_state<R>
            void* mem = resource()->allocate(sizeof(detail::future_state<R>), alignof(detail::future_state<R>));
            auto* state = new (mem) detail::future_state<R>(resource());

            // Share state with message (holds reference)
            msg->set_result_slot(state);

            // Check if actor is being destroyed - reject new enqueue()
            // This prevents race where enqueue() happens after destructor wait loop
            // but before behavior_t members are destroyed
            if (is_destroying(state_.load(std::memory_order_acquire))) {
                state->set_state(detail::future_state_enum::error);
                unique_future<R> future(state, false);  // needs_sched = false
                return future;
            }

            // Enqueue message to mailbox
            auto result = mailbox().push_back(std::move(msg));

            // Create future with correct needs_scheduling flag
            bool needs_sched = false;

            switch (result) {
                case detail::enqueue_result::unblocked_reader: {
                    // Actor was blocked and got unblocked - MAY need scheduling
                    // Atomic transition: idle → scheduled (or running → running, no scheduling needed)
                    // Use CAS to ensure only ONE thread successfully schedules
                    auto current = state_.load(std::memory_order_acquire);
                    actor_state desired;

                    while (true) {
                        // If already running, don't schedule (will self-reschedule if needed)
                        // If destroying, don't schedule
                        if (is_running(current) || is_destroying(current)) {
                            needs_sched = false;
                            break;
                        }

                        // If already scheduled, another thread won the race
                        if (is_scheduled(current)) {
                            needs_sched = false;
                            break;
                        }

                        // Try transition: idle → scheduled
                        desired = set_scheduled(current, true);
                        if (state_.compare_exchange_weak(current, desired,
                                                         std::memory_order_acq_rel,
                                                         std::memory_order_acquire)) {
                            // CAS succeeded - we are the one to schedule
                            needs_sched = true;
                            break;
                        }
                        // CAS failed - current updated, retry
                    }
                    break;
                }

                case detail::enqueue_result::success:
                    // Message enqueued, actor already has messages - no scheduling needed
                    needs_sched = false;
                    break;

                case detail::enqueue_result::queue_closed:
                    // Queue closed - mark state as error
                    state->set_state(detail::future_state_enum::error);
                    needs_sched = false;
                    break;

                default:
                    assert(false && "enqueue_result: unreachable");
                    needs_sched = false;
                    break;
            }

            // Create future with state reference
            unique_future<R> future(state, needs_sched);
            return future;
        }

        /// @brief Resume execution - process messages from mailbox
        /// @param max_throughput Maximum number of messages to process
        /// @return Resume information with status and messages processed
        ///
        /// Flow scenarios:
        /// A) Scheduled after enqueue: inbox is unblocked, has messages
        /// B) Batch: many enqueue() calls, then single schedule - inbox unblocked with multiple messages
        /// C) Synchronous: direct resume() call on fresh actor - inbox may be blocked
        scheduler::resume_info resume(size_t max_throughput) noexcept {
            assert(max_throughput > 0 && "max_throughput must be greater than 0");

            // CONCURRENT RESUME DETECTION: RAII guard to detect concurrent resume()
            // Also handles state transitions to prevent race conditions
            struct resume_guard {
                std::atomic<actor_state>& state_ref_;
                bool keep_scheduled_;

                explicit resume_guard(std::atomic<actor_state>& state)
                    : state_ref_(state)
                    , keep_scheduled_(false) {
                    // Atomic transition: scheduled → running_scheduled (or idle → running)
                    auto current = state_ref_.load(std::memory_order_acquire);
                    actor_state desired;

#ifndef NDEBUG
                    int cas_attempts = 0;
                    constexpr int MAX_CAS_ATTEMPTS = 100000;  // Failsafe: detect livelock
#endif

                    while (true) {
#ifndef NDEBUG
                        assert(++cas_attempts < MAX_CAS_ATTEMPTS && "resume_guard constructor: CAS loop - possible livelock!");
#endif
                        // Detect concurrent resume() - running bit already set
                        if (is_running(current)) {
                            assert(false && "Concurrent resume() detected - scheduler BUG!");
                        }

                        // Set running bit, preserve scheduled and destroying bits
                        desired = set_running(current, true);

                        if (state_ref_.compare_exchange_weak(current, desired,
                                                             std::memory_order_seq_cst,
                                                             std::memory_order_acquire)) {
                            // CAS succeeded - we are now running
                            break;
                        }
                        // CAS failed - current updated, retry
                    }
                }

                ~resume_guard() {
                    // Atomic transition: clear running bit, optionally clear scheduled bit
                    // Preserves destroying bit
                    auto current = state_ref_.load(std::memory_order_acquire);
                    actor_state desired;

#ifndef NDEBUG
                    int cas_attempts = 0;
                    constexpr int MAX_CAS_ATTEMPTS = 100000;  // Failsafe: detect livelock
#endif

                    while (true) {
#ifndef NDEBUG
                        assert(++cas_attempts < MAX_CAS_ATTEMPTS && "resume_guard destructor: CAS loop - possible livelock!");
#endif
                        assert(is_running(current) && "resume_guard: not running!");

                        // Clear running bit
                        desired = set_running(current, false);

                        // Clear scheduled bit if not keeping it
                        if (!keep_scheduled_) {
                            desired = set_scheduled(desired, false);
                        }

                        if (state_ref_.compare_exchange_weak(current, desired,
                                                             std::memory_order_seq_cst,
                                                             std::memory_order_acquire)) {
                            // CAS succeeded - state updated
                            break;
                        }
                        // CAS failed - current updated, retry
                    }
                }

                void keep_scheduled() {
                    keep_scheduled_ = true;
                }
            };
            resume_guard guard(state_);

            // Helper lambda: finalize resume with race window check
            auto finalize = [&guard](scheduler::resume_result result, size_t handled, bool keep_scheduled) -> scheduler::resume_info {
                // If keep_scheduled=true, we detected race window with new messages
                // Tell guard to keep scheduled bit set for scheduler re-enqueue
                if (keep_scheduled) {
                    guard.keep_scheduled();
                }
                // Otherwise, guard destructor will clear both running and scheduled bits

                return scheduler::resume_info(result, handled);
            };

            // Helper to check for race window: new messages arrived after we decided to await
            auto check_race_window = [this]() -> bool {
                // Check if new messages arrived - IMPORTANT: Check !blocked() first to avoid assertion
                return !mailbox().blocked() && !mailbox().empty();
            };

            size_t handled = 0;

            // Check if actor is being destroyed - exit immediately without processing
            // This prevents calling behavior() on destroyed behavior_t members
            if (is_destroying(state_.load(std::memory_order_acquire))) {
                return finalize(scheduler::resume_result::done, 0, false);
            }

            // Check if inbox is closed first (shutdown scenario)
            if (mailbox().closed()) {
                return finalize(scheduler::resume_result::done, 0, false);
            }

            // Check if blocked - can happen in:
            // - Synchronous resume() on fresh actor (Flow C)
            // - Spurious scheduler wakeup (rare)
            // For scheduled actors (Flow A, B), inbox is already unblocked by enqueue()
            if (mailbox().blocked()) {
                return finalize(scheduler::resume_result::awaiting, 0, false);
            }

            if (mailbox().empty()) {
                auto result = mailbox().try_block()
                                  ? scheduler::resume_result::awaiting
                                  : scheduler::resume_result::resume;
                // Race window check: if try_block succeeded (awaiting), check if new messages arrived
                bool keep_scheduled = (result == scheduler::resume_result::awaiting && check_race_window());
                if (keep_scheduled) {
                    result = scheduler::resume_result::resume;  // Tell scheduler to re-enqueue
                }
                return finalize(result, 0, keep_scheduled);
            }

            while (handled < max_throughput) {
                // Check if inbox closed during processing
                if (mailbox().closed()) {
                    return finalize(scheduler::resume_result::done, handled, false);
                }

                // CRITICAL: Check if blocked before pop_front() to avoid assertion
                // Can happen in multithreaded scenario with concurrent enqueue()
                if (mailbox().blocked()) {
                    return finalize(scheduler::resume_result::awaiting, handled, false);
                }

                const size_t before = handled;

                auto msg = mailbox().pop_front();
                if (msg) {
                    // RAII guard for message processing
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
                            // CRITICAL: Release result_slot FIRST while actor is still alive
                            // If we reset current_message_ first, actor might be destroyed before release()
                            if (message_ && message_->result_slot()) {
                                message_->result_slot()->release();
                            }

                            // THEN reset current_message_ (safe even if actor being destroyed)
                            actor_->current_message_ = prev_message_;
                        }

                        mailbox::message* get() const noexcept { return message_.get(); }
                    };

                    message_guard guard(this, std::move(msg));

                    // CRITICAL: Check destroying flag IMMEDIATELY before calling behavior()
                    // This prevents bad_function_call when:
                    // - Main thread enters ~cooperative_actor(), sets destroying=true
                    // - Scheduler thread is here in resume() with running=true
                    // - Destructor waits for running=false
                    // - We check destroying and skip behavior() call
                    // - behavior_t members are still alive (destructor waiting)
                    // Similar to future_state magic_ check pattern
                    if (!is_destroying(state_.load(std::memory_order_acquire))) {
                        // Safe to call behavior() - actor not being destroyed
                        self()->behavior(guard.get());
                    }
                    // else: skip behavior() call - actor is shutting down

                    ++handled;
                }

                if (handled == before) {
                    // Check again before try_block
                    if (mailbox().closed()) {
                        return finalize(scheduler::resume_result::done, handled, false);
                    }
                    auto result = mailbox().try_block()
                                      ? scheduler::resume_result::awaiting
                                      : scheduler::resume_result::resume;
                    // Race window check: if try_block succeeded (awaiting), check if new messages arrived
                    bool keep_scheduled = (result == scheduler::resume_result::awaiting && check_race_window());
                    if (keep_scheduled) {
                        result = scheduler::resume_result::resume;  // Tell scheduler to re-enqueue
                    }
                    return finalize(result, handled, keep_scheduled);
                }
            }

            // Check before final try_block
            if (mailbox().closed()) {
                return finalize(scheduler::resume_result::done, handled, false);
            }

            auto result = mailbox().try_block()
                              ? scheduler::resume_result::awaiting
                              : scheduler::resume_result::resume;
            // Race window check: if try_block succeeded (awaiting), check if new messages arrived
            bool keep_scheduled = (result == scheduler::resume_result::awaiting && check_race_window());
            if (keep_scheduled) {
                result = scheduler::resume_result::resume;  // Tell scheduler to re-enqueue
            }
            return finalize(result, handled, keep_scheduled);
        }

        /// @brief Get PMR memory resource used by this actor
        pmr::memory_resource* resource() const noexcept {
            return resource_;
        }

        cooperative_actor()= delete;

        ~cooperative_actor() {
            // SHUTDOWN PROTOCOL: Ensure safe destruction while scheduler may be running
            //
            // CRITICAL THREE-PHASE SHUTDOWN:
            // Phase 1: Set destroying bit to reject new enqueue() calls (via begin_shutdown())
            // Phase 2: Wait for in-progress resume() to complete
            // Phase 3: Close mailbox and cleanup
            //
            // This prevents race where enqueue() happens after wait but before destruction
            //
            // Step 0: Set destroying bit if not already set
            // IMPORTANT: Derived classes with behavior_t members MUST call begin_shutdown()
            // at the beginning of their destructor to avoid bad_function_call
            auto current = state_.load(std::memory_order_acquire);
            bool already_shutdown = is_destroying(current);

            // Set destroying bit atomically
            while (!is_destroying(current)) {
                auto desired = set_destroying(current);
                if (state_.compare_exchange_weak(current, desired,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
                    break;
                }
            }

            (void)already_shutdown;  // Suppress unused warning in release
#ifndef NDEBUG
            // NOTE: If this assertion fires, add begin_shutdown() call to derived destructor
            // This is only needed if derived class has behavior_t members
            // Example:
            // ~MyActor() override { begin_shutdown(); /* rest of destructor */ }
            // assert(already_shutdown && "IMPORTANT: Derived class with behavior_t members must call begin_shutdown() in destructor!");
#endif

            // Fence to ensure destroying write is visible before we proceed
            std::atomic_thread_fence(std::memory_order_seq_cst);

            // Step 1: Wait for any in-progress resume() to complete
            // The running bit is set by resume_guard and cleared when resume() exits
            // This ensures message_guard destructor completes before we deallocate actor
            //
            // SAFETY: This prevents heap-use-after-free when:
            // - Scheduler thread is in resume() processing messages
            // - Main thread calls actor.reset()
            // - message_guard destructor would access freed actor memory
            //
            // IMPORTANT: We must wait BEFORE destroying members (behavior_t, etc.)
            // because resume() may still be accessing them!
            //
            // PERFORMANCE: Typically completes immediately (resume() is fast)
            // Worst case: waits for current message batch (max_throughput messages)
#ifndef NDEBUG
            // Debug: detect if we're stuck waiting (indicates bug in resume())
            int spin_count = 0;
            constexpr int MAX_SPINS = 10000000; // ~10 seconds on modern CPU
#endif
            // CRITICAL: Must use seq_cst to ensure TSan sees the synchronization
            // acquire alone is not sufficient for TSan to detect happens-before relationship
            while (is_running(state_.load(std::memory_order_seq_cst))) {
                std::this_thread::yield();
#ifndef NDEBUG
                if (++spin_count > MAX_SPINS) {
                    assert(false && "Destructor waiting too long for resume() - possible deadlock!");
                }
#endif
            }

            // Extra fence to ensure all previous operations are visible
            std::atomic_thread_fence(std::memory_order_seq_cst);

            // Step 2: Transition to idle_destroying (clear scheduled bit if set)
            current = state_.load(std::memory_order_acquire);
            while (true) {
                assert(!is_running(current) && "Destructor: still running after wait!");
                auto desired = make_state(false, false, true);  // idle_destroying
                if (state_.compare_exchange_weak(current, desired,
                                                 std::memory_order_release,
                                                 std::memory_order_acquire)) {
                    break;
                }
            }

            // Step 3: Mailbox will be closed automatically by ~mailbox_t destructor
            // This is safe because:
            // - We've waited for resume() to complete
            // - destroying bit prevents new resume() calls from processing messages
            // - No race with drain() because resume() has exited
            //
            // Note: ~mailbox_t runs AFTER this destructor body completes

            // Now safe to destroy - guaranteed:
            // 1. No in-progress resume() (waited for running=false)
            // 2. No new message processing (destroying checked in resume())
            // 3. No access to 'this' pointer from scheduler threads
        }

    protected:
        explicit cooperative_actor(pmr::memory_resource* in_resource)
            : actor_mixin<Actor>()
            , resource_(check_ptr(in_resource))
            , current_message_(nullptr)
            , mailbox_() {
            // Проверка наличия dispatch_traits (Actor уже полностью определен здесь)
            static_assert(check_dispatch_traits_exists(),
                "Actor must define nested 'struct dispatch_traits { using methods = type_list<...>; }'");
            mailbox().try_block();
        }

        /// @brief Call this at the BEGINNING of derived class destructor
        /// This prevents bad_function_call when behavior_t members are destroyed
        /// while resume() is still processing messages
        ///
        /// CRITICAL: This method:
        /// 1. Sets destroying bit immediately (prevents new behavior() calls)
        /// 2. Waits for in-progress resume() to complete
        /// 3. Returns when it's safe to destroy behavior_t members
        void begin_shutdown() noexcept {
            // Step 1: Set destroying bit immediately - prevents new resume() from calling behavior()
            auto current = state_.load(std::memory_order_acquire);
            while (!is_destroying(current)) {
                auto desired = set_destroying(current);
                if (state_.compare_exchange_weak(current, desired,
                                                 std::memory_order_release,
                                                 std::memory_order_acquire)) {
                    break;
                }
            }
            std::atomic_thread_fence(std::memory_order_seq_cst);

            // Step 2: Wait for any in-progress resume() to complete
            // This ensures behavior() completes before derived class destroys behavior_t members
            //
            // CRITICAL: Without this wait, we have a race:
            // - Thread 1 (main): begin_shutdown() returns → destroy behavior_t
            // - Thread 2 (worker): still in behavior() → reads behavior_t → RACE!
#ifndef NDEBUG
            int spin_count = 0;
            constexpr int MAX_SPINS = 10000000;
#endif
            while (is_running(state_.load(std::memory_order_seq_cst))) {
                std::this_thread::yield();
#ifndef NDEBUG
                if (++spin_count > MAX_SPINS) {
                    assert(false && "begin_shutdown() waiting too long for resume() - possible deadlock!");
                }
#endif
            }

            // Extra fence to ensure all previous operations are visible
            std::atomic_thread_fence(std::memory_order_seq_cst);
        }

    private:
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

        pmr::memory_resource* resource_;
        mailbox::message* current_message_;
        mailbox_t mailbox_;
        std::atomic<actor_state> state_{actor_state::idle};
    };

}} // namespace actor_zeta::base

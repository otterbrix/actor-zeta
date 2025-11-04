#pragma once

#include "traits_actor.hpp"
#include <actor-zeta/base/actor_abstract.hpp>
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

    template<class Target>
    Target* check_ptr(Target* ptr) {
        assert(ptr);
        return ptr;
    }

    template<class Actor, class MailBox>
    class cooperative_actor<Actor, MailBox, actor_type::classic>
        : public actor_abstract_t {
    private:
        static constexpr bool check_dispatch_traits_exists() {
            using dispatch_traits_check = typename Actor::dispatch_traits;
            (void)sizeof(dispatch_traits_check); // Suppress unused warning
            return true;
        }

    public:
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

            // Allocate future_state<R> (replaces slot_refcount)
            void* mem = resource()->allocate(sizeof(detail::future_state<R>), alignof(detail::future_state<R>));
            auto* state = new (mem) detail::future_state<R>(resource());

            // Create cancellation token (will be shared between message and future)
            auto token = make_counted<detail::cancellation_token>();

            // Share cancellation token with message
            msg->set_cancellation_token(token);
            msg->set_result_slot(state);

            // Check if actor is being destroyed - reject new enqueue()
            // This prevents race where enqueue() happens after destructor wait loop
            // but before behavior_t members are destroyed
            if (destroying_.load(std::memory_order_acquire)) {
                state->set_state(detail::future_state_enum::error);
                unique_future<R> future(state, false);  // needs_sched = false
                future.set_cancellation_token(std::move(token));
                return future;
            }

            // Enqueue message to mailbox
            auto result = mailbox().push_back(std::move(msg));

            // Create future with correct needs_scheduling flag
            bool needs_sched = false;

            switch (result) {
                case detail::enqueue_result::unblocked_reader: {
                    // Actor was blocked and got unblocked - MAY need scheduling
                    // Use CAS to ensure only ONE thread sets is_scheduled_ flag
                    // This prevents concurrent resume() calls from multiple scheduler threads
                    bool expected = false;
                    if (is_scheduled_.compare_exchange_strong(expected, true,
                                                             std::memory_order_acq_rel,
                                                             std::memory_order_acquire)) {
                        // CAS succeeded - but check if resume() is still running
                        // CRITICAL RACE WINDOW FIX:
                        // Thread A: ~resume_guard() → resuming_=false → is_scheduled_=false
                        // Thread B: CAS is_scheduled_ succeeds HERE (between the two stores)
                        // Thread B: Would schedule duplicate resume()!
                        //
                        // Solution: Check if resume() still active AFTER CAS
                        if (resuming_.load(std::memory_order_acquire)) {
                            // resume() still running - rollback CAS and don't schedule
                            is_scheduled_.store(false, std::memory_order_release);
                            needs_sched = false;
                        } else {
                            // Safe to schedule - resume() has fully exited
                            needs_sched = true;
                        }
                    } else {
                        // Another thread already scheduled this actor - skip scheduling
                        // This can happen when:
                        // - Multiple threads send messages concurrently
                        // - Actor processed messages and blocked again between our enqueue attempts
                        needs_sched = false;
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

            // Create future and set the shared cancellation token
            unique_future<R> future(state, needs_sched);
            // Replace the token created in constructor with our shared token
            future.set_cancellation_token(std::move(token));

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
            // Also handles is_scheduled_ cleanup to prevent race condition
            struct resume_guard {
                std::atomic<bool>& resuming_flag_;
                std::atomic<bool>& scheduled_flag_;
                bool clear_scheduled_;

                explicit resume_guard(std::atomic<bool>& resuming, std::atomic<bool>& scheduled)
                    : resuming_flag_(resuming)
                    , scheduled_flag_(scheduled)
                    , clear_scheduled_(true) {
                    bool expected = false;
                    // Use seq_cst for TSan visibility
                    if (!resuming_flag_.compare_exchange_strong(expected, true, std::memory_order_seq_cst)) {
                        assert(false && "Concurrent resume() detected - scheduler BUG!");
                    }
                }

                ~resume_guard() {
                    // CRITICAL ORDER: Clear resuming_ FIRST, then is_scheduled_
                    // This ensures no concurrent resume() can start until we've fully exited
                    //
                    // Thread A (resume exit):           Thread B (enqueue):
                    // ~resume_guard() starts
                    //   resuming_ = false [1]
                    //                                    enqueue() → CAS is_scheduled_
                    //                                    resume() called
                    //                                    resuming_.CAS fails ✓ (already false)
                    //   is_scheduled_ = false [2]
                    // ~resume_guard() ends
                    //
                    // Use seq_cst to ensure destructor wait sees this change
                    resuming_flag_.store(false, std::memory_order_seq_cst);

                    if (clear_scheduled_) {
                        scheduled_flag_.store(false, std::memory_order_release);
                    }
                }

                void keep_scheduled() {
                    clear_scheduled_ = false;
                }
            };
            resume_guard guard(resuming_, is_scheduled_);

            // Helper lambda: finalize resume with race window check
            auto finalize = [&guard](scheduler::resume_result result, size_t handled, bool keep_scheduled) -> scheduler::resume_info {
                // If keep_scheduled=true, we detected race window with new messages
                // Tell guard to keep is_scheduled_=true for scheduler re-enqueue
                if (keep_scheduled) {
                    guard.keep_scheduled();
                }
                // Otherwise, guard destructor will clear is_scheduled_=false atomically with resuming_

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
            if (destroying_.load(std::memory_order_acquire)) {
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
                            actor_->current_message_ = prev_message_;

                            // Release actor's refcount if message has result slot
                            if (message_ && message_->result_slot()) {
                                message_->result_slot()->release();
                            }
                        }

                        mailbox::message* get() const noexcept { return message_.get(); }
                    };

                    message_guard guard(this, std::move(msg));

                    // CRITICAL: Check destroying_ flag IMMEDIATELY before calling behavior()
                    // This prevents bad_function_call when:
                    // - Main thread enters ~cooperative_actor(), sets destroying_=true
                    // - Scheduler thread is here in resume() with resuming_=true
                    // - Destructor waits for resuming_=false
                    // - We check destroying_ and skip behavior() call
                    // - behavior_t members are still alive (destructor waiting)
                    // Similar to future_state magic_ check pattern
                    if (!destroying_.load(std::memory_order_acquire)) {
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

        ~cooperative_actor() override {
            // SHUTDOWN PROTOCOL: Ensure safe destruction while scheduler may be running
            //
            // CRITICAL THREE-PHASE SHUTDOWN:
            // Phase 1: Set destroying_ flag to reject new enqueue() calls (via begin_shutdown())
            // Phase 2: Wait for in-progress resume() to complete
            // Phase 3: Close mailbox and cleanup
            //
            // This prevents race where enqueue() happens after wait but before destruction
            //
            // Step 0: Set destroying_ flag if not already set
            // IMPORTANT: Derived classes with behavior_t members MUST call begin_shutdown()
            // at the beginning of their destructor to avoid bad_function_call
            bool already_shutdown = destroying_.exchange(true, std::memory_order_acq_rel);
            (void)already_shutdown;  // Suppress unused warning in release
#ifndef NDEBUG
            // NOTE: If this assertion fires, add begin_shutdown() call to derived destructor
            // This is only needed if derived class has behavior_t members
            // Example:
            // ~MyActor() override { begin_shutdown(); /* rest of destructor */ }
            // assert(already_shutdown && "IMPORTANT: Derived class with behavior_t members must call begin_shutdown() in destructor!");
#endif

            // Fence to ensure destroying_ write is visible before we proceed
            std::atomic_thread_fence(std::memory_order_seq_cst);

            // Step 1: Wait for any in-progress resume() to complete
            // The resuming_ flag is set by resume_guard and cleared when resume() exits
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
            while (resuming_.load(std::memory_order_seq_cst)) {
                std::this_thread::yield();
#ifndef NDEBUG
                if (++spin_count > MAX_SPINS) {
                    assert(false && "Destructor waiting too long for resume() - possible deadlock!");
                }
#endif
            }

            // Extra fence to ensure all previous operations are visible
            std::atomic_thread_fence(std::memory_order_seq_cst);

            // Step 2: Clear is_scheduled_ flag (cleanup, not strictly necessary)
            is_scheduled_.store(false, std::memory_order_release);

            // Step 3: Mailbox will be closed automatically by ~mailbox_t destructor
            // This is safe because:
            // - We've waited for resume() to complete
            // - destroying_ flag prevents new resume() calls from processing messages
            // - No race with drain() because resume() has exited
            //
            // Note: ~mailbox_t runs AFTER this destructor body completes

            // Now safe to destroy - guaranteed:
            // 1. No in-progress resume() (waited for resuming_=false)
            // 2. No new message processing (destroying_ checked in resume())
            // 3. No access to 'this' pointer from scheduler threads
        }

    protected:
        explicit cooperative_actor(pmr::memory_resource* in_resource)
            : actor_abstract_t(check_ptr(in_resource))
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
        void begin_shutdown() noexcept {
            // Set destroying_ flag immediately - prevents new resume() from calling behavior()
            destroying_.store(true, std::memory_order_release);
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

        mailbox::message* current_message_;
        mailbox_t mailbox_;
        std::atomic<bool> resuming_{false};  // Concurrent resume() detection
        std::atomic<bool> is_scheduled_{false};  // Actor in scheduler queue?
        std::atomic<bool> destroying_{false};  // Actor is being destroyed - reject new enqueue()
    };

}} // namespace actor_zeta::base

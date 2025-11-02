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
                        // Success - we're the first to schedule this actor
                        needs_sched = true;
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
            struct resume_guard {
                std::atomic<bool>& flag;
                explicit resume_guard(std::atomic<bool>& f) : flag(f) {
                    bool expected = false;
                    if (!flag.compare_exchange_strong(expected, true, std::memory_order_acquire)) {
                        assert(false && "Concurrent resume() detected - scheduler BUG!");
                    }
                }
                ~resume_guard() {
                    flag.store(false, std::memory_order_release);
                }
            };
            resume_guard guard(resuming_);

            // Helper lambda: finalize resume with race window check
            auto finalize = [this](scheduler::resume_result result, size_t handled, bool keep_scheduled) -> scheduler::resume_info {
                // If keep_scheduled=true, we detected race window with new messages
                // Keep is_scheduled_=true so concurrent enqueue_impl() will fail CAS
                if (!keep_scheduled) {
                    // Clear is_scheduled_ flag - we're done processing
                    is_scheduled_.store(false, std::memory_order_release);
                }

                return scheduler::resume_info(result, handled);
            };

            // Helper to check for race window: new messages arrived after we decided to await
            auto check_race_window = [this]() -> bool {
                // Check if new messages arrived - IMPORTANT: Check !blocked() first to avoid assertion
                return !mailbox().blocked() && !mailbox().empty();
            };

            size_t handled = 0;

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

                    // Check for cancelled messages - skip processing if cancelled
                    // TEMPORARY DEBUG: Always process to see if cancellation is the issue
                    // if (!guard.get()->is_cancelled()) {
                        // Normal processing (includes orphaned messages - still process them)
                        self()->behavior(guard.get());
                    // }

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

        ~cooperative_actor() override = default;

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
    };

}} // namespace actor_zeta::base

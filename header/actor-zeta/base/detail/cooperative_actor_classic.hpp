#pragma once

#include "traits_actor.hpp"
#include <actor-zeta/base/actor_abstract.hpp>
#include <actor-zeta/base/behavior.hpp>
#include <actor-zeta/base/forwards.hpp>
#include <actor-zeta/detail/memory.hpp>
#include <actor-zeta/detail/type_traits.hpp>
#include <actor-zeta/scheduler/resumable.hpp>
#include <actor-zeta/detail/ignore_unused.hpp>

#include <actor-zeta/mailbox/mailbox.hpp>
#include <actor-zeta/mailbox/default_mailbox.hpp>

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
        // Ленивая проверка dispatch_traits - инстанцируется только при вызове
        static constexpr bool check_dispatch_traits_exists() {
            using dispatch_traits_check = typename Actor::dispatch_traits;
            (void)sizeof(dispatch_traits_check); // Suppress unused warning
            return true;
        }

    public:
        using mailbox_t = MailBox;
        using unique_actor = std::unique_ptr<cooperative_actor<Actor, MailBox, actor_type::classic>, pmr::deleter_t>;

        /// @brief Resume execution - process messages from mailbox
        /// @param max_throughput Maximum number of messages to process
        /// @return Resume information with status and messages processed
        ///
        /// Flow scenarios:
        /// A) Scheduled after enqueue: inbox is unblocked, has messages
        /// B) Batch: many enqueue() calls, then single schedule - inbox unblocked with multiple messages
        /// C) Synchronous: direct resume() call on fresh actor - inbox may be blocked
        scheduler::resume_info resume(size_t max_throughput) noexcept {
            size_t handled = 0;

            // Check if inbox is closed first (shutdown scenario)
            if (mailbox().closed()) {
                return scheduler::resume_info(scheduler::resume_result::done, 0);
            }

            // Check if blocked - can happen in:
            // - Synchronous resume() on fresh actor (Flow C)
            // - Spurious scheduler wakeup (rare)
            // For scheduled actors (Flow A, B), inbox is already unblocked by enqueue()
            if (mailbox().blocked()) {
                return scheduler::resume_info(scheduler::resume_result::awaiting, 0);
            }

            if (mailbox().empty()) {
                auto result = mailbox().try_block()
                                  ? scheduler::resume_result::awaiting
                                  : scheduler::resume_result::resume;
                return scheduler::resume_info(result, 0);
            }

            while (handled < max_throughput) {
                // Check if inbox closed during processing
                if (mailbox().closed()) {
                    return scheduler::resume_info(scheduler::resume_result::done, handled);
                }

                const size_t before = handled;

                auto msg = mailbox().pop_front();
                if (msg) {
                    current_msg_guard guard(this, msg.get());
                    self()->behavior(current_message_);
                    ++handled;
                }

                if (handled == before) {
                    // Check again before try_block
                    if (mailbox().closed()) {
                        return scheduler::resume_info(scheduler::resume_result::done, handled);
                    }
                    auto result = mailbox().try_block()
                                      ? scheduler::resume_result::awaiting
                                      : scheduler::resume_result::resume;
                    return scheduler::resume_info(result, handled);
                }
            }

            // Check before final try_block
            if (mailbox().closed()) {
                return scheduler::resume_info(scheduler::resume_result::done, handled);
            }
            auto result = mailbox().try_block()
                              ? scheduler::resume_result::awaiting
                              : scheduler::resume_result::resume;
            return scheduler::resume_info(result, handled);
        }

        ~cooperative_actor() override {

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

    private:

        bool enqueue_impl(mailbox::message_ptr msg) final {
            assert(msg.get() != nullptr);
            switch (mailbox().push_back(std::move(msg))) {
                case detail::enqueue_result::unblocked_reader: {
                    // Actor was blocked, now unblocked - needs to be scheduled!
                    return true;
                }
                case detail::enqueue_result::success: {
                    // Actor is already active (executing or in queue) - don't schedule again!
                    return false;
                }
                case detail::enqueue_result::queue_closed: {
                    return false;
                }
                default: {
                    assert(false && "enqueue_result: unreachable");
                    return false;
                }
            }
        }

    private:
        class current_msg_guard final {
        public:
            current_msg_guard(cooperative_actor* s, mailbox::message* m) noexcept
                : self(s)
                , prev(s->current_message_) { self->current_message_ = m; }
            ~current_msg_guard() noexcept { self->current_message_ = nullptr; }

        private:
            cooperative_actor* self;
            mailbox::message* prev;
            current_msg_guard(const current_msg_guard&);
            current_msg_guard& operator=(const current_msg_guard&);
        };

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
    };

}} // namespace actor_zeta::base

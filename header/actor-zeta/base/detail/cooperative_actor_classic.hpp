#pragma once

#include "traits_actor.hpp"
#include <actor-zeta/base/actor_abstract.hpp>
#include <actor-zeta/base/behavior.hpp>
#include <actor-zeta/base/forwards.hpp>
#include <actor-zeta/detail/memory.hpp>
#include <actor-zeta/detail/type_traits.hpp>
#include <actor-zeta/scheduler/resumable.hpp>
#include <actor-zeta/detail/ignore_unused.hpp>

namespace actor_zeta { namespace base {

    template<class Target>
    Target* check_ptr(Target* ptr) {
        assert(ptr);
        return ptr;
    }

    template<class Actor, class Traits>
    class cooperative_actor<Actor, Traits, actor_type::classic>
         : public actor_abstract_t
         , public scheduler::resumable_t {
    public:
        using unique_actor = std::unique_ptr<cooperative_actor<Actor, Traits, actor_type::classic>, pmr::deleter_t>;

        scheduler::resume_info resume(actor_zeta::scheduler::scheduler_abstract_t* sched, size_t max_throughput) noexcept final {
            detail::ignore_unused(sched);
            return resume_core_(max_throughput);
        }

        scheduler::resume_info resume(size_t max_throughput) noexcept {
            return resume_core_(max_throughput);
        }

        void intrusive_ptr_add_ref_impl() final {
            ref();
        }

        void intrusive_ptr_release_impl() final {
            deref();
        }

    protected:
        cooperative_actor(pmr::memory_resource* in_resource)
            : actor_abstract_t(check_ptr(in_resource))
            , inbox_(mailbox::priority_message(),
                     high_priority_queue(mailbox::high_priority_message()),
                     normal_priority_queue(mailbox::normal_priority_message())) {
            inbox().try_block(); //todo: bug
        }

        bool enqueue_impl(mailbox::message_ptr msg) final {
            assert(msg.get() != nullptr);
            switch (inbox().push_back(std::move(msg))) {
                case detail::enqueue_result::unblocked_reader: {
                    return true;
                }
                case detail::enqueue_result::success: {
                    return true;
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
                : self(s), prev(s->current_message_) { self->current_message_ = m; }
            ~current_msg_guard() noexcept { self->current_message_ = nullptr; }
        private:
            cooperative_actor* self;
            mailbox::message*  prev;
            current_msg_guard(const current_msg_guard&);
            current_msg_guard& operator=(const current_msg_guard&);
        };

        scheduler::resume_info resume_core_(size_t max_throughput) noexcept {
            const size_t nq = 3u;
            const size_t hq = nq * 3u;
            size_t handled = 0;

            // Check if inbox is closed first (shutdown scenario)
            if (inbox().closed()) {
                return scheduler::resume_info(scheduler::resume_result::done, 0);
            }

            // Check if inbox is blocked to avoid assertion in empty()
            if (inbox().blocked()) {
                // Inbox is blocked, try to resume (another thread may have enqueued)
                return scheduler::resume_info(scheduler::resume_result::resume, 0);
            }

            if (inbox().empty()) {
                auto result = inbox().try_block()
                              ? scheduler::resume_result::awaiting
                              : scheduler::resume_result::resume;
                return scheduler::resume_info(result, 0);
            }

            auto handler = [this, &handled, max_throughput](mailbox::message& m) noexcept -> detail::task_result {
                current_msg_guard guard(this, &m);
                self()->behavior(current_message_);
                ++handled;
                return (handled < max_throughput)
                       ? detail::task_result::resume
                       : detail::task_result::stop_all;
            };

            while (handled < max_throughput) {
                // Check if inbox closed during processing
                if (inbox().closed()) {
                    return scheduler::resume_info(scheduler::resume_result::done, handled);
                }

                inbox().fetch_more();
                const size_t before = handled;

                high(inbox()).new_round(hq, handler);
                normal(inbox()).new_round(nq, handler);

                if (handled == before) {
                    // Check again before try_block
                    if (inbox().closed()) {
                        return scheduler::resume_info(scheduler::resume_result::done, handled);
                    }
                    auto result = inbox().try_block()
                                  ? scheduler::resume_result::awaiting
                                  : scheduler::resume_result::resume;
                    return scheduler::resume_info(result, handled);
                }
            }

            // Check before final try_block
            if (inbox().closed()) {
                return scheduler::resume_info(scheduler::resume_result::done, handled);
            }
            auto result = inbox().try_block()
                          ? scheduler::resume_result::awaiting
                          : scheduler::resume_result::resume;
            return scheduler::resume_info(result, handled);
        }


        mailbox::message* current_message() noexcept { return current_message_; }

        inline const Actor* self() const noexcept {
            return static_cast<const Actor*>(this);
        }

        inline Actor* self() noexcept {
            return static_cast<Actor*>(this);
        }

        inline traits::inbox_t& inbox() {
            return inbox_;
        }

        mailbox::message* current_message_;
        typename Traits::inbox_t inbox_;
    };

}} // namespace actor_zeta::base

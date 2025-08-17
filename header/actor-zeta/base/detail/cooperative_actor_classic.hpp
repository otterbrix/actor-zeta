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

        scheduler::resume_result resume(actor_zeta::scheduler::scheduler_t*scheduler,size_t max_throughput) final {
            detail::ignore_unused(scheduler);
            static constexpr size_t quantum = 3;
            size_t handled_msgs = 0;
            mailbox::message_ptr ptr;

            auto handle_async = [this, max_throughput, &handled_msgs](mailbox::message& x) -> detail::task_result {
                reactivate(x);
                return ++handled_msgs < max_throughput
                           ? detail::task_result::resume
                           : detail::task_result::stop_all;
            };

            while (handled_msgs < max_throughput) {
                if (!inbox().empty()) {
                    inbox().fetch_more();
                    auto prev_handled_msgs = handled_msgs;
                    high(inbox()).new_round(quantum * 3, handle_async);
                    normal(inbox()).new_round(quantum, handle_async);
                    if (handled_msgs == prev_handled_msgs && inbox().try_block()) {
                        return scheduler::resume_result::awaiting;
                    }
                }
            }
            if (inbox().try_block()) {
                return scheduler::resume_result::awaiting;
            }
            return scheduler::resume_result::resume;
        }

        scheduler::resume_result resume(size_t max_throughput) {
            static constexpr size_t quantum = 3;
            size_t handled_msgs = 0;
            mailbox::message_ptr ptr;

            auto handle_async = [this, max_throughput, &handled_msgs](mailbox::message& x) -> detail::task_result {
                reactivate(x);
                return ++handled_msgs < max_throughput
                           ? detail::task_result::resume
                           : detail::task_result::stop_all;
            };

            while (handled_msgs < max_throughput) {
                if (!inbox().empty()) {
                    inbox().fetch_more();
                    auto prev_handled_msgs = handled_msgs;
                    high(inbox()).new_round(quantum * 3, handle_async);
                    normal(inbox()).new_round(quantum, handle_async);
                    if (handled_msgs == prev_handled_msgs && inbox().try_block()) {
                        return scheduler::resume_result::awaiting;
                    }
                }
            }
            if (inbox().try_block()) {
                return scheduler::resume_result::awaiting;
            }
            return scheduler::resume_result::resume;
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
            assert(msg);
            switch (inbox().push_back(std::move(msg))) {
                case detail::enqueue_result::unblocked_reader: {
                    intrusive_ptr_add_ref(this);
                    return true;
                }
                case detail::enqueue_result::success: {
                    return true;
                }
                case detail::enqueue_result::queue_closed: {
                    return false;
                }
                default: {
                    return false;
                }
            }
        }

        auto current_message() -> mailbox::message* {
            return current_message_;
        }

        auto set_current_message(mailbox::message_ptr msg) -> void {
            current_message_ = msg.release();
        }

    private:
        auto self() noexcept -> Actor* {
            return static_cast<Actor*>(this);
        }

        auto self() const noexcept -> Actor* {
            return static_cast<Actor*>(this);
        }

        inline traits::inbox_t& inbox() {
            return inbox_;
        }

        auto reactivate(mailbox::message& x) -> void {
            current_message_ = &x;
            self()->behavior(current_message_);
        }

        mailbox::message* current_message_;
        typename Traits::inbox_t inbox_;
    };

}} // namespace actor_zeta::base

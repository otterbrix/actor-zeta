#pragma once

#include <actor-zeta/base/forwards.hpp>
#include <actor-zeta/base/handler.hpp>
#include <actor-zeta/detail/callable_trait.hpp>
#include <actor-zeta/detail/memory_resource.hpp>
#include <actor-zeta/mailbox/id.hpp>
#include <actor-zeta/mailbox/message.hpp>

namespace actor_zeta { namespace base {

    class behavior_t final {
    public:
        behavior_t() = delete;
        behavior_t(const behavior_t&) = delete;
        behavior_t& operator=(const behavior_t&) = delete;

        behavior_t(behavior_t&&) = default;
        behavior_t& operator=(behavior_t&&) = default;

        behavior_t(actor_zeta::pmr::memory_resource*, action handler):handler_ (std::move(handler)) {}

        explicit operator bool() {
            return bool(handler_);
        }

        void operator()(mailbox::message* msg) {
            handler_(msg);
        }

    private:
        action handler_;
    };

    template<class Value>
    behavior_t make_behavior(actor_zeta::pmr::memory_resource* resource, Value&& f) {
        return {resource, make_handler(resource,std::forward<Value>(f))};
    }


    template<class Actor, typename F>
    behavior_t make_behavior(actor_zeta::pmr::memory_resource* resource, Actor* ptr, F&& f) {
        return {resource, make_handler(resource,ptr, std::forward<F>(f))};
    }


}} // namespace actor_zeta::base
#pragma once

#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <actor-zeta.hpp>
#include <actor-zeta/detail/type_traits.hpp>

#include "enums.hpp"
#include "counters.h"

template<class... Args>
struct counter_args_t {
    static constexpr size_t value = sizeof...(Args);
};

template<class Sup>
auto set_done(Sup* sup_ptr_) -> void {
#ifdef BM_TRACE
    std::cout << std::this_thread::get_id() << " :: Going to sem done ... .. . " << reinterpret_cast<void*>(sup_ptr_) << std::endl;
#endif
    sup_ptr_->done();
}

class actor_t final : public actor_zeta::basic_actor<actor_t> {
    actor_zeta::behavior_t add_address_;
    actor_zeta::behavior_t start_;
    actor_zeta::behavior_t ping_;
    actor_zeta::behavior_t pong_;

    std::map<name_t, actor_zeta::address_t> address_book_;

    template<size_t... I>
    auto perform(command_t cmd, actor_zeta::type_traits::index_sequence<I...>) -> void {
        actor_zeta::send(address_book_.begin()->second, address(), cmd, I...);
    }

public:
    template<class Supervisor, class... Args>
    actor_t(Supervisor* ptr, Args...)
        : actor_zeta::basic_actor<actor_t>(ptr)
        , add_address_(
            actor_zeta::make_behavior(
                resource(),
                command_t::add_address, [this](actor_zeta::address_t address, name_t name) -> void {
                    if (address && this != address.get()) {
                        address_book_.emplace(name, std::move(address));
                    }
                }))
        , start_(
            actor_zeta::make_behavior(
                resource(),
                command_t::start, [this]() -> void {
                    perform(command_t::ping, actor_zeta::type_traits::make_index_sequence<counter_args_t<Args...>::value>());
                }))
        , ping_(
            actor_zeta::make_behavior(
                resource(),
                command_t::ping, [this](Args... ) -> void {
                    counter_g++;
#ifdef BM_TRACE
                    std::cout << std::this_thread::get_id() << " :: PING " << reinterpret_cast<void*>(this) << std::endl;
#endif
                    perform(command_t::pong, actor_zeta::type_traits::make_index_sequence<counter_args_t<Args...>::value>());
                }))
        , pong_(
            actor_zeta::make_behavior(
                resource(),
                command_t::pong, [ptr](Args... ) -> void {
                    counter_g++;
#ifdef BM_TRACE
                    std::cout << std::this_thread::get_id() << " :: PONG " << reinterpret_cast<void*>(this) << std::endl;
#endif
                    set_done(ptr);
                }))
    {}

    const char* make_type() const noexcept {
        return "actor";
    }

    actor_zeta::behavior_t behavior() {
        return actor_zeta::make_behavior(
            resource(),
            [this](actor_zeta::message* msg) -> void {
                switch (msg->command()) {
                    case actor_zeta::make_message_id(command_t::add_address): {
                        add_address_(msg);
                        break;
                    }
                    case actor_zeta::make_message_id(command_t::start): {
                        start_(msg);
                        break;
                    }
                    case actor_zeta::make_message_id(command_t::ping): {
                        ping_(msg);
                        break;
                    }
                    case actor_zeta::make_message_id(command_t::pong): {
                        pong_(msg);
                        break;
                    }
                }
            });
    }
};

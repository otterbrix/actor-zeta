#pragma once

#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <actor-zeta.hpp>
#include <actor-zeta/detail/type_traits.hpp>

template<class... Args>
struct counter_args_t {
    static constexpr size_t value = sizeof...(Args);
};

#define DEFINE_ACTOR(actor_name, supervisor_name, ...)                                                                    \
    class actor_name final : public actor_zeta::basic_async_actor {                                                       \
        std::map<name_t, actor_zeta::address_t> address_book_;                                                            \
        template<size_t... I>                                                                                             \
        auto perform(command_t cmd, actor_zeta::type_traits::index_sequence<I...>) -> void {                              \
            actor_zeta::send(address_book_.begin()->second, address(), cmd, I...);                                        \
        }                                                                                                                 \
                                                                                                                          \
    public:                                                                                                               \
        actor_name(supervisor_name* ptr)                                                                                  \
            : actor_zeta::basic_async_actor(ptr, names::actor) {                                                          \
            add_handler(command_t::add_address, &actor_name::add_address);                                                \
            add_handler(command_t::start, &actor_name::start);                                                            \
            add_handler(command_t::ping, &actor_name::ping);                                                              \
            add_handler(command_t::pong, &actor_name::pong);                                                              \
        }                                                                                                                 \
                                                                                                                          \
        ~actor_name() override = default;                                                                                 \
                                                                                                                          \
        void add_address(actor_zeta::address_t address, name_t name) {                                                    \
            if (address && this != address.get()) {                                                                       \
                address_book_.emplace(name, std::move(address));                                                          \
            }                                                                                                             \
        }                                                                                                                 \
                                                                                                                          \
        void start() {                                                                                                    \
            perform(command_t::ping, actor_zeta::type_traits::make_index_sequence<counter_args_t<__VA_ARGS__>::value>()); \
        }                                                                                                                 \
                                                                                                                          \
        void ping(__VA_ARGS__) {                                                                                          \
            /*std::cout << " PING " << reinterpret_cast<void*>(this) << std::endl;*/                                      \
            perform(command_t::pong, actor_zeta::type_traits::make_index_sequence<counter_args_t<__VA_ARGS__>::value>()); \
        }                                                                                                                 \
                                                                                                                          \
        void pong(__VA_ARGS__) {                                                                                          \
            /*std::cout << " PONG " << reinterpret_cast<void*>(this) << std::endl;*/                                      \
        }                                                                                                                 \
                                                                                                                          \
    private:                                                                                                              \
        auto enqueue_impl(actor_zeta::message_ptr msg, actor_zeta::execution_unit*) -> void final {                       \
            {                                                                                                             \
                auto ptr = msg.get();                                                                                     \
                set_current_message(std::move(msg));                                                                      \
                execute(this, current_message());                                                                         \
                delete ptr;                                                                                               \
            }                                                                                                             \
        }                                                                                                                 \
    }

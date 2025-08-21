#pragma once

// clang-format off
#include <actor-zeta/detail/type_traits.hpp>
#include <actor-zeta/base/address.hpp>
#include <actor-zeta/mailbox/message.hpp>
// clang-format on

#include <utility>
#include <actor-zeta/detail/type_traits.hpp>

namespace actor_zeta {
/*
    namespace impl {

        // SFINAE #1: message_id
        template<typename Name>
        typename std::enable_if<
            std::is_same<typename std::decay<Name>::type, mailbox::message_id>::value,
            mailbox::message_id>::type
        to_message_id(Name&& n) {
            return std::forward<Name>(n);
        }

        // SFINAE #2: enum or integral
        template<typename Name>
        typename std::enable_if<
            !std::is_same<typename std::decay<Name>::type, mailbox::message_id>::value &&
                (std::is_enum<typename std::decay<Name>::type>::value ||
                 std::is_integral<typename std::decay<Name>::type>::value),
            mailbox::message_id>::type
        to_message_id(Name&& n) {
            return mailbox::make_message_id(static_cast<std::uint64_t>(n));
        }

    } // namespace impl

    template<class Name>
    message_ptr make_message(actor_zeta::pmr::memory_resource* resource,
                                    actor_zeta::base::address_t sender,
                                    Name&& name) {
        assert(resource);
        mailbox::message_id id = impl::to_message_id<Name>(std::forward<Name>(name));
        return pmr_make_message(*resource, resource, std::move(sender), id);
    }

    template<class Name, class Arg, class... Args>
    message_ptr make_message(actor_zeta::pmr::memory_resource* resource,
                                    actor_zeta::base::address_t sender,
                                    Name&& name,
                                    Arg&& arg, Args&&... args) {
        assert(resource);
        mailbox::message_id id = impl::to_message_id<Name>(std::forward<Name>(name));


        actor_zeta::detail::rtt body(
            resource,
            typename std::decay<Arg>::type(std::forward<Arg>(arg)),
            typename std::decay<Args>::type(std::forward<Args>(args))... // rvalue -> move, lvalue -> copy
        );

        return pmr_make_message(*resource, resource, std::move(sender), id, std::move(body));
    }
*/
    template<class T>
    auto make_message_ptr(actor_zeta::pmr::memory_resource* resource,base::address_t sender_, T name) -> mailbox::message_ptr {
        return  mailbox::message_ptr( new mailbox::message(resource,std::move(sender_), mailbox::make_message_id(static_cast<uint64_t>(name))));
    }

    template<class T, typename Arg>
    auto make_message_ptr(actor_zeta::pmr::memory_resource* resource,base::address_t sender_, T name, Arg&& arg) -> mailbox::message_ptr {
        return mailbox::message_ptr( new mailbox::message(resource,std::move(sender_), mailbox::make_message_id(static_cast<uint64_t>(name)), detail::rtt(resource, std::forward<type_traits::decay_t<Arg>>(arg))));
    }

    template<class T, typename... Args>
    auto make_message_ptr(actor_zeta::pmr::memory_resource* resource,base::address_t sender_, T name, Args&&... args) -> mailbox::message_ptr {
        return mailbox::message_ptr(new mailbox::message(resource,std::move(sender_), mailbox::make_message_id(static_cast<uint64_t>(name)), detail::rtt(resource, std::forward<Args>(args)...)));
    }

    template<class T>
    auto make_message(actor_zeta::pmr::memory_resource* resource,base::address_t sender_, T name) -> mailbox::message_ptr {
        return mailbox::message_ptr(new mailbox::message(resource,std::move(sender_), mailbox::make_message_id(static_cast<uint64_t>(name))));
    }

    template<class T, typename Arg>
    auto make_message(actor_zeta::pmr::memory_resource* resource,base::address_t sender_, T name, Arg&& arg) -> mailbox::message_ptr {
        return mailbox::message_ptr(new mailbox::message(resource,std::move(sender_), mailbox::make_message_id(static_cast<uint64_t>(name)), detail::rtt(resource, std::forward<type_traits::decay_t<Arg>>(arg))));
    }

    template<class T, typename... Args>
    auto make_message(actor_zeta::pmr::memory_resource* resource,base::address_t sender_, T name, Args&&... args) -> mailbox::message_ptr {
        return mailbox::message_ptr(new mailbox::message(resource,std::move(sender_), mailbox::make_message_id(static_cast<uint64_t>(name)), detail::rtt(resource, std::forward<Args>(args)...)));
    }

    auto make_message(base::address_t sender_, mailbox::message_id id) -> mailbox::message_ptr;

    template<typename Arg>
    auto make_message(actor_zeta::pmr::memory_resource* resource,base::address_t sender_, mailbox::message_id id, Arg&& arg) -> mailbox::message_ptr {
        return mailbox::message_ptr(new mailbox::message(resource,std::move(sender_), id, detail::rtt(resource, std::forward<type_traits::decay_t<Arg>>(arg))));
    }

    template<typename... Args>
    auto make_message(actor_zeta::pmr::memory_resource* resource,base::address_t sender_, mailbox::message_id id, Args&&... args) -> mailbox::message_ptr {
        return mailbox::message_ptr(new mailbox::message(resource,std::move(sender_), id, detail::rtt(resource, std::forward<Args>(args)...)));
    }

} // namespace actor_zeta
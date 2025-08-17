#pragma once

// clang-format off
#include <actor-zeta/detail/type_traits.hpp>
#include <actor-zeta/base/address.hpp>
#include <actor-zeta/mailbox/message.hpp>
// clang-format on

namespace actor_zeta {

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

    template<class Name, class Arg1, class... Args>
    message_ptr make_message(actor_zeta::pmr::memory_resource* resource,
                                    actor_zeta::base::address_t sender,
                                    Name&& name,
                                    Arg1&& a1, Args&&... rest) {
        assert(resource);
        mailbox::message_id id = impl::to_message_id<Name>(std::forward<Name>(name));


        actor_zeta::detail::rtt body(resource,
                                      std::forward<Arg1>(a1),
                                      std::forward<Args>(rest)...);

        return pmr_make_message(*resource, resource, std::move(sender), id, std::move(body));
    }

} // namespace actor_zeta
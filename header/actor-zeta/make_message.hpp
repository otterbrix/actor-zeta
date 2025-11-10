#pragma once

// clang-format off
#include <actor-zeta/detail/type_traits.hpp>
#include <actor-zeta/base/address.hpp>
#include <actor-zeta/mailbox/message.hpp>
// clang-format on

#include <actor-zeta/detail/type_traits.hpp>
#include <utility>

namespace actor_zeta {

    namespace detail {

        template<typename Name>
        typename std::enable_if<
            std::is_same<typename std::decay<Name>::type, mailbox::message_id>::value,
            mailbox::message_id>::type
        to_message_id(Name&& name) {
            return std::forward<Name>(name);
        }

        template<typename Name>
        typename std::enable_if<
            !std::is_same<typename std::decay<Name>::type, mailbox::message_id>::value &&
                (std::is_enum<typename std::decay<Name>::type>::value ||
                 std::is_integral<typename std::decay<Name>::type>::value),
            mailbox::message_id>::type
        to_message_id(Name&& name) {
            return mailbox::make_message_id(static_cast<uint64_t>(name));
        }

        template<typename Name>
        struct is_valid_name_type {
            typedef typename std::decay<Name>::type decayed_type;
            static const bool value =
                std::is_same<decayed_type, mailbox::message_id>::value ||
                std::is_enum<decayed_type>::value ||
                std::is_integral<decayed_type>::value;
        };

        template<typename T>
        struct is_valid_rtt_type {
            typedef typename std::decay<T>::type decayed_type;

            static const bool value =
                !std::is_reference<T>::value &&
                !std::is_abstract<decayed_type>::value &&
                (std::is_copy_constructible<decayed_type>::value ||
                 std::is_move_constructible<decayed_type>::value);
        };

        template<bool...>
        struct bool_pack {};

        template<bool... values>
        using all_true = std::is_same<bool_pack<values..., true>, bool_pack<true, values...>>;

        template<typename... Args>
        struct all_valid_rtt_types : all_true<is_valid_rtt_type<Args>::value...> {};

        /// @brief Internal API: Create message with message_id (no arguments)
        /// @note Prefer using send() with method pointers for type-safe messaging
        template<typename Name>
        typename std::enable_if<
            is_valid_name_type<Name>::value,
            mailbox::message_ptr>::type
        make_message(actor_zeta::pmr::memory_resource* resource,
                     base::address_t sender,
                     Name&& name) {
            assert(resource);
            return mailbox::pmr_make_message(resource, resource, std::move(sender),
                                             to_message_id(std::forward<Name>(name)));
        }

        /// @brief Internal API: Create message with message_id and arguments
        /// @note Prefer using send() with method pointers for type-safe messaging
        template<typename Name, typename... Args>
        typename std::enable_if<
            is_valid_name_type<Name>::value &&
                all_valid_rtt_types<Args...>::value,
            mailbox::message_ptr>::type
        make_message(actor_zeta::pmr::memory_resource* resource,
                     base::address_t sender,
                     Name&& name,
                     Args... args) {
            assert(resource);
            return mailbox::pmr_make_message(resource, resource, std::move(sender),
                                             to_message_id(std::forward<Name>(name)),
                                             rtt(resource, std::move(args)...));
        }

    } // namespace detail

} // namespace actor_zeta
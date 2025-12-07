#pragma once

// clang-format off
#include <actor-zeta/detail/type_traits.hpp>
#include <actor-zeta/actor/address.hpp>
#include <actor-zeta/mailbox/message.hpp>
#include <actor-zeta/detail/future.hpp>
// clang-format on

#include <utility>

namespace actor_zeta::detail {

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

    // =====================================================================
    // Type validation traits for message arguments
    // =====================================================================

    /// @brief Check if type can be stored in RTT (message body)
    /// Requirements:
    /// - Not a reference (will be stored by value)
    /// - Not abstract
    /// - Move or copy constructible
    template<typename T>
    struct is_valid_rtt_type {
        using decayed_type = typename std::decay<T>::type;

        static constexpr bool value =
            !std::is_reference<T>::value &&
            !std::is_abstract<decayed_type>::value &&
            (std::is_copy_constructible<decayed_type>::value ||
             std::is_move_constructible<decayed_type>::value);
    };

    template<typename T>
    inline constexpr bool is_valid_rtt_type_v = is_valid_rtt_type<T>::value;

    /// @brief Check if provided argument type is compatible with expected method parameter type
    /// After decay, types should be convertible
    template<typename Expected, typename Provided>
    struct is_convertible_arg {
        using expected_decay = typename std::decay<Expected>::type;
        using provided_decay = typename std::decay<Provided>::type;

        static constexpr bool value =
            std::is_same<expected_decay, provided_decay>::value ||
            std::is_convertible<provided_decay, expected_decay>::value;
    };

    template<typename Expected, typename Provided>
    inline constexpr bool is_convertible_arg_v = is_convertible_arg<Expected, Provided>::value;

    template<bool...>
    struct bool_pack {};

    template<bool... values>
    using all_true = std::is_same<bool_pack<values..., true>, bool_pack<true, values...>>;

    template<typename... Args>
    struct all_valid_rtt_types : all_true<is_valid_rtt_type<Args>::value...> {};

    template<typename... Args>
    inline constexpr bool all_valid_rtt_types_v = all_valid_rtt_types<Args...>::value;

    // =====================================================================
    // Type list argument validation
    // =====================================================================

    /// @brief Check if all provided args are compatible with expected args (type_list)
    template<typename ExpectedList, typename ProvidedList>
    struct args_compatible;

    // Base case: empty lists are compatible
    template<>
    struct args_compatible<type_traits::type_list<>, type_traits::type_list<>> {
        static constexpr bool value = true;
    };

    // Mismatch count
    template<typename... Expected>
    struct args_compatible<type_traits::type_list<Expected...>, type_traits::type_list<>> {
        static constexpr bool value = false;
    };

    template<typename... Provided>
    struct args_compatible<type_traits::type_list<>, type_traits::type_list<Provided...>> {
        static constexpr bool value = false;
    };

    // Recursive check
    template<typename ExpHead, typename... ExpTail, typename ProvHead, typename... ProvTail>
    struct args_compatible<
        type_traits::type_list<ExpHead, ExpTail...>,
        type_traits::type_list<ProvHead, ProvTail...>> {
        static constexpr bool value =
            is_convertible_arg<ExpHead, ProvHead>::value &&
            args_compatible<
                type_traits::type_list<ExpTail...>,
                type_traits::type_list<ProvTail...>>::value;
    };

    template<typename ExpectedList, typename ProvidedList>
    inline constexpr bool args_compatible_v = args_compatible<ExpectedList, ProvidedList>::value;

    /// @brief Check that all provided args are valid for RTT storage
    template<typename... Args>
    struct all_args_storable {
        static constexpr bool value = all_valid_rtt_types<typename std::decay<Args>::type...>::value;
    };

    template<typename... Args>
    inline constexpr bool all_args_storable_v = all_args_storable<Args...>::value;

    // =====================================================================
    // make_message<R>() - creates message + promise in one call
    // Returns pair<message_ptr, unique_future<R>>
    // R defaults to void for fire-and-forget messages
    // =====================================================================

    /// @brief Create message with result slot (no arguments)
    /// @tparam R Result type for the future (default: void)
    /// @param resource Memory resource for allocation
    /// @param sender Sender address
    /// @param name Message command/name
    /// @return pair<message_ptr, unique_future<R>> - message and future for result
    template<typename R = void, typename Name>
    typename std::enable_if<
        is_valid_name_type<Name>::value,
        std::pair<mailbox::message_ptr, actor_zeta::unique_future<R>>>::type
    make_message(
        std::pmr::memory_resource* resource,
        actor::address_t sender,
        Name&& name) {
        static_assert(!std::is_reference_v<R>, "Result type R must not be a reference");
        assert(resource);
        actor_zeta::promise<R> p(resource);
        auto msg = mailbox::pmr_make_message(resource, resource, std::move(sender),
                                             to_message_id(std::forward<Name>(name)),
                                             p.internal_state_base());
        return {std::move(msg), p.get_future()};
    }

    /// @brief Create message with result slot and arguments
    /// @tparam R Result type for the future (default: void)
    /// @param resource Memory resource for allocation
    /// @param sender Sender address
    /// @param name Message command/name
    /// @param args Message arguments
    /// @return pair<message_ptr, unique_future<R>> - message and future for result
    template<typename R = void, typename Name, typename... Args>
    typename std::enable_if<
        is_valid_name_type<Name>::value &&
            all_valid_rtt_types<std::decay_t<Args>...>::value,
        std::pair<mailbox::message_ptr, actor_zeta::unique_future<R>>>::type
    make_message(
        std::pmr::memory_resource* resource,
        actor::address_t sender,
        Name&& name,
        Args&&... args) {
        static_assert(!std::is_reference_v<R>, "Result type R must not be a reference");
        assert(resource);
        actor_zeta::promise<R> p(resource);
        auto msg = mailbox::pmr_make_message(resource, resource, std::move(sender),
                                             to_message_id(std::forward<Name>(name)),
                                             rtt(resource, std::forward<Args>(args)...),
                                             p.internal_state_base());
        return {std::move(msg), p.get_future()};
    }

} // namespace actor_zeta::detail
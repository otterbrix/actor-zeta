#pragma once

// clang-format off
#include <actor-zeta/detail/type_traits.hpp>
#include <actor-zeta/actor/address.hpp>
#include <actor-zeta/mailbox/message.hpp>
#include <actor-zeta/detail/future.hpp>
// clang-format on

#include <utility>

namespace actor_zeta::detail {

    /// @brief Concept for message_id type
    template<typename Name>
    concept is_message_id = std::is_same_v<std::decay_t<Name>, mailbox::message_id>;

    /// @brief Concept for types convertible to message_id (enum or integral)
    template<typename Name>
    concept is_convertible_to_message_id =
        !is_message_id<Name> &&
        (std::is_enum_v<std::decay_t<Name>> || std::is_integral_v<std::decay_t<Name>>);

    /// @brief Concept for valid message name types
    template<typename Name>
    concept valid_message_name =
        is_message_id<Name> || is_convertible_to_message_id<Name>;

    template<typename Name>
        requires is_message_id<Name>
    mailbox::message_id to_message_id(Name&& name) {
        return std::forward<Name>(name);
    }

    template<typename Name>
        requires is_convertible_to_message_id<Name>
    mailbox::message_id to_message_id(Name&& name) {
        return mailbox::make_message_id(static_cast<uint64_t>(name));
    }

    // Legacy type trait for backward compatibility
    template<typename Name>
    struct is_valid_name_type {
        typedef typename std::decay<Name>::type decayed_type;
        static const bool value = valid_message_name<Name>;
    };

    // =====================================================================
    // Type validation concepts for message arguments
    // =====================================================================

    /// @brief Concept: type can be stored in RTT (message body)
    /// Requirements:
    /// - Not a reference (will be stored by value)
    /// - Not abstract
    /// - Move or copy constructible
    template<typename T>
    concept valid_rtt_type =
        !std::is_reference_v<T> &&
        !std::is_abstract_v<std::decay_t<T>> &&
        (std::is_copy_constructible_v<std::decay_t<T>> ||
         std::is_move_constructible_v<std::decay_t<T>>);

    // Legacy compatibility
    template<typename T>
    inline constexpr bool is_valid_rtt_type_v = valid_rtt_type<T>;

    /// @brief Concept: provided argument type is compatible with expected method parameter type
    /// After decay, types should be convertible
    template<typename Expected, typename Provided>
    concept convertible_arg =
        std::is_same_v<std::decay_t<Expected>, std::decay_t<Provided>> ||
        std::is_convertible_v<std::decay_t<Provided>, std::decay_t<Expected>>;

    // Legacy compatibility
    template<typename Expected, typename Provided>
    inline constexpr bool is_convertible_arg_v = convertible_arg<Expected, Provided>;

    /// @brief Concept: all types are valid for RTT storage
    template<typename... Args>
    concept all_valid_rtt = (valid_rtt_type<Args> && ...);

    // Legacy compatibility
    template<typename... Args>
    inline constexpr bool all_valid_rtt_types_v = all_valid_rtt<Args...>;

    // =====================================================================
    // Type list argument validation
    // =====================================================================

    namespace detail_args {
        // Helper to check args compatibility with index sequence
        template<typename ExpectedList, typename ProvidedList, typename IndexSeq>
        struct args_compatible_impl;

        template<typename... Expected, typename... Provided, std::size_t... Is>
        struct args_compatible_impl<
            type_traits::type_list<Expected...>,
            type_traits::type_list<Provided...>,
            std::index_sequence<Is...>> {
            static constexpr bool value =
                (convertible_arg<
                    type_traits::type_list_at_t<type_traits::type_list<Expected...>, Is>,
                    type_traits::type_list_at_t<type_traits::type_list<Provided...>, Is>> && ...);
        };
    } // namespace detail_args

    /// @brief Concept: all provided args are compatible with expected args (type_list)
    template<typename ExpectedList, typename ProvidedList>
    concept compatible_args = requires {
        requires (type_traits::type_list_size_v<ExpectedList> ==
                  type_traits::type_list_size_v<ProvidedList>);
    } && detail_args::args_compatible_impl<
            ExpectedList, ProvidedList,
            std::make_index_sequence<type_traits::type_list_size_v<ExpectedList>>>::value;

    // Legacy compatibility
    template<typename ExpectedList, typename ProvidedList>
    inline constexpr bool args_compatible_v = compatible_args<ExpectedList, ProvidedList>;

    /// @brief Concept: all provided args are valid for RTT storage
    template<typename... Args>
    concept all_args_storable = all_valid_rtt<std::decay_t<Args>...>;

    // Legacy compatibility
    template<typename... Args>
    inline constexpr bool all_args_storable_v = all_args_storable<Args...>;

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
        requires valid_message_name<Name>
    std::pair<mailbox::message_ptr, actor_zeta::unique_future<R>>
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
        requires(valid_message_name<Name> && all_valid_rtt_types_v<std::decay_t<Args>...>)
    std::pair<mailbox::message_ptr, actor_zeta::unique_future<R>>
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
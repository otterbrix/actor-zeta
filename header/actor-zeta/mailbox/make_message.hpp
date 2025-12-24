#pragma once

// clang-format off
#include <actor-zeta/detail/type_traits.hpp>
#include <actor-zeta/actor/address.hpp>
#include <actor-zeta/mailbox/message.hpp>
#include <actor-zeta/detail/future.hpp>
#include <actor-zeta/detail/generator.hpp>
// clang-format on

#include <utility>

namespace actor_zeta::detail {

    template<typename Name>
    concept is_message_id = std::is_same_v<std::decay_t<Name>, mailbox::message_id>;

    template<typename Name>
    concept is_convertible_to_message_id =
        !is_message_id<Name> &&
        (std::is_enum_v<std::decay_t<Name>> || std::is_integral_v<std::decay_t<Name>>);

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

    template<typename Name>
    struct is_valid_name_type {
        typedef typename std::decay<Name>::type decayed_type;
        static const bool value = valid_message_name<Name>;
    };

    // Type must be storable in RTT: not reference, not abstract, move or copy constructible
    template<typename T>
    concept valid_rtt_type =
        !std::is_reference_v<T> &&
        !std::is_abstract_v<std::decay_t<T>> &&
        (std::is_copy_constructible_v<std::decay_t<T>> ||
         std::is_move_constructible_v<std::decay_t<T>>);

    template<typename T>
    inline constexpr bool is_valid_rtt_type_v = valid_rtt_type<T>;

    template<typename Expected, typename Provided>
    concept convertible_arg =
        std::is_same_v<std::decay_t<Expected>, std::decay_t<Provided>> ||
        std::is_convertible_v<std::decay_t<Provided>, std::decay_t<Expected>>;

    template<typename Expected, typename Provided>
    inline constexpr bool is_convertible_arg_v = convertible_arg<Expected, Provided>;

    template<typename... Args>
    concept all_valid_rtt = (valid_rtt_type<Args> && ...);

    template<typename... Args>
    inline constexpr bool all_valid_rtt_types_v = all_valid_rtt<Args...>;

    namespace detail_args {
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

    template<typename ExpectedList, typename ProvidedList>
    concept compatible_args = requires {
        requires (type_traits::type_list_size_v<ExpectedList> ==
                  type_traits::type_list_size_v<ProvidedList>);
    } && detail_args::args_compatible_impl<
            ExpectedList, ProvidedList,
            std::make_index_sequence<type_traits::type_list_size_v<ExpectedList>>>::value;

    template<typename ExpectedList, typename ProvidedList>
    inline constexpr bool args_compatible_v = compatible_args<ExpectedList, ProvidedList>;

    template<typename... Args>
    concept all_args_storable = all_valid_rtt<std::decay_t<Args>...>;

    template<typename... Args>
    inline constexpr bool all_args_storable_v = all_args_storable<Args...>;

    // Creates message + promise, returns pair<message_ptr, unique_future<R>>
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

    template<typename T>
    generator_state<T>* allocate_generator_state(std::pmr::memory_resource* resource) {
        void* mem = resource->allocate(sizeof(generator_state<T>), alignof(generator_state<T>));
        return new (mem) generator_state<T>(resource);
    }

    // Creates message + generator_state, returns pair<message_ptr, generator<T>>
    template<typename T, typename Name>
        requires valid_message_name<Name>
    std::pair<mailbox::message_ptr, actor_zeta::generator<T>>
    make_generator_message(
        std::pmr::memory_resource* resource,
        actor::address_t sender,
        Name&& name) {
        assert(resource);
        auto* state = allocate_generator_state<T>(resource);
        state->add_ref();
        auto msg = mailbox::pmr_make_message(resource, resource, std::move(sender),
                                             to_message_id(std::forward<Name>(name)),
                                             intrusive_ptr<future_state_base>(state));
        return {std::move(msg), generator<T>(state)};
    }

    template<typename T, typename Name, typename... Args>
        requires(valid_message_name<Name> && all_valid_rtt_types_v<std::decay_t<Args>...>)
    std::pair<mailbox::message_ptr, actor_zeta::generator<T>>
    make_generator_message(
        std::pmr::memory_resource* resource,
        actor::address_t sender,
        Name&& name,
        Args&&... args) {
        assert(resource);
        auto* state = allocate_generator_state<T>(resource);
        state->add_ref();
        auto msg = mailbox::pmr_make_message(resource, resource, std::move(sender),
                                             to_message_id(std::forward<Name>(name)),
                                             rtt(resource, std::forward<Args>(args)...),
                                             intrusive_ptr<future_state_base>(state));
        return {std::move(msg), generator<T>(state)};
    }

} // namespace actor_zeta::detail
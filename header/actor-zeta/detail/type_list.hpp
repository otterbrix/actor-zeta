#pragma once

#include <cstddef>
#include <type_traits>

namespace actor_zeta { namespace type_traits {

    struct unit_t final {
        constexpr unit_t() noexcept {}

        constexpr unit_t(const unit_t&) noexcept {}

        template<class T>
        explicit constexpr unit_t(T&&) noexcept {}

        template<class... args>
        constexpr unit_t operator()(args&&...) const noexcept {
            return {};
        }
    };

    template<class... args>
    struct type_list {
        constexpr type_list() = default;
    };

    using empty_type_list = type_list<>;

    namespace detail {
        template<class T>
        struct is_type_list_impl : std::false_type {};

        template<class... args>
        struct is_type_list_impl<type_list<args...>> : std::true_type {};
    }

    template<typename T>
    concept type_list_type = detail::is_type_list_impl<T>::value;

    template<class T>
    struct is_type_list {
        static constexpr bool value = type_list_type<T>;
    };

    template<class List>
    struct type_list_size;

    template<class... args>
    struct type_list_size<type_list<args...>> {
        static constexpr size_t value = sizeof...(args);
    };

    template<class List>
    inline constexpr size_t type_list_size_v = type_list_size<List>::value;

    template<size_t N, class... E>
    struct type_list_at_impl;

    template<size_t N, typename E0, class... E>
    struct type_list_at_impl<N, E0, E...> {
        using type = typename type_list_at_impl<N - 1, E...>::type;
    };

    template<class E0, class... E>
    struct type_list_at_impl<0, E0, E...> {
        using type = E0;
    };

    template<class List, size_t N>
    struct type_list_at;

    template<size_t N, class... E>
    struct type_list_at<type_list<E...>, N> {
        using type = typename type_list_at_impl<N, E...>::type;
    };

    template<class List, size_t N>
    using type_list_at_t = typename type_list_at<List, N>::type;

    template<class List>
    struct tl_head;

    template<>
    struct tl_head<type_list<>> {
        using type = void;
    };

    template<typename T0, class... args>
    struct tl_head<type_list<T0, args...>> {
        using type = T0;
    };

    template<class List>
    using tl_head_t = typename tl_head<List>::type;

    template<class List>
    struct tl_tail;

    template<>
    struct tl_tail<type_list<>> {
        using type = type_list<>;
    };

    template<typename T0, class... args>
    struct tl_tail<type_list<T0, args...>> {
        using type = type_list<args...>;
    };

    template<class List>
    using tl_tail_t = typename tl_tail<List>::type;

    template<class List>
    struct tl_size;

    template<class... args>
    struct tl_size<type_list<args...>> {
        static constexpr size_t value = sizeof...(args);
    };

    template<class... args>
    constexpr size_t tl_size<type_list<args...>>::value;

    template<class List>
    inline constexpr size_t tl_size_v = tl_size<List>::value;

    template<size_t LeftOffset, size_t Remaining, typename PadType, class List, class... args>
    struct tl_slice_impl {
        using type = typename tl_slice_impl<LeftOffset - 1, Remaining, PadType,
                                            tl_tail_t<List>, args...>::type;
    };

    template<size_t Remaining, typename PadType, class List, class... args>
    struct tl_slice_impl<0, Remaining, PadType, List, args...> {
        using type =
            typename tl_slice_impl<0, Remaining - 1, PadType, tl_tail_t<List>, args...,
                                   tl_head_t<List>>::type;
    };

    template<size_t Remaining, typename PadType, class... args>
    struct tl_slice_impl<0, Remaining, PadType, empty_type_list, args...> {
        using type = typename tl_slice_impl<0, Remaining - 1, PadType,
                                            empty_type_list, args..., PadType>::type;
    };

    template<class PadType, class List, class... T>
    struct tl_slice_impl<0, 0, PadType, List, T...> {
        using type = type_list<T...>;
    };

    template<class PadType, class... T>
    struct tl_slice_impl<0, 0, PadType, empty_type_list, T...> {
        using type = type_list<T...>;
    };

    template<class List, size_t ListSize, size_t First, size_t Last, typename PadType = unit_t>
    struct tl_slice_ {
        using type =
            typename tl_slice_impl<First, (Last - First), PadType, List>::type;
    };

    template<class List, size_t ListSize, typename PadType>
    struct tl_slice_<List, ListSize, 0, ListSize, PadType> {
        using type = List;
    };

    template<class List, size_t First, size_t Last>
    struct tl_slice {
        using type = typename tl_slice_<List, type_list_size_v<List>,
                                        (First > Last ? Last : First), Last>::type;
    };

    template<class List, size_t First, size_t Last>
    using tl_slice_t = typename tl_slice<List, First, Last>::type;

}} // namespace actor_zeta::type_traits
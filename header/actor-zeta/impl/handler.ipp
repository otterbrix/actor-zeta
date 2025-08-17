#pragma once

#include <actor-zeta/base/forwards.hpp>
#include <actor-zeta/detail/callable_trait.hpp>
#include <actor-zeta/detail/type_list.hpp>

namespace actor_zeta { namespace base {
    // clang-format off
        template <class List, std::size_t I>
        using forward_arg =
        typename std::conditional<std::is_lvalue_reference<type_traits::type_list_at_t<List, I>>::value,
        typename std::add_lvalue_reference<type_traits::decay_t<type_traits::type_list_at_t<List, I>>>::type,
        typename std::add_rvalue_reference<type_traits::decay_t<type_traits::type_list_at_t<List, I>>>::type>::type;
    // clang-format on
    /// type list to  Tuple
    template<class List>
    struct type_list_to_tuple;

    template<class... args>
    struct type_list_to_tuple<type_traits::type_list<args...>> {
        using type = std::tuple<type_traits::decay_t<args>...>;
    };

    template<class... args>
    using type_list_to_tuple_t = typename type_list_to_tuple<args...>::type;

    // clang-format off
    template<class F, std::size_t... I>
    void apply_impl(F &&f, mailbox::message *ctx, type_traits::index_sequence<I...>) {
        using call_trait =  type_traits::get_callable_trait_t<type_traits::remove_reference_t<F>>;
        constexpr int args_size = call_trait::number_of_arguments;
        using args_type_list = type_traits::tl_slice_t<typename call_trait::args_types, 0, args_size>;
       /// using Tuple =  type_list_to_tuple_t<args_type_list>;
        auto &args = ctx->body();
        ///f(static_cast< forward_arg<args_type_list, I>>(std::get<I>(args))...);
        f((actor_zeta::detail::get<I, args_type_list>(args))...);
    }

    // clang-format on

    template<typename Fun>
    struct is_fun_ptr : std::integral_constant<bool, std::is_pointer<Fun>::value && std::is_function<typename std::remove_pointer<Fun>::type>::value> {
    };

    template<typename F,
             class Args = typename type_traits::get_callable_trait<F>::args_types,
             int Args_size = type_traits::get_callable_trait<F>::number_of_arguments,
             bool method = is_fun_ptr<F>::value>
    struct transformer;

    template<typename F, class Args, int Args_size>
    struct transformer<F, Args, Args_size, false> {
        auto operator()(actor_zeta::pmr::memory_resource* resource, F&& f) -> action {
            action tmp(resource, [func = type_traits::decay_t<F>(f)](mailbox::message* ctx) -> void {
                apply_impl(func, ctx, type_traits::make_index_sequence<Args_size>{});
            });
            return tmp;
        }
    };

    template<typename F, class Args>
    struct transformer<F, Args, 0, false> final {
        auto operator()(actor_zeta::pmr::memory_resource* resource, F&& f) -> action {
            action tmp(resource, [func = type_traits::decay_t<F>(f)](mailbox::message*) -> void {
                func();
            });
            return tmp;
        }
    };

    template<typename F, class Args>
    struct transformer<F, Args, 1, false> final {
        auto operator()(actor_zeta::pmr::memory_resource* resource, F&& f) -> action {
            action tmp(resource, [func = type_traits::decay_t<F>(f)](mailbox::message* ctx) -> void {
                using arg_type = type_traits::type_list_at_t<Args, 0>;
                using clear_args_type = type_traits::decay_t<arg_type>;
                auto& tmp = ctx->body();
                func(tmp.get<clear_args_type>(0));
            });
            return tmp;
        }
    };

    template<typename F, class Args, int Args_size>
    struct transformer<F, Args, Args_size, true> {
        auto operator()(actor_zeta::pmr::memory_resource* resource, F&& f) -> action {
            action tmp(resource, [func = std::move(f)](mailbox::message* ctx) -> void {
                apply_impl(func, ctx, type_traits::make_index_sequence<Args_size>{});
            });
            return tmp;
        }
    };

    template<typename F, class Args>
    struct transformer<F, Args, 0, true> final {
        auto operator()(actor_zeta::pmr::memory_resource* resource, F&& f) -> action {
            action tmp(resource, [func = std::move(f)](mailbox::message*) -> void {
                func();
            });
            return tmp;
        }
    };

    template<typename F, class Args>
    struct transformer<F, Args, 1, true> final {
        auto operator()(actor_zeta::pmr::memory_resource* resource, F&& f) -> action {
            action tmp(resource, [func = std::move(f)](mailbox::message* ctx) -> void {
                using arg_type = type_traits::type_list_at_t<Args, 0>;
                using clear_args_type = type_traits::decay_t<arg_type>;
                auto& tmp = ctx->body();
                func(tmp.get<clear_args_type>(0));
            });
            return tmp;
        }
    };

    /// class method
    // clang-format off
    template< typename ClassPtr, class F, std::size_t... I>
    void apply_impl_for_class(ClassPtr *ptr,F &&f, mailbox::message *ctx, type_traits::index_sequence<I...>) {
        using call_trait =  type_traits::get_callable_trait_t<type_traits::remove_reference_t<F>>;
        using args_type_list = typename call_trait::args_types;
        //using result_type = typename call_trait::result_type;
        ///using Tuple =  type_list_to_tuple_t<args_type_list>;
        auto &args = ctx->body();
        //(ptr->*f)(static_cast< forward_arg<args_type_list, I>>(std::get<I>(args))...);
        (ptr->*f)((actor_zeta::detail::get<I, args_type_list>(args))...);
    }

    // clang-format on
    template<
        typename ClassPtr,
        typename F,
        class Args = typename type_traits::get_callable_trait<F>::args_types,
        int Args_size = type_traits::get_callable_trait<F>::number_of_arguments>
    struct transformer_for_class {
        auto operator()(actor_zeta::pmr::memory_resource* resource, ClassPtr* ptr, F&& f) -> action {
            action tmp(resource, [func = std::move(f), ptr](mailbox::message* ctx) -> void {
                apply_impl_for_class(ptr, func, ctx, type_traits::make_index_sequence<Args_size>{});
            });
            return tmp;
        }
    };

    template<
        typename ClassPtr,
        typename F,
        class Args>
    struct transformer_for_class<ClassPtr, F, Args, 0> final {
        auto operator()(actor_zeta::pmr::memory_resource* resource, ClassPtr* ptr, F&& f) -> action {
            action tmp(resource, [func = std::move(f), ptr](mailbox::message*) -> void {
                (ptr->*func)();
            });
            return tmp;
        }
    };

    template<
        typename ClassPtr,
        typename F,
        class Args>
    struct transformer_for_class<ClassPtr, F, Args, 1> final {
        auto operator()(actor_zeta::pmr::memory_resource* resource, ClassPtr* ptr, F&& f) -> action {
            action tmp(resource, [func = std::move(f), ptr](mailbox::message* arg) -> void {
                using arg_type_0 = type_traits::type_list_at_t<Args, 0>;
                using decay_arg_type_0 = type_traits::decay_t<arg_type_0>;
                auto& tmp = arg->body();
                using original_arg_type_0 = forward_arg<Args, 0>;
                (ptr->*func)(std::forward<original_arg_type_0>(static_cast<original_arg_type_0>(tmp.get<decay_arg_type_0>(0))));
            });
            return tmp;
        }
    };

    template<typename F>
    auto make_handler(actor_zeta::pmr::memory_resource* resource, F&& f) -> action {
        return transformer<F>{}(resource, std::forward<F>(f));
    }

    template<typename ClassPtr, typename F>
    auto make_handler(actor_zeta::pmr::memory_resource* resource, ClassPtr* self, F&& f) -> action {
        return transformer_for_class<ClassPtr, F>{}(resource, self, std::forward<F>(f));
    }

}} // namespace actor_zeta::base

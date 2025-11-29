#pragma once

#include <actor-zeta/base/forwards.hpp>
#include <actor-zeta/mailbox/message.hpp>
#include <actor-zeta/detail/callable_trait.hpp>
#include <actor-zeta/detail/type_list.hpp>
#include <actor-zeta/detail/type_traits.hpp>
#include <iostream>  // for debug output

// Forward declaration for unique_future (will be included via future.hpp in user code)
namespace actor_zeta {
    template<typename T>
    class unique_future;
}

namespace actor_zeta { namespace base {

    /// @brief Link future result to message result_slot via continuation (NON-BLOCKING)
    /// @param source Future from method call (e.g., (ptr->*f)(args...))
    /// @param target_slot Message's result_slot to receive result
    /// @note Uses continuation mechanism - NEVER calls .get() on suspended futures
    /// @note Thread-safe: CAS protection in set_result_rtt()
    /// @note Refcount management: source state kept alive by continuation_ link
    ///
    /// @details
    /// FAST PATH (source ready):
    ///   - Extract result via .get()
    ///   - Set target_slot->result_ directly
    ///   - Set target_slot->state_ = ready
    ///   - Total: 1 allocation (target_slot), NO blocking
    ///
    /// SLOW PATH (source not ready):
    ///   - Call source.release_state() (returns raw pointer, no refcount change)
    ///   - Call source_state->set_continuation(target_slot)
    ///     * Increments source_state refcount (keeps it alive)
    ///     * Stores target_slot in source_state->continuation_
    ///   - When source becomes ready:
    ///     * set_result_rtt() propagates result to continuation_
    ///     * Decrements source_state refcount (may destroy it)
    ///   - Total: 0 additional allocations, NO blocking
    ///
    /// Example usage:
    /// @code
    /// // BEFORE (blocking):
    /// T value = std::move(future).get();  // BLOCKS if not ready!
    /// if (auto slot = msg->result_slot()) {
    ///     slot->set_result_rtt(detail::rtt(slot->memory_resource(), std::move(value)));
    /// }
    ///
    /// // AFTER (non-blocking):
    /// if (auto slot = msg->result_slot()) {
    ///     link_future_to_slot(std::move(future), slot);  // NEVER blocks!
    /// }
    /// @endcode
    template<typename T>
    void link_future_to_slot(
        unique_future<T>& source,
        intrusive_ptr<detail::future_state_base> target_slot
    ) noexcept {
        auto* source_ptr = source.get_state();
        std::cerr << "[link_future_to_slot] source=" << static_cast<void*>(source_ptr)
                  << " target=" << static_cast<void*>(target_slot.get())
                  << " source.is_ready()=" << source.is_ready() << std::endl;

        // FAST PATH: Already ready - transfer result immediately
        if (source.is_ready()) {
            std::cerr << "[link_future_to_slot] FAST PATH: source ready, propagating result..." << std::endl;

            auto* source_state = source.get_state();
            if (!source_state) {
                std::cerr << "[link_future_to_slot] FAST PATH: source has no state, skipping" << std::endl;
                return;
            }

            // Use virtual method - source_state knows its type and can copy result
            source_state->propagate_result_to(target_slot.get());

            std::cerr << "[link_future_to_slot] FAST PATH done" << std::endl;
            return;
        }

        // SLOW PATH: NOT ready - set continuation (NO BLOCKING!)
        // DON'T use release_state() - keep future valid for coroutine extraction!
        std::cerr << "[link_future_to_slot] SLOW PATH: source NOT ready, setting continuation" << std::endl;
        auto* source_state = source.get_state();

        // set_continuation() increments source_state refcount to keep it alive
        // When source becomes ready, set_result_rtt() will:
        //   1. Propagate result to target_slot
        //   2. Decrement source_state refcount (potentially destroying it)
        // NOTE: source future still owns its state - user can extract coroutine handle
        source_state->set_continuation(target_slot);

        std::cerr << "[link_future_to_slot] SLOW PATH done, continuation set" << std::endl;
    }
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
    void apply_impl(F &&f, mailbox::message *msg, type_traits::index_sequence<I...>) {
        using call_trait =  type_traits::get_callable_trait_t<type_traits::remove_reference_t<F>>;
        constexpr int args_size = call_trait::number_of_arguments;
        using args_type_list = type_traits::tl_slice_t<typename call_trait::args_types, 0, args_size>;
       /// using Tuple =  type_list_to_tuple_t<args_type_list>;
        auto &args = msg->body();
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
            action tmp(resource, [func = type_traits::decay_t<F>(f)](mailbox::message* msg) -> void {
                apply_impl(func, msg, type_traits::make_index_sequence<Args_size>{});
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
            action tmp(resource, [func = type_traits::decay_t<F>(f)](mailbox::message* msg) -> void {
                using arg_type = type_traits::type_list_at_t<Args, 0>;
                using clear_args_type = type_traits::decay_t<arg_type>;
                auto& tmp = msg->body();
                func(tmp.get<clear_args_type>(0));
            });
            return tmp;
        }
    };

    template<typename F, class Args, int Args_size>
    struct transformer<F, Args, Args_size, true> {
        auto operator()(actor_zeta::pmr::memory_resource* resource, F&& f) -> action {
            action tmp(resource, [func = std::move(f)](mailbox::message* msg) -> void {
                apply_impl(func, msg, type_traits::make_index_sequence<Args_size>{});
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
            action tmp(resource, [func = std::move(f)](mailbox::message* msg) -> void {
                using arg_type = type_traits::type_list_at_t<Args, 0>;
                using clear_args_type = type_traits::decay_t<arg_type>;
                auto& tmp = msg->body();
                func(tmp.get<clear_args_type>(0));
            });
            return tmp;
        }
    };

    /// class method
    // clang-format off
    template< typename ClassPtr, class F, std::size_t... I>
    void apply_impl_for_class(ClassPtr *ptr,F &&f, mailbox::message *msg, type_traits::index_sequence<I...>) {
        using call_trait =  type_traits::get_callable_trait_t<type_traits::remove_reference_t<F>>;
        using args_type_list = typename call_trait::args_types;
        using result_type = typename call_trait::result_type;
        auto &args = msg->body();

        if constexpr (std::is_void<result_type>::value) {
            // Method returns void (not unique_future)
            (ptr->*f)((actor_zeta::detail::get<I, args_type_list>(args))...);
            msg->set_error(mailbox::slot_error_code::ok);
        } else {
            // Method returns unique_future<T>
            // Result linking is handled by resume() via link_future_to_slot()
            (ptr->*f)((actor_zeta::detail::get<I, args_type_list>(args))...);
        }
    }

    // clang-format on
    template<
        typename ClassPtr,
        typename F,
        class Args = typename type_traits::get_callable_trait<F>::args_types,
        int Args_size = type_traits::get_callable_trait<F>::number_of_arguments>
    struct transformer_for_class {
        auto operator()(actor_zeta::pmr::memory_resource* resource, ClassPtr* ptr, F&& f) -> action {
            action tmp(resource, [func = std::move(f), ptr](mailbox::message* msg) -> void {
                apply_impl_for_class(ptr, func, msg, type_traits::make_index_sequence<Args_size>{});
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
            using call_trait = type_traits::get_callable_trait_t<type_traits::remove_reference_t<F>>;
            using result_type = typename call_trait::result_type;

            action tmp(resource, [func = std::move(f), ptr](mailbox::message* msg) -> void {
                if constexpr (std::is_void<result_type>::value) {
                    // Method returns void (not unique_future)
                    (ptr->*func)();
                    msg->set_error(mailbox::slot_error_code::ok);
                } else {
                    // Method returns unique_future<T>
                    // Result linking is handled by resume() via link_future_to_slot()
                    (ptr->*func)();
                }
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
            using call_trait = type_traits::get_callable_trait_t<type_traits::remove_reference_t<F>>;
            using result_type = typename call_trait::result_type;

            action tmp(resource, [func = std::move(f), ptr](mailbox::message* msg) -> void {
                using arg_type_0 = type_traits::type_list_at_t<Args, 0>;
                using decay_arg_type_0 = type_traits::decay_t<arg_type_0>;
                auto& tmp = msg->body();
                using original_arg_type_0 = forward_arg<Args, 0>;

                if constexpr (std::is_void<result_type>::value) {
                    // Method returns void (not unique_future)
                    (ptr->*func)(std::forward<original_arg_type_0>(static_cast<original_arg_type_0>(tmp.get<decay_arg_type_0>(0))));
                    msg->set_error(mailbox::slot_error_code::ok);
                } else {
                    // Method returns unique_future<T>
                    // Result linking is handled by resume() via link_future_to_slot()
                    (ptr->*func)(std::forward<original_arg_type_0>(static_cast<original_arg_type_0>(tmp.get<decay_arg_type_0>(0))));
                }
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

#pragma once

#include <type_traits>

namespace actor_zeta { namespace detail {

    // How must elements of T be freed?
    //
    // A queue frees whatever is left in it when it is closed or destroyed.
    // std::default_delete is correct for `new`-allocated nodes and CATASTROPHIC for
    // PMR-allocated ones: mailbox::message lives inside a PMR block behind a BlockHdr,
    // so `delete p` hands the global allocator a pointer that is not the allocation
    // base -- ASan reports "attempting free on address which was not malloc()-ed",
    // and without ASan it is silent heap corruption at shutdown.
    //
    // A type that knows how it must be freed says so by declaring `deleter_type`. The
    // queues static_assert on it, turning a wrong-deleter instantiation into a compile
    // error instead of a run-time corruption.
    template<class T, class = void>
    struct declared_deleter {
        static constexpr bool present = false;
        using type = void;
    };

    template<class T>
    struct declared_deleter<T, std::void_t<typename T::deleter_type>> {
        static constexpr bool present = true;
        using type = typename T::deleter_type;
    };

}} // namespace actor_zeta::detail

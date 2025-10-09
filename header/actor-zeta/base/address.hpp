#pragma once

#include <cassert>

#include "forwards.hpp"
#include <type_traits>

namespace actor_zeta { namespace base {
    ///
    /// @brief
    ///

    template<class Target>
    actor_abstract_t* safe_cast_ptr(Target* ptr) {
        assert(ptr != nullptr);
        return ptr;
    }

    class address_t final {
    public:
        address_t(address_t&& other) noexcept;
        address_t(const address_t& other);
        address_t& operator=(address_t&& other) noexcept;
        address_t& operator=(const address_t& other);
        bool operator==(const address_t& rhs) noexcept;
        ~address_t() noexcept;

        template<class Target>
        explicit address_t(Target*ptr):ptr_(safe_cast_ptr(ptr)) {}

        static auto empty_address() -> address_t;

        inline actor_abstract_t* operator->() const noexcept {
            return ptr_;
        }

        operator bool() const noexcept;
        auto operator!() const noexcept -> bool;
        void swap(address_t& other);

    private:
        address_t() noexcept;
        actor_abstract_t* ptr_;
    };

    static_assert(!std::is_default_constructible<address_t>::value, "");
    static_assert(std::is_move_constructible<address_t>::value, "");
    static_assert(std::is_move_assignable<address_t>::value, "");
    static_assert(std::is_copy_constructible<address_t>::value, "");
    static_assert(std::is_copy_assignable<address_t>::value, "");

}} // namespace actor_zeta::base

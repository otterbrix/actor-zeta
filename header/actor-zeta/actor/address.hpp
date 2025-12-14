#pragma once

#include <cassert>

#include <type_traits>

namespace actor_zeta::actor {

    template<class Target>
    void* safe_cast_ptr(Target* ptr) {
        assert(ptr != nullptr);
        return static_cast<void*>(ptr);
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
        explicit address_t(Target* ptr)
            : ptr_(safe_cast_ptr(ptr)) {}

        address_t(void* ptr);

        static auto empty_address() -> address_t;

        void* get() const noexcept;
        operator bool() const noexcept;
        auto operator!() const noexcept -> bool;
        void swap(address_t& other);

    private:
        address_t() noexcept;
        void* ptr_;
    };

    static_assert(!std::is_default_constructible_v<address_t>);
    static_assert(std::is_move_constructible_v<address_t>);
    static_assert(std::is_move_assignable_v<address_t>);
    static_assert(std::is_copy_constructible_v<address_t>);
    static_assert(std::is_copy_assignable_v<address_t>);

} // namespace actor_zeta::actor

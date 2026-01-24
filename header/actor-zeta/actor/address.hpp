#pragma once

#include <cassert>
#include <concepts>
#include <memory_resource>
#include <type_traits>
#include <utility>

#include <actor-zeta/detail/queue/enqueue_result.hpp>
#include <actor-zeta/mailbox/forwards.hpp>

namespace actor_zeta::actor {

    template<class Target>
    void* safe_cast_ptr(Target* ptr) {
        assert(ptr != nullptr);
        return static_cast<void*>(ptr);
    }

    // Concept to detect if T has resource() method returning memory_resource*
    template<typename T>
    concept has_resource_method = requires(T* t) {
        { t->resource() } -> std::convertible_to<std::pmr::memory_resource*>;
    };

    // Concept to detect actors with enqueue_impl
    template<typename T>
    concept has_enqueue_impl = requires(T* t, mailbox::message_ptr msg) {
        { t->enqueue_impl(std::move(msg)) } -> std::same_as<std::pair<bool, detail::enqueue_result>>;
    };

    class address_t final {
    public:
        /// Type-erased enqueue function pointer
        /// Uses raw message* because function pointers can't take unique_ptr by value
        /// Returns: {needs_scheduling, enqueue_result}
        using enqueue_fn_t = std::pair<bool, detail::enqueue_result>(*)(void*, mailbox::message*);

        address_t(address_t&& other) noexcept;
        address_t(const address_t& other);
        address_t& operator=(address_t&& other) noexcept;
        address_t& operator=(const address_t& other);
        bool operator==(const address_t& rhs) const noexcept;
        ~address_t() noexcept;

        /// Template constructor captures enqueue_impl for polymorphic dispatch
        template<typename Target>
            requires has_resource_method<Target> && has_enqueue_impl<Target>
        explicit address_t(Target* ptr)
            : resource_(ptr->resource())
            , ptr_(safe_cast_ptr(ptr))
            , enqueue_fn_(+[](void* p, mailbox::message* msg_raw) {
                  return static_cast<Target*>(p)->enqueue_impl(
                      mailbox::message_ptr(msg_raw));
              }) {}

        explicit address_t(std::pmr::memory_resource*, void* );

        static auto empty_address() -> address_t;

        void* get() const noexcept;
        std::pmr::memory_resource* resource() const noexcept;
        operator bool() const noexcept;
        auto operator!() const noexcept -> bool;
        void swap(address_t& other);

        /// Type-erased enqueue - transfers message ownership
        std::pair<bool, detail::enqueue_result> enqueue_impl(mailbox::message_ptr msg) const;

    private:
        address_t() noexcept;
        std::pmr::memory_resource* resource_;
        void* ptr_;
        enqueue_fn_t enqueue_fn_ = nullptr;
    };

    static_assert(!std::is_default_constructible_v<address_t>);
    static_assert(std::is_move_constructible_v<address_t>);
    static_assert(std::is_move_assignable_v<address_t>);
    static_assert(std::is_copy_constructible_v<address_t>);
    static_assert(std::is_copy_assignable_v<address_t>);

} // namespace actor_zeta::actor

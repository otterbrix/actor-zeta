#pragma once

#include <memory_resource>

#include <actor-zeta/detail/intrusive_ptr.hpp>
#include <actor-zeta/detail/queue/singly_linked.hpp>
#include <actor-zeta/detail/rtt.hpp>
#include <actor-zeta/mailbox/id.hpp>
#include <actor-zeta/mailbox/priority.hpp>

// Forward declarations to break include cycles
namespace actor_zeta { namespace actor {
    class address_t;
}} // namespace actor_zeta::actor
namespace actor_zeta { namespace detail {
    class future_state_base;
    template<typename T>
    class generator_state;
}} // namespace actor_zeta::detail
namespace actor_zeta {
    template<typename T>
    class promise;
}

namespace actor_zeta { namespace mailbox {

    class message final : public actor_zeta::detail::singly_linked<message> {
    public:
        message() = delete;
        message(const message&) = delete;
        message& operator=(const message&) = delete;
        message(message&& other) noexcept; // Cannot be default due to atomic fields
        message& operator=(message&& other) noexcept;

        explicit message(std::pmr::memory_resource* /* resource */);
        message(std::pmr::memory_resource* /* resource */, actor::address_t /*sender*/, message_id /*name*/, intrusive_ptr<actor_zeta::detail::future_state_base> result_slot);
        message(std::pmr::memory_resource* /* resource */, actor::address_t /*sender*/, message_id /*name*/, actor_zeta::detail::rtt&& /*body*/, intrusive_ptr<actor_zeta::detail::future_state_base> result_slot);

        // Allocator-extended move constructor (PMR migration)
        message(std::allocator_arg_t, std::pmr::memory_resource* resource, message&& other) noexcept;

        ~message() noexcept;
        message* prev;
        auto command() const noexcept -> message_id;
        auto sender() const noexcept -> actor::address_t;

        auto body() -> actor_zeta::detail::rtt&;
        operator bool();
        void swap(message& other) noexcept;
        bool is_high_priority() const;

        template<typename T>
        [[nodiscard]] actor_zeta::promise<T> get_result_promise() const noexcept;

        template<typename T>
        [[nodiscard]] actor_zeta::detail::generator_state<T>* get_generator_state() const noexcept;

    private:
        void* sender_;
        message_id command_;
        actor_zeta::detail::rtt body_;
        intrusive_ptr<actor_zeta::detail::future_state_base> result_slot_;
    };

    static_assert(std::is_move_constructible_v<message>);
    static_assert(!std::is_copy_constructible_v<message>);

    namespace detail {

        constexpr std::size_t align_up(std::size_t n, std::size_t a) {
            return (n + (a - 1)) & ~(a - 1);
        }

        constexpr std::size_t kAllocAlign =
            alignof(std::max_align_t) < alignof(message)
                ? alignof(message)
                : alignof(std::max_align_t);

        struct BlockHdr {
            std::pmr::memory_resource* r;
            std::size_t total;
        };

        constexpr std::size_t kFront = align_up(sizeof(BlockHdr), alignof(message));

        inline BlockHdr* hdr_from_message(void* pmsg) {
            unsigned char* base = static_cast<unsigned char*>(pmsg) - kFront;
            return reinterpret_cast<BlockHdr*>(base);
        }

        inline void* base_from_message(void* pmsg) {
            return static_cast<unsigned char*>(pmsg) - kFront;
        }

    } // namespace detail

    // Custom deleter for heap-allocated messages
    struct message_deleter {
        void operator()(message* p) const noexcept {
            if (!p)
                return;
            detail::BlockHdr* hdr = detail::hdr_from_message(p);
            p->~message();
            hdr->r->deallocate(detail::base_from_message(p), hdr->total, detail::kAllocAlign);
        }
    };

    static_assert(std::is_empty_v<message_deleter>, "EBO expected");

    using message_ptr = std::unique_ptr<message, message_deleter>;

    // Factory function for heap-allocated messages with PMR
    template<class... Args>
    message_ptr pmr_make_message(std::pmr::memory_resource* resource, Args&&... args) {
        constexpr std::size_t front = detail::kFront;
        constexpr std::size_t msg_size = sizeof(message);
        const std::size_t total = front + msg_size;

        void* raw = resource->allocate(total, detail::kAllocAlign);
        unsigned char* base = static_cast<unsigned char*>(raw);

        detail::BlockHdr* hdr = reinterpret_cast<detail::BlockHdr*>(base);
        hdr->r = resource;
        hdr->total = total;

        void* msg_place = base + front;
        message* msg = new (msg_place) message(std::forward<Args>(args)...);

        return message_ptr(msg, message_deleter{});
    }

}} // namespace actor_zeta::mailbox

inline void swap(actor_zeta::mailbox::message& lhs, actor_zeta::mailbox::message& rhs) noexcept {
    lhs.swap(rhs);
}

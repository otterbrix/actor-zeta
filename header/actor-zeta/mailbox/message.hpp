#pragma once

#include <cassert>
#include <atomic>
#include <memory_resource>

#include "actor-zeta/detail/memory.hpp"
#include <actor-zeta/actor/forwards.hpp>
#include <actor-zeta/detail/future_state.hpp>
#include <actor-zeta/detail/intrusive_ptr.hpp>
#include <actor-zeta/detail/queue/singly_linked.hpp>
#include <actor-zeta/detail/rtt.hpp>
#include <actor-zeta/mailbox/id.hpp>
#include <actor-zeta/mailbox/priority.hpp>

namespace actor_zeta {
    /// @brief Tag for internal constructors (public but convention: don't use in user code)
    struct internal_construct_tag {};
    template<typename T> class promise;
}

namespace actor_zeta { namespace mailbox {

    /// @brief Message with optional async result support

    class message final : public actor_zeta::detail::singly_linked<message> {
    public:
        message() = delete;
        message(const message&) = delete;
        message& operator=(const message&) = delete;
        message(message&& other) noexcept;  // Cannot be default due to atomic fields
        message& operator=(message&& other) noexcept;
        explicit message(std::pmr::memory_resource* /* resource */);
        message(std::pmr::memory_resource* /* resource */, actor::address_t /*sender*/, message_id /*name*/);
        message(std::pmr::memory_resource* /* resource */, actor::address_t /*sender*/, message_id /*name*/, actor_zeta::detail::rtt&& /*body*/);

        // Allocator-extended move constructor (PMR migration)
        message(std::allocator_arg_t, std::pmr::memory_resource* resource, message&& other) noexcept;

        ~message() noexcept;
        message* prev;
        auto command() const noexcept -> message_id;
        auto sender() & noexcept -> actor::address_t&;
        auto sender() && noexcept -> actor::address_t&&;
        auto sender() const& noexcept -> actor::address_t const&;

        auto body() -> actor_zeta::detail::rtt&;
        operator bool();
        void swap(message& other) noexcept;
        bool is_high_priority() const;

        /// @brief Get future state (result slot) - returns intrusive_ptr (copy, calls add_ref())
        /// This ensures future_state stays alive during method calls, preventing race conditions
        intrusive_ptr<actor_zeta::detail::future_state_base> result_slot() const noexcept { return result_slot_; }

        /// @brief Set future state (result slot) - accepts intrusive_ptr
        void set_result_slot(const intrusive_ptr<actor_zeta::detail::future_state_base>& slot) noexcept { result_slot_ = slot; }

        /// @brief Get current state from result_slot
        [[nodiscard]] actor_zeta::detail::future_state_enum state() const noexcept {
            return result_slot_ ? result_slot_->state() : actor_zeta::detail::future_state_enum::invalid;
        }

        /// @brief Get result promise (typed) for this message's result slot
        /// @note Uses internal_construct_tag to wrap existing state without allocating
        /// @note Returns promise that shares refcount with result_slot_
        /// @note Requires future.hpp to be included before instantiation
        template<typename T>
        [[nodiscard]] actor_zeta::promise<T> get_result_promise() const noexcept {
            auto* typed_state = static_cast<actor_zeta::detail::future_state<T>*>(result_slot_.get());
            return actor_zeta::promise<T>(
                actor_zeta::internal_construct_tag{},
                typed_state,
                typed_state ? typed_state->memory_resource() : nullptr
            );
        }

        /// @brief Check if message has a result slot
        [[nodiscard]] bool has_result_slot() const noexcept {
            return result_slot_ != nullptr;
        }

    private:
        actor::address_t sender_;
        message_id command_;
        actor_zeta::detail::rtt body_;

        // Future state for request-response pattern (automatic lifetime management via intrusive_ptr)
        intrusive_ptr<actor_zeta::detail::future_state_base> result_slot_;
    };

    static_assert(std::is_move_constructible<message>::value, "");
    static_assert(not std::is_copy_constructible<message>::value, "");

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

    }

    // Custom deleter for heap-allocated messages
    struct message_deleter {
        void operator()(message* p) const noexcept {
            if (!p) return;
            detail::BlockHdr* hdr = detail::hdr_from_message(p);
            p->~message();
            hdr->r->deallocate(detail::base_from_message(p), hdr->total, detail::kAllocAlign);
        }
    };

    static_assert(std::is_empty<message_deleter>::value, "EBO expected");

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

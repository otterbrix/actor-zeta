#pragma once

#include <cassert>
#include <atomic>

#include <actor-zeta/base/forwards.hpp>
#include <actor-zeta/mailbox/priority.hpp>
#include <actor-zeta/mailbox/id.hpp>
#include <actor-zeta/detail/rtt.hpp>
#include <actor-zeta/detail/slot_refcount.hpp>
#include <actor-zeta/detail/queue/singly_linked.hpp>
#include <actor-zeta/detail/memory_resource.hpp>
#include "actor-zeta/detail/memory.hpp"

namespace actor_zeta { namespace mailbox {

    /// @brief Unified slot status - combines state and error code
    /// Values 0-1: Normal states (pending, ready)
    /// Values 2+: Error states with specific error reasons
    enum class slot_error_code : int {
        pending = 0,            // Awaiting processing (initial state)
        ok = 1,                 // Result is ready (success)
        slot_pool_exhausted = 2,
        mailbox_full = 3,
        broken_promise = 4,     // Actor destroyed with pending futures
        cancelled = 5,          // Cancelled via future.cancel()
        orphaned = 6            // Future destroyed (forced cancellation)
    };

    /// @brief Message with optional async result support

    class message final : public actor_zeta::detail::singly_linked<message> {
    public:
        message() = delete;
        message(const message&) = delete;
        message& operator=(const message&) = delete;
        message(message&& other) noexcept;  // Cannot be default due to atomic fields
        message& operator=(message&& other) noexcept;
        explicit message(actor_zeta::pmr::memory_resource* /* resource */);
        message(actor_zeta::pmr::memory_resource* /* resource */, base::address_t /*sender*/, message_id /*name*/);
        message(actor_zeta::pmr::memory_resource* /* resource */, base::address_t /*sender*/, message_id /*name*/, actor_zeta::detail::rtt&& /*body*/);

        // Allocator-extended move constructor (PMR migration)
        message(std::allocator_arg_t, actor_zeta::pmr::memory_resource* resource, message&& other) noexcept;

        ~message() noexcept;
        message* prev;
        auto command() const noexcept -> message_id;
        auto sender() & noexcept -> base::address_t&;
        auto sender() && noexcept -> base::address_t&&;
        auto sender() const& noexcept -> base::address_t const&;

        auto body() -> actor_zeta::detail::rtt&;
        operator bool();
        void swap(message& other) noexcept;
        bool is_high_priority() const;

        actor_zeta::detail::slot_refcount* result_slot() const noexcept { return result_slot_; }
        void set_result_slot(actor_zeta::detail::slot_refcount* slot) noexcept { result_slot_ = slot; }

        /// @brief Get current error/status code
        slot_error_code error() const noexcept {
            return error_.load(std::memory_order_acquire);
        }

        /// @brief Set error/status code (atomic write)
        void set_error(slot_error_code e) noexcept {
            error_.store(e, std::memory_order_release);
        }

        void cancel() noexcept { cancelled_.store(true, std::memory_order_release); }
        bool is_cancelled() const noexcept { return cancelled_.load(std::memory_order_acquire); }

        void mark_orphaned() noexcept { orphaned_.store(true, std::memory_order_release); }
        bool is_orphaned() const noexcept { return orphaned_.load(std::memory_order_acquire); }

    private:
        base::address_t sender_;
        message_id command_;
        actor_zeta::detail::rtt body_;
        actor_zeta::detail::slot_refcount* result_slot_{nullptr};
        std::atomic<slot_error_code> error_{slot_error_code::pending};
        std::atomic<bool> cancelled_{false};
        std::atomic<bool> orphaned_{false};
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
            actor_zeta::pmr::memory_resource* r;
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
    message_ptr pmr_make_message(actor_zeta::pmr::memory_resource* resource, Args&&... args) {
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

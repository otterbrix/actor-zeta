#pragma once

#include <cassert>
#include <atomic>

#include <actor-zeta/base/forwards.hpp>
#include <actor-zeta/mailbox/priority.hpp>
#include <actor-zeta/mailbox/id.hpp>
#include <actor-zeta/detail/rtt.hpp>
#include <actor-zeta/detail/future_state.hpp>
#include <actor-zeta/detail/intrusive_ptr.hpp>
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

        /// @brief Get future state (result slot)
        actor_zeta::detail::future_state_base* result_slot() const noexcept { return result_slot_; }

        /// @brief Set future state (result slot)
        void set_result_slot(actor_zeta::detail::future_state_base* slot) noexcept { result_slot_ = slot; }

        /// @brief Get cancellation token (creates on first access if needed)
        intrusive_ptr<actor_zeta::detail::cancellation_token> cancellation_token() {
            if (!cancellation_token_) {
                cancellation_token_ = make_counted<actor_zeta::detail::cancellation_token>();
            }
            return cancellation_token_;
        }

        /// @brief Share cancellation token with future
        void set_cancellation_token(intrusive_ptr<actor_zeta::detail::cancellation_token> token) noexcept {
            cancellation_token_ = std::move(token);
        }

        /// @brief Check if cancelled (via shared cancellation token)
        bool is_cancelled() const noexcept {
            return cancellation_token_ && cancellation_token_->is_cancelled();
        }

        /// @brief Request cancellation (sets flag in shared token)
        void cancel() noexcept {
            if (cancellation_token_) {
                cancellation_token_->cancel();
            }
        }

        /// @brief Get current state from result_slot
        [[nodiscard]] actor_zeta::detail::future_state_enum state() const noexcept {
            return result_slot_ ? result_slot_->state() : actor_zeta::detail::future_state_enum::invalid;
        }

        // DEPRECATED: Kept for backward compatibility, will be removed in future versions
        slot_error_code error() const noexcept {
            auto s = state();
            if (s == actor_zeta::detail::future_state_enum::ready) return slot_error_code::ok;
            if (s == actor_zeta::detail::future_state_enum::cancelled) return slot_error_code::cancelled;
            return slot_error_code::pending;
        }

        void set_error(slot_error_code e) noexcept {
            // Map legacy error codes to new state enum
            if (!result_slot_) return;

            switch (e) {
                case slot_error_code::ok:
                    result_slot_->set_state(actor_zeta::detail::future_state_enum::ready);
                    break;
                case slot_error_code::cancelled:
                case slot_error_code::orphaned:
                    result_slot_->set_state(actor_zeta::detail::future_state_enum::cancelled);
                    break;
                case slot_error_code::broken_promise:
                case slot_error_code::mailbox_full:
                case slot_error_code::slot_pool_exhausted:
                    result_slot_->set_state(actor_zeta::detail::future_state_enum::error);
                    break;
                default:
                    break;
            }
        }

        void mark_orphaned() noexcept { cancel(); }  // Orphaned = cancelled
        bool is_orphaned() const noexcept { return is_cancelled(); }

    private:
        base::address_t sender_;
        message_id command_;
        actor_zeta::detail::rtt body_;

        // NEW: unified future state (replaces slot_refcount)
        actor_zeta::detail::future_state_base* result_slot_{nullptr};

        // NEW: shared cancellation token (lazy initialization)
        intrusive_ptr<actor_zeta::detail::cancellation_token> cancellation_token_;

        // REMOVED: atomic<slot_error_code> error_  (state now in future_state)
        // REMOVED: atomic<bool> cancelled_  (now in cancellation_token)
        // REMOVED: atomic<bool> orphaned_  (now in cancellation_token)
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

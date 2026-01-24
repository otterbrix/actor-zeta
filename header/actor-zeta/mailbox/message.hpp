#pragma once

#include <memory_resource>

#include <actor-zeta/actor/forwards.hpp>
#include <actor-zeta/detail/forwards.hpp>
#include <actor-zeta/detail/queue/singly_linked.hpp>
#include <actor-zeta/detail/rtt.hpp>
#include <actor-zeta/mailbox/forwards.hpp>
#include <actor-zeta/mailbox/id.hpp>

// Forward declarations for new shared_state
namespace actor_zeta::detail {
    template<typename T> struct shared_state;
}

namespace actor_zeta {
    template<typename T> class promise;
    template<typename T> class generator;
}

namespace actor_zeta { namespace mailbox {

    class message final : public actor_zeta::detail::singly_linked<message> {
    public:
        message() = delete;
        message(const message&) = delete;
        message& operator=(const message&) = delete;
        message(message&& other) noexcept;
        message& operator=(message&& other) noexcept;

        explicit message(std::pmr::memory_resource* /* resource */);
        message(std::pmr::memory_resource* /* resource */, message_id /*name*/);
        message(std::pmr::memory_resource* /* resource */, message_id /*name*/, actor_zeta::detail::rtt&& /*body*/);

        // Allocator-extended move constructor (PMR migration)
        message(std::allocator_arg_t, std::pmr::memory_resource* resource, message&& other) noexcept;

        ~message() noexcept;
        message* prev;
        auto command() const noexcept -> message_id;

        auto body() -> actor_zeta::detail::rtt&;
        operator bool();
        void swap(message& other) noexcept;
        bool is_high_priority() const;

        // === Unified type-erased result slot API ===

        // Initialize future slot with shared_state
        template<typename T>
        void init_future_slot(::actor_zeta::detail::shared_state<T>* state) noexcept {
            result_slot_ = state;
            cleanup_fn_ = [](void* p) {
                auto* s = static_cast<::actor_zeta::detail::shared_state<T>*>(p);
                s->set_error(std::make_error_code(std::errc::operation_canceled));
                s->release_promise();
            };
        }

        // Initialize generator slot with generator_state
        template<typename T>
        void init_generator_slot(::actor_zeta::detail::generator_state<T>* state) noexcept {
            result_slot_ = state;
            // Generator state uses refcount, add ref for message
            state->add_ref();
            cleanup_fn_ = [](void* p) {
                auto* s = static_cast<::actor_zeta::detail::generator_state<T>*>(p);
                s->release();  // decrement refcount
            };
        }

        // Get promise view for dispatch (returns promise that doesn't own the state)
        template<typename T>
        [[nodiscard]] actor_zeta::promise<T> get_result_promise() const noexcept;

        // Get generator state for dispatch
        template<typename T>
        [[nodiscard]] ::actor_zeta::detail::generator_state<T>* get_generator_state() const noexcept {
            return static_cast<::actor_zeta::detail::generator_state<T>*>(result_slot_);
        }

        // Transfer ownership from message to dispatch coroutine
        // After this call, ~message() will NOT call cleanup_fn_
        void transfer_ownership() noexcept {
            ownership_transferred_ = true;
        }

        // Check if slot is initialized
        [[nodiscard]] bool has_result_slot() const noexcept {
            return result_slot_ != nullptr;
        }

    private:
        message_id command_;
        actor_zeta::detail::rtt body_;

        // === Unified result slot ===
        void* result_slot_ = nullptr;
        void (*cleanup_fn_)(void*) = nullptr;
        bool ownership_transferred_ = false;
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

    // Implementation of message_deleter::operator() (declared in forwards.hpp)
    inline void message_deleter::operator()(message* p) const noexcept {
        if (!p)
            return;
        detail::BlockHdr* hdr = detail::hdr_from_message(p);
        p->~message();
        hdr->r->deallocate(detail::base_from_message(p), hdr->total, detail::kAllocAlign);
    }

    static_assert(std::is_empty_v<message_deleter>, "EBO expected");

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

// Include future.hpp for promise<T> full definition (needed for get_result_promise template)
#include <actor-zeta/detail/future.hpp>

// Template method implementation (must be in header for template instantiation)
namespace actor_zeta { namespace mailbox {

template<typename T>
actor_zeta::promise<T> message::get_result_promise() const noexcept {
    auto* state = static_cast<::actor_zeta::detail::shared_state<T>*>(result_slot_);
    return actor_zeta::promise<T>(state);
}

}} // namespace actor_zeta::mailbox

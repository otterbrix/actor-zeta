#pragma once

#include <atomic>
#include <actor-zeta/detail/queue/enqueue_result.hpp>

namespace actor_zeta { namespace detail {

    template<class T>
    class lifo_inbox {
    public:
        using value_type = T;
        using pointer = value_type*;
        using node_type = typename value_type::node_type;
        using node_pointer = node_type*;
        using unique_pointer = std::unique_ptr<value_type>;
        using deleter_type = typename unique_pointer::deleter_type;

        static pointer promote(node_pointer ptr) noexcept {
            return static_cast<pointer>(ptr);
        }

        enqueue_result push_front(pointer new_element) noexcept {
            assert(new_element != nullptr);
            pointer last_element = stack_.load();
            auto eof = stack_closed_tag();
            auto blk = reader_blocked_tag();
            while (last_element != eof) {
                new_element->next = last_element != blk ? last_element : nullptr;
                if (stack_.compare_exchange_strong(last_element, new_element))
                    return last_element == reader_blocked_tag() ? enqueue_result::unblocked_reader
                                                                : enqueue_result::success;
            }
            deleter_type deleter;
            deleter(new_element);
            return enqueue_result::queue_closed;
        }

        enqueue_result push_front(unique_pointer new_element) noexcept {
            return push_front(new_element.release());
        }

        template<class... Args>
        enqueue_result emplace_front(Args&&... args) {
            return push_front(new value_type(std::forward<Args>(args)...));
        }

        bool empty() const noexcept {
            assert(!closed());
            assert(!blocked());
            return stack_.load() == stack_empty_tag();
        }

        bool closed() const noexcept {
            return stack_.load() == stack_closed_tag();
        }

        bool blocked() const noexcept {
            return stack_.load() == reader_blocked_tag();
        }

        bool try_block() noexcept {
            auto empty_tag = stack_empty_tag();
            return stack_.compare_exchange_strong(empty_tag, reader_blocked_tag());
        }

        bool try_unblock() noexcept {
            auto blocked_tag = reader_blocked_tag();
            return stack_.compare_exchange_strong(blocked_tag, stack_empty_tag());
        }

        pointer take_head(pointer new_head) noexcept {
            assert(new_head == stack_closed_tag() || new_head == stack_empty_tag());
            pointer last_element = stack_.load();
            assert(last_element != stack_closed_tag());
            assert(last_element != reader_blocked_tag() || new_head == stack_closed_tag());

            while (last_element != new_head) {
                if (stack_.compare_exchange_weak(last_element, new_head)) {
                    assert(last_element != stack_closed_tag());
                    if (is_empty_or_blocked_tag(last_element)) {
                        assert(new_head == stack_closed_tag());
                        return nullptr;
                    }
                    return last_element;
                }
            }
            return nullptr;
        }

        pointer take_head() noexcept {
            return take_head(stack_empty_tag());
        }

        void close() {
            close(deleter_type{});
        }

        template<class F>
        /// TODO: old void close(F& func) noexcept(noexcept(func(std::declval<pointer>()))) {
        void close(F&& func) noexcept(noexcept(func(std::declval<pointer>()))) {
            node_pointer ptr = take_head(stack_closed_tag());
            while (ptr != nullptr) {
                auto next = ptr->next;
                func(promote(ptr));
                ptr = next;
            }
        }

        lifo_inbox() noexcept {
            stack_ = stack_empty_tag();
        }

        ~lifo_inbox() noexcept {
            if (!closed()) {
                close();
            }
        }

    private:
        static constexpr pointer stack_empty_tag() {
            return static_cast<pointer>(nullptr);
        }

        pointer stack_closed_tag() const noexcept {
            return reinterpret_cast<pointer>(reinterpret_cast<intptr_t>(this) + 1);
        }

        pointer reader_blocked_tag() const noexcept {
            return reinterpret_cast<pointer>(const_cast<lifo_inbox*>(this));
        }

        bool is_empty_or_blocked_tag(pointer x) const noexcept {
            return x == stack_empty_tag() || x == reader_blocked_tag();
        }

        std::atomic<pointer> stack_;
    };

}} // namespace actor_zeta::detail
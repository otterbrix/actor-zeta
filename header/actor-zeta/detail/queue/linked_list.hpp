#pragma once

#include <cassert>
#include <memory>

#include "forward_iterator.hpp"

namespace actor_zeta { namespace detail {

    template<class T>
    class linked_list {
    public:
        using value_type = T;
        using node_type = typename value_type::node_type;
        using node_pointer = node_type*;
        using pointer = value_type*;
        using const_pointer = const value_type*;
        using reference = value_type&;
        using const_reference = const value_type&;
        using unique_pointer = std::unique_ptr<T>;
        using iterator = forward_iterator<value_type>;
        using const_iterator = forward_iterator<const value_type>;
        static pointer promote(node_pointer ptr) noexcept {
            return static_cast<pointer>(ptr);
        }
        linked_list() noexcept = default;
        linked_list(linked_list&& other) noexcept {
            if (!other.empty()) {
                head_.next = other.head_.next;
                tail_.next = other.tail_.next;
                tail_.next->next = &tail_;
                size_ = other.size_;
                other.init();
            }
        }

        linked_list& operator=(linked_list&& other) noexcept {
            clear();
            if (!other.empty()) {
                head_.next = other.head_.next;
                tail_.next = other.tail_.next;
                tail_.next->next = &tail_;
                size_ = other.size_;
                other.init();
            }
            return *this;
        }

        linked_list(const linked_list&) = delete;

        linked_list& operator=(const linked_list&) = delete;

        ~linked_list() {
            clear();
        }

        size_t size() const noexcept {
            return size_;
        }

        bool empty() const noexcept {
            return size() == 0;
        }

        void clear() noexcept {
            typename unique_pointer::deleter_type fn;
            drain(fn);
        }

        unique_pointer pop_front() {
            unique_pointer result;
            if (!empty()) {
                auto ptr = promote(head_.next);
                head_.next = ptr->next;
                if (--size_ == 0) {
                    assert(head_.next == &tail_);
                    tail_.next = &head_;
                }
                result.reset(ptr);
            }
            return result;
        }

        iterator begin() noexcept {
            return head_.next;
        }

        const_iterator begin() const noexcept {
            return head_.next;
        }

        const_iterator cbegin() const noexcept {
            return begin();
        }

        iterator end() noexcept {
            return &tail_;
        }

        const_iterator end() const noexcept {
            return &tail_;
        }

        const_iterator cend() const noexcept {
            return end();
        }

        iterator before_begin() noexcept {
            return &head_;
        }

        iterator before_end() noexcept {
            return tail_.next;
        }

        pointer front() noexcept {
            return promote(head_.next);
        }

        pointer back() noexcept {
            CAF_ASSERT(head_.next != &tail_);
            return promote(tail_.next);
        }

        pointer peek() noexcept {
            return size_ > 0 ? front() : nullptr;
        }

        void push_back(pointer ptr) noexcept {
            assert(ptr != nullptr);
            tail_.next->next = ptr;
            tail_.next = ptr;
            ptr->next = &tail_;
            ++size_;
        }

        void push_back(unique_pointer ptr) noexcept {
            push_back(ptr.release());
        }

        template<class... Ts>
        void emplace_back(Ts&&... xs) {
            push_back(new value_type(std::forward<Ts>(xs)...));
        }

        void push_front(pointer ptr) noexcept {
            assert(ptr != nullptr);
            if (empty()) {
                push_back(ptr);
                return;
            }
            ptr->next = head_.next;
            head_.next = ptr;
            ++size_;
        }

        void push_front(unique_pointer ptr) noexcept {
            push_front(ptr.release());
        }

        template<class... Ts>
        void emplace_front(Ts&&... xs) {
            push_front(new value_type(std::forward<Ts>(xs)...));
        }

        iterator insert_after(iterator pos, pointer ptr) {
            assert(pos != end());
            assert(ptr != nullptr);
            auto next = pos->next;
            ptr->next = next;
            pos->next = ptr;
            if (next == &tail_)
                tail_.next = ptr;
            ++size_;
            return iterator{ptr};
        }

        template<class F>
        void drain(F fn) {
            for (auto i = head_.next; i != &tail_;) {
                auto ptr = i;
                i = i->next;
                fn(promote(ptr));
            }
            init();
        }

    private:
        void init() noexcept {
            head_.next = &tail_;
            tail_.next = &head_;
            size_ = 0;
        }

        node_type head_{&tail_};

        node_type tail_{&head_};

        size_t size_ = 0;
    };

}} // namespace actor_zeta::detail
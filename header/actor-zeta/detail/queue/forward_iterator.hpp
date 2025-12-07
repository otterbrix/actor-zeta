#pragma once

#include <cstddef>
#include <iterator>
#include <type_traits>

namespace actor_zeta { namespace detail {

    template<class T>
    class forward_iterator {
    public:
        using difference_type = std::ptrdiff_t;
        using value_type = T;
        using pointer = value_type*;
        using const_pointer = const value_type*;
        using reference = value_type&;
        using const_reference = const value_type&;
        using node_type = typename std::conditional_t<std::is_const_v<T>, const typename T::node_type, typename T::node_type>;
        using node_pointer = node_type*;
        using iterator_category = std::forward_iterator_tag;

        static pointer promote(node_pointer ptr) noexcept {
            return static_cast<pointer>(ptr);
        }

        constexpr forward_iterator(node_pointer init = nullptr)
            : ptr(init) {}
        forward_iterator(const forward_iterator&) = default;
        forward_iterator& operator=(const forward_iterator&) = default;

        forward_iterator next() {
            return ptr->next;
        }

        forward_iterator& operator++() {
            ptr = ptr->next;
            return *this;
        }

        forward_iterator operator++(int) {
            forward_iterator res = *this;
            ptr = ptr->next;
            return res;
        }

        reference operator*() {
            return *promote(ptr);
        }

        const_reference operator*() const {
            return *promote(ptr);
        }

        pointer operator->() {
            return promote(ptr);
        }

        const_pointer operator->() const {
            return promote(ptr);
        }

        node_pointer ptr;
    };

    template<class T>
    bool operator==(const forward_iterator<T>& x, const forward_iterator<T>& y) {
        return x.ptr == y.ptr;
    }

    template<class T>
    bool operator!=(const forward_iterator<T>& x, const forward_iterator<T>& y) {
        return x.ptr != y.ptr;
    }

}} // namespace actor_zeta::detail
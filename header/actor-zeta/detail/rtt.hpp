#pragma once

#include <actor-zeta/detail/memory_resource.hpp>
#include <actor-zeta/detail/pmr/aligned_allocate.hpp>
#include <actor-zeta/detail/type_list.hpp>
#include <actor-zeta/detail/type_traits.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace actor_zeta { namespace detail {

#ifdef __ENABLE_TESTS_MEASUREMENTS__

    namespace rtt_test {

        static size_t default_ctor_ = 0;
        static size_t move_ctor_ = 0;
        static size_t templated_ctor_ = 0;
        static size_t dtor_ = 0;

        static size_t move_operator_ = 0;

        inline void clear() {
            default_ctor_ = 0;
            move_ctor_ = 0;
            templated_ctor_ = 0;
            dtor_ = 0;
            move_operator_ = 0;
        }

    } // namespace rtt_test

#endif

    inline void dummy(std::initializer_list<int>) {}

#define EXPAND_VARIADIC(expression) \
    dummy({(expression, 0)...})

    template<size_t N>
    constexpr size_t getSize() {
        return N;
    }

    template<size_t N, class Head, class... Args>
    constexpr size_t getSize() {
        return getSize<((N + alignof(Head) - 1) & -(alignof(Head))) + sizeof(Head), Args...>();
    }

    template<typename T>
    void destroy(void* object) noexcept {
        static_cast<T*>(object)->~T();
    }

    class rtt final {
        using destroyer_t = void (*)(void*);

        struct objects_t {
            std::size_t offset;
            destroyer_t destroyer;
        };

        actor_zeta::pmr::memory_resource* memory_resource_ = nullptr;

        std::size_t capacity_ = 0;
        std::size_t volume_ = 0;

        void* allocation = nullptr;
        char* data_ = nullptr;

        objects_t* objects_ = nullptr;
        std::size_t objects_idx_ = 0;

        // TODO mark noexcept?
        void clear() {
            auto tmp = data_;
            for (std::size_t i = 0; i < objects_idx_; ++i) {
                objects_[i].destroyer(tmp + objects_[i].offset);
            }

            // Note: capacity_ stores the actual data size, not aligned size
            // So we need to recalculate aligned size for deallocation
            // However, the actual allocated size is implementation-defined
            // For simplicity, we'll use the formula that matches allocation
            if (allocation) {
                // Align capacity to objects_t boundary
                constexpr std::size_t objects_alignment = alignof(objects_t);
                std::size_t aligned_capacity = (capacity_ + objects_alignment - 1) & ~(objects_alignment - 1);
                std::size_t allocated_size = aligned_capacity + objects_idx_ * sizeof(objects_t);
                memory_resource_->deallocate(allocation, allocated_size);
            }

            volume_ = 0;
            objects_idx_ = 0;
            allocation = nullptr;
            data_ = nullptr;
            objects_ = nullptr;
            capacity_ = 0;
        }

    public:
        template<typename... Args>
        explicit rtt(actor_zeta::pmr::memory_resource* resource, Args&&... args)
            : memory_resource_([](actor_zeta::pmr::memory_resource* resource) {assert(resource);return resource; }(resource))
            , capacity_(0)
            , volume_(0)
            , allocation(nullptr)
            , data_(nullptr)
            , objects_(nullptr)
            , objects_idx_(0) {

            // Empty optimization - if no arguments, don't allocate
            if constexpr (sizeof...(Args) == 0) {
#ifdef __ENABLE_TESTS_MEASUREMENTS__
                rtt_test::templated_ctor_++;
#endif
                return;
            }

            constexpr std::size_t sz = getSize<0, Args...>();
            capacity_ = sz;

            // Align capacity to objects_t boundary to prevent misaligned access
            constexpr std::size_t objects_alignment = alignof(objects_t);
            std::size_t aligned_capacity = (capacity_ + objects_alignment - 1) & ~(objects_alignment - 1);

            allocation = memory_resource_->allocate(aligned_capacity + sizeof...(Args) * sizeof(objects_t));
            assert(allocation);
            data_ = static_cast<char*>(allocation);
            assert(data_);
            objects_ = static_cast<objects_t*>(static_cast<void*>(data_ + aligned_capacity));
            assert(objects_);

            EXPAND_VARIADIC(push_back_no_realloc(std::forward<Args>(args)));

#ifdef __ENABLE_TESTS_MEASUREMENTS__
            rtt_test::templated_ctor_++;
#endif
        }

        rtt(actor_zeta::pmr::memory_resource* resource)
            : memory_resource_([](actor_zeta::pmr::memory_resource* resource) {assert(resource);return resource; }(resource))
            , capacity_(0)
            , volume_(0)
            , allocation(nullptr)
            , data_(nullptr)
            , objects_(nullptr)
            , objects_idx_(0) {
#ifdef __ENABLE_TESTS_MEASUREMENTS__
            rtt_test::default_ctor_++;
#endif
        }

        rtt() = delete;

        rtt(rtt&& other) noexcept
            : memory_resource_(other.memory_resource_)
            , capacity_(other.capacity_)
            , volume_(other.volume_)
            , allocation(other.allocation)
            , data_(other.data_)
            , objects_(other.objects_)
            , objects_idx_(other.objects_idx_) {
            other.memory_resource_ = nullptr;
            other.capacity_ = 0;
            other.volume_ = 0;
            other.allocation = nullptr;
            other.data_ = nullptr;
            other.objects_ = nullptr;
            other.objects_idx_ = 0;
#ifdef __ENABLE_TESTS_MEASUREMENTS__
            rtt_test::move_ctor_++;
#endif
        }


        rtt(std::allocator_arg_t, actor_zeta::pmr::memory_resource* resource, rtt&& other) noexcept
            : memory_resource_([](actor_zeta::pmr::memory_resource* resource) {assert(resource);return resource; }(resource))
            , capacity_(0)
            , volume_(0)
            , allocation(nullptr)
            , data_(nullptr)
            , objects_(nullptr)
            , objects_idx_(0) {

            // Only same-arena move is supported for type-erased container
            // Cross-arena migration would require copying non-trivial types which is not possible
            // without knowing their actual types at runtime
            assert(resource == other.memory_resource_ && "Cross-arena RTT migration is not supported");

            // Same arena - cheap move (just steal pointers)
            capacity_ = other.capacity_;
            volume_ = other.volume_;
            allocation = other.allocation;
            data_ = other.data_;
            objects_ = other.objects_;
            objects_idx_ = other.objects_idx_;

            other.memory_resource_ = nullptr;
            other.capacity_ = 0;
            other.volume_ = 0;
            other.allocation = nullptr;
            other.data_ = nullptr;
            other.objects_ = nullptr;
            other.objects_idx_ = 0;

#ifdef __ENABLE_TESTS_MEASUREMENTS__
            rtt_test::move_ctor_++;
#endif
        }
        rtt(const rtt& other) = delete;
        rtt(rtt& other) = delete;
        ~rtt() {
            clear();
        }

        rtt& operator=(rtt&& other) noexcept {
            if (this == &other) return *this;

            // Only same-arena move assignment is supported
            // If different arenas, we require explicit handling by the user
            assert((memory_resource_ == other.memory_resource_ || memory_resource_ == nullptr)
                   && "Cross-arena RTT move assignment is not supported");

            clear();

            memory_resource_ = other.memory_resource_;
            capacity_ = other.capacity_;
            volume_ = other.volume_;
            allocation = other.allocation;
            data_ = other.data_;
            objects_ = other.objects_;
            objects_idx_ = other.objects_idx_;

            other.memory_resource_ = nullptr;
            other.capacity_ = 0;
            other.volume_ = 0;
            other.allocation = nullptr;
            other.data_ = nullptr;
            other.objects_ = nullptr;
            other.objects_idx_ = 0;

#ifdef __ENABLE_TESTS_MEASUREMENTS__
            rtt_test::move_operator_++;
#endif

            return *this;
        }

        rtt& operator=(const rtt& other) = delete;
        rtt& operator=(rtt& other) = delete;

        template<typename T>
        char* try_to_align(const T&) {
            auto space_left = capacity_ - volume_;
            void* creation_place = data_ + volume_;
            auto aligned_place = actor_zeta::detail::align(alignof(T), sizeof(T), creation_place, space_left);
            return static_cast<char*>(aligned_place);
        }

        template<typename T>
        char* force_align(const T& object) {
            auto creation_place = try_to_align(object);
            assert(creation_place != nullptr);

            return creation_place;
        }

        template<typename T>
        void push_back_no_realloc(T&& object) {
            auto creation_place = force_align(object);

            using raw_type = actor_zeta::type_traits::decay_t<T>;
            new (creation_place) raw_type(std::forward<T>(object));
            const auto new_offset = static_cast<std::size_t>(creation_place - data_);
            objects_[objects_idx_++] = objects_t{new_offset, &destroy<raw_type>};
            volume_ = new_offset + sizeof(raw_type);
        }

        void swap(rtt& that) {
            using std::swap;

            swap(this->memory_resource_, that.memory_resource_);
            swap(this->capacity_, that.capacity_);
            swap(this->volume_, that.volume_);
            swap(this->allocation, that.allocation);
            swap(this->data_, that.data_);
            swap(this->objects_, that.objects_);
            swap(this->objects_idx_, that.objects_idx_);
        }

        template<typename T>
        const T& get(std::size_t index) const {
            return get_by_offset<T>(offset(index));
        }

        template<typename T>
        T& get(std::size_t index) {
            return get_by_offset<T>(offset(index));
        }

        std::size_t offset(std::size_t index) const {
            return objects_[index].offset;
        }

        template<typename T>
        const T& get_by_offset(std::size_t offset) const {
            return *static_cast<const T*>(static_cast<const void*>(data_ + offset));
        }

        template<typename T>
        T& get_by_offset(std::size_t offset) {
            return *static_cast<T*>(static_cast<void*>(data_ + offset)); // this is necessary to use double static_cast here, compile error
        }

        std::size_t size() const {
            return objects_idx_;
        }

        std::size_t volume() const {
            return volume_;
        }

        std::size_t capacity() const {
            return capacity_;
        }

        bool empty() const {
            return objects_idx_ == 0;
        }

        /// @brief Get memory resource used by this rtt
        /// @return Pointer to memory resource, or nullptr if rtt was moved-from
        actor_zeta::pmr::memory_resource* memory_resource() const noexcept {
            return memory_resource_;
        }
    };

    inline void swap(rtt& left, rtt& right) {
        left.swap(right);
    }

    /// @brief Extract argument from rtt at index I according to type list List
    ///
    /// Handles different parameter types:
    /// - const T&: return const reference to stored value (no copy, no move)
    /// - T: use std::move to support move-only types (e.g., unique_ptr)
    ///
    /// Note: Non-const lvalue references (T&) are forbidden by dispatch() static_assert
    template<std::size_t I, class List>
    auto get(rtt& r) -> typename type_traits::type_list_at_t<List, I> {
        using requested_type = typename type_traits::type_list_at_t<List, I>;
        using decay_type = typename type_traits::decay_t<requested_type>;

        if constexpr (std::is_lvalue_reference_v<requested_type>) {
            // For const T& (non-const T& is forbidden by dispatch static_assert)
            // Return reference to stored value
            return r.get<decay_type>(I);
        } else {
            // For value types: use std::move to support move-only types
            return std::move(r.get<decay_type>(I));
        }
    }

}} // namespace actor_zeta::detail

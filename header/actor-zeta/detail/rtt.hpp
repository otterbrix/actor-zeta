#pragma once

#include <memory_resource>
#include <actor-zeta/detail/type_list.hpp>
#include <actor-zeta/detail/type_traits.hpp>

#include <cassert>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

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

    constexpr std::size_t align_up(std::size_t n, std::size_t align) noexcept {
        return (n + align - 1) & ~(align - 1);
    }

    inline std::pmr::memory_resource* check_resource(std::pmr::memory_resource* r) noexcept {
        assert(r && "memory_resource must not be nullptr");
        return r;
    }

    template<std::size_t N>
    constexpr std::size_t getSize() {
        return N;
    }

    template<std::size_t N, class Head, class... Args>
    constexpr std::size_t getSize() {
        return getSize<align_up(N, alignof(Head)) + sizeof(Head), Args...>();
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

        std::pmr::memory_resource* memory_resource_ = nullptr;

        std::size_t capacity_ = 0;
        std::size_t volume_ = 0;

        void* allocation_ = nullptr;
        char* data_ = nullptr;

        objects_t* objects_ = nullptr;
        std::size_t objects_idx_ = 0;

        void clear() noexcept {
            auto tmp = data_;
            // Destroy in reverse order (LIFO)
            for (std::size_t i = objects_idx_; i-- > 0; ) {
                objects_[i].destroyer(tmp + objects_[i].offset);
            }

            if (allocation_) {
                std::size_t aligned_capacity = align_up(capacity_, alignof(objects_t));
                std::size_t allocated_size = aligned_capacity + objects_idx_ * sizeof(objects_t);
                memory_resource_->deallocate(allocation_, allocated_size);
            }

            volume_ = 0;
            objects_idx_ = 0;
            allocation_ = nullptr;
            data_ = nullptr;
            objects_ = nullptr;
            capacity_ = 0;
        }

        template<typename T>
        char* try_to_align() {
            auto space_left = capacity_ - volume_;
            void* creation_place = data_ + volume_;
            auto aligned_place = std::align(alignof(T), sizeof(T), creation_place, space_left);
            return static_cast<char*>(aligned_place);
        }

        template<typename T>
        char* force_align() {
            auto creation_place = try_to_align<T>();
            assert(creation_place != nullptr);

            return creation_place;
        }

        template<typename T>
        void push_back_no_realloc(T&& object) {
            using raw_type = actor_zeta::type_traits::decay_t<T>;
            auto creation_place = force_align<raw_type>();
            new (creation_place) raw_type(std::forward<T>(object));
            const auto new_offset = static_cast<std::size_t>(creation_place - data_);
            objects_[objects_idx_++] = objects_t{new_offset, &destroy<raw_type>};
            volume_ = new_offset + sizeof(raw_type);
        }

        std::size_t offset(std::size_t index) const {
            assert(index < objects_idx_ && "rtt::offset(): index out of bounds");
            return objects_[index].offset;
        }

        template<typename T>
        const T& get_by_offset(std::size_t offset) const {
            return *static_cast<const T*>(static_cast<const void*>(data_ + offset));
        }

        template<typename T>
        T& get_by_offset(std::size_t offset) {
            return *static_cast<T*>(static_cast<void*>(data_ + offset));
        }

    public:
        template<typename... Args>
        explicit rtt(std::pmr::memory_resource* resource, Args&&... args)
            : memory_resource_(check_resource(resource))
            , capacity_(0)
            , volume_(0)
            , allocation_(nullptr)
            , data_(nullptr)
            , objects_(nullptr)
            , objects_idx_(0) {

            if constexpr (sizeof...(Args) == 0) {
#ifdef __ENABLE_TESTS_MEASUREMENTS__
                rtt_test::templated_ctor_++;
#endif
                return;
            }

            constexpr std::size_t sz = getSize<0, Args...>();
            constexpr std::size_t aligned_capacity = align_up(sz, alignof(objects_t));
            constexpr std::size_t total_size = aligned_capacity + sizeof...(Args) * sizeof(objects_t);

            capacity_ = sz;
            allocation_ = memory_resource_->allocate(total_size);
            assert(allocation_);
            data_ = static_cast<char*>(allocation_);
            assert(data_);
            objects_ = static_cast<objects_t*>(static_cast<void*>(data_ + aligned_capacity));
            assert(objects_);

            (push_back_no_realloc(std::forward<Args>(args)), ...);

#ifdef __ENABLE_TESTS_MEASUREMENTS__
            rtt_test::templated_ctor_++;
#endif
        }

        rtt(std::pmr::memory_resource* resource)
            : memory_resource_(check_resource(resource))
            , capacity_(0)
            , volume_(0)
            , allocation_(nullptr)
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
            , allocation_(other.allocation_)
            , data_(other.data_)
            , objects_(other.objects_)
            , objects_idx_(other.objects_idx_) {
            other.memory_resource_ = nullptr;
            other.capacity_ = 0;
            other.volume_ = 0;
            other.allocation_ = nullptr;
            other.data_ = nullptr;
            other.objects_ = nullptr;
            other.objects_idx_ = 0;
#ifdef __ENABLE_TESTS_MEASUREMENTS__
            rtt_test::move_ctor_++;
#endif
        }

        rtt(std::allocator_arg_t, std::pmr::memory_resource* resource, rtt&& other) noexcept
            : memory_resource_(check_resource(resource))
            , capacity_(0)
            , volume_(0)
            , allocation_(nullptr)
            , data_(nullptr)
            , objects_(nullptr)
            , objects_idx_(0) {
            // Only same-arena move is supported
            assert(resource == other.memory_resource_ && "Cross-arena RTT migration is not supported");

            capacity_ = other.capacity_;
            volume_ = other.volume_;
            allocation_ = other.allocation_;
            data_ = other.data_;
            objects_ = other.objects_;
            objects_idx_ = other.objects_idx_;

            other.memory_resource_ = nullptr;
            other.capacity_ = 0;
            other.volume_ = 0;
            other.allocation_ = nullptr;
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
#ifdef __ENABLE_TESTS_MEASUREMENTS__
            rtt_test::dtor_++;
#endif
        }

        rtt& operator=(rtt&& other) noexcept {
            if (this == &other) return *this;

            assert((memory_resource_ == other.memory_resource_ || memory_resource_ == nullptr)
                   && "Cross-arena RTT move assignment is not supported");

            clear();

            memory_resource_ = other.memory_resource_;
            capacity_ = other.capacity_;
            volume_ = other.volume_;
            allocation_ = other.allocation_;
            data_ = other.data_;
            objects_ = other.objects_;
            objects_idx_ = other.objects_idx_;

            other.memory_resource_ = nullptr;
            other.capacity_ = 0;
            other.volume_ = 0;
            other.allocation_ = nullptr;
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
        const T& get(std::size_t index) const {
            return get_by_offset<T>(offset(index));
        }

        template<typename T>
        T& get(std::size_t index) {
            return get_by_offset<T>(offset(index));
        }

        std::size_t size() const noexcept {
            return objects_idx_;
        }

        std::size_t volume() const noexcept {
            return volume_;
        }

        std::size_t capacity() const noexcept {
            return capacity_;
        }

        bool empty() const noexcept {
            return objects_idx_ == 0;
        }

        std::pmr::memory_resource* memory_resource() const noexcept {
            return memory_resource_;
        }
    };

    template<std::size_t I, class List>
    auto get(rtt& r) -> typename type_traits::type_list_at_t<List, I> {
        using requested_type = typename type_traits::type_list_at_t<List, I>;
        using decay_type = typename type_traits::decay_t<requested_type>;

        if constexpr (std::is_lvalue_reference_v<requested_type>) {
            return r.get<decay_type>(I);
        } else {
            return std::move(r.get<decay_type>(I));
        }
    }

}} // namespace actor_zeta::detail
#pragma once

#include <string>

#include "forwards.hpp"
#include <actor-zeta/detail/memory_resource.hpp>
#include <actor-zeta/detail/ref_counted.hpp>
#include <actor-zeta/mailbox/message.hpp>

namespace actor_zeta { namespace base {
    ///
    /// @brief Abstract concept of an actor
    ///

    class actor_abstract_t : public ref_counted {
    public:
        struct placement_tag {};
        constexpr static placement_tag placement = {};

        static void* operator new(size_t, void* ptr, placement_tag) noexcept {
            return ptr;
        }

        static void operator delete(void*, void*, placement_tag) noexcept {}

        static void operator delete(void* ptr) noexcept;

        static void* operator new(size_t, void* ptr) = delete;
        static void* operator new(size_t) = delete;
        static void* operator new[](size_t) = delete;
        static void operator delete[](void*) = delete;

        virtual ~actor_abstract_t();

        class id_t final {
        public:
            id_t() = delete;

            explicit id_t(actor_abstract_t* impl) noexcept
                : impl_{impl} {
            }

            bool operator==(id_t const& other) const noexcept {
                return impl_ == other.impl_;
            }

            bool operator!=(id_t const& other) const noexcept {
                return impl_ != other.impl_;
            }

            bool operator<(id_t const& other) const noexcept {
                return impl_ < other.impl_;
            }

            bool operator>(id_t const& other) const noexcept {
                return other.impl_ < impl_;
            }

            bool operator<=(id_t const& other) const noexcept {
                return !(*this > other);
            }

            bool operator>=(id_t const& other) const noexcept {
                return !(*this < other);
            }

            template<typename charT, class traitsT>
            friend std::basic_ostream<charT, traitsT>&
            operator<<(std::basic_ostream<charT, traitsT>& os, id_t const& other) {
                if (nullptr != other.impl_) {
                    return os << other.impl_;
                }
                return os << "{not-valid}";
            }

            explicit operator bool() const noexcept {
                return nullptr != impl_;
            }

            bool operator!() const noexcept {
                return nullptr == impl_;
            }

        private:
            actor_abstract_t* impl_{nullptr};
        };

        address_t address() noexcept;
        id_t id() const;
        bool enqueue(mailbox::message_ptr);
        pmr::memory_resource* resource() const noexcept;

        template<class T>
        pmr::polymorphic_allocator<T> allocator() const noexcept {
            return {resource_};
        }

    protected:
        explicit actor_abstract_t(pmr::memory_resource* resource);

        actor_abstract_t() = delete;
        actor_abstract_t(const actor_abstract_t&) = delete;
        actor_abstract_t& operator=(const actor_abstract_t&) = delete;
        actor_abstract_t(actor_abstract_t&&) = delete;
        actor_abstract_t& operator=(actor_abstract_t&&) = delete;

        virtual bool enqueue_impl(mailbox::message_ptr) = 0;

    private:
        actor_zeta::pmr::memory_resource* resource_;
    };

}} // namespace actor_zeta::base

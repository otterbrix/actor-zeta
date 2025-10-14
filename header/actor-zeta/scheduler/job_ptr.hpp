#pragma once

#include <type_traits>
#include <cstddef>
#include <cassert>

#include "resumable.hpp"
#include "forwards.hpp"

namespace actor_zeta { namespace scheduler {

    namespace detail {
        // Template functions for type erasure - generated at compile-time for each type T
        template<class T>
        resume_info resume_impl(void* ptr, size_t max_throughput) {
            assert(ptr != nullptr);
            return static_cast<T*>(ptr)->resume(max_throughput);
        }

        template<class T>
        void add_ref_impl(void* ptr) {
            assert(ptr != nullptr);
            // ADL will find the right intrusive_ptr_add_ref in T's namespace
            T* typed_ptr = static_cast<T*>(ptr);
            intrusive_ptr_add_ref(typed_ptr);
        }

        template<class T>
        void release_impl(void* ptr) {
            assert(ptr != nullptr);
            // ADL will find the right intrusive_ptr_release in T's namespace
            T* typed_ptr = static_cast<T*>(ptr);
            intrusive_ptr_release(typed_ptr);
        }

        // SFINAE check for resume method
        template<class T>
        struct has_resume_method {
        private:
            template<class U>
            static auto test(int) -> decltype(
                std::declval<U*>()->resume(std::declval<size_t>()),
                std::true_type{}
            );

            template<class>
            static std::false_type test(...);

        public:
            static constexpr bool value = decltype(test<T>(0))::value;
        };

    } // namespace detail

    /// @brief Type-erased job pointer for scheduler
    ///
    /// Uses function pointers for type erasure instead of virtual functions.
    /// Each type T gets its own compile-time generated function pointers.
    /// This is a POD structure that can be efficiently passed by value.
    struct job_ptr {
        void* ptr;
        resume_info (*resume_fn)(void*, size_t);
        void (*add_ref_fn)(void*);
        void (*release_fn)(void*);

        /// @brief Create type-erased job pointer from typed pointer
        /// @tparam T Type that has resume(size_t) method and intrusive_ptr support
        /// @param p Pointer to object to wrap
        /// @return Type-erased job_ptr
        template<class T>
        static job_ptr wrap(T* p) {
            static_assert(detail::has_resume_method<T>::value,
                         "Type T must have 'resume_info resume(size_t)' method");

            assert(p != nullptr && "Cannot wrap nullptr");

            return job_ptr{
                p,
                &detail::resume_impl<T>,
                &detail::add_ref_impl<T>,
                &detail::release_impl<T>
            };
        }

        /// @brief Resume execution of the job
        /// @param max_throughput Maximum number of operations to perform
        /// @return Resume information with result status and messages processed
        resume_info resume(size_t max_throughput) const {
            assert(ptr != nullptr && "Cannot resume null job");
            assert(resume_fn != nullptr && "Resume function is null");
            return resume_fn(ptr, max_throughput);
        }

        /// @brief Increment reference count
        void add_ref() const {
            assert(ptr != nullptr && "Cannot add_ref null job");
            assert(add_ref_fn != nullptr && "add_ref function is null");
            add_ref_fn(ptr);
        }

        /// @brief Decrement reference count (may destroy object)
        void release() const {
            assert(ptr != nullptr && "Cannot release null job");
            assert(release_fn != nullptr && "release function is null");
            release_fn(ptr);
        }

        /// @brief Check if job_ptr is valid (not null)
        explicit operator bool() const noexcept {
            return ptr != nullptr;
        }

        /// @brief Get raw pointer (use with caution)
        void* raw_ptr() const noexcept {
            return ptr;
        }
    };

    // Inline function definitions for intrusive_ptr support on job_ptr itself
    inline void intrusive_ptr_add_ref(const job_ptr& job) {
        job.add_ref();
    }

    inline void intrusive_ptr_release(const job_ptr& job) {
        job.release();
    }

}} // namespace actor_zeta::scheduler
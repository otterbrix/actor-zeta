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

        /// @brief Create type-erased job pointer from typed pointer
        /// @tparam T Type that has resume(size_t) method
        /// @param p Pointer to object to wrap (not owned by job_ptr)
        /// @return Type-erased job_ptr
        template<class T>
        static job_ptr wrap(T* p) {
            static_assert(detail::has_resume_method<T>::value,
                         "Type T must have 'resume_info resume(size_t)' method");

            assert(p != nullptr && "Cannot wrap nullptr");

            return job_ptr{
                p,
                &detail::resume_impl<T>
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

        /// @brief Check if job_ptr is valid (not null)
        explicit operator bool() const noexcept {
            return ptr != nullptr;
        }

        /// @brief Get raw pointer (use with caution)
        void* raw_ptr() const noexcept {
            return ptr;
        }
    };

}} // namespace actor_zeta::scheduler
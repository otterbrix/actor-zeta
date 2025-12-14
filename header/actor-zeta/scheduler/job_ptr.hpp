#pragma once

#include <cassert>
#include <concepts>
#include <cstddef>
#include <type_traits>

#include "forwards.hpp"
#include "resumable.hpp"
#include <actor-zeta/detail/queue/singly_linked.hpp>

namespace actor_zeta { namespace scheduler {

    namespace detail {
        // Template functions for type erasure - generated at compile-time for each type T
        template<class T>
        resume_info resume_impl(void* ptr, size_t max_throughput) {
            assert(ptr != nullptr);
            return static_cast<T*>(ptr)->resume(max_throughput);
        }

        /// @brief Concept to check if type has resume(size_t) method
        template<typename T>
        concept resumable_type = requires(T* t, size_t n) {
            { t->resume(n) } -> std::same_as<resume_info>;
        };

        /// @brief Type trait for has_resume_method (using concept)
        template<class T>
        struct has_resume_method {
            static constexpr bool value = resumable_type<T>;
        };

    } // namespace detail

    /// @brief Type-erased job pointer for scheduler
    ///
    /// Uses function pointers for type erasure instead of virtual functions.
    /// Each type T gets its own compile-time generated function pointers.
    /// Intrusive structure for use with linked_list (singly-linked).
    struct job_ptr : actor_zeta::detail::singly_linked<job_ptr> {
        using node_type = actor_zeta::detail::singly_linked<job_ptr>;

        void* ptr;
        resume_info (*resume_fn)(void*, size_t);

        /// @brief Default constructor - creates null job_ptr
        job_ptr() noexcept
            : ptr(nullptr)
            , resume_fn(nullptr) {}

        /// @brief Construct from raw pointer and function
        /// @param p Pointer to resumable object (must not be null for non-default construction)
        /// @param fn Resume function pointer (must not be null)
        job_ptr(void* p, resume_info (*fn)(void*, size_t)) noexcept
            : ptr(p)
            , resume_fn(fn) {
            assert((p != nullptr || fn == nullptr) && "Non-null function requires non-null pointer");
            assert((fn != nullptr || p == nullptr) && "Non-null pointer requires non-null function");
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
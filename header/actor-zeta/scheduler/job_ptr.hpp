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
        template<class T>
        resume_info resume_impl(void* ptr, size_t max_throughput) {
            assert(ptr != nullptr);
            return static_cast<T*>(ptr)->resume(max_throughput);
        }

        template<typename T>
        concept resumable_type = requires(T* t, size_t n) {
            { t->resume(n) } -> std::same_as<resume_info>;
        };

        template<class T>
        struct has_resume_method {
            static constexpr bool value = resumable_type<T>;
        };

    } // namespace detail

    // Type-erased job pointer using function pointers instead of virtual functions
    struct job_ptr : actor_zeta::detail::singly_linked<job_ptr> {
        using node_type = actor_zeta::detail::singly_linked<job_ptr>;

        void* ptr;
        resume_info (*resume_fn)(void*, size_t);

        job_ptr() noexcept
            : ptr(nullptr)
            , resume_fn(nullptr) {}

        job_ptr(void* p, resume_info (*fn)(void*, size_t)) noexcept
            : ptr(p)
            , resume_fn(fn) {
            assert((p != nullptr || fn == nullptr) && "Non-null function requires non-null pointer");
            assert((fn != nullptr || p == nullptr) && "Non-null pointer requires non-null function");
        }

        resume_info resume(size_t max_throughput) const {
            assert(ptr != nullptr && "Cannot resume null job");
            assert(resume_fn != nullptr && "Resume function is null");
            return resume_fn(ptr, max_throughput);
        }

        explicit operator bool() const noexcept {
            return ptr != nullptr;
        }

        void* raw_ptr() const noexcept {
            return ptr;
        }
    };

}} // namespace actor_zeta::scheduler
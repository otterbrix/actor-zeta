#pragma once

#include <type_traits>
#include <cstddef>

#include "forwards.hpp"

namespace actor_zeta {

    using max_throughput_t = size_t;

    namespace scheduler {

    enum class resume_result {
        resume,
        awaiting,
        done,
        shutdown
    };

    /// Extended resume result with additional information about execution
    struct resume_info {
        resume_result result;           // Execution status
        size_t messages_processed;      // Number of messages processed in this resume call

        resume_info() noexcept
            : result(resume_result::done)
            , messages_processed(0) {
        }

        resume_info(resume_result r, size_t processed = 0) noexcept
            : result(r)
            , messages_processed(processed) {
        }

        // Implicit conversion to resume_result for backward compatibility
        operator resume_result() const noexcept {
            return result;
        }
    };

    struct resumable {
        resumable();
        virtual ~resumable();
        virtual resume_info resume(scheduler_abstract_t*, max_throughput_t) = 0;
        virtual void intrusive_ptr_add_ref_impl() = 0;
        virtual void intrusive_ptr_release_impl() = 0;
    };

    using resumable_t = resumable;

    template<class T>
    typename std::enable_if<std::is_same<T*, resumable*>::value>::type
    intrusive_ptr_add_ref(T* ptr) {
        ptr->intrusive_ptr_add_ref_impl();
    }

    template<class T>
    typename std::enable_if<std::is_same<T*, resumable*>::value>::type
    intrusive_ptr_release(T* ptr) {
        ptr->intrusive_ptr_release_impl();
    }

}} // namespace actor_zeta::scheduler
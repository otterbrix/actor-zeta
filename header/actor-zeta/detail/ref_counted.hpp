#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>

namespace actor_zeta {

    class ref_counted {
    public:
        virtual ~ref_counted();

        ref_counted();

        ref_counted(const ref_counted&);

        ref_counted& operator=(const ref_counted&);

        void ref() const noexcept {
#ifndef NDEBUG
            auto old_rc = rc_.load(std::memory_order_acquire);
            assert(old_rc > 0 && "ref(): use-after-free - refcount is 0, object deleted!");
#endif
            rc_.fetch_add(1, std::memory_order_relaxed);
        }

        void deref() const noexcept {
#ifndef NDEBUG
            auto old_rc = rc_.load(std::memory_order_acquire);
            assert(old_rc > 0 && "deref(): refcount underflow - called deref() more than ref()!");
#endif
            if (unique() || rc_.fetch_sub(1, std::memory_order_acq_rel) == 1)
                delete this;
        }

        bool unique() const noexcept {
#ifndef NDEBUG
            auto count = rc_.load(std::memory_order_acquire);
            assert(count > 0 && "unique(): use-after-free - refcount is 0!");
#endif
            return rc_ == 1;
        }

        size_t get_reference_count() const noexcept {
#ifndef NDEBUG
            auto count = rc_.load(std::memory_order_acquire);
            assert(count > 0 && "get_reference_count(): use-after-free - refcount is 0!");
#endif
            return rc_;
        }

    protected:
        mutable std::atomic<size_t> rc_;
    };

    inline void intrusive_ptr_add_ref(const ref_counted* p) {
        p->ref();
    }

    inline void intrusive_ptr_release(const ref_counted* p) {
        p->deref();
    }

} // namespace actor_zeta
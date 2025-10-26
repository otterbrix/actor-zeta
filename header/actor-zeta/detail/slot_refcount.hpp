#pragma once

#include <actor-zeta/detail/rtt.hpp>
#include <actor-zeta/detail/memory_resource.hpp>
#include <atomic>
#include <cassert>
#include <cstdint>

namespace actor_zeta { namespace detail {

    class slot_refcount final {
    public:
        explicit slot_refcount(pmr::memory_resource* res)
            : resource_(res)
            , result_(res)
            , ready_(false)
            , refcount_(2)
#ifndef NDEBUG
            , magic_(kMagicAlive)
            , generation_(next_generation())
#endif
        {}

        slot_refcount(const slot_refcount&) = delete;
        slot_refcount& operator=(const slot_refcount&) = delete;

        void add_ref() noexcept {
#ifndef NDEBUG
            assert(magic_ == kMagicAlive && "Use-after-free: add_ref() on deleted slot!");
            int old_count = refcount_.load(std::memory_order_relaxed);
            assert(old_count > 0 && "Refcount underflow: add_ref() called on dead slot!");
            assert(old_count < 10000 && "Refcount overflow: suspicious refcount value!");
#endif
            refcount_.fetch_add(1, std::memory_order_relaxed);
        }

        void release() noexcept {
#ifndef NDEBUG
            assert(magic_ == kMagicAlive && "Double delete: release() on already deleted slot!");
            int old_count = refcount_.load(std::memory_order_relaxed);
            assert(old_count > 0 && "Refcount underflow: release() called with refcount=0!");
#endif
            if (refcount_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
#ifndef NDEBUG
                magic_ = kMagicDead;
#endif
                this->~slot_refcount();
                resource_->deallocate(this, sizeof(slot_refcount), alignof(slot_refcount));
            }
        }

        void set_result(rtt&& value) noexcept {
#ifndef NDEBUG
            assert(magic_ == kMagicAlive && "Use-after-free: set_result() on deleted slot!");
            assert(!ready_.load(std::memory_order_relaxed) && "Race: Double set_result() detected!");
#endif
            result_ = std::move(value);
            ready_.store(true, std::memory_order_release);
        }

        bool is_ready() const noexcept {
#ifndef NDEBUG
            assert(magic_ == kMagicAlive && "Use-after-free: is_ready() on deleted slot!");
#endif
            return ready_.load(std::memory_order_acquire);
        }

        rtt& result() noexcept {
#ifndef NDEBUG
            assert(magic_ == kMagicAlive && "Use-after-free: result() on deleted slot!");
            assert(ready_.load(std::memory_order_relaxed) && "Race: result() called before set_result()!");
#endif
            return result_;
        }

        const rtt& result() const noexcept {
#ifndef NDEBUG
            assert(magic_ == kMagicAlive && "Use-after-free: result() on deleted slot!");
            assert(ready_.load(std::memory_order_relaxed) && "Race: result() called before set_result()!");
#endif
            return result_;
        }

        pmr::memory_resource* memory_resource() const noexcept {
            return resource_;
        }

#ifndef NDEBUG
        uint64_t generation() const noexcept { return generation_; }
#endif

    private:
        pmr::memory_resource* resource_;
        rtt result_;
        std::atomic<bool> ready_;
        std::atomic<int> refcount_;

#ifndef NDEBUG
        static constexpr uint32_t kMagicAlive = 0xABCDEF01;
        static constexpr uint32_t kMagicDead = 0xDEADBEEF;

        uint32_t magic_;
        uint64_t generation_;

        static uint64_t next_generation() {
            static std::atomic<uint64_t> counter{0};
            return counter.fetch_add(1, std::memory_order_relaxed);
        }
#endif
    };

}} // namespace actor_zeta::detail
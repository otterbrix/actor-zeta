#pragma once

#include <memory_resource>
#include <cstddef>
#include <cstdint>

namespace actor_zeta::detail {

    /// @brief Header stored before coroutine frame to track memory_resource
    /// This allows operator delete to know which allocator to use
    /// Layout: [header][padding][coroutine_frame]
    struct coro_frame_header {
        std::pmr::memory_resource* resource;
        std::size_t frame_size;  // Store frame size for unsized delete

        /// @brief Size of header with alignment padding
        /// Ensures coroutine frame starts at max_align_t boundary
        static constexpr std::size_t padded_size() noexcept {
            constexpr std::size_t header_size = sizeof(coro_frame_header);
            constexpr std::size_t align = alignof(std::max_align_t);
            // Round up to alignment boundary
            return (header_size + align - 1) & ~(align - 1);
        }
    };

    /// @brief Allocate coroutine frame with header
    /// @param res Memory resource to use (nullptr = use global new)
    /// @param frame_size Size of coroutine frame
    /// @return Pointer to coroutine frame (after header)
    inline void* allocate_coro_frame(std::pmr::memory_resource* res, std::size_t frame_size) noexcept {
        const std::size_t total_size = coro_frame_header::padded_size() + frame_size;
        const std::size_t align = alignof(std::max_align_t);

        void* raw = nullptr;
        if (res) {
            raw = res->allocate(total_size, align);
        } else {
            raw = ::operator new(total_size, std::align_val_t{align}, std::nothrow);
        }

        if (!raw) {
            return nullptr;
        }

        // Store resource pointer and frame size in header
        auto* header = static_cast<coro_frame_header*>(raw);
        header->resource = res;
        header->frame_size = frame_size;

        // Return pointer to coroutine frame (after header)
        return static_cast<char*>(raw) + coro_frame_header::padded_size();
    }

    /// @brief Deallocate coroutine frame with header (sized version)
    /// @param frame Pointer to coroutine frame (returned by allocate_coro_frame)
    /// @param frame_size Size of coroutine frame
    inline void deallocate_coro_frame(void* frame, std::size_t frame_size) noexcept {
        if (!frame) {
            return;
        }

        // Get header before the frame
        void* raw = static_cast<char*>(frame) - coro_frame_header::padded_size();
        auto* header = static_cast<coro_frame_header*>(raw);

        const std::size_t total_size = coro_frame_header::padded_size() + frame_size;
        const std::size_t align = alignof(std::max_align_t);

        if (header->resource) {
            header->resource->deallocate(raw, total_size, align);
        } else {
            ::operator delete(raw, std::align_val_t{align}, std::nothrow);
        }
    }

    /// @brief Deallocate coroutine frame with header (unsized version for GCC)
    /// @param frame Pointer to coroutine frame (returned by allocate_coro_frame)
    /// @note Recovers frame_size from header - used when compiler doesn't pass size
    inline void deallocate_coro_frame_unsized(void* frame) noexcept {
        if (!frame) {
            return;
        }

        // Get header before the frame
        void* raw = static_cast<char*>(frame) - coro_frame_header::padded_size();
        auto* header = static_cast<coro_frame_header*>(raw);

        // Recover frame size from header
        const std::size_t frame_size = header->frame_size;
        const std::size_t total_size = coro_frame_header::padded_size() + frame_size;
        const std::size_t align = alignof(std::max_align_t);

        if (header->resource) {
            header->resource->deallocate(raw, total_size, align);
        } else {
            ::operator delete(raw, std::align_val_t{align}, std::nothrow);
        }
    }

} // namespace actor_zeta::detail
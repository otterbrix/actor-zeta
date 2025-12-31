#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory_resource>

namespace actor_zeta::detail {

    // Header stored before coroutine frame: [header][padding][coroutine_frame]
    struct coro_frame_header {
        std::pmr::memory_resource* resource;
        std::size_t frame_size;

        static constexpr std::size_t padded_size() noexcept {
            constexpr std::size_t header_size = sizeof(coro_frame_header);
            constexpr std::size_t align = alignof(std::max_align_t);
            return (header_size + align - 1) & ~(align - 1);
        }
    };

    // Allocate coroutine frame with header, returns pointer after header
    // Requires: res != nullptr (coroutines must be actor member functions)
    inline void* allocate_coro_frame(std::pmr::memory_resource* res, std::size_t frame_size) noexcept {
        assert(res != nullptr && "allocate_coro_frame: resource must not be null");

        const std::size_t total_size = coro_frame_header::padded_size() + frame_size;
        const std::size_t align = alignof(std::max_align_t);

        void* raw = res->allocate(total_size, align);
        if (!raw) {
            return nullptr;
        }

        auto* header = static_cast<coro_frame_header*>(raw);
        header->resource = res;
        header->frame_size = frame_size;

        return static_cast<char*>(raw) + coro_frame_header::padded_size();
    }

    // Deallocate coroutine frame (sized version)
    inline void deallocate_coro_frame(void* frame, std::size_t frame_size) noexcept {
        if (!frame) {
            return;
        }

        void* raw = static_cast<char*>(frame) - coro_frame_header::padded_size();
        auto* header = static_cast<coro_frame_header*>(raw);

        assert(header->resource != nullptr && "deallocate_coro_frame: stored resource is null");

        const std::size_t total_size = coro_frame_header::padded_size() + frame_size;
        const std::size_t align = alignof(std::max_align_t);

        header->resource->deallocate(raw, total_size, align);
    }

    // Deallocate coroutine frame (unsized version for GCC - recovers size from header)
    inline void deallocate_coro_frame_unsized(void* frame) noexcept {
        if (!frame) {
            return;
        }

        void* raw = static_cast<char*>(frame) - coro_frame_header::padded_size();
        auto* header = static_cast<coro_frame_header*>(raw);

        assert(header->resource != nullptr && "deallocate_coro_frame_unsized: stored resource is null");

        const std::size_t frame_size = header->frame_size;
        const std::size_t total_size = coro_frame_header::padded_size() + frame_size;
        const std::size_t align = alignof(std::max_align_t);

        header->resource->deallocate(raw, total_size, align);
    }

} // namespace actor_zeta::detail
#pragma once

namespace actor_zeta {

    // ════════════════════════════════════════════════════════════════════════════
    // COROUTINES REQUIREMENT: Project REQUIRES C++20 coroutines support
    // ════════════════════════════════════════════════════════════════════════════
    //
    // This library REQUIRES either:
    // 1. Standard C++20 coroutines: <coroutine> with __cpp_impl_coroutine
    // 2. Experimental coroutines: <experimental/coroutine> (GCC 9, Clang 8-13)
    //
    // WHY REQUIRED:
    // - behavior_t customization relies on coroutines for async handling
    // - Future/promise system needs coroutine support
    // - Removing conditional compilation simplifies code complexity
    //
    // MINIMUM VERSIONS:
    // - GCC 10+ (full <coroutine> support)
    // - Clang 14+ (full <coroutine> support)
    // - Clang 8-13 (experimental coroutines via <experimental/coroutine>)
    // - MSVC 2019 16.8+ (full <coroutine> support)
    //
    // BUILD ERROR: If coroutines not available, build will fail with static_assert
    // ════════════════════════════════════════════════════════════════════════════

#if defined(__has_include)
// Try standard C++20 <coroutine> first
#if __has_include(<coroutine>) && defined(__cpp_impl_coroutine)
#define HAVE_COROUTINES 1
#define HAVE_EXPERIMENTAL_COROUTINES 0
// Fallback to experimental coroutines (GCC 9, Clang 8-13)
#elif __has_include(<experimental/coroutine>)
#define HAVE_COROUTINES 1
#define HAVE_EXPERIMENTAL_COROUTINES 1
#else
// No coroutine support detected - will trigger static_assert below
#define HAVE_COROUTINES 0
#define HAVE_EXPERIMENTAL_COROUTINES 0
#endif
#else
// Compiler doesn't support __has_include - assume no coroutines
#define HAVE_COROUTINES 0
#define HAVE_EXPERIMENTAL_COROUTINES 0
#endif

    // ════════════════════════════════════════════════════════════════════════════
    // COMPILE-TIME REQUIREMENT CHECK
    // ════════════════════════════════════════════════════════════════════════════
    //
    // Static assert fires if coroutines not available
    // This ensures project cannot be built without coroutine support
    // ════════════════════════════════════════════════════════════════════════════

#if !HAVE_COROUTINES
    // Trigger compile error with helpful message
    namespace actor_zeta_config_check {
        // Static assert in namespace to provide clear error location
        static_assert(
            HAVE_COROUTINES,
            "\n"
            "════════════════════════════════════════════════════════════════════════════\n"
            "  actor-zeta REQUIRES C++20 Coroutines Support\n"
            "════════════════════════════════════════════════════════════════════════════\n"
            "\n"
            "This library requires either:\n"
            "  1. Standard C++20 coroutines: <coroutine> header\n"
            "  2. Experimental coroutines: <experimental/coroutine> header\n"
            "\n"
            "Minimum compiler versions:\n"
            "  - GCC 10+        (full <coroutine> support)\n"
            "  - Clang 14+      (full <coroutine> support)\n"
            "  - Clang 8-13     (experimental coroutines, use -fcoroutines-ts)\n"
            "  - MSVC 2019 16.8+ (full <coroutine> support)\n"
            "\n"
            "Current compiler detection:\n"
            "  __has_include(<coroutine>): NO\n"
            "  __has_include(<experimental/coroutine>): NO\n"
            "\n"
            "To fix:\n"
            "  - Update compiler to supported version\n"
            "  - Add -std=c++20 (or -std=c++2a for older compilers)\n"
            "  - For experimental coroutines: add -fcoroutines-ts (Clang/GCC)\n"
            "\n"
            "════════════════════════════════════════════════════════════════════════════\n");
    } // namespace actor_zeta_config_check
#endif

// C++20 Atomic Wait/Notify feature detection
// Enables efficient blocking wait using futex (Linux), __ulock_wait (macOS), or WaitOnAddress (Windows)
// Benefits: Zero CPU usage while waiting (vs exponential backoff polling)
#if defined(__cpp_lib_atomic_wait)
#if __cpp_lib_atomic_wait >= 201907L
#define HAVE_ATOMIC_WAIT 1
#else
#define HAVE_ATOMIC_WAIT 0
#endif
#else
#define HAVE_ATOMIC_WAIT 0
#endif

#define CACHE_LINE_SIZE 64

} // namespace actor_zeta
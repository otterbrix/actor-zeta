#pragma once

namespace actor_zeta {

// Coroutine support detection
#if defined(__has_include)
#if __has_include(<coroutine>) && defined(__cpp_impl_coroutine)
#define HAVE_COROUTINES 1
#define HAVE_EXPERIMENTAL_COROUTINES 0
#elif __has_include(<experimental/coroutine>)
#define HAVE_COROUTINES 1
#define HAVE_EXPERIMENTAL_COROUTINES 1
#else
#define HAVE_COROUTINES 0
#define HAVE_EXPERIMENTAL_COROUTINES 0
#endif
#else
#define HAVE_COROUTINES 0
#define HAVE_EXPERIMENTAL_COROUTINES 0
#endif

#if !HAVE_COROUTINES
    namespace actor_zeta_config_check {
        static_assert(
            HAVE_COROUTINES,
            "\n"
            "actor-zeta REQUIRES C++20 Coroutines Support\n"
            "\n"
            "Required: <coroutine> or <experimental/coroutine>\n"
            "Minimum: GCC 10+, Clang 14+ (or Clang 8-13 with -fcoroutines-ts), MSVC 2019 16.8+\n"
            "\n"
            "Fix: Update compiler, add -std=c++20, or add -fcoroutines-ts for experimental\n");
    }
#endif

// C++20 Atomic Wait/Notify feature detection
#if defined(__cpp_lib_atomic_wait) && __cpp_lib_atomic_wait >= 201907L
#define HAVE_ATOMIC_WAIT 1
#else
#define HAVE_ATOMIC_WAIT 0
#endif

#define CACHE_LINE_SIZE 64

} // namespace actor_zeta
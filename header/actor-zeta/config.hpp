#pragma once

namespace actor_zeta {

#ifndef CONFIG_NO_EXCEPTIONS
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
#define CONFIG_NO_EXCEPTIONS 0
#else
#define CONFIG_NO_EXCEPTIONS 1
#endif
#endif

#ifndef CPLUSPLUS
#if defined(_MSVC_LANG) && !defined(__clang__)
#define CPLUSPLUS (_MSC_VER == 1900 ? 201103L : _MSVC_LANG)
#else
#define CPLUSPLUS __cplusplus
#endif
#endif

#define COMPILER_VERSION(major, minor, patch) (10 * (10 * (major) + (minor)) + (patch))

#if defined(__clang__)
#define COMPILER_CLANG_VERSION COMPILER_VERSION(__clang_major__, __clang_minor__, __clang_patchlevel__)
#else
#define COMPILER_CLANG_VERSION 0
#endif

#if defined(__GNUC__) && !defined(__clang__)
#define COMPILER_GNUC_VERSION COMPILER_VERSION(__GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__)
#else
#define COMPILER_GNUC_VERSION 0
#endif

#define CPP11_OR_GREATER (CPLUSPLUS >= 201103L)
#define CPP14_OR_GREATER (CPLUSPLUS >= 201402L)
#define CPP17_OR_GREATER (CPLUSPLUS >= 201703L)
#define CPP20_OR_GREATER (CPLUSPLUS >= 202000L)

#if defined(__has_include)
#if __has_include(<coroutine>) && defined(__cpp_impl_coroutine)
#define HAVE_STD_COROUTINES 1
#else
#define HAVE_STD_COROUTINES 0
#endif
#else
#define HAVE_STD_COROUTINES 0
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


#ifndef REQUIRE_CONST_INIT
#define REQUIRE_CONST_INIT
#if __cpp_constinit >= 201907L
#undef REQUIRE_CONST_INIT
#define REQUIRE_CONST_INIT constinit
#elif defined(__clang__) && defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::require_constant_initialization)
#undef REQUIRE_CONST_INIT
#define REQUIRE_CONST_INIT [[clang::require_constant_initialization]]
#endif
#endif
#endif

#ifndef WEAK_CONSTINIT
#if defined(_MSC_VER) && !defined(__clang__) && _MSC_VER < 1920
#define WEAK_CONSTINIT
#elif defined(__clang__) && __clang_major__ < 4
#define WEAK_CONSTINIT
#endif
#endif

#ifndef NO_DESTROY
#if defined(__clang__) && defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::no_destroy)
#define NO_DESTROY [[clang::no_destroy]]
#endif
#endif
#endif

#define DEBUG NDEBUG

} // namespace actor_zeta

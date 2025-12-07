#pragma once

#include <thread>
#include <type_traits>

#ifdef __APPLE__

#include <pthread.h>
namespace actor_zeta { namespace detail {

    void set_thread_name(const char* name) {
        pthread_setname_np(name);
    }

}} // namespace actor_zeta::detail

#else // __APPLE__

#include <sys/prctl.h>
namespace actor_zeta { namespace detail {

    void set_thread_name(const char* name) {
        prctl(PR_SET_NAME, name, 0, 0, 0);
    }

}} // namespace actor_zeta::detail

#endif // __APPLE__

#pragma once

// Under ASIO_NO_EXCEPTIONS Asio turns every internal `throw X;` into a call to
// asio::detail::throw_exception(), which it only DECLARES: the application must
// define it. Include this before any <asio/...> header so the macros land first.

#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif
#ifndef ASIO_NO_EXCEPTIONS
#define ASIO_NO_EXCEPTIONS
#endif

#include <cassert>
#include <cstdlib>

#include <asio/detail/config.hpp>

namespace asio {
    namespace detail {

        // ASIO_SOURCE_LOCATION_PARAM is Asio's own macro, so this matches its declaration across versions.
        template<typename Exception>
        void throw_exception(const Exception& /*e*/
                             ASIO_SOURCE_LOCATION_PARAM) {
            assert(false && "asio exception raised under ASIO_NO_EXCEPTIONS");
            std::abort();
        }

    } // namespace detail
} // namespace asio

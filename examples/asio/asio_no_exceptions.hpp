#pragma once

// -----------------------------------------------------------------------------
// Exception-free Asio integration.
//
// actor-zeta compiles with -fno-exceptions (NO throw/try/catch). Standalone Asio
// throws by default, but when ASIO_NO_EXCEPTIONS is defined Asio replaces every
// internal `throw X;` with a call to the user-provided
//
//     template<typename Exception>
//     void asio::detail::throw_exception(const Exception&,
//                                         const asio::detail::source_location&);
//
// (the source_location parameter was added in newer Asio; we provide an overload
// set that matches both the old single-arg and the newer two-arg signatures).
//
// We must DEFINE that function — Asio only declares it under ASIO_NO_EXCEPTIONS.
// Our definition never throws and never returns: it asserts and aborts, which
// keeps the whole translation unit honest under -fno-exceptions while still
// terminating loudly if Asio ever hits an error path (e.g. a failed allocation).
//
// We reuse Asio's OWN signature macro (ASIO_SOURCE_LOCATION_PARAM) so our single
// definition is an EXACT match of Asio's declaration regardless of whether this
// Asio build added the trailing source_location parameter. Defining two separate
// overloads would instead risk an ambiguity / signature mismatch.
//
// IMPORTANT: this header MUST be included BEFORE any other <asio/...> header so
// the macros are set and the declaration/definition agree.
// -----------------------------------------------------------------------------

#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif
#ifndef ASIO_NO_EXCEPTIONS
#define ASIO_NO_EXCEPTIONS
#endif

#include <cassert>
#include <cstdlib>

// Pulls in ASIO_SOURCE_LOCATION_PARAM / ASIO_HAS_SOURCE_LOCATION and, when
// available, the asio::detail::source_location type used by the macro expansion.
#include <asio/detail/config.hpp>

namespace asio {
    namespace detail {

        // Single definition matching Asio's declaration exactly. The trailing
        // ASIO_SOURCE_LOCATION_PARAM expands to ", const source_location& loc"
        // on Asio versions that carry it, or to nothing otherwise. Marked
        // noreturn + exception-free: satisfies the no-exceptions rule by
        // terminating instead of unwinding.
        template<typename Exception>
        void throw_exception(const Exception& /*e*/
                             ASIO_SOURCE_LOCATION_PARAM) {
            assert(false && "asio exception raised under ASIO_NO_EXCEPTIONS");
            std::abort();
        }

    } // namespace detail
} // namespace asio

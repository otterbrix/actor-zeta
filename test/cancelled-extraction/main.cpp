/// @file
/// Extracting from a future with an error and no value must be refused: the value
/// lives in a union next to `has_value_`, so an assert-only refusal would, under
/// NDEBUG, move-construct a T from bytes that never held one. Built with -DNDEBUG on
/// purpose, since that is what ships; pins refuse_valueless_extraction(). A SIGABRT
/// handler exits 0, so "refused" passes and a returned value fails under plain ctest.

#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory_resource>
#include <unistd.h>
#include <actor-zeta.hpp>

using namespace actor_zeta;

namespace {

    struct tracked {
        char payload[16];
        static int moves;
        static int dtors;

        tracked() { std::memset(payload, 'A', sizeof(payload)); }
        tracked(tracked&& other) noexcept {
            ++moves;
            std::memcpy(payload, other.payload, sizeof(payload));
        }
        tracked& operator=(tracked&&) = delete;
        ~tracked() { ++dtors; }
    };

    int tracked::moves = 0;
    int tracked::dtors = 0;

    extern "C" void on_refusal(int) {
        static const char msg[] = "REFUSED: the library declined to extract\n";
        ssize_t written = ::write(STDOUT_FILENO, msg, sizeof(msg) - 1);
        (void) written;
        ::_exit(0); // only write() and _exit() here: both async-signal-safe
    }

} // namespace

// unique_future<void> has no value to read, so the union UB the T case is about
// cannot happen -- but "extraction refuses on a valueless future" must still hold,
// or a failed void operation reports success under NDEBUG.
int run_void_case() {
    auto* resource = std::pmr::get_default_resource();
    promise<void> p(resource);
    auto future = p.get_future();
    p.error(std::make_error_code(std::errc::operation_canceled));

    std::signal(SIGABRT, on_refusal);
    std::move(future).take_ready();
    std::printf("EXTRACTED FROM A VALUELESS unique_future<void>: take_ready() returned\n");
    return 1;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "void") == 0) {
        return run_void_case();
    }
    auto* resource = std::pmr::get_default_resource();
    const bool cancelled_case = (argc > 1);

    if (cancelled_case) {
        std::signal(SIGABRT, on_refusal);
    }

    promise<tracked> p(resource);
    auto future = p.get_future();

    if (cancelled_case) {
        // Exactly what ~message's cleanup_fn_ does for a send to a closed mailbox.
        p.error(std::make_error_code(std::errc::operation_canceled));
    } else {
        p.set_value(tracked{}); // control: same path with a real value, so a broken harness cannot pass
    }

    std::printf("is_ready=%d failed=%d\n",
                static_cast<int>(future.is_ready()), static_cast<int>(future.failed()));
    if (future.is_ready() != true) {
        std::printf("HARNESS BROKEN: a released promise must report ready\n");
        return 2;
    }
    if (future.failed() != cancelled_case) {
        std::printf("HARNESS BROKEN: failed() does not match the case\n");
        return 2;
    }

    const int moves_before = tracked::moves;
    auto value = std::move(future).take_ready();
    const int moves_by_extraction = tracked::moves - moves_before;

    if (cancelled_case) {
        std::printf("EXTRACTED FROM A VALUELESS FUTURE: %d move-construction(s) "
                    "out of a union member that was never made active\n",
                    moves_by_extraction);
        return 1;   // reaching here at all is the defect
    }

    if (value.payload[0] != 'A') {
        std::printf("control case returned a damaged value\n");
        return 2;
    }
    std::printf("control case extracted its value cleanly\n");
    return 0;
}

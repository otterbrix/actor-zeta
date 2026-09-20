/// @file
/// Extracting from a future that carries an error and no value.
///
/// result_storage keeps the value in `union storage_t { char dummy_; T value_; }`
/// with a separate `bool has_value_`. take() and get() assert has_value_ and then
/// read storage_.value_ regardless -- so once NDEBUG removes the assert, a state
/// with an error and no value move-constructs a T out of bytes that never held one
/// and then runs ~T() on them. That is undefined behaviour, and it is silent.
///
/// The state is ordinary, not exotic: send() to an actor whose mailbox is closing
/// returns queue_closed, ~message runs cleanup_fn_, which does
/// set_error(operation_canceled) + release_promise(). is_ready() then reports true
/// -- it is the promise_released bit -- while no value was ever written.
///
/// Built with -DNDEBUG on purpose: with asserts live the process stops at the
/// assert and the defect is invisible. NDEBUG is what ships.
///
/// The negative case traps the refusal: a SIGABRT handler reports it and exits 0,
/// so "refused" is a pass and "returned a value that does not exist" is a failure,
/// under ordinary ctest pass/fail. The harness does not care HOW the refusal is
/// spelled beyond the fact that it does not return. The positive case runs the same
/// code path on a future that DOES hold a value, so a regression in the harness
/// itself cannot be mistaken for the defect being fixed.

#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory_resource>
#include <unistd.h>
#include <actor-zeta.hpp>

using namespace actor_zeta;

namespace {

    // Move-construction and destruction are observable, so the test can say whether
    // the union was touched at all.
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

    // The refusal is an abort(). Catch it so the outcome reaches ctest as an exit
    // code instead of a crash. Only write() and _exit() run here -- both are
    // async-signal-safe.
    extern "C" void on_refusal(int) {
        static const char msg[] = "REFUSED: the library declined to extract\n";
        ssize_t written = ::write(STDOUT_FILENO, msg, sizeof(msg) - 1);
        (void) written;
        ::_exit(0);
    }

} // namespace

int main(int argc, char** argv) {
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
        p.set_value(tracked{});
    }

    std::printf("is_ready=%d failed=%d\n", (int) future.is_ready(), (int) future.failed());
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

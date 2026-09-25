/// @file
/// Contract violations that must stop the process instead of corrupting memory. One check
/// per argument. A SIGABRT handler exits 0, so "stopped" passes and "carried on" fails under
/// plain ctest. Asserts are forced on (see CMakeLists.txt): some checks are debug-only.
/// The handler goes in right before the violating call, so an earlier abort cannot pass.

#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <memory_resource>
#include <unistd.h>
#include <actor-zeta.hpp>

using namespace actor_zeta;

namespace {

    extern "C" void on_stop(int) {
        static const char msg[] = "STOPPED: the contract check fired\n";
        ssize_t written = ::write(STDOUT_FILENO, msg, sizeof(msg) - 1);
        (void) written;
        ::_exit(0); // only write() and _exit() here: both async-signal-safe
    }

    [[noreturn]] void carried_on(const char* what) {
        std::printf("CARRIED ON: %s\n", what);
        std::fflush(stdout);
        ::_exit(1); // skip the destructors: the state they would run on is already broken
    }

    class probe_t final : public basic_actor<probe_t> {
    public:
        explicit probe_t(std::pmr::memory_resource* ptr)
            : basic_actor<probe_t>(ptr) {}

        // resume() inside resume(): the one-thread form of two resume() calls at once.
        unique_future<void> reenter() {
            std::signal(SIGABRT, on_stop);
            [[maybe_unused]] const auto nested = self_->resume(1);
            carried_on("resume() ran inside resume()");
            co_return; // unreachable; makes this a coroutine
        }

        // delete from inside behavior(): delete waits for everyone inside the actor -- itself too.
        unique_future<void> delete_self() {
            std::signal(SIGABRT, on_stop);
            owner_->reset();
            carried_on("delete from inside behavior() returned");
            co_return; // unreachable; makes this a coroutine
        }

        unique_future<int> answer() {
            co_return 42;
        }

        // Awaits a reply from its own mailbox, where it waits behind the behavior awaiting it.
        unique_future<void> await_self() {
            auto [needs_sched, reply] = send(this, &probe_t::answer);
            if (needs_sched) {
                std::printf("HARNESS BROKEN: a send() to the actor holding the turn handed out another\n");
                ::_exit(2);
            }
            std::signal(SIGABRT, on_stop);
            [[maybe_unused]] const int value = co_await std::move(reply);
            carried_on("an actor's await on its own send() completed");
        }

        // The control: awaits a neighbour. The obligation goes to the owner, who runs the neighbour.
        unique_future<int> await_peer() {
            auto [needs_sched, reply] = send(peer_, &probe_t::answer);
            peer_owed_ = needs_sched;
            co_return co_await std::move(reply);
        }

        using dispatch_traits = actor_zeta::dispatch_traits<
            &probe_t::reenter,
            &probe_t::delete_self,
            &probe_t::answer,
            &probe_t::await_self,
            &probe_t::await_peer>;

        behavior_t behavior(mailbox::message* msg) {
            if (msg->command() == msg_id<probe_t, &probe_t::reenter>) {
                co_await dispatch(this, &probe_t::reenter, msg);
            } else if (msg->command() == msg_id<probe_t, &probe_t::delete_self>) {
                co_await dispatch(this, &probe_t::delete_self, msg);
            } else if (msg->command() == msg_id<probe_t, &probe_t::answer>) {
                co_await dispatch(this, &probe_t::answer, msg);
            } else if (msg->command() == msg_id<probe_t, &probe_t::await_self>) {
                co_await dispatch(this, &probe_t::await_self, msg);
            } else if (msg->command() == msg_id<probe_t, &probe_t::await_peer>) {
                co_await dispatch(this, &probe_t::await_peer, msg);
            }
        }

        probe_t* self_ = nullptr;
        std::unique_ptr<probe_t, pmr::deleter_t>* owner_ = nullptr;
        probe_t* peer_ = nullptr;
        bool peer_owed_ = false;
    };

    // Two futures on one state: each would release it, the second into freed memory.
    int second_get_future() {
        std::pmr::unsynchronized_pool_resource resource;
        promise<int> p(&resource);
        [[maybe_unused]] auto first = p.get_future();

        std::signal(SIGABRT, on_stop);
        [[maybe_unused]] auto second = p.get_future();

        carried_on("get_future() handed out a second future");
    }

    int nested_resume() {
        std::pmr::unsynchronized_pool_resource resource;
        auto actor = spawn<probe_t>(&resource);
        actor->self_ = actor.get();
        auto [needs_sched, future] = send(actor.get(), &probe_t::reenter);
        future.detach();
        if (!needs_sched) {
            std::printf("HARNESS BROKEN: the first send to a fresh actor must hand out the turn\n");
            return 2;
        }
        [[maybe_unused]] const auto verdict = actor->resume(1);
        std::printf("HARNESS BROKEN: behavior() never re-entered resume()\n");
        return 2;
    }

    // Born parked: nobody handed out a turn, so nobody may run the actor.
    int resume_without_turn() {
        std::pmr::unsynchronized_pool_resource resource;
        auto actor = spawn<probe_t>(&resource);

        std::signal(SIGABRT, on_stop);
        [[maybe_unused]] const auto verdict = actor->resume(1);

        carried_on("resume() ran on a parked actor");
    }

    // close() of a parked actor closes it at once; nobody holds a turn after that.
    int resume_after_close() {
        std::pmr::unsynchronized_pool_resource resource;
        auto actor = spawn<probe_t>(&resource);
        auto closed = actor->close();
        if (!closed.is_ready()) {
            std::printf("HARNESS BROKEN: close() of a parked actor must be ready at once\n");
            return 2;
        }

        std::signal(SIGABRT, on_stop);
        [[maybe_unused]] const auto verdict = actor->resume(1);

        carried_on("resume() ran on a closed actor");
    }

    // A resource that never frees, so the second delete reads what the first one left behind.
    int double_delete() {
        std::pmr::monotonic_buffer_resource arena;
        auto actor = spawn<probe_t>(&arena);
        auto* raw = actor.release();
        pmr::deleter_t deleter(&arena);
        deleter(raw);

        std::signal(SIGABRT, on_stop);
        deleter(raw);

        carried_on("the actor was deleted twice");
    }

    // Waits out its bounded time (seconds): delete cannot finish while it is inside the actor itself.
    int delete_from_inside() {
        std::pmr::unsynchronized_pool_resource resource;
        auto actor = spawn<probe_t>(&resource);
        actor->owner_ = &actor;
        auto [needs_sched, future] = send(actor.get(), &probe_t::delete_self);
        future.detach();
        if (!needs_sched) {
            std::printf("HARNESS BROKEN: the first send to a fresh actor must hand out the turn\n");
            return 2;
        }
        [[maybe_unused]] const auto verdict = actor->resume(1);
        std::printf("HARNESS BROKEN: behavior() never deleted the actor\n");
        return 2;
    }

    // The reply waits in the actor's own mailbox, behind the behavior awaiting it: the actor would
    // keep its turn, take `resume` verdicts and spin a worker for good.
    int self_await() {
        std::pmr::unsynchronized_pool_resource resource;
        auto actor = spawn<probe_t>(&resource);
        auto [needs_sched, future] = send(actor.get(), &probe_t::await_self);
        future.detach();
        if (!needs_sched) {
            std::printf("HARNESS BROKEN: the first send to a fresh actor must hand out the turn\n");
            return 2;
        }
        for (int i = 0; i < 100; ++i) {
            if (actor->resume(1).result != scheduler::resume_result::resume) {
                break;
            }
        }
        carried_on("an actor awaiting its own send() kept its turn");
    }

    // The control for self_await: awaiting a neighbour completes. No SIGABRT trap here, so a false
    // alarm crashes the test.
    int await_neighbour() {
        std::pmr::unsynchronized_pool_resource resource;
        auto neighbour = spawn<probe_t>(&resource);
        auto actor = spawn<probe_t>(&resource);
        actor->peer_ = neighbour.get();

        auto [needs_sched, result] = send(actor.get(), &probe_t::await_peer);
        if (!needs_sched) {
            std::printf("HARNESS BROKEN: the first send to a fresh actor must hand out the turn\n");
            return 2;
        }
        const auto first = actor->resume(1); // sends to the neighbour and awaits it: keeps the turn
        if (first.result != scheduler::resume_result::resume || !actor->peer_owed_) {
            std::printf("HARNESS BROKEN: the actor must await its neighbour, which it must have woken\n");
            return 2;
        }
        const auto peer = neighbour->resume(1); // the owner discharges the obligation
        if (peer.result != scheduler::resume_result::awaiting) {
            std::printf("HARNESS BROKEN: the neighbour must answer and park\n");
            return 2;
        }
        const auto second = actor->resume(1); // picks the reply up
        if (second.result != scheduler::resume_result::awaiting || !result.is_ready() || result.failed()
            || std::move(result).take_ready() != 42) {
            std::printf("FAILED: the actor did not finish on its neighbour's reply\n");
            return 1;
        }
        return 0;
    }

    struct check {
        const char* name;
        int (*run)();
    };

    const check checks[] = {
        {"second_get_future", second_get_future},
        {"nested_resume", nested_resume},
        {"resume_without_turn", resume_without_turn},
        {"resume_after_close", resume_after_close},
        {"double_delete", double_delete},
        {"delete_from_inside", delete_from_inside},
        {"self_await", self_await},
        {"await_neighbour", await_neighbour},
    };

} // namespace

int main(int argc, char** argv) {
    for (const auto& c : checks) {
        if (argc > 1 && std::strcmp(argv[1], c.name) == 0) {
            return c.run();
        }
    }
    std::printf("unknown check\n");
    return 2;
}

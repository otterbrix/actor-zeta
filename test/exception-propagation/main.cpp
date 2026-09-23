/// @file
/// An exception escaping a coroutine body must reach the consumer. Built with
/// -fexceptions while the rest of the suite is -fno-exceptions: without exceptions
/// the compiler emits no catch wrapper for a coroutine body, so unhandled_exception()
/// is dead code. An unhandled_exception() that RETURNS means "handled, carry on", so a
/// swallowed exception shows as is_ready()==1, failed()==0 and a garbage value --
/// silently under NDEBUG. Every case asserts a poller sees the failure and extraction
/// rethrows the ORIGINAL exception. No Catch2: a -fexceptions TU next to a
/// -fno-exceptions library would mix two compilation modes.

#include <cstdio>
#include <memory_resource>
#include <thread>
#include <stdexcept>
#include <string>

#include <actor-zeta.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>
#include <actor-zeta/actor/dispatch.hpp>

using namespace actor_zeta;

namespace {

    int failures = 0;

    void check(bool ok, const char* what) {
        std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) {
            ++failures;
        }
    }

    class thrower_actor final : public basic_actor<thrower_actor> {
    public:
        explicit thrower_actor(std::pmr::memory_resource* resource)
            : basic_actor<thrower_actor>(resource) {}

        unique_future<int> inner(int x) {
            if (x < 0) {
                throw std::runtime_error("inner said no");
            }
            co_return x * 2;
        }

        /// Awaits inner(), so a failure has to cross a co_await to get here.
        unique_future<int> outer(int x) {
            const int v = co_await inner(x);
            co_return v + 1;
        }

        using dispatch_traits = actor_zeta::dispatch_traits<&thrower_actor::inner,
                                                            &thrower_actor::outer>;

        behavior_t behavior(mailbox::message* msg) {
            const auto cmd = msg->command();
            if (cmd == msg_id<thrower_actor, &thrower_actor::inner>) {
                co_await dispatch(this, &thrower_actor::inner, msg);
            } else if (cmd == msg_id<thrower_actor, &thrower_actor::outer>) {
                co_await dispatch(this, &thrower_actor::outer, msg);
            }
        }
    };

    // Suspends on a pending future, then throws after resume. Every other case throws
    // BEFORE its first co_await (await_ready() true), never exercising the resume path.
    class late_thrower final : public basic_actor<late_thrower> {
    public:
        explicit late_thrower(std::pmr::memory_resource* ptr)
            : basic_actor<late_thrower>(ptr) {}

        unique_future<int> after_gate() {
            auto gate = std::move(gate_);
            co_await std::move(gate);
            throw std::runtime_error("threw after resuming");
            co_return 0;
        }

        // void all the way, for the void branch of owning_awaiter::await_resume.
        unique_future<void> void_inner() {
            throw std::runtime_error("void inner said no");
            co_return;
        }

        unique_future<void> void_outer() {
            auto inner = void_inner();
            co_await std::move(inner);
            co_return;
        }

        using dispatch_traits = actor_zeta::dispatch_traits<&late_thrower::after_gate,
                                                            &late_thrower::void_outer>;

        behavior_t behavior(mailbox::message* msg) {
            const auto cmd = msg->command();
            if (cmd == msg_id<late_thrower, &late_thrower::after_gate>) {
                co_await dispatch(this, &late_thrower::after_gate, msg);
            } else if (cmd == msg_id<late_thrower, &late_thrower::void_outer>) {
                co_await dispatch(this, &late_thrower::void_outer, msg);
            }
        }

        unique_future<void> gate_;
    };

    // Throws from behavior() itself: behavior_t is the chain root, its future read by nobody.
    class rude_actor final : public basic_actor<rude_actor> {
    public:
        explicit rude_actor(std::pmr::memory_resource* ptr)
            : basic_actor<rude_actor>(ptr) {}

        unique_future<void> ping() { co_return; }

        using dispatch_traits = actor_zeta::dispatch_traits<&rude_actor::ping>;

        behavior_t behavior(mailbox::message*) {
            throw std::runtime_error("behavior said no");
            co_return;
        }
    };

} // namespace

int main() {
    auto* resource = std::pmr::get_default_resource();
    auto actor = spawn<thrower_actor>(resource);

    // A poller never extracts, so it never sees the exception. It must still see a
    // failure, with a code distinct from "released without an outcome" (state_not_recoverable).
    {
        auto future = actor->inner(-1);
        check(future.is_ready(), "poller: a thrown body still completes the future");
        check(future.failed(), "poller: and it is reported as failed");
        check(future.error() == std::make_error_code(std::errc::interrupted),
              "poller: with a code distinct from state_not_recoverable");
        future.detach();
    }

    // Extracting rethrows the ORIGINAL exception, not a stand-in.
    {
        auto future = actor->inner(-1);
        bool rethrown = false;
        std::string what;
        try {
            const int value = std::move(future).take_ready();
            std::printf("     take_ready() returned %d instead of rethrowing\n", value);
        } catch (const std::runtime_error& e) {
            rethrown = true;
            what = e.what();
        }
        check(rethrown, "take_ready(): rethrows rather than returning a value");
        check(what == "inner said no", "take_ready(): the original exception, not a stand-in");
    }

    // Across a co_await: inner's exception surfaces in outer's await_resume and is re-captured there.
    {
        auto future = actor->outer(-1);
        bool rethrown = false;
        std::string what;
        try {
            const int value = std::move(future).take_ready();
            std::printf("     take_ready() returned %d instead of rethrowing\n", value);
        } catch (const std::runtime_error& e) {
            rethrown = true;
            what = e.what();
        }
        check(rethrown, "co_await chain: the failure propagates through the awaiter");
        check(what == "inner said no", "co_await chain: the original exception survives");
    }

    // Filling a promise by hand (a router taking msg->get_result_promise<T>()) is
    // supported; a caught exception needs a channel error() would flatten to a code.
    {
        promise<int> p(resource);
        auto future = p.get_future();
        try {
            throw std::runtime_error("router caught this");
        } catch (...) {
            p.exception(std::current_exception());
        }

        check(future.is_ready(), "promise::exception(): the future completes");
        check(future.failed(), "promise::exception(): and is reported as failed");
        check(future.error() == std::make_error_code(std::errc::interrupted),
              "promise::exception(): with the interrupted code, not the totality repair");

        bool rethrown = false;
        std::string what;
        try {
            const int value = std::move(future).take_ready();
            std::printf("     take_ready() returned %d instead of rethrowing\n", value);
        } catch (const std::runtime_error& e) {
            rethrown = true;
            what = e.what();
        }
        check(rethrown, "promise::exception(): extraction rethrows");
        check(what == "router caught this", "promise::exception(): the original exception");
    }

    // The same for void, where there is no value for the exception to stand in for.
    {
        promise<void> p(resource);
        auto future = p.get_future();
        try {
            throw std::runtime_error("void router caught this");
        } catch (...) {
            p.exception(std::current_exception());
        }

        bool rethrown = false;
        try {
            std::move(future).take_ready();
        } catch (const std::runtime_error&) {
            rethrown = true;
        }
        check(rethrown, "promise<void>::exception(): extraction rethrows");
    }

    // Through send()/dispatch(): the exception must cross into the CALLER's state. Without
    // a catch there, the caller's promise is settled only by ~promise: broken_pipe, no exception.
    {
        auto driven = spawn<thrower_actor>(resource);
        auto sent = send(driven.get(), &thrower_actor::inner, -1);
        while (driven->resume(4).messages_processed != 0) {
        }

        check(sent.second.is_ready(), "send(): a thrown body still completes the future");
        check(sent.second.failed(), "send(): and it is reported as failed");
        check(sent.second.error() == std::make_error_code(std::errc::interrupted),
              "send(): with the interrupted code, not broken_pipe");

        bool rethrown = false;
        std::string what;
        try {
            const int value = std::move(sent.second).take_ready();
            std::printf("     take_ready() returned %d instead of rethrowing\n", value);
        } catch (const std::runtime_error& e) {
            rethrown = true;
            what = e.what();
        }
        check(rethrown, "send(): extraction rethrows");
        check(what == "inner said no", "send(): the original exception crossed actors");
    }

    // A throw from behavior(): the only question is whether the actor survives.
    {
        auto rude = spawn<rude_actor>(resource);
        auto sent = send(rude.get(), &rude_actor::ping);
        sent.second.detach();

        const auto verdict = rude->resume(4);
        check(verdict.messages_processed == 1, "behavior() throw: the message was taken");

        auto again = send(rude.get(), &rude_actor::ping);
        again.second.detach();
        const auto second = rude->resume(4);
        check(second.messages_processed == 1, "behavior() throw: the actor survives it");
    }

    // A throw AFTER a real suspension: parked on a pending gate settled by hand, so the
    // exception travels the drain path rather than an await_ready() shortcut.
    {
        auto actor2 = spawn<late_thrower>(resource);
        promise<void> gate(resource);
        actor2->gate_ = gate.get_future();

        auto sent = send(actor2.get(), &late_thrower::after_gate);
        const auto suspended = actor2->resume(4);
        check(suspended.result == scheduler::resume_result::resume,
              "late throw: the method parked on a pending future");
        check(!sent.second.is_ready(), "late throw: and the caller is still waiting");

        gate.set_value();
        while (actor2->resume(4).result == scheduler::resume_result::resume) {
        }

        check(sent.second.is_ready(), "late throw: settling the gate finishes the caller");
        bool rethrown = false;
        std::string what;
        try {
            const int value = std::move(sent.second).take_ready();
            std::printf("     take_ready() returned %d instead of rethrowing\n", value);
        } catch (const std::runtime_error& e) {
            rethrown = true;
            what = e.what();
        }
        check(rethrown, "late throw: extraction rethrows");
        check(what == "threw after resuming", "late throw: the exception from the resumed body");
    }

    // void awaited by void (see void_inner above).
    {
        auto actor3 = spawn<late_thrower>(resource);
        auto sent = send(actor3.get(), &late_thrower::void_outer);
        while (actor3->resume(4).messages_processed != 0) {
        }

        check(sent.second.failed(), "void chain: the caller is told it failed");
        bool rethrown = false;
        std::string what;
        try {
            std::move(sent.second).take_ready();
        } catch (const std::runtime_error& e) {
            rethrown = true;
            what = e.what();
        }
        check(rethrown, "void chain: extraction rethrows");
        check(what == "void inner said no", "void chain: the original exception");
    }

    // Across threads, on a real scheduler -- the shape a caller actually uses.
    {
        auto sched = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(2, 8);
        sched->start();
        {
            auto worker = spawn<thrower_actor>(resource);
            auto [needs_sched, future] = send(worker.get(), &thrower_actor::inner, -1);
            if (needs_sched) {
                sched->enqueue(worker.get());
            }
            while (!future.is_ready()) {
                std::this_thread::yield();
            }
            check(future.failed(), "cross-thread: reported as failed");

            bool rethrown = false;
            std::string what;
            try {
                const int value = std::move(future).take_ready();
                std::printf("     take_ready() returned %d instead of rethrowing\n", value);
            } catch (const std::runtime_error& e) {
                rethrown = true;
                what = e.what();
            }
            check(rethrown, "cross-thread: extraction rethrows");
            check(what == "inner said no", "cross-thread: the original exception");
            sched->stop();   // workers out before the actor goes
        }
    }

    // The success path must be untouched by any of this.
    {
        auto future = actor->outer(21);
        check(future.is_ready() && !future.failed(), "success path: ready and not failed");
        check(std::move(future).take_ready() == 43, "success path: outer(21) == 43");
    }

    std::printf("\n%s (%d failure(s))\n", failures == 0 ? "PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}

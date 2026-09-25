/// @file
/// close(): the owner stops an actor taking messages, gracefully. What was sent before close()
/// runs, a suspended behavior finishes, then the mailbox closes and every later send() is refused
/// with operation_canceled. The future is ready once the actor takes no more messages -- at once
/// for a parked actor, which the owner closes itself. close() is idempotent: however many calls,
/// from however many threads, every future comes out ready and not failed. Once it is ready,
/// deleting the actor is safe while the scheduler keeps running.

#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <memory_resource>
#include <optional>
#include <system_error>
#include <thread>
#include <vector>

using namespace actor_zeta;

namespace {

    class worker_t final : public basic_actor<worker_t> {
    public:
        explicit worker_t(std::pmr::memory_resource* res)
            : basic_actor<worker_t>(res) {}

        unique_future<int> twice(int x) {
            processed.fetch_add(1, std::memory_order_relaxed);
            co_return x * 2;
        }

        // Suspends until the test settles gate_.
        unique_future<int> gated() {
            gate_.emplace(resource());
            const int v = co_await gate_->get_future();
            co_return v + 1;
        }

        void open_gate(int v) {
            gate_->set_value(v);
            gate_.reset();
        }

        using dispatch_traits = actor_zeta::dispatch_traits<&worker_t::twice, &worker_t::gated>;

        behavior_t behavior(mailbox::message* msg) {
            switch (msg->command()) {
                case msg_id<worker_t, &worker_t::twice>:
                    co_await dispatch(this, &worker_t::twice, msg);
                    break;
                case msg_id<worker_t, &worker_t::gated>:
                    co_await dispatch(this, &worker_t::gated, msg);
                    break;
                default:
                    break;
            }
        }

        std::atomic<int> processed{0};

    private:
        std::optional<promise<int>> gate_;
    };

    // Runs the actor on the turn the caller holds, following the verdict; returns the last one.
    scheduler::resume_result drive(worker_t* actor) {
        auto verdict = scheduler::resume_result::resume;
        for (int i = 0; i < 64 && verdict == scheduler::resume_result::resume; ++i) {
            verdict = actor->resume(8).result;
        }
        return verdict;
    }

    bool refused(unique_future<int>& f) {
        return f.is_ready() && f.failed() && f.error() == std::make_error_code(std::errc::operation_canceled);
    }

} // namespace

TEST_CASE("close(): a parked actor closes at once, and send() is refused after it") {
    std::pmr::synchronized_pool_resource resource;
    auto actor = spawn<worker_t>(&resource);

    auto closed = actor->close();
    REQUIRE(closed.is_ready());
    REQUIRE_FALSE(closed.failed());

    auto [needs_sched, late] = send(actor.get(), &worker_t::twice, 1);
    REQUIRE_FALSE(needs_sched);
    REQUIRE(refused(late));
}

TEST_CASE("close(): what was sent before it runs first") {
    std::pmr::synchronized_pool_resource resource;
    auto actor = spawn<worker_t>(&resource);

    auto [owed, first] = send(actor.get(), &worker_t::twice, 1);
    REQUIRE(owed);
    auto [owed_again, second] = send(actor.get(), &worker_t::twice, 2);
    REQUIRE_FALSE(owed_again);

    auto closed = actor->close();
    REQUIRE_FALSE(closed.is_ready()); // the turn is owed: the holder closes when it gets there

    REQUIRE(drive(actor.get()) == scheduler::resume_result::done);
    REQUIRE(closed.is_ready());
    REQUIRE_FALSE(closed.failed());
    REQUIRE(std::move(first).take_ready() == 2);
    REQUIRE(std::move(second).take_ready() == 4);

    auto [needs_sched, late] = send(actor.get(), &worker_t::twice, 3);
    REQUIRE_FALSE(needs_sched);
    REQUIRE(refused(late));
}

TEST_CASE("close(): a suspended behavior finishes before the actor closes") {
    std::pmr::synchronized_pool_resource resource;
    auto actor = spawn<worker_t>(&resource);

    auto [owed, gated] = send(actor.get(), &worker_t::gated);
    REQUIRE(owed);
    REQUIRE(actor->resume(8).result == scheduler::resume_result::resume); // suspended, turn kept

    auto closed = actor->close();
    REQUIRE_FALSE(closed.is_ready());
    REQUIRE(actor->resume(8).result == scheduler::resume_result::resume); // still waiting
    REQUIRE_FALSE(closed.is_ready());

    actor->open_gate(41);
    REQUIRE(drive(actor.get()) == scheduler::resume_result::done);
    REQUIRE(closed.is_ready());
    REQUIRE_FALSE(closed.failed());
    REQUIRE(std::move(gated).take_ready() == 42);
}

TEST_CASE("close(): idempotent") {
    std::pmr::synchronized_pool_resource resource;
    SECTION("twice, and again after it closed") {
        auto actor = spawn<worker_t>(&resource);
        auto [owed, first] = send(actor.get(), &worker_t::twice, 1);
        REQUIRE(owed);

        auto a = actor->close();
        auto b = actor->close();
        REQUIRE(drive(actor.get()) == scheduler::resume_result::done);
        auto c = actor->close();

        for (auto* f : {&a, &b, &c}) {
            REQUIRE(f->is_ready());
            REQUIRE_FALSE(f->failed());
        }
        REQUIRE(std::move(first).take_ready() == 2);
    }

    SECTION("from two threads at once") {
        for (int round = 0; round < 200; ++round) {
            auto actor = spawn<worker_t>(&resource);
            unique_future<void> a;
            unique_future<void> b;
            std::thread other([&] { b = actor->close(); });
            a = actor->close();
            other.join();
            REQUIRE(a.is_ready());
            REQUIRE_FALSE(a.failed());
            REQUIRE(b.is_ready());
            REQUIRE_FALSE(b.failed());
        }
    }
}

TEST_CASE("close(): delete before the actor got to it still settles the future") {
    std::pmr::synchronized_pool_resource resource;
    unique_future<void> closed;
    unique_future<int> pending;
    {
        auto actor = spawn<worker_t>(&resource);
        auto [owed, first] = send(actor.get(), &worker_t::twice, 1);
        REQUIRE(owed); // held by the test and never run: no job anywhere, so delete is allowed
        pending = std::move(first);
        closed = actor->close();
        REQUIRE_FALSE(closed.is_ready());
    }
    REQUIRE(closed.is_ready());
    REQUIRE_FALSE(closed.failed());
    REQUIRE(refused(pending));
}

// Four senders, close(), then delete while the scheduler still runs. Meaningful under ASan/TSan.
TEST_CASE("close(): delete after close() while the sharing scheduler runs") {
    std::pmr::synchronized_pool_resource resource; // the scheduler and the actor share it; outlives both
    auto scheduler = std::make_unique<scheduler::sharing_scheduler>(&resource, 2, 8);
    scheduler->start();

    auto actor = spawn<worker_t>(&resource);
    auto* raw = actor.get();

    std::atomic<bool> stop{false};
    std::atomic<int> answered{0};
    std::atomic<int> refused_count{0};
    std::atomic<int> unexpected{0}; // no Catch2 macros off the main thread: a race in Catch2 v2
    std::vector<std::thread> senders;
    for (int t = 0; t < 4; ++t) {
        senders.emplace_back([&, t] {
            for (int i = 0; !stop.load(std::memory_order_acquire); ++i) {
                auto [needs_sched, f] = send(raw, &worker_t::twice, t * 1000 + i);
                if (needs_sched) {
                    scheduler->enqueue(raw);
                }
                while (!f.is_ready()) {
                    std::this_thread::yield();
                }
                if (f.failed()) {
                    auto& counter = f.error() == std::make_error_code(std::errc::operation_canceled)
                                        ? refused_count
                                        : unexpected;
                    counter.fetch_add(1, std::memory_order_relaxed);
                } else {
                    answered.fetch_add(1, std::memory_order_relaxed);
                }
                if (i > 200) {
                    std::this_thread::yield();
                }
            }
        });
    }

    while (answered.load(std::memory_order_acquire) < 100) {
        std::this_thread::yield();
    }
    auto closed = actor->close();
    while (!closed.is_ready()) {
        std::this_thread::yield();
    }
    REQUIRE_FALSE(closed.failed());

    stop.store(true, std::memory_order_release);
    for (auto& s : senders) {
        s.join();
    }
    actor.reset(); // the scheduler keeps running: the closed actor has no job and gets none

    REQUIRE(answered.load() >= 100);
    REQUIRE(unexpected.load() == 0);
    scheduler->stop();
}

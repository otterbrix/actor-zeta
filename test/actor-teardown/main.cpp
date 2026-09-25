/// @file
/// Destroying an actor is destroying a coroutine: what its behavior is suspended in unwinds
/// first -- the method's locals, the futures it awaits, the caller's promise -- while the actor
/// is whole; then ~Actor runs and exactly sizeof(Actor) goes back to the resource.

#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <system_error>
#include <thread>

using namespace actor_zeta;

namespace {

    // Blocks and bytes: a deallocation with the wrong size leaves bytes behind.
    struct counting_resource final : std::pmr::memory_resource {
        std::pmr::memory_resource* upstream_ = std::pmr::new_delete_resource();
        std::atomic<long> blocks_{0};
        std::atomic<long> bytes_{0};

        void* do_allocate(std::size_t bytes, std::size_t align) override {
            blocks_.fetch_add(1, std::memory_order_relaxed);
            bytes_.fetch_add(static_cast<long>(bytes), std::memory_order_relaxed);
            return upstream_->allocate(bytes, align);
        }

        void do_deallocate(void* p, std::size_t bytes, std::size_t align) override {
            blocks_.fetch_sub(1, std::memory_order_relaxed);
            bytes_.fetch_sub(static_cast<long>(bytes), std::memory_order_relaxed);
            upstream_->deallocate(p, bytes, align);
        }

        bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
            return this == &other;
        }
    };

    struct teardown_log {
        int next = 0;
        int local_at = -1; // the suspended method's local
        int actor_at = -1; // ~waiter
    };

    struct local_probe {
        teardown_log* log_;
        ~local_probe() { log_->local_at = log_->next++; }
    };

    class waiter final : public basic_actor<waiter> {
    public:
        waiter(std::pmr::memory_resource* res, teardown_log* log)
            : basic_actor<waiter>(res)
            , log_(log) {}

        ~waiter() { log_->actor_at = log_->next++; }

        // Suspends on a promise nobody settles while the actor lives: at teardown it is mid-body.
        unique_future<int> wait() {
            local_probe probe{log_};
            pending_.emplace(resource());
            int v = co_await pending_->get_future();
            co_return v;
        }

        using dispatch_traits = actor_zeta::dispatch_traits<&waiter::wait>;

        behavior_t behavior(mailbox::message* msg) {
            if (msg->command() == msg_id<waiter, &waiter::wait>) {
                co_await dispatch(this, &waiter::wait, msg);
            }
        }

    private:
        teardown_log* log_;
        std::optional<promise<int>> pending_;
    };

    // Keeps a worker inside resume() until the owner is about to destroy the actor.
    class holder final : public basic_actor<holder> {
    public:
        holder(std::pmr::memory_resource* res, std::atomic<int>* stage)
            : basic_actor<holder>(res)
            , stage_(stage) {}

        unique_future<void> hold() {
            stage_->store(1, std::memory_order_release);
            while (stage_->load(std::memory_order_acquire) != 2) {
                std::this_thread::yield();
            }
            // Give the owner time to reach delete's wait for everyone inside.
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            co_return;
        }

        using dispatch_traits = actor_zeta::dispatch_traits<&holder::hold>;

        behavior_t behavior(mailbox::message* msg) {
            if (msg->command() == msg_id<holder, &holder::hold>) {
                co_await dispatch(this, &holder::hold, msg);
            }
        }

    private:
        std::atomic<int>* stage_;
    };

    // Sends wait() and runs the actor once: it comes back with the behavior suspended in wait().
    unique_future<int> suspend_in_wait(waiter* actor) {
        auto [needs_sched, caller] = send(actor, &waiter::wait);
        REQUIRE(needs_sched);
        const auto verdict = actor->resume(1);
        REQUIRE(verdict.result == scheduler::resume_result::resume);
        REQUIRE_FALSE(caller.is_ready());
        return std::move(caller);
    }

} // namespace

TEST_CASE("actor destroyed mid-await: the caller gets broken_pipe and nothing leaks") {
    counting_resource res;
    teardown_log log;
    {
        auto actor = spawn<waiter>(&res, &log);
        auto caller = suspend_in_wait(actor.get());

        actor.reset();

        REQUIRE(caller.is_ready());
        REQUIRE(caller.failed());
        REQUIRE(caller.error() == std::make_error_code(std::errc::broken_pipe));
    }
    REQUIRE(res.blocks_.load() == 0);
    REQUIRE(res.bytes_.load() == 0);
}

TEST_CASE("actor destroyed mid-await: the method's locals unwind before ~Actor") {
    counting_resource res;
    teardown_log log;
    {
        auto actor = spawn<waiter>(&res, &log);
        auto caller = suspend_in_wait(actor.get());
        actor.reset();
    }
    REQUIRE(log.local_at != -1);
    REQUIRE(log.local_at < log.actor_at);
}

TEST_CASE("unique_actor: destroying through the base runs ~Actor and frees sizeof(Actor)") {
    counting_resource res;
    teardown_log log;
    {
        waiter::unique_actor actor = spawn<waiter>(&res, &log);
    }
    REQUIRE(log.actor_at != -1);
    REQUIRE(res.blocks_.load() == 0);
    REQUIRE(res.bytes_.load() == 0);
}

// The destructor waits out a resume() still running, and resume() must not touch the actor
// once it leaves the actor -- it may be freed the next instant. ASan sees a later touch.
TEST_CASE("actor destroyed while a worker is finishing resume()") {
    counting_resource res;
    std::atomic<int> stage{0};
    {
        auto actor = spawn<holder>(&res, &stage);
        auto [needs_sched, sent] = send(actor.get(), &holder::hold);
        sent.detach();
        REQUIRE(needs_sched);

        std::thread worker([raw = actor.get()] {
            [[maybe_unused]] const auto verdict = raw->resume(1); // the actor is going away
        });
        while (stage.load(std::memory_order_acquire) != 1) {
            std::this_thread::yield();
        }
        stage.store(2, std::memory_order_release);
        actor.reset();
        worker.join();
    }
    REQUIRE(res.blocks_.load() == 0);
}

// The actor's loop is a coroutine generator whose frame comes from the actor's resource: spawn
// takes the actor and the frame, delete gives both back.
TEST_CASE("the actor's loop frame lives in the actor's resource") {
    counting_resource res;
    teardown_log log;
    {
        auto actor = spawn<waiter>(&res, &log);
        REQUIRE(res.blocks_.load() == 2);
    }
    REQUIRE(res.blocks_.load() == 0);
    REQUIRE(res.bytes_.load() == 0);
}

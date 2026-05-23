// -----------------------------------------------------------------------------
// actor-zeta + standalone Asio integration example.
//
// GOAL: inside an asio::awaitable<> coroutine, co_await a unique_future PRODUCED
// BY AN ACTOR.
//
// Model: an actor IS a coroutine (behavior_t) and runs on actor-zeta's own
// sharing_scheduler. Asio does not drive actors. The integration is the other
// direction — an Asio coroutine consumes an actor's unique_future.
//
// Two facts force the shape below:
//   * asio::awaitable / io_context use typeid in their executor machinery, so the
//     project's -fno-rtti requires ASIO_NO_TYPEID (see CMakeLists).
//   * asio::awaitable's promise has a CLOSED await_transform that only accepts Asio
//     async operations — an arbitrary awaiter cannot be co_awaited directly. So we
//     WRAP the unique_future as an Asio async operation via asio::async_initiate and
//     co_await it with asio::use_awaitable. That is the idiomatic "co_await a
//     unique_future inside asio::awaitable".
//
// The actor-zeta core never learns about Asio; only non-blocking is_ready()/
// take_ready() are used. Compiles with -fno-exceptions / -fno-rtti.
// Build: enable ALLOW_EXAMPLES and ALLOW_ASIO_EXAMPLE.
// -----------------------------------------------------------------------------

#include "asio_no_exceptions.hpp" // MUST be first: sets ASIO_* macros + throw shim

#include <iostream>
#include <memory>
#include <memory_resource>
#include <utility>

#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/use_awaitable.hpp>

#include <actor-zeta.hpp>

using namespace actor_zeta;

// An actor (= behavior_t coroutine). Its method produces a unique_future<int>.
class compute_actor final : public basic_actor<compute_actor> {
public:
    explicit compute_actor(std::pmr::memory_resource* res)
        : basic_actor<compute_actor>(res) {}

    unique_future<int> doubler(int x) {
        co_return x * 2;
    }

    using dispatch_traits = actor_zeta::dispatch_traits<&compute_actor::doubler>;

    behavior_t behavior(mailbox::message* msg) {
        if (msg->command() == msg_id<compute_actor, &compute_actor::doubler>) {
            co_await dispatch(this, &compute_actor::doubler, msg);
        }
    }
};

// -----------------------------------------------------------------------------
// EXAMPLE-LOCAL adapter: expose "wait until this unique_future is ready" as an
// Asio async operation, so it can be co_awaited inside an asio::awaitable.
//
// future_poller re-posts itself onto the io_context until the future is ready
// (the actor produces it on a sharing_scheduler worker thread), then completes
// the Asio handler with the value — on the io_context thread (race-free).
// -----------------------------------------------------------------------------
template<typename Handler>
struct future_poller {
    std::shared_ptr<unique_future<int>> fut;
    asio::io_context* ctx;
    Handler handler;

    void operator()() {
        if (fut->is_ready()) {
            int v = std::move(*fut).take_ready();
            std::move(handler)(v);
        } else {
            asio::post(*ctx, future_poller{std::move(fut), ctx, std::move(handler)});
        }
    }
};

template<typename CompletionToken>
auto async_get_future(std::shared_ptr<unique_future<int>> fut,
                      asio::io_context& ctx,
                      CompletionToken&& token) {
    return asio::async_initiate<CompletionToken, void(int)>(
        [&ctx](auto handler, std::shared_ptr<unique_future<int>> f) {
            using H = std::decay_t<decltype(handler)>;
            asio::post(ctx, future_poller<H>{std::move(f), &ctx, std::move(handler)});
        },
        token, std::move(fut));
}

// The Asio coroutine: co_await a unique_future produced by an actor.
asio::awaitable<void> consumer(asio::io_context& ctx,
                               compute_actor* actor,
                               scheduler::sharing_scheduler* sched,
                               int* out) {
    auto [needs_sched, fut] = send(actor, &compute_actor::doubler, 21);
    if (needs_sched) {
        sched->enqueue(actor); // actor runs on a scheduler worker thread
    }
    auto fp = std::make_shared<unique_future<int>>(std::move(fut));
    int v = co_await async_get_future(fp, ctx, asio::use_awaitable); // co_await INSIDE awaitable
    *out = v;
    co_return;
}

int main() {
    std::cout << "=== actor-zeta + Asio: co_await an actor's unique_future inside asio::awaitable ===\n\n";

    auto* res = std::pmr::get_default_resource();
    asio::io_context ctx;

    auto sched = std::make_unique<scheduler::sharing_scheduler>(2, 100);
    sched->start();
    auto actor = spawn<compute_actor>(res);

    int out = -1;
    asio::co_spawn(ctx, consumer(ctx, actor.get(), sched.get(), &out), asio::detached);

    ctx.run(); // drive the Asio coroutine to completion

    std::cout << "[main] asio::awaitable co_awaited actor doubler(21) = " << out << "\n";

    sched->stop(); // stop scheduler BEFORE the actor is destroyed (lifetime rule)

    std::cout << "\n=== Example complete ===\n";
    return out == 42 ? 0 : 1;
}
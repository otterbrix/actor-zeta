// co_await an actor's unique_future inside an asio::awaitable.
//
// Asio never drives actors. asio::awaitable's await_transform accepts only Asio
// operations, so the future is wrapped via async_initiate. ASIO_NO_TYPEID is
// required because -fno-rtti (set in CMakeLists.txt).

#include "asio_no_exceptions.hpp" // must precede every other <asio/...> include

#include <iostream>
#include <memory>
#include <memory_resource>
#include <utility>

#include <asio/as_tuple.hpp>
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/use_awaitable.hpp>

#include <actor-zeta.hpp>

using namespace actor_zeta;

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

// is_ready() is promise_released, which a promise dying without a value also
// sets. failed() is the value gate; take_ready() without it aborts.
template<typename Handler>
struct future_poller {
    std::shared_ptr<unique_future<int>> fut;
    asio::io_context* ctx;
    Handler handler;

    void operator()() {
        if (!fut->is_ready()) {
            asio::post(*ctx, future_poller{std::move(fut), ctx, std::move(handler)});
            return;
        }
        if (fut->failed()) {
            std::move(handler)(fut->error(), 0);
            return;
        }
        int v = std::move(*fut).take_ready();
        std::move(handler)(std::error_code{}, v);
    }
};

template<typename CompletionToken>
auto async_get_future(std::shared_ptr<unique_future<int>> fut,
                      asio::io_context& ctx,
                      CompletionToken&& token) {
    return asio::async_initiate<CompletionToken, void(std::error_code, int)>(
        [&ctx](auto handler, std::shared_ptr<unique_future<int>> f) {
            using H = std::decay_t<decltype(handler)>;
            asio::post(ctx, future_poller<H>{std::move(f), &ctx, std::move(handler)});
        },
        token, std::move(fut));
}

// as_tuple, not use_awaitable: the latter throws on error, and here that aborts.
asio::awaitable<void> consumer(asio::io_context& ctx,
                               compute_actor* actor,
                               scheduler::sharing_scheduler* sched,
                               int* out) {
    auto [needs_sched, fut] = send(actor, &compute_actor::doubler, 21);
    if (needs_sched) {
        sched->enqueue(actor);
    }
    auto fp = std::make_shared<unique_future<int>>(std::move(fut));
    auto [ec, v] = co_await async_get_future(fp, ctx, asio::as_tuple(asio::use_awaitable));
    if (ec) {
        std::cout << "[consumer] the actor did not produce a value: " << ec.message() << "\n";
        co_return;
    }
    *out = v;
    co_return;
}

// A future that completes without a value -- what a cancelled send leaves.
asio::awaitable<void> cancelled_consumer(asio::io_context& ctx,
                                         std::pmr::memory_resource* res,
                                         bool* saw_error) {
    promise<int> p(res);
    auto fp = std::make_shared<unique_future<int>>(p.get_future());
    p.error(std::make_error_code(std::errc::operation_canceled));

    auto [ec, v] = co_await async_get_future(fp, ctx, asio::as_tuple(asio::use_awaitable));
    *saw_error = static_cast<bool>(ec);
    std::cout << "[cancelled] ec = " << ec.message() << ", value = " << v << "\n";
    co_return;
}

int main() {
    std::cout << "=== actor-zeta + Asio: co_await an actor's unique_future inside asio::awaitable ===\n\n";

    auto* res = std::pmr::get_default_resource();
    asio::io_context ctx;

    auto sched = std::make_unique<scheduler::sharing_scheduler>(res, 2, 100);
    sched->start();
    auto actor = spawn<compute_actor>(res);

    int out = -1;
    bool saw_error = false;
    asio::co_spawn(ctx, consumer(ctx, actor.get(), sched.get(), &out), asio::detached);
    asio::co_spawn(ctx, cancelled_consumer(ctx, res, &saw_error), asio::detached);

    ctx.run();

    std::cout << "[main] asio::awaitable co_awaited actor doubler(21) = " << out << "\n";

    sched->stop(); // stop scheduler BEFORE the actor is destroyed (lifetime rule)

    std::cout << "\n=== Example complete ===\n";
    return (out == 42 && saw_error) ? 0 : 1;
}

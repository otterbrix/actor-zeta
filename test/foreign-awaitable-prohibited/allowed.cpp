/// The two awaitables a behavior may use, and nothing else.

#include <cstdio>
#include <memory_resource>

#include <actor-zeta.hpp>

using namespace actor_zeta;

namespace {

    class actor_t final : public basic_actor<actor_t> {
    public:
        explicit actor_t(std::pmr::memory_resource* ptr)
            : basic_actor<actor_t>(ptr) {}

        unique_future<int> compute(int x) { co_return x * 2; }

        unique_future<int> chain(int x) {
            // A method coroutine's own future: the other legitimate awaitable.
            auto inner = compute(x);
            const int doubled = co_await std::move(inner);
            co_return doubled + 1;
        }

        using dispatch_traits = actor_zeta::dispatch_traits<&actor_t::compute, &actor_t::chain>;

        behavior_t behavior(mailbox::message* msg) {
            const auto cmd = msg->command();
            if (cmd == msg_id<actor_t, &actor_t::compute>) {
                co_await dispatch(this, &actor_t::compute, msg);
            } else if (cmd == msg_id<actor_t, &actor_t::chain>) {
                co_await dispatch(this, &actor_t::chain, msg);
            }
        }
    };

} // namespace

int main() {
    auto* resource = std::pmr::get_default_resource();
    auto actor = spawn<actor_t>(resource);

    auto sent = send(actor.get(), &actor_t::chain, 20);
    sent.second.detach();
    while (actor->resume(4).messages_processed != 0) {
    }

    auto again = send(actor.get(), &actor_t::chain, 20);
    while (!again.second.is_ready()) {
        if (actor->resume(4).messages_processed == 0) {
            break;
        }
    }
    if (again.second.failed() || std::move(again.second).take_ready() != 41) {
        std::puts("the permitted awaitables stopped working");
        return 1;
    }

    std::puts("behaviors co_await actor-zeta awaitables, and only those");
    return 0;
}

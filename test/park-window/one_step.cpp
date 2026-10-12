/// @file
/// resume() is next(): one step of the actor's loop per call. A message that lands between the
/// loop's last pop_front() and the park makes the park fail -- the turn stays with the caller
/// (`resume`), and the message waits for the next resume(), not for a second step in this one.
/// Deterministic: a probe mailbox sends from inside try_block_impl(), before the CAS.

#include <cstdio>
#include <memory_resource>

#include <actor-zeta.hpp>

using namespace actor_zeta;

namespace {

    class worker_t;
    worker_t* g_target = nullptr;
    bool g_armed = false;

    void send_one_more();

    class probe_mailbox_impl : public mailbox::default_mailbox_impl {
    public:
        bool try_block_impl() {
            if (g_armed) {
                g_armed = false;
                send_one_more(); // lands on an empty inbox: the park below fails
            }
            return mailbox::default_mailbox_impl::try_block_impl();
        }
    };

    using probe_mailbox = mailbox::mailbox_t<probe_mailbox_impl>;

    class worker_t final : public actor::cooperative_actor<worker_t, probe_mailbox> {
    public:
        explicit worker_t(std::pmr::memory_resource* ptr)
            : actor::cooperative_actor<worker_t, probe_mailbox>(ptr) {}

        unique_future<void> ping() {
            ++processed;
            co_return;
        }

        using dispatch_traits = actor_zeta::dispatch_traits<&worker_t::ping>;

        behavior_t behavior(mailbox::message* msg) {
            if (msg->command() == msg_id<worker_t, &worker_t::ping>) {
                co_await dispatch(this, &worker_t::ping, msg);
            }
        }

        int processed = 0;
    };

    void send_one_more() {
        auto [needs_sched, f] = send(g_target, &worker_t::ping);
        f.detach();
        if (needs_sched) {
            std::printf("HARNESS BROKEN: the runner holds the turn; the late send cannot take it\n");
        }
    }

} // namespace

// Returns 0 on success, 1 on a failed check, 2 if the harness itself misfired.
int main() {
    std::pmr::synchronized_pool_resource resource;
    auto actor = spawn<worker_t>(&resource);
    g_target = actor.get();

    auto [owed, first] = send(actor.get(), &worker_t::ping);
    first.detach();
    if (!owed) {
        std::printf("HARNESS BROKEN: the first send to a fresh actor must hand out the turn\n");
        return 2;
    }

    g_armed = true;
    const auto step = actor->resume(8);
    if (g_armed) {
        std::printf("HARNESS BROKEN: the runner never tried to park\n");
        return 2;
    }
    if (step.result != scheduler::resume_result::resume || step.messages_processed != 1) {
        std::printf("one resume() ran more than one step: verdict %d, %zu messages (want resume, 1)\n",
                    static_cast<int>(step.result), step.messages_processed);
        return 1;
    }

    const auto next = actor->resume(8);
    if (next.result != scheduler::resume_result::awaiting || next.messages_processed != 1 ||
        actor->processed != 2) {
        std::printf("the next resume() did not take the late message: verdict %d, %zu messages, "
                    "%d processed\n",
                    static_cast<int>(next.result), next.messages_processed, actor->processed);
        return 1;
    }

    std::printf("one step per resume(): the late message came with the next one\n");
    return 0;
}

/// @file
/// The window between the park -- a successful try_block() -- and the end of resume(). Once the mailbox
/// is blocked, the turn is back in it: a sender landing in the window must be handed the turn
/// (needs_sched == true), the runner must report `awaiting`, and running the actor on the
/// sender's turn delivers the message -- one turn, never zero (stranded), never two.
/// Deterministic, not stress: a probe mailbox holds the runner inside the window.

#include <atomic>
#include <cstdio>
#include <memory_resource>
#include <thread>

#include <actor-zeta.hpp>

using namespace actor_zeta;

namespace {

    std::atomic<bool> g_hold_runner{false};
    std::atomic<bool> g_runner_parked{false};
    std::atomic<bool> g_sender_done{false};

    class probe_mailbox_impl : public mailbox::default_mailbox_impl {
    public:
        bool try_block_impl() {
            const bool blocked = mailbox::default_mailbox_impl::try_block_impl();
            // The mailbox is blocked and the runner is still inside resume(): the window.
            if (blocked && g_hold_runner.load(std::memory_order_acquire)) {
                g_runner_parked.store(true, std::memory_order_release);
                while (!g_sender_done.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
            }
            return blocked;
        }
    };

    using probe_mailbox = mailbox::mailbox_t<probe_mailbox_impl>;

    class worker_t final : public actor::cooperative_actor<worker_t, probe_mailbox> {
    public:
        explicit worker_t(std::pmr::memory_resource* ptr)
            : actor::cooperative_actor<worker_t, probe_mailbox>(ptr) {}

        unique_future<void> ping() {
            processed.fetch_add(1, std::memory_order_relaxed);
            co_return;
        }

        using dispatch_traits = actor_zeta::dispatch_traits<&worker_t::ping>;

        behavior_t behavior(mailbox::message* msg) {
            if (msg->command() == msg_id<worker_t, &worker_t::ping>) {
                co_await dispatch(this, &worker_t::ping, msg);
            }
        }

        static std::atomic<int> processed;
    };

    std::atomic<int> worker_t::processed{0};

} // namespace

// Returns 0 on success, 1 on a failed check, 2 if the harness itself misfired.
int main() {
    std::pmr::synchronized_pool_resource pool;
    auto* resource = &pool;
    auto actor = spawn<worker_t>(resource);
    auto* raw = actor.get();

    // Born parked: the first send hands out the turn; the resume drains it and parks -- the window.
    auto first = send(raw, &worker_t::ping);
    if (!first.first) {
        std::printf("HARNESS BROKEN: the first send to a fresh actor must report needs_sched\n");
        return 2;
    }
    first.second.detach();

    g_hold_runner.store(true, std::memory_order_release);

    std::atomic<bool> second_needs_sched{false};
    std::thread sender([&] {
        while (!g_runner_parked.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        auto sent = send(raw, &worker_t::ping);
        second_needs_sched.store(sent.first, std::memory_order_release);
        sent.second.detach();
        g_sender_done.store(true, std::memory_order_release);
    });

    const auto verdict = raw->resume(8);
    sender.join();
    g_hold_runner.store(false, std::memory_order_release);

    if (!g_runner_parked.load(std::memory_order_acquire)) {
        std::printf("HARNESS BROKEN: the runner never reached the window\n");
        return 2;
    }

    if (!second_needs_sched.load(std::memory_order_acquire)) {
        std::printf("the sender unblocked a parked mailbox but was not handed the turn: nobody "
                    "runs the actor for its message\n");
        return 1;
    }

    if (verdict.result != scheduler::resume_result::awaiting) {
        std::printf("the runner parked but did not report `awaiting`: with the sender's turn "
                    "that is two turns for one actor\n");
        return 1;
    }

    // The sender's turn, honoured the way a worker does; the message must come out.
    while (raw->resume(8).result == scheduler::resume_result::resume) {
    }

    if (worker_t::processed.load(std::memory_order_acquire) != 2) {
        std::printf("processed %d of 2\n", worker_t::processed.load(std::memory_order_acquire));
        return 1;
    }

    std::printf("the parked runner gave the turn to the sender; both messages delivered\n");
    return 0;
}

/// @file
/// A sender already inside enqueue_impl when the actor is destroyed.
///
/// enqueue_impl checks is_destroying() and then calls mailbox().push_back(). The
/// destructor sets destroying and waits -- but wait_for_resume_to_complete() waits
/// only on the `running` bit, and a sender is not running. So the destructor can
/// finish, ~default_mailbox_impl can free the queues, and the sender is left
/// writing into them.
///
/// The is_destroying() check at the top of enqueue_impl is what makes this a defect
/// rather than a contract violation: the check exists precisely to make a concurrent
/// send safe during teardown, and half-closing the window is worse than not trying.
/// What is guaranteed is narrow and worth stating exactly: a sender that has
/// REGISTERED is waited for. A sender still between its first load of the state word
/// and a successful registration holds a raw pointer to an object that may already be
/// gone, and no design here can help that -- it is the "Destroy actor while scheduler
/// running" row in CLAUDE.md, widened to any thread that sends.
///
/// Deterministic, not statistical: MailBox is a template parameter, so the test
/// supplies one that parks inside push_back until told to continue. The main thread
/// releases it on a timer, then destroys the actor. Without the fix the destructor
/// returns first and the sender wakes up holding freed memory; with the fix the
/// destructor waits for the sender to leave.
///
/// Meaningful under AddressSanitizer. In an ordinary build it still exercises the
/// ordering and must not hang.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory_resource>
#include <thread>

#include <actor-zeta.hpp>

using namespace actor_zeta;

namespace {

    std::atomic<bool> g_sender_inside{false};
    std::atomic<bool> g_sender_may_continue{false};

    // Parks inside push_back so the destructor can be made to race a sender that is
    // provably already past enqueue_impl's is_destroying() check.
    class probe_mailbox_impl : public mailbox::default_mailbox_impl {
    public:
        actor_zeta::detail::enqueue_result push_back_impl(mailbox::message_ptr ptr) {
            g_sender_inside.store(true, std::memory_order_release);
            while (!g_sender_may_continue.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            return mailbox::default_mailbox_impl::push_back_impl(std::move(ptr));
        }
    };

    using probe_mailbox = mailbox::mailbox_t<probe_mailbox_impl>;

    class worker_t final : public actor::cooperative_actor<worker_t, probe_mailbox> {
    public:
        explicit worker_t(std::pmr::memory_resource* ptr)
            : actor::cooperative_actor<worker_t, probe_mailbox>(ptr) {}

        unique_future<void> ping() { co_return; }

        using dispatch_traits = actor_zeta::dispatch_traits<&worker_t::ping>;

        behavior_t behavior(mailbox::message* msg) {
            if (msg->command() == msg_id<worker_t, &worker_t::ping>) {
                co_await dispatch(this, &worker_t::ping, msg);
            }
        }
    };

} // namespace

int main() {
    auto* resource = std::pmr::get_default_resource();
    auto actor = spawn<worker_t>(resource);
    auto* raw = actor.get();

    std::thread sender([raw] {
        auto sent = send(raw, &worker_t::ping);
        sent.second.detach();
    });

    while (!g_sender_inside.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    // Let the sender out only after the destructor has had time to run to completion
    // on its own. Without the fix it does exactly that, and the sender then writes
    // into a freed mailbox.
    std::thread releaser([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        g_sender_may_continue.store(true, std::memory_order_release);
    });

    actor.reset();

    releaser.join();
    sender.join();

    std::puts("destructor waited for the in-flight sender");
    return 0;
}

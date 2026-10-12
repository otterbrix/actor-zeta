/// @file
/// A sender already inside enqueue_impl when the actor is destroyed. It entered the
/// actor in the same RMW that read `destroying`, and delete waits for everyone inside;
/// without it the mailbox is freed under push_back. Only a REGISTERED sender is covered
/// -- one before registration holds a raw pointer to a possibly-dead object. Deterministic:
/// the probe MailBox parks in push_back until a timer releases it after the destructor had
/// time to finish. Meaningful under ASan; a plain build only proves it does not hang.

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

    std::thread releaser([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50)); // after the destructor has had time to finish on its own
        g_sender_may_continue.store(true, std::memory_order_release);
    });

    actor.reset();

    releaser.join();
    sender.join();

    std::puts("destructor waited for the in-flight sender");
    return 0;
}

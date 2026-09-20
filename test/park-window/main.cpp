/// @file
/// The window between park()'s try_block() and ~resume_guard's CAS. A sender landing
/// there gets `unblocked_reader` while the runner still holds `running`, so
/// leave_and_maybe_schedule() must set the `scheduled` bit and report needs_sched ==
/// FALSE; ~resume_guard then upgrades awaiting -> resume: one job node, never zero
/// (stranded), never two. Deterministic, not stress: a probe mailbox parks the runner
/// inside the window, once per guard. Phase A parks in try_block_impl(), where
/// check_race_window() downgrades on its own. Phase B parks in blocked_impl() and
/// returns the pre-sender value, so park() commits to `awaiting` and only the
/// `scheduled` bit can rescue the message; without B, dropping the bit stays green.

#include <atomic>
#include <cstdio>
#include <memory_resource>
#include <thread>

#include <actor-zeta.hpp>

using namespace actor_zeta;

namespace {

    enum class phase { off, before_race_window, after_race_window };

    std::atomic<phase> g_phase{phase::off};
    std::atomic<bool> g_runner_parked{false};
    std::atomic<bool> g_sender_done{false};
    std::atomic<bool> g_just_blocked{false};

    void park_runner_until_sender_is_done() {
        g_runner_parked.store(true, std::memory_order_release);
        while (!g_sender_done.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }

    class probe_mailbox_impl : public mailbox::default_mailbox_impl {
    public:
        bool try_block_impl() {
            const bool blocked = mailbox::default_mailbox_impl::try_block_impl();
            if (!blocked) {
                return blocked;
            }
            // Inbox is parked and `running` is still held: this is the window.
            if (g_phase.load(std::memory_order_acquire) == phase::before_race_window) {
                park_runner_until_sender_is_done();
            } else if (g_phase.load(std::memory_order_acquire) == phase::after_race_window) {
                g_just_blocked.store(true, std::memory_order_release);
            }
            return blocked;
        }

        bool blocked_impl() const noexcept {
            const bool value = mailbox::default_mailbox_impl::blocked_impl();
            // Only the call check_race_window() makes right after try_block() succeeded.
            if (g_phase.load(std::memory_order_acquire) == phase::after_race_window &&
                g_just_blocked.exchange(false, std::memory_order_acq_rel)) {
                park_runner_until_sender_is_done(); // then return the stale value: park() must commit to awaiting
            }
            return value;
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

namespace {

    // Returns 0 on success, 1 on a failed check, 2 if the harness itself misfired.
    int run_phase(phase which, const char* label) {
        g_phase.store(phase::off, std::memory_order_release);
        g_runner_parked.store(false, std::memory_order_release);
        g_sender_done.store(false, std::memory_order_release);
        g_just_blocked.store(false, std::memory_order_release);
        worker_t::processed.store(0, std::memory_order_release);

        auto* resource = std::pmr::get_default_resource();
        auto actor = spawn<worker_t>(resource);
        auto* raw = actor.get();

        // Born parked: the first send owes the scheduling; the resume drains it and parks again -- the window.
        auto first = send(raw, &worker_t::ping);
        if (!first.first) {
            std::printf("[%s] HARNESS BROKEN: first send to a fresh actor must report "
                        "needs_sched\n", label);
            return 2;
        }
        first.second.detach();

        g_phase.store(which, std::memory_order_release);

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
        g_phase.store(phase::off, std::memory_order_release);

        if (!g_runner_parked.load(std::memory_order_acquire)) {
            std::printf("[%s] HARNESS BROKEN: the runner never reached the window\n", label);
            return 2;
        }

        if (second_needs_sched.load(std::memory_order_acquire)) {
            std::printf("[%s] the sender was told to enqueue while the runner still held "
                        "running -- that is a second job node\n", label);
            return 1;
        }

        if (verdict.result != scheduler::resume_result::resume) {
            std::printf("[%s] verdict was `awaiting`: the only job node is dropped and the "
                        "message the sender left behind is stranded\n", label);
            return 1;
        }

        // Honour the verdict the way a worker does; the message must come out.
        for (int i = 0; i < 16 && worker_t::processed.load(std::memory_order_acquire) < 2; ++i) {
            const auto info = raw->resume(8);
            if (info.result == scheduler::resume_result::awaiting) {
                break;
            }
        }

        if (worker_t::processed.load(std::memory_order_acquire) != 2) {
            std::printf("[%s] processed %d of 2\n", label,
                        worker_t::processed.load(std::memory_order_acquire));
            return 1;
        }

        std::printf("[%s] sender owes nothing, verdict upgraded to resume, both "
                    "messages delivered\n", label);
        return 0;
    }

} // namespace

int main() {
    if (const int rc = run_phase(phase::before_race_window, "check_race_window")) {
        return rc;
    }
    if (const int rc = run_phase(phase::after_race_window, "scheduled_while_running")) {
        return rc;
    }
    return 0;
}

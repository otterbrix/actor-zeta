/// @file
/// The verdict a second resume() gets while another thread holds `running`.
///
/// It used to be `done`. That is the verdict for "finished", and the worker reacts
/// to it by calling policy_.after_completion() and dropping the node -- which is
/// harmless only because the shipped policy's hook is empty. Any profiling policy
/// that retires an actor there, or any external driver that treats `done` as
/// terminal, would retire an actor that is merely contended.
///
/// What actually happened is `awaiting`: try_acquire_running() has just set the
/// scheduled bit on behalf of the running thread, and that thread discharges the
/// obligation by returning `resume`. "Drop your node, the wakeup belongs to somebody
/// else" is exactly what awaiting means.
///
/// No threads here. Re-entering resume() from inside behavior() is deterministic:
/// the actor's own `running` bit is already set, so the nested call takes the
/// already-running branch every time.

#include <cstdio>
#include <memory_resource>

#include <actor-zeta.hpp>

using namespace actor_zeta;

namespace {

    class probe_t final : public basic_actor<probe_t> {
    public:
        explicit probe_t(std::pmr::memory_resource* ptr)
            : basic_actor<probe_t>(ptr) {}

        unique_future<void> poke() {
            // Re-enter while we provably hold `running`.
            nested = self_->resume(1);
            saw_nested = true;
            co_return;
        }

        using dispatch_traits = actor_zeta::dispatch_traits<&probe_t::poke>;

        behavior_t behavior(mailbox::message* msg) {
            if (msg->command() == msg_id<probe_t, &probe_t::poke>) {
                co_await dispatch(this, &probe_t::poke, msg);
            }
        }

        probe_t* self_ = nullptr;
        scheduler::resume_info nested{};
        bool saw_nested = false;
    };

} // namespace

int main() {
    auto* resource = std::pmr::get_default_resource();
    auto actor = spawn<probe_t>(resource);
    actor->self_ = actor.get();

    // A fresh actor is born with a blocked inbox, so the first send owes a schedule.
    auto sent = send(actor.get(), &probe_t::poke);
    if (!sent.first) {
        std::puts("HARNESS BROKEN: first send to a fresh actor must report needs_sched");
        return 2;
    }
    sent.second.detach();

    const auto outer = actor->resume(1);

    if (!actor->saw_nested) {
        std::puts("HARNESS BROKEN: behavior() never re-entered resume()");
        return 2;
    }

    if (actor->nested.result != scheduler::resume_result::awaiting) {
        std::printf("contended resume reported %d, expected awaiting\n",
                    static_cast<int>(actor->nested.result));
        return 1;
    }

    if (actor->nested.messages_processed != 0) {
        std::puts("contended resume claimed to have processed messages");
        return 1;
    }

    // The obligation the nested call handed over must come back out as `resume`.
    if (outer.result != scheduler::resume_result::resume) {
        std::printf("outer verdict %d, expected resume: the scheduled bit the nested "
                    "call set has no job behind it\n", static_cast<int>(outer.result));
        return 1;
    }

    // And the actor must still be reachable afterwards.
    auto again = send(actor.get(), &probe_t::poke);
    again.second.detach();
    while (actor->resume(1).messages_processed != 0) {
    }
    auto last = send(actor.get(), &probe_t::poke);
    last.second.detach();
    if (!last.first) {
        std::puts("actor stranded: a parked actor must report needs_sched");
        return 1;
    }

    std::puts("contended resume: awaiting, and the runner returns resume");
    return 0;
}

/// The shape that must compile and work: send() first, discharge needs_sched, then
/// co_await the future on its own.

#include <cstdio>
#include <memory_resource>

#include <actor-zeta.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>

using namespace actor_zeta;

namespace {

    class worker_t final : public basic_actor<worker_t> {
    public:
        explicit worker_t(std::pmr::memory_resource* ptr)
            : basic_actor<worker_t>(ptr) {}

        unique_future<int> compute(int x) { co_return x * 2; }

        using dispatch_traits = actor_zeta::dispatch_traits<&worker_t::compute>;

        behavior_t behavior(mailbox::message* msg) {
            if (msg->command() == msg_id<worker_t, &worker_t::compute>) {
                co_await dispatch(this, &worker_t::compute, msg);
            }
        }
    };

} // namespace

int main() {
    auto* resource = std::pmr::get_default_resource();
    auto worker = spawn<worker_t>(resource);

    // The two-step form: take the obligation, discharge it, then await.
    auto [needs_sched, future] = send(worker.get(), &worker_t::compute, 21);
    while (!future.is_ready()) {
        auto info = worker->resume(1);
        if (!info.messages_processed && !needs_sched) {
            break;
        }
        needs_sched = false;
    }

    if (future.failed()) {
        std::puts("two-step send failed");
        return 1;
    }
    const int result = std::move(future).take_ready();
    if (result != 42) {
        std::printf("expected 42, got %d\n", result);
        return 1;
    }
    std::puts("two-step send(): obligation discharged, future awaited separately");
    return 0;
}

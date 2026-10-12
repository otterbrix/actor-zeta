/// The shape that must compile and work: the argument is taken by value, so the method
/// owns it for the whole call, across any suspension.

#include <cstddef>
#include <cstdio>
#include <memory_resource>
#include <string>

#include <actor-zeta.hpp>

using namespace actor_zeta;

namespace {

    class worker_t final : public basic_actor<worker_t> {
    public:
        explicit worker_t(std::pmr::memory_resource* ptr)
            : basic_actor<worker_t>(ptr) {}

        unique_future<std::size_t> store(std::string value) { co_return value.size(); }

        using dispatch_traits = actor_zeta::dispatch_traits<&worker_t::store>;

        behavior_t behavior(mailbox::message* msg) {
            if (msg->command() == msg_id<worker_t, &worker_t::store>) {
                co_await dispatch(this, &worker_t::store, msg);
            }
        }
    };

} // namespace

int main() {
    std::pmr::unsynchronized_pool_resource pool;
    auto* resource = &pool;
    auto worker = spawn<worker_t>(resource);

    auto [needs_sched, future] = send(worker.get(), &worker_t::store, std::string("abc"));
    if (!needs_sched) {
        std::puts("HARNESS BROKEN: a fresh actor must report needs_sched");
        return 2;
    }
    for (int i = 0; i < 16 && !future.is_ready(); ++i) {
        if (worker->resume(1).result != scheduler::resume_result::resume) {
            break;
        }
    }

    if (!future.is_ready() || future.failed()) {
        std::puts("by-value parameter: the call did not complete with a value");
        return 1;
    }
    const std::size_t result = std::move(future).take_ready();
    if (result != 3) {
        std::printf("expected 3, got %zu\n", result);
        return 1;
    }
    std::puts("by-value parameter: argument owned by the method");
    return 0;
}

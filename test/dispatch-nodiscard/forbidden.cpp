// Must NOT compile with -Werror: dispatch()'s future is dropped, and with it the method's run
// (a dropped future destroys its producer's frame).
#include <actor-zeta.hpp>

using namespace actor_zeta;

class worker_t final : public basic_actor<worker_t> {
public:
    explicit worker_t(std::pmr::memory_resource* res)
        : basic_actor<worker_t>(res) {}

    unique_future<void> ping() { co_return; }

    using dispatch_traits = actor_zeta::dispatch_traits<&worker_t::ping>;

    behavior_t behavior(mailbox::message* msg) {
        dispatch(this, &worker_t::ping, msg); // forgot co_await
        co_return;
    }
};

int main() {
    std::pmr::unsynchronized_pool_resource resource;
    auto worker = spawn<worker_t>(&resource);
    return worker ? 0 : 1;
}

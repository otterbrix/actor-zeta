/// A behavior that co_yields. Same hazard as a foreign awaitable: yield_value would
/// be a suspension point outside await_transform. Built only by try_compile.

#include <memory_resource>

#include <actor-zeta.hpp>

using namespace actor_zeta;

namespace {

    class actor_t final : public basic_actor<actor_t> {
    public:
        explicit actor_t(std::pmr::memory_resource* ptr)
            : basic_actor<actor_t>(ptr) {}

        unique_future<void> ping() { co_return; }

        using dispatch_traits = actor_zeta::dispatch_traits<&actor_t::ping>;

        behavior_t behavior(mailbox::message*) {
            co_yield 1;
        }
    };

} // namespace

int main() { return 0; }

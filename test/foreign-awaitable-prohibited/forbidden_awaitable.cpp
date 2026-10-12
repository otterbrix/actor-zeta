/// A behavior that co_awaits something that is not an actor-zeta awaitable.
/// Built only by try_compile; the configure step fails if this file builds.

#include <memory_resource>

#include <actor-zeta.hpp>

using namespace actor_zeta;

namespace {

    struct foreign_awaitable {
        bool await_ready() const noexcept { return true; }
        void await_suspend(detail::coroutine_handle<>) const noexcept {}
        void await_resume() const noexcept {}
    };

    class actor_t final : public basic_actor<actor_t> {
    public:
        explicit actor_t(std::pmr::memory_resource* ptr)
            : basic_actor<actor_t>(ptr) {}

        unique_future<void> ping() { co_return; }

        using dispatch_traits = actor_zeta::dispatch_traits<&actor_t::ping>;

        behavior_t behavior(mailbox::message*) {
            // A suspension point that never runs propagate_awaited_state(), so the
            // behavior could be suspended and not is_busy() -- and the actor's loop
            // would take the live behavior for finished.
            co_await foreign_awaitable{};
        }
    };

} // namespace

int main() { return 0; }

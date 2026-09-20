/// The shape that must NOT compile. Built only by try_compile from CMakeLists.txt,
/// which fails the configure step if this file builds.

#include <memory_resource>

#include <actor-zeta.hpp>

using namespace actor_zeta;

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

class caller_t final : public basic_actor<caller_t> {
public:
    caller_t(std::pmr::memory_resource* ptr, address_t peer)
        : basic_actor<caller_t>(ptr)
        , peer_(std::move(peer)) {}

    unique_future<int> ask(int x) {
        // send() returns {needs_sched, future}. Awaiting the pair suspends without
        // ever scheduling peer_, so this can never complete.
        auto [needs_sched, result] = co_await send(peer_, &worker_t::compute, x);
        (void) needs_sched;
        co_return result;
    }

    using dispatch_traits = actor_zeta::dispatch_traits<&caller_t::ask>;

    behavior_t behavior(mailbox::message* msg) {
        if (msg->command() == msg_id<caller_t, &caller_t::ask>) {
            co_await dispatch(this, &caller_t::ask, msg);
        }
    }

private:
    address_t peer_;
};

int main() { return 0; }

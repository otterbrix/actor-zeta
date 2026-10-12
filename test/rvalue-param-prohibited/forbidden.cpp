/// The shape that must NOT compile. Built only by try_compile from CMakeLists.txt,
/// which fails the configure step if this file builds.

#include <cstddef>
#include <memory_resource>
#include <string>

#include <actor-zeta.hpp>

using namespace actor_zeta;

class worker_t final : public basic_actor<worker_t> {
public:
    explicit worker_t(std::pmr::memory_resource* ptr)
        : basic_actor<worker_t>(ptr) {}

    // A copyable T&&: the parameter would refer into the message body instead of owning
    // the argument, so the argument does not cross the actor boundary by value.
    unique_future<std::size_t> store(std::string&& value) { co_return value.size(); }

    using dispatch_traits = actor_zeta::dispatch_traits<&worker_t::store>;

    behavior_t behavior(mailbox::message* msg) {
        if (msg->command() == msg_id<worker_t, &worker_t::store>) {
            co_await dispatch(this, &worker_t::store, msg);
        }
    }
};

int main() { return 0; }

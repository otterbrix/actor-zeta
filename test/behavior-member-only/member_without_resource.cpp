// Must NOT compile: the owner of behavior() has no resource() for the frame.
#include <actor-zeta.hpp>

struct not_an_actor {
    actor_zeta::behavior_t behavior(actor_zeta::mailbox::message*) {
        co_return;
    }
};

int main() { return 0; }

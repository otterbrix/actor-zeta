// Must NOT compile: a free function has no actor to take the frame's memory resource from.
#include <actor-zeta.hpp>

actor_zeta::behavior_t free_behavior(actor_zeta::mailbox::message*) {
    co_return;
}

int main() { return 0; }

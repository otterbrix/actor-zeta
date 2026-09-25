// Must NOT compile: no arguments at all, so nothing can supply the frame's memory resource.
#include <actor-zeta.hpp>

actor_zeta::behavior_t free_behavior() {
    co_return;
}

int main() { return 0; }

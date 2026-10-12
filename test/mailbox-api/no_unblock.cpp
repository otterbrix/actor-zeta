// Must NOT compile: a mailbox hands its turn out only through push_back(); unparking it any other way
// would hand out a second one.
#include <actor-zeta.hpp>

int main() {
    actor_zeta::mailbox::default_mailbox box;
    return box.try_unblock() ? 0 : 1;
}

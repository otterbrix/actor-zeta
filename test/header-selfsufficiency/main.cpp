// The check is the BUILD: one generated translation unit per public header,
// each including only that header. Reaching main() means every one of them
// compiled alone.
//
// Nothing is asserted at run time on purpose -- a header cannot fail to be
// self-sufficient at run time, only at compile time.

#include <cstdio>

int main() {
    std::puts("every public header compiled standalone");
    return 0;
}

/// @file
/// The sharing scheduler takes its memory from a resource, never from the global operator new:
/// once warmed up, send -> enqueue -> resume makes no global allocation at all. Global new is
/// counted here, program-wide; the actor and the scheduler both run on a malloc-backed resource.
/// No Catch2: it allocates on its own.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <memory_resource>
#include <new>
#include <thread>

#include <actor-zeta.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>

namespace {

    std::atomic<long> g_news{0};
    std::atomic<bool> g_counting{false};

    void* counted(std::size_t n, std::size_t align) {
        if (g_counting.load(std::memory_order_relaxed)) {
            g_news.fetch_add(1, std::memory_order_relaxed);
        }
        void* p = nullptr;
        if (posix_memalign(&p, align < alignof(void*) ? alignof(void*) : align, n ? n : 1) != 0) {
            std::abort();
        }
        return p;
    }

} // namespace

void* operator new(std::size_t n) { return counted(n, alignof(std::max_align_t)); }
void* operator new[](std::size_t n) { return counted(n, alignof(std::max_align_t)); }
void* operator new(std::size_t n, std::align_val_t a) { return counted(n, static_cast<std::size_t>(a)); }
void* operator new[](std::size_t n, std::align_val_t a) { return counted(n, static_cast<std::size_t>(a)); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }

namespace {

    struct malloc_resource final : std::pmr::memory_resource {
        void* do_allocate(std::size_t bytes, std::size_t align) override {
            void* p = nullptr;
            if (posix_memalign(&p, align < alignof(void*) ? alignof(void*) : align, bytes ? bytes : 1) != 0) {
                std::abort();
            }
            return p;
        }

        void do_deallocate(void* p, std::size_t, std::size_t) override { std::free(p); }

        bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
            return this == &other;
        }
    };

    class echo_t final : public actor_zeta::basic_actor<echo_t> {
    public:
        explicit echo_t(std::pmr::memory_resource* res)
            : actor_zeta::basic_actor<echo_t>(res) {}

        actor_zeta::unique_future<int> twice(int x) { co_return x * 2; }

        using dispatch_traits = actor_zeta::dispatch_traits<&echo_t::twice>;

        actor_zeta::behavior_t behavior(actor_zeta::mailbox::message* msg) {
            if (msg->command() == actor_zeta::msg_id<echo_t, &echo_t::twice>) {
                co_await actor_zeta::dispatch(this, &echo_t::twice, msg);
            }
        }
    };

    template<class Scheduler>
    bool round_trip(Scheduler& scheduler, echo_t* actor, int i) {
        auto [needs_sched, f] = actor_zeta::send(actor, &echo_t::twice, i);
        if (needs_sched) {
            scheduler.enqueue(actor);
        }
        while (!f.is_ready()) {
            std::this_thread::yield();
        }
        return !f.failed() && std::move(f).take_ready() == i * 2;
    }

} // namespace

int main() {
    malloc_resource resource;
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(&resource, 2, 16);
    scheduler->start();
    {
        auto actor = actor_zeta::spawn<echo_t>(&resource);

        for (int i = 0; i < 2000; ++i) { // warm up: the scheduler's buffers reach their size
            if (!round_trip(*scheduler, actor.get(), i)) {
                std::printf("HARNESS BROKEN: a round trip failed during warm-up\n");
                return 2;
            }
        }

        g_counting.store(true, std::memory_order_relaxed);
        for (int i = 0; i < 20000; ++i) {
            if (!round_trip(*scheduler, actor.get(), i)) {
                std::printf("HARNESS BROKEN: a round trip failed\n");
                return 2;
            }
        }
        g_counting.store(false, std::memory_order_relaxed);
    }
    scheduler->stop();

    const long news = g_news.load();
    if (news != 0) {
        std::printf("%ld global allocations in 20000 send -> enqueue -> resume cycles (want 0)\n", news);
        return 1;
    }
    std::printf("send -> enqueue -> resume: no global allocation\n");
    return 0;
}

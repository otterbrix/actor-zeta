#include <benchmark/benchmark.h>
#include <actor-zeta.hpp>
#include <cstdint>

using namespace actor_zeta;

// =====================================================================
// OLD STYLE: Switch-based dispatch
// =====================================================================

class old_style_actor : public base::basic_actor<old_style_actor> {
public:
    using dispatch_traits = base::dispatch_traits<
        &old_style_actor::method1,
        &old_style_actor::method2,
        &old_style_actor::method3,
        &old_style_actor::method4,
        &old_style_actor::method5
    >;

    explicit old_style_actor(pmr::memory_resource* resource)
        : base::basic_actor<old_style_actor>(resource)
        , behavior1_(make_behavior(resource, this, &old_style_actor::method1))
        , behavior2_(make_behavior(resource, this, &old_style_actor::method2))
        , behavior3_(make_behavior(resource, this, &old_style_actor::method3))
        , behavior4_(make_behavior(resource, this, &old_style_actor::method4))
        , behavior5_(make_behavior(resource, this, &old_style_actor::method5))
        , counter_(0) {}

    void behavior(mailbox::message* msg) override {
        auto cmd = msg->command();
        if (cmd == msg_id<old_style_actor, &old_style_actor::method1>) {
            behavior1_(msg);
        } else if (cmd == msg_id<old_style_actor, &old_style_actor::method2>) {
            behavior2_(msg);
        } else if (cmd == msg_id<old_style_actor, &old_style_actor::method3>) {
            behavior3_(msg);
        } else if (cmd == msg_id<old_style_actor, &old_style_actor::method4>) {
            behavior4_(msg);
        } else if (cmd == msg_id<old_style_actor, &old_style_actor::method5>) {
            behavior5_(msg);
        }
    }

    void method1(int x) { counter_ += x; }
    void method2(int x) { counter_ += x * 2; }
    void method3(int x) { counter_ += x * 3; }
    void method4(int x) { counter_ += x * 4; }
    void method5(int x) { counter_ += x * 5; }

    int counter() const { return counter_; }

private:
    base::behavior_t behavior1_;
    base::behavior_t behavior2_;
    base::behavior_t behavior3_;
    base::behavior_t behavior4_;
    base::behavior_t behavior5_;
    int counter_;
};

// =====================================================================
// Benchmark: Old style dispatch
// =====================================================================

static void BM_OldStyleDispatch(benchmark::State& state) {
    auto resource = pmr::get_default_resource();
    auto actor = spawn<old_style_actor>(resource);

    int method_id = static_cast<int>(state.range(0));

    for (auto _ : state) {
        switch (method_id) {
            case 0: {
                auto f = send(actor, base::address_t::empty_address(), &old_style_actor::method1, 1);
                while (!f.is_ready()) { actor->resume(1); }
                benchmark::DoNotOptimize(f.get());
                break;
            }
            case 1: {
                auto f = send(actor, base::address_t::empty_address(), &old_style_actor::method2, 2);
                while (!f.is_ready()) { actor->resume(1); }
                benchmark::DoNotOptimize(f.get());
                break;
            }
            case 2: {
                auto f = send(actor, base::address_t::empty_address(), &old_style_actor::method3, 3);
                while (!f.is_ready()) { actor->resume(1); }
                benchmark::DoNotOptimize(f.get());
                break;
            }
            case 3: {
                auto f = send(actor, base::address_t::empty_address(), &old_style_actor::method4, 4);
                while (!f.is_ready()) { actor->resume(1); }
                benchmark::DoNotOptimize(f.get());
                break;
            }
            case 4: {
                auto f = send(actor, base::address_t::empty_address(), &old_style_actor::method5, 5);
                while (!f.is_ready()) { actor->resume(1); }
                benchmark::DoNotOptimize(f.get());
                break;
            }
        }
    }

    state.SetItemsProcessed(state.iterations());
    benchmark::DoNotOptimize(actor->counter());
}

BENCHMARK(BM_OldStyleDispatch)->DenseRange(0, 4)->Unit(benchmark::kNanosecond);

// =====================================================================
// TODO: NEW STYLE with fold expression dispatch (after implementation)
// =====================================================================

// static void BM_NewStyleDispatch(benchmark::State& state) {
//     // Will be implemented in Phase 2-3
// }
// BENCHMARK(BM_NewStyleDispatch)->DenseRange(0, 4)->Unit(benchmark::kNanosecond);

BENCHMARK_MAIN();
#include <benchmark/benchmark.h>
#include <actor-zeta.hpp>
#include <actor-zeta/dispatch.hpp>
#include <cstdint>

using namespace actor_zeta;

// =====================================================================
// OLD STYLE: Switch-based dispatch
// =====================================================================

class old_style_actor : public base::basic_actor<old_style_actor> {
public:
    // Declare methods first (needed by dispatch_traits)
    unique_future<void> method1(int x) {
        counter_ += x;
        return make_ready_future_void(resource());
    }
    unique_future<void> method2(int x) {
        counter_ += x * 2;
        return make_ready_future_void(resource());
    }
    unique_future<void> method3(int x) {
        counter_ += x * 3;
        return make_ready_future_void(resource());
    }
    unique_future<void> method4(int x) {
        counter_ += x * 4;
        return make_ready_future_void(resource());
    }
    unique_future<void> method5(int x) {
        counter_ += x * 5;
        return make_ready_future_void(resource());
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &old_style_actor::method1,
        &old_style_actor::method2,
        &old_style_actor::method3,
        &old_style_actor::method4,
        &old_style_actor::method5
    >;

    explicit old_style_actor(pmr::memory_resource* resource)
        : base::basic_actor<old_style_actor>(resource)
        , counter_(0) {}

    unique_future<void> behavior(mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == msg_id<old_style_actor, &old_style_actor::method1>) {
            dispatch(this, &old_style_actor::method1, msg);
        } else if (cmd == msg_id<old_style_actor, &old_style_actor::method2>) {
            dispatch(this, &old_style_actor::method2, msg);
        } else if (cmd == msg_id<old_style_actor, &old_style_actor::method3>) {
            dispatch(this, &old_style_actor::method3, msg);
        } else if (cmd == msg_id<old_style_actor, &old_style_actor::method4>) {
            dispatch(this, &old_style_actor::method4, msg);
        } else if (cmd == msg_id<old_style_actor, &old_style_actor::method5>) {
            dispatch(this, &old_style_actor::method5, msg);
        }
        return make_ready_future_void(resource());
    }

    int counter() const { return counter_; }

private:
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
                auto f = send(actor.get(), actor->address(), &old_style_actor::method1, 1);
                while (!f.available()) { actor->resume(1); }
                std::move(f).get();  // Wait for completion (returns void)
                break;
            }
            case 1: {
                auto f = send(actor.get(), actor->address(), &old_style_actor::method2, 2);
                while (!f.available()) { actor->resume(1); }
                std::move(f).get();  // Wait for completion (returns void)
                break;
            }
            case 2: {
                auto f = send(actor.get(), actor->address(), &old_style_actor::method3, 3);
                while (!f.available()) { actor->resume(1); }
                std::move(f).get();  // Wait for completion (returns void)
                break;
            }
            case 3: {
                auto f = send(actor.get(), actor->address(), &old_style_actor::method4, 4);
                while (!f.available()) { actor->resume(1); }
                std::move(f).get();  // Wait for completion (returns void)
                break;
            }
            case 4: {
                auto f = send(actor.get(), actor->address(), &old_style_actor::method5, 5);
                while (!f.available()) { actor->resume(1); }
                std::move(f).get();  // Wait for completion (returns void)
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
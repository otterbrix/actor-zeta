#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta/actor/dispatch.hpp>
#include <actor-zeta.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <memory>
#include <array>

// Futures destroyed the instant is_ready() reads true, with the producing
// coroutine's locals instrumented to notice a second destruction. Guards, not
// repros: is_ready() is promise_released, set only after the value is written and
// the continuation claimed, and a finished producer parks at final_suspend for
// release() to reclaim rather than racing the consumer for the frame. Most
// meaningful under AddressSanitizer.

// Magic-number sentinel: a second destruction or a use-after-free is counted.
class sentinel_data {
public:
    static constexpr uint64_t MAGIC_ALIVE = 0xFEEDFACECAFEBABEULL;
    static constexpr uint64_t MAGIC_DEAD = 0xDEADBEEFDEADBEEFULL;

    explicit sentinel_data(int value)
        : magic_(MAGIC_ALIVE)
        , value_(value) {
        constructions_.fetch_add(1, std::memory_order_relaxed);
    }

    ~sentinel_data() {
        if (magic_ != MAGIC_ALIVE) {
            double_destructions_.fetch_add(1, std::memory_order_relaxed);
        }
        magic_ = MAGIC_DEAD;
        destructions_.fetch_add(1, std::memory_order_relaxed);
    }

    sentinel_data(const sentinel_data&) = delete;
    sentinel_data& operator=(const sentinel_data&) = delete;

    sentinel_data(sentinel_data&& other) noexcept
        : magic_(MAGIC_ALIVE)
        , value_(other.value_) {
        other.magic_ = MAGIC_DEAD;
        constructions_.fetch_add(1, std::memory_order_relaxed);
    }

    int value() const {
        assert(magic_ == MAGIC_ALIVE && "Use-after-free!");
        return value_;
    }

    bool is_valid() const { return magic_ == MAGIC_ALIVE; }

    static int constructions() { return constructions_.load(std::memory_order_acquire); }
    static int destructions() { return destructions_.load(std::memory_order_acquire); }
    static int double_destructions() { return double_destructions_.load(std::memory_order_acquire); }
    static void reset_counters() {
        constructions_.store(0, std::memory_order_release);
        destructions_.store(0, std::memory_order_release);
        double_destructions_.store(0, std::memory_order_release);
    }

private:
    uint64_t magic_;
    int value_;
    static std::atomic<int> constructions_;
    static std::atomic<int> destructions_;
    static std::atomic<int> double_destructions_;
};

std::atomic<int> sentinel_data::constructions_{0};
std::atomic<int> sentinel_data::destructions_{0};
std::atomic<int> sentinel_data::double_destructions_{0};

class available_race_actor final : public actor_zeta::basic_actor<available_race_actor> {
public:
    explicit available_race_actor(std::pmr::memory_resource* resource)
        : actor_zeta::basic_actor<available_race_actor>(resource)
        , processed_count_(0) {}

    actor_zeta::unique_future<int> process_simple(int value) {
        sentinel_data local_data(value);
        std::this_thread::yield(); // widen the window
        int result = local_data.value() * 2;
        processed_count_.fetch_add(1, std::memory_order_relaxed);
        co_return result;
    }

    actor_zeta::unique_future<int> process_complex(int value) {
        sentinel_data data1(value);
        sentinel_data data2(value + 1);
        sentinel_data data3(value + 2);
        std::array<sentinel_data*, 3> ptrs = {&data1, &data2, &data3};

        int sum = 0;
        for (auto* p : ptrs) {
            sum += p->value();
            std::this_thread::yield(); // widen the window
        }

        processed_count_.fetch_add(1, std::memory_order_relaxed);
        co_return sum;
    }

    actor_zeta::unique_future<int> process_nested(int value) {
        sentinel_data outer(value);
        int result = 0;
        {
            sentinel_data inner1(value * 2);
            {
                sentinel_data inner2(value * 3);
                result = inner2.value();
            }
            result += inner1.value();
        }
        result += outer.value();
        processed_count_.fetch_add(1, std::memory_order_relaxed);
        co_return result;
    }

    actor_zeta::behavior_t behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<available_race_actor, &available_race_actor::process_simple>) {
            co_await dispatch(this, &available_race_actor::process_simple, msg);
        } else if (cmd == actor_zeta::msg_id<available_race_actor, &available_race_actor::process_complex>) {
            co_await dispatch(this, &available_race_actor::process_complex, msg);
        } else if (cmd == actor_zeta::msg_id<available_race_actor, &available_race_actor::process_nested>) {
            co_await dispatch(this, &available_race_actor::process_nested, msg);
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &available_race_actor::process_simple,
        &available_race_actor::process_complex,
        &available_race_actor::process_nested>;

    std::size_t processed_count() const { return processed_count_.load(std::memory_order_acquire); }

private:
    std::atomic<std::size_t> processed_count_;
};

TEST_CASE("available race: basic destroy on available") {
    sentinel_data::reset_counters();

    auto* resource = std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(4, 100);
    scheduler->start();

    auto actor = actor_zeta::spawn<available_race_actor>(resource);

    constexpr int NUM_ITERATIONS = 2000;
    std::atomic<int> races_triggered{0};

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                       &available_race_actor::process_simple, i);

        if (needs_sched) {
            scheduler->enqueue(actor.get());
        }

        std::thread destroyer([fut = std::move(future), &races_triggered]() mutable {
            // No yield: the destroy should land as close to readiness as possible.
            int spins = 0;
            while (!fut.is_ready() && spins < 100000) {
                ++spins;
            }

            int before = sentinel_data::double_destructions();

            // Destroyed untaken, the instant is_ready() read true.
            { auto temp = std::move(fut); }

            int after = sentinel_data::double_destructions();
            if (after > before) {
                races_triggered.fetch_add(after - before, std::memory_order_relaxed);
            }
        });

        destroyer.join();
    }

    scheduler->stop();

    int total_double = sentinel_data::double_destructions();

    std::cout << "\n=== Basic available() Race Test ===\n";
    std::cout << "Iterations:          " << NUM_ITERATIONS << "\n";
    std::cout << "Constructions:       " << sentinel_data::constructions() << "\n";
    std::cout << "Destructions:        " << sentinel_data::destructions() << "\n";
    std::cout << "Double destructions: " << total_double << "\n";
    std::cout << "Races triggered:     " << races_triggered.load() << "\n";
    std::cout << "===================================\n\n";

    // Under ASan a use-after-free aborts before this line.
    WARN("Double destructions detected: " << total_double);

    REQUIRE(sentinel_data::constructions() == sentinel_data::destructions());
}

TEST_CASE("available race: complex coroutine with multiple locals") {
    sentinel_data::reset_counters();

    auto* resource = std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(4, 50);
    scheduler->start();

    auto actor = actor_zeta::spawn<available_race_actor>(resource);

    constexpr int NUM_ITERATIONS = 1000;

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                       &available_race_actor::process_complex, i);

        if (needs_sched) {
            scheduler->enqueue(actor.get());
        }

        while (!future.is_ready()) {
        }
    }

    scheduler->stop();

    std::cout << "\n=== Complex Coroutine Race Test ===\n";
    std::cout << "Iterations:          " << NUM_ITERATIONS << "\n";
    std::cout << "Constructions:       " << sentinel_data::constructions() << "\n";
    std::cout << "Destructions:        " << sentinel_data::destructions() << "\n";
    std::cout << "Double destructions: " << sentinel_data::double_destructions() << "\n";
    std::cout << "===================================\n\n";

    WARN("Double destructions: " << sentinel_data::double_destructions());
    REQUIRE(sentinel_data::constructions() == sentinel_data::destructions());
}

TEST_CASE("available race: high concurrency stress") {
    sentinel_data::reset_counters();

    auto* resource = std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(4, 50);
    scheduler->start();

    auto actor = actor_zeta::spawn<available_race_actor>(resource);

    constexpr int NUM_THREADS = 8;
    constexpr int ITERATIONS_PER_THREAD = 250;

    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;

    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < ITERATIONS_PER_THREAD && !stop.load(); ++i) {
                auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                               &available_race_actor::process_nested,
                                               t * ITERATIONS_PER_THREAD + i);

                if (needs_sched) {
                    scheduler->enqueue(actor.get());
                }

                int spins = 0;
                while (!future.is_ready() && spins < 50000) {
                    ++spins;
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    scheduler->stop();

    std::cout << "\n=== High Concurrency Stress Test ===\n";
    std::cout << "Total operations:    " << NUM_THREADS * ITERATIONS_PER_THREAD << "\n";
    std::cout << "Constructions:       " << sentinel_data::constructions() << "\n";
    std::cout << "Destructions:        " << sentinel_data::destructions() << "\n";
    std::cout << "Double destructions: " << sentinel_data::double_destructions() << "\n";
    std::cout << "====================================\n\n";

    WARN("Double destructions in stress test: " << sentinel_data::double_destructions());
    REQUIRE(sentinel_data::constructions() == sentinel_data::destructions());
}

TEST_CASE("available race: poll_pending pattern simulation") {
    sentinel_data::reset_counters();

    auto* resource = std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(4, 100);
    scheduler->start();

    auto actor = actor_zeta::spawn<available_race_actor>(resource);

    constexpr int BATCH_SIZE = 30;
    constexpr int NUM_BATCHES = 50;

    for (int batch = 0; batch < NUM_BATCHES; ++batch) {
        std::vector<actor_zeta::unique_future<int>> pending;
        pending.reserve(BATCH_SIZE);

        for (int i = 0; i < BATCH_SIZE; ++i) {
            auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                           &available_race_actor::process_simple,
                                           batch * BATCH_SIZE + i);

            if (needs_sched) {
                scheduler->enqueue(actor.get());
            }

            pending.push_back(std::move(future));
        }

        // Erase (destroy) each future as soon as it reads ready.
        while (!pending.empty()) {
            for (auto it = pending.begin(); it != pending.end();) {
                if (it->is_ready()) {
                    it = pending.erase(it);
                } else {
                    ++it;
                }
            }

            if (!pending.empty()) {
                std::this_thread::yield();
            }
        }
    }

    scheduler->stop();

    std::cout << "\n=== poll_pending Pattern Test ===\n";
    std::cout << "Total futures:       " << BATCH_SIZE * NUM_BATCHES << "\n";
    std::cout << "Constructions:       " << sentinel_data::constructions() << "\n";
    std::cout << "Destructions:        " << sentinel_data::destructions() << "\n";
    std::cout << "Double destructions: " << sentinel_data::double_destructions() << "\n";
    std::cout << "=================================\n\n";

    WARN("Double destructions in poll_pending: " << sentinel_data::double_destructions());
    REQUIRE(sentinel_data::constructions() == sentinel_data::destructions());
}

TEST_CASE("available race: safe destroy with delay (workaround demo)") {
    sentinel_data::reset_counters();

    auto* resource = std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(2, 100);
    scheduler->start();

    auto actor = actor_zeta::spawn<available_race_actor>(resource);

    constexpr int NUM_ITERATIONS = 500;

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto [needs_sched, future] = actor_zeta::send(actor.get(),
                                       &available_race_actor::process_complex, i);

        if (needs_sched) {
            scheduler->enqueue(actor.get());
        }

        while (!future.is_ready()) {
            std::this_thread::yield();
        }

        // The delay is the point of this case; the cases above destroy with none.
        std::this_thread::sleep_for(std::chrono::microseconds(10));
    }

    scheduler->stop();

    std::cout << "\n=== Safe Destroy (Workaround Demo) ===\n";
    std::cout << "Iterations:          " << NUM_ITERATIONS << "\n";
    std::cout << "Double destructions: " << sentinel_data::double_destructions() << "\n";
    std::cout << "======================================\n\n";

    REQUIRE(sentinel_data::double_destructions() == 0);
    REQUIRE(sentinel_data::constructions() == sentinel_data::destructions());
}
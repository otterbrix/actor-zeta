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

// Destroying a future the moment is_ready() reads true must be safe, across a
// dispatcher -> worker chain under a real scheduler. is_ready() is
// promise_released, which final_awaiter sets only after the value is written and
// the continuation claimed; a finished producer parks at final_suspend for
// release() to reclaim, so consumer and producer never race for the frame.

namespace {
constexpr auto kChainTimeout = std::chrono::seconds(10);

// Poll a cross-actor future to ready without taking it, discharging the
// obligations the dispatcher recorded: the test owns the actors, so scheduling
// them is its job. No blind enqueues -- a strand here means an obligation was
// dropped, and that is exactly what these tests must catch.
template<typename Fut, typename Dispatcher, typename Worker, typename Sched>
[[nodiscard]] inline bool drive_until_ready(Fut& fut, Dispatcher* dispatcher,
                                            Worker* worker, Sched* sched) {
    const auto start = std::chrono::steady_clock::now();
    while (!fut.is_ready()) {
        if (dispatcher->take_worker_obligations() > 0) {
            sched->enqueue(worker);
        }
        if (std::chrono::steady_clock::now() - start > kChainTimeout) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}
} // namespace

class destruction_tracker {
public:
    static constexpr uint64_t MAGIC = 0xCAFEBABEDEADBEEFULL;

    explicit destruction_tracker(int id)
        : magic_(MAGIC)
        , id_(id) {
        alive_.fetch_add(1, std::memory_order_relaxed);
    }

    ~destruction_tracker() {
        if (magic_ != MAGIC) {
            double_destroy_.fetch_add(1, std::memory_order_relaxed);
        }
        magic_ = 0;
        alive_.fetch_sub(1, std::memory_order_relaxed);
    }

    destruction_tracker(const destruction_tracker&) = delete;
    destruction_tracker& operator=(const destruction_tracker&) = delete;

    int id() const { return id_; }

    static int alive_count() { return alive_.load(std::memory_order_acquire); }
    static int double_destroy_count() { return double_destroy_.load(std::memory_order_acquire); }
    static void reset() {
        alive_.store(0, std::memory_order_release);
        double_destroy_.store(0, std::memory_order_release);
    }

private:
    uint64_t magic_;
    int id_;
    static std::atomic<int> alive_;
    static std::atomic<int> double_destroy_;
};

std::atomic<int> destruction_tracker::alive_{0};
std::atomic<int> destruction_tracker::double_destroy_{0};

class worker_actor;

class worker_actor final : public actor_zeta::basic_actor<worker_actor> {
public:
    explicit worker_actor(std::pmr::memory_resource* resource)
        : actor_zeta::basic_actor<worker_actor>(resource) {}

    actor_zeta::unique_future<int> execute(int value) {
        destruction_tracker tracker(value);

        // Plain int, not volatile: a compound assignment to a volatile object is
        // deprecated in C++20. The value is co_returned, so the loop still stands.
        int sum = 0;
        for (int i = 0; i < 100; ++i) {
            sum += tracker.id();
        }

        co_return sum;
    }

    actor_zeta::behavior_t behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<worker_actor, &worker_actor::execute>) {
            co_await dispatch(this, &worker_actor::execute, msg);
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<&worker_actor::execute>;
};

class dispatcher_actor final : public actor_zeta::basic_actor<dispatcher_actor> {
public:
    explicit dispatcher_actor(std::pmr::memory_resource* resource)
        : actor_zeta::basic_actor<dispatcher_actor>(resource)
        , completed_(0)
        , worker_address_(actor_zeta::address_t::empty_address()) {}

    // An address_t is all a peer needs to send; launching actors belongs to the
    // owner. The obligation send() reports is recorded, not dropped.
    void set_worker(const actor_zeta::address_t& addr) {
        worker_address_ = addr;
    }

    std::size_t take_worker_obligations() {
        return worker_owed_.exchange(0, std::memory_order_acq_rel);
    }

    actor_zeta::unique_future<int> process(int value) {
        destruction_tracker tracker(value + 1000);

        auto [needs_sched, future] = actor_zeta::send(worker_address_, &worker_actor::execute, value);
        if (needs_sched) {
            worker_owed_.fetch_add(1, std::memory_order_release);
        }

        int result = co_await std::move(future);

        completed_.fetch_add(1, std::memory_order_relaxed);
        co_return result + tracker.id();
    }

    actor_zeta::behavior_t behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<dispatcher_actor, &dispatcher_actor::process>) {
            co_await dispatch(this, &dispatcher_actor::process, msg);
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<&dispatcher_actor::process>;

    int completed() const { return completed_.load(std::memory_order_acquire); }

private:
    std::atomic<int> completed_;
    std::atomic<std::size_t> worker_owed_{0};
    actor_zeta::address_t worker_address_;
};

TEST_CASE("available race chaining: basic chain is safe") {
    destruction_tracker::reset();

    auto* resource = std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(2, 100);
    scheduler->start();

    auto worker = actor_zeta::spawn<worker_actor>(resource);
    auto dispatcher = actor_zeta::spawn<dispatcher_actor>(resource);
    dispatcher->set_worker(worker->address());

    constexpr int NUM_ITERATIONS = 100;

    for (int i = 0; i < NUM_ITERATIONS; ++i) {
        auto [needs_sched, future] = actor_zeta::send(dispatcher.get(),
                                       &dispatcher_actor::process, i);

        if (needs_sched) {
            scheduler->enqueue(dispatcher.get());
        }

        REQUIRE(drive_until_ready(future, dispatcher.get(), worker.get(), scheduler.get()));

        // Destroyed untaken, the instant is_ready() read true.
        { auto temp = std::move(future); }
    }

    // Let pending work complete
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    scheduler->stop();

    INFO("Completed: " << dispatcher->completed());
    INFO("Double destroys: " << destruction_tracker::double_destroy_count());

    REQUIRE(destruction_tracker::double_destroy_count() == 0);
    REQUIRE(dispatcher->completed() == NUM_ITERATIONS);
}

TEST_CASE("available race chaining: poll_pending pattern") {
    destruction_tracker::reset();

    auto* resource = std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(2, 50);
    scheduler->start();

    auto worker = actor_zeta::spawn<worker_actor>(resource);
    auto dispatcher = actor_zeta::spawn<dispatcher_actor>(resource);
    dispatcher->set_worker(worker->address());

    constexpr int BATCH_SIZE = 10;
    constexpr int NUM_BATCHES = 5;

    for (int batch = 0; batch < NUM_BATCHES; ++batch) {
        std::vector<actor_zeta::unique_future<int>> pending;
        pending.reserve(BATCH_SIZE);

        for (int i = 0; i < BATCH_SIZE; ++i) {
            auto [needs_sched, future] = actor_zeta::send(dispatcher.get(),
                                           &dispatcher_actor::process, batch * BATCH_SIZE + i);

            if (needs_sched) {
                scheduler->enqueue(dispatcher.get());
            }

            pending.push_back(std::move(future));
        }

        // Erase (destroy) each future as soon as it reads ready.
        const auto batch_start = std::chrono::steady_clock::now();
        bool batch_timed_out = false;
        while (!pending.empty()) {
            if (dispatcher->take_worker_obligations() > 0) {
                scheduler->enqueue(worker.get());
            }
            if (std::chrono::steady_clock::now() - batch_start > kChainTimeout) {
                batch_timed_out = true;
                break;
            }
            for (auto it = pending.begin(); it != pending.end();) {
                if (it->is_ready()) {
                    it = pending.erase(it);
                } else {
                    ++it;
                }
            }
            std::this_thread::yield();
        }
        REQUIRE_FALSE(batch_timed_out);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    scheduler->stop();

    INFO("Completed: " << dispatcher->completed());
    INFO("Double destroys: " << destruction_tracker::double_destroy_count());

    REQUIRE(destruction_tracker::double_destroy_count() == 0);
    REQUIRE(dispatcher->completed() == BATCH_SIZE * NUM_BATCHES);
}

TEST_CASE("available race chaining: concurrent senders") {
    destruction_tracker::reset();

    auto* resource = std::pmr::get_default_resource();
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(4, 100);
    scheduler->start();

    auto worker = actor_zeta::spawn<worker_actor>(resource);
    auto dispatcher = actor_zeta::spawn<dispatcher_actor>(resource);
    dispatcher->set_worker(worker->address());

    constexpr int OPERATIONS_PER_THREAD = 20;
    constexpr int NUM_THREADS = 4;
    std::vector<std::thread> threads;

    // Catch2 v2 assertion macros are not thread-safe; workers record, main asserts.
    std::atomic<int> stranded{0};

    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < OPERATIONS_PER_THREAD; ++i) {
                auto [needs_sched, future] = actor_zeta::send(dispatcher.get(),
                                               &dispatcher_actor::process,
                                               t * OPERATIONS_PER_THREAD + i);

                if (needs_sched) {
                    scheduler->enqueue(dispatcher.get());
                }

                if (!drive_until_ready(future, dispatcher.get(), worker.get(), scheduler.get())) {
                    stranded.fetch_add(1, std::memory_order_relaxed);
                }

                { auto temp = std::move(future); }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    REQUIRE(stranded.load() == 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    scheduler->stop();

    INFO("Completed: " << dispatcher->completed());
    INFO("Double destroys: " << destruction_tracker::double_destroy_count());

    REQUIRE(destruction_tracker::double_destroy_count() == 0);
    REQUIRE(dispatcher->completed() == NUM_THREADS * OPERATIONS_PER_THREAD);
}

TEST_CASE("available race chaining: is_ready vs has_result") {
    auto* resource = std::pmr::get_default_resource();

    auto* state = actor_zeta::detail::allocate_shared_state<int>(resource);

    REQUIRE_FALSE(state->has_result());
    REQUIRE_FALSE(state->is_ready());

    state->set_value(42);
    REQUIRE(state->has_result());
    REQUIRE_FALSE(state->is_ready());  // is_ready() is promise_released, not the value

    const bool deallocated = state->release_promise();
    REQUIRE_FALSE(deallocated);
    REQUIRE(state->is_ready());

    state->release_future();
}
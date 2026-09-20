#include <cstdlib>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

#include <actor-zeta.hpp>

class calculator_actor;

class calculator_actor final : public actor_zeta::basic_actor<calculator_actor> {
public:
    explicit calculator_actor(std::pmr::memory_resource* ptr)
        : actor_zeta::basic_actor<calculator_actor>(ptr) {
        std::cout << "[Calculator " << id() << "] Created\n";
    }

    ~calculator_actor() = default;

    actor_zeta::unique_future<int> add(int a, int b) {
        std::cout << "[Calculator] add(" << a << ", " << b << ")\n";
        co_return a + b;
    }

    actor_zeta::unique_future<int> multiply(int a, int b) {
        std::cout << "[Calculator] multiply(" << a << ", " << b << ")\n";
        co_return a * b;
    }

    actor_zeta::unique_future<int> square(int x) {
        std::cout << "[Calculator] ASYNC square(" << x << ") - START\n";

        // An actor cannot await a message it posted to itself: resume() will not pop the
        // next message while the current behavior is still suspended, so send(this, ...)
        // + co_await never completes (see test/coroutines/main.cpp, "Recursive coroutines
        // are NOT SUPPORTED"). Calling the handler gives back the same unique_future
        // without going through the mailbox.
        std::cout << "[Calculator] ASYNC square - awaiting multiply...\n";
        int result = co_await multiply(x, x);
        std::cout << "[Calculator] ASYNC square - multiply completed: " << result << "\n";

        co_return result;
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &calculator_actor::add,
        &calculator_actor::multiply,
        &calculator_actor::square
    >;

    actor_zeta::behavior_t behavior(actor_zeta::mailbox::message* msg) {
        switch (msg->command()) {
            case actor_zeta::msg_id<calculator_actor, &calculator_actor::add>:
                co_await actor_zeta::dispatch(this, &calculator_actor::add, msg);
                break;
            case actor_zeta::msg_id<calculator_actor, &calculator_actor::multiply>:
                co_await actor_zeta::dispatch(this, &calculator_actor::multiply, msg);
                break;
            case actor_zeta::msg_id<calculator_actor, &calculator_actor::square>: {
                // dispatch() returns unique_future<void> which forwards to caller's promise
                co_await actor_zeta::dispatch(this, &calculator_actor::square, msg);
                break;
            }
            default:
                std::cerr << "[Calculator] Unknown message\n";
                break;
        }
    }

    // A caller cannot resume a suspended coroutine by polling: a method coroutine
    // parked on co_await is resumed by driving its OWNING actor -- resume() or the
    // scheduler -- which runs the drain block inside cooperative_actor::resume_impl.
};

// Wait for a result the scheduler is producing. The worker owns the resume verdict and
// re-enqueues the actor for as long as it asks, so this thread only polls.
template<typename T>
T await_result(actor_zeta::unique_future<T>& future) {
    // is_ready() is the promise_released bit, which a promise dying without a value sets
    // too, and take_ready() only ASSERTS has_result() -- an assert that is gone in the
    // Release builds examples ship as. Hence failed(). The bound turns a future that
    // never completes into a visible error instead of a silent hang.
    constexpr int kAwaitCap = 10'000'000;
    for (int i = 0; i < kAwaitCap && !future.is_ready(); ++i) {
        std::this_thread::yield();
    }
    if (!future.is_ready() || future.failed()) {
        std::cerr << "await_result: future did not complete with a value\n";
        std::abort();
    }
    return std::move(future).take_ready();
}

int main() {
    std::cout << "=== Mixed Sync/Async Actor Example ===\n\n";

    auto* resource = std::pmr::get_default_resource();
    auto calculator = actor_zeta::spawn<calculator_actor>(resource);

    // Declared after the actor, so stopped explicitly before it is released below:
    // scheduler_t has no stop() in its destructor, and a worker must never resume an
    // actor that has already been freed.
    auto scheduler = std::make_unique<actor_zeta::scheduler::sharing_scheduler>(1, 100);
    scheduler->start();

    std::cout << "\n--- Testing SYNC methods ---\n\n";

    {
        auto [needs_sched, future] = actor_zeta::send(calculator.get(),
                                       &calculator_actor::add, 10, 20);
        if (needs_sched) {
            scheduler->enqueue(calculator.get());
        }
        int result = await_result(future);
        std::cout << "Result: 10 + 20 = " << result << "\n\n";
    }

    {
        auto [needs_sched, future] = actor_zeta::send(calculator.get(),
                                       &calculator_actor::multiply, 7, 8);
        if (needs_sched) {
            scheduler->enqueue(calculator.get());
        }
        int result = await_result(future);
        std::cout << "Result: 7 * 8 = " << result << "\n\n";
    }

    std::cout << "--- Testing ASYNC method (coroutine) ---\n\n";

    {
        auto [needs_sched, future] = actor_zeta::send(calculator.get(),
                                       &calculator_actor::square, 5);
        if (needs_sched) {
            scheduler->enqueue(calculator.get());
        }

        int result = await_result(future);
        std::cout << "Result: 5^2 = " << result << "\n\n";
    }

    std::cout << "--- Key observations ---\n";
    std::cout << "1. All methods have SAME signature: unique_future<T>\n";
    std::cout << "2. Caller uses SAME code: send(), then drive and take_ready()\n";
    std::cout << "3. All methods must be coroutines using 'co_return'\n";
    std::cout << "4. Async methods can use 'co_await' to suspend\n";
    std::cout << "5. A coroutine suspended on co_await is resumed by driving its\n";
    std::cout << "   actor -- resume()/the scheduler -- not by the caller polling\n";
    std::cout << "\n NOTE: Recursive coroutines (like factorial, power) are NOT supported\n";
    std::cout << "   Use iterative algorithms instead.\n\n";

    std::cout << "--- Cleanup ---\n\n";

    scheduler->stop();
    calculator.reset();

    std::cout << "\n=== Example completed ===\n";

    return 0;
}
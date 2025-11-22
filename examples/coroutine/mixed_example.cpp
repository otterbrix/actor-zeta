#include <actor-zeta.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>
#include <iostream>
#include <chrono>
#include <thread>

// Forward declaration
class calculator_actor;

/// @brief Calculator actor with BOTH sync and async methods
/// Demonstrates unified interface - caller doesn't need to know which is which
class calculator_actor final : public actor_zeta::basic_actor<calculator_actor> {
public:
    explicit calculator_actor(actor_zeta::pmr::memory_resource* ptr)
        : actor_zeta::basic_actor<calculator_actor>(ptr)
        , add_(actor_zeta::make_behavior(resource(), this, &calculator_actor::add))
        , multiply_(actor_zeta::make_behavior(resource(), this, &calculator_actor::multiply))
        , square_(actor_zeta::make_behavior(resource(), this, &calculator_actor::square)) {
        std::cout << "[Calculator " << id() << "] Created\n";
    }

    // NOTE: No explicit destructor needed!
    // shutdown_guard_t automatically calls begin_shutdown() before base class destructor.
    // This prevents race condition between main thread destroying actor and worker threads accessing it.
    ~calculator_actor() = default;

    // ========================================
    // SYNC METHODS - regular functions with make_ready_future
    // ========================================

    /// @brief Simple addition - synchronous method
    /// Returns unique_future<int> using make_ready_future
    actor_zeta::unique_future<int> add(int a, int b) {
        std::cout << "[Calculator] SYNC add(" << a << ", " << b << ")\n";
        return actor_zeta::make_ready_future(resource(), a + b);
    }

    /// @brief Simple multiplication - synchronous method
    actor_zeta::unique_future<int> multiply(int a, int b) {
        std::cout << "[Calculator] SYNC multiply(" << a << ", " << b << ")\n";
        return actor_zeta::make_ready_future(resource(), a * b);
    }

    // ========================================
    // ASYNC METHODS - coroutines with co_await and co_return
    // ========================================

    /// @brief Square function - asynchronous coroutine method
    /// Uses co_await to call multiply() without blocking
    /// NOTE: This is NON-RECURSIVE
    actor_zeta::unique_future<int> square(int x) {
        std::cout << "[Calculator] ASYNC square(" << x << ") - START\n";

        // Call multiply() asynchronously - this will suspend without blocking!
        auto future = actor_zeta::send(this, address(), &calculator_actor::multiply, x, x);

        std::cout << "[Calculator] ASYNC square - awaiting multiply...\n";
        int result = co_await std::move(future);  // Suspend here, thread is free for other actors
        std::cout << "[Calculator] ASYNC square - multiply completed: " << result << "\n";

        co_return result;
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &calculator_actor::add,
        &calculator_actor::multiply,
        &calculator_actor::square
    >;

    void behavior(actor_zeta::mailbox::message* msg) {
        //  NEW: Resume any suspended coroutines before processing message
        // This enables STATE mode - coroutines can suspend and resume later
        actor_zeta::resume_all(square_);  // Only async behaviors need resume

        switch (msg->command()) {
            case actor_zeta::msg_id<calculator_actor, &calculator_actor::add>: {
                add_(msg);
                break;
            }
            case actor_zeta::msg_id<calculator_actor, &calculator_actor::multiply>: {
                multiply_(msg);
                break;
            }
            case actor_zeta::msg_id<calculator_actor, &calculator_actor::square>: {
                square_(msg);
                break;
            }
            default:
                std::cerr << "[Calculator] Unknown message\n";
                break;
        }
    }

private:
    actor_zeta::behavior_t add_;
    actor_zeta::behavior_t multiply_;
    actor_zeta::behavior_t square_;
};

int main() {
    std::cout << "=== Mixed Sync/Async Actor Example ===\n\n";

    // Create PMR resource
    auto* resource = actor_zeta::pmr::get_default_resource();

    // Create calculator actor
    auto calculator = actor_zeta::spawn<calculator_actor>(resource);

    std::cout << "\n--- Testing SYNC methods ---\n\n";

    // Call sync method - add
    {
        auto future = actor_zeta::send(calculator.get(), calculator->address(),
                                       &calculator_actor::add, 10, 20);
        calculator->resume(100);
        int result = std::move(future).get();
        std::cout << "Result: 10 + 20 = " << result << "\n\n";
    }

    // Call sync method - multiply
    {
        auto future = actor_zeta::send(calculator.get(), calculator->address(),
                                       &calculator_actor::multiply, 7, 8);
        calculator->resume(100);
        int result = std::move(future).get();
        std::cout << "Result: 7 * 8 = " << result << "\n\n";
    }

    std::cout << "--- Testing ASYNC method (coroutine) ---\n\n";

    // Call async method - square (will use co_await internally)
    {
        auto future = actor_zeta::send(calculator.get(), calculator->address(),
                                       &calculator_actor::square, 5);
        // Need multiple resume calls for coroutine to complete
        calculator->resume(100);  // Process square message
        calculator->resume(100);  // Resume coroutine after multiply completes
        int result = std::move(future).get();
        std::cout << "Result: 5^2 = " << result << "\n\n";
    }

    std::cout << "--- Key observations ---\n";
    std::cout << "1. Both sync and async methods have SAME signature: unique_future<int>\n";
    std::cout << "2. Caller uses SAME code: send() and get()\n";
    std::cout << "3. Sync methods use 'make_ready_future()' for immediate results\n";
    std::cout << "4. Async methods use 'co_await' and 'co_return'\n";
    std::cout << "5. No difference from caller's perspective!\n";
    std::cout << "\n NOTE: Recursive coroutines (like factorial, power) are NOT supported\n";
    std::cout << "   Use iterative algorithms instead.\n\n";

    std::cout << "--- Cleanup ---\n\n";

    // Actors destroyed automatically
    calculator.reset();

    std::cout << "\n=== Example completed ===\n";

    return 0;
}
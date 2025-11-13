// Mixed Sync/Async Actor Example
// Demonstrates that actor can have BOTH sync and async methods with unified interface

#if HAVE_STD_COROUTINES

#include <actor-zeta.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>
#include <iostream>
#include <chrono>
#include <thread>

// Forward declaration
class calculator_actor;

/// @brief Calculator actor with BOTH sync and async methods
/// Demonstrates unified interface - caller doesn't need to know which is which
class calculator_actor final : public actor_zeta::coroutine_actor<calculator_actor> {
public:
    explicit calculator_actor(actor_zeta::pmr::memory_resource* ptr)
        : actor_zeta::coroutine_actor<calculator_actor>(ptr)
        , add_(actor_zeta::make_behavior(resource(), this, &calculator_actor::add))
        , multiply_(actor_zeta::make_behavior(resource(), this, &calculator_actor::multiply))
        , power_(actor_zeta::make_behavior(resource(), this, &calculator_actor::power))
        , factorial_(actor_zeta::make_behavior(resource(), this, &calculator_actor::factorial)) {
        std::cout << "[Calculator " << id() << "] Created\n";
    }

    ~calculator_actor() override {
        begin_shutdown();
        std::cout << "[Calculator " << id() << "] Destroyed\n";
    }

    // ========================================
    // SYNC METHODS - regular functions with implicit conversion
    // ========================================

    /// @brief Simple addition - synchronous method
    /// Returns unique_future<int> but implemented as regular function
    actor_zeta::unique_future<int> add(int a, int b) {
        std::cout << "[Calculator] SYNC add(" << a << ", " << b << ")\n";
        return a + b;  // ✅ int → unique_future<int> implicit conversion
    }

    /// @brief Simple multiplication - synchronous method
    actor_zeta::unique_future<int> multiply(int a, int b) {
        std::cout << "[Calculator] SYNC multiply(" << a << ", " << b << ")\n";
        return a * b;  // ✅ int → unique_future<int> implicit conversion
    }

    // ========================================
    // ASYNC METHODS - coroutines with co_await and co_return
    // ========================================

    /// @brief Power function - asynchronous coroutine method
    /// Uses co_await to call other methods without blocking
    actor_zeta::unique_future<int> power(int base, int exponent) {
        std::cout << "[Calculator] ASYNC power(" << base << ", " << exponent << ") - START\n";

        if (exponent == 0) {
            std::cout << "[Calculator] ASYNC power - base case\n";
            co_return 1;
        }

        // Call multiply() asynchronously - this will suspend without blocking!
        auto future = actor_zeta::send(this, address(), &calculator_actor::multiply, base, base);

        std::cout << "[Calculator] ASYNC power - awaiting multiply...\n";
        int squared = co_await future;  // ✅ Suspend here, thread is free for other actors
        std::cout << "[Calculator] ASYNC power - multiply completed: " << squared << "\n";

        if (exponent == 2) {
            co_return squared;
        }

        // Recursively call power for higher exponents
        auto power_future = actor_zeta::send(this, address(), &calculator_actor::power, base, exponent - 1);
        int prev_power = co_await power_future;

        auto result_future = actor_zeta::send(this, address(), &calculator_actor::multiply, base, prev_power);
        int result = co_await result_future;

        std::cout << "[Calculator] ASYNC power - DONE: " << result << "\n";
        co_return result;
    }

    /// @brief Factorial - asynchronous coroutine method
    /// Demonstrates multiple async calls in sequence
    actor_zeta::unique_future<int> factorial(int n) {
        std::cout << "[Calculator] ASYNC factorial(" << n << ") - START\n";

        if (n <= 1) {
            std::cout << "[Calculator] ASYNC factorial - base case\n";
            co_return 1;
        }

        // Recursive call
        auto factorial_future = actor_zeta::send(this, address(), &calculator_actor::factorial, n - 1);
        int prev_factorial = co_await factorial_future;  // ✅ Suspend without blocking

        // Multiply result
        auto multiply_future = actor_zeta::send(this, address(), &calculator_actor::multiply, n, prev_factorial);
        int result = co_await multiply_future;  // ✅ Another suspend point

        std::cout << "[Calculator] ASYNC factorial - DONE: " << result << "\n";
        co_return result;
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &calculator_actor::add,
        &calculator_actor::multiply,
        &calculator_actor::power,
        &calculator_actor::factorial
    >;

    void behavior(actor_zeta::mailbox::message* msg) override {
        // ✅ NEW: Resume any suspended coroutines before processing message
        // This enables STATE mode - coroutines can suspend and resume later
        actor_zeta::resume_all(power_, factorial_);  // Only async behaviors need resume

        switch (msg->command()) {
            case actor_zeta::msg_id<calculator_actor, &calculator_actor::add>: {
                add_(msg);
                break;
            }
            case actor_zeta::msg_id<calculator_actor, &calculator_actor::multiply>: {
                multiply_(msg);
                break;
            }
            case actor_zeta::msg_id<calculator_actor, &calculator_actor::power>: {
                power_(msg);
                break;
            }
            case actor_zeta::msg_id<calculator_actor, &calculator_actor::factorial>: {
                factorial_(msg);
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
    actor_zeta::behavior_t power_;
    actor_zeta::behavior_t factorial_;
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
        auto future = actor_zeta::send(calculator.get(), actor_zeta::address_t{},
                                       &calculator_actor::add, 10, 20);
        int result = std::move(future).get();
        std::cout << "Result: 10 + 20 = " << result << "\n\n";
    }

    // Call sync method - multiply
    {
        auto future = actor_zeta::send(calculator.get(), actor_zeta::address_t{},
                                       &calculator_actor::multiply, 7, 8);
        int result = std::move(future).get();
        std::cout << "Result: 7 * 8 = " << result << "\n\n";
    }

    std::cout << "--- Testing ASYNC methods (coroutines) ---\n\n";

    // Call async method - power (will use co_await internally)
    {
        auto future = actor_zeta::send(calculator.get(), actor_zeta::address_t{},
                                       &calculator_actor::power, 3, 2);
        int result = std::move(future).get();
        std::cout << "Result: 3^2 = " << result << "\n\n";
    }

    // Call async method - factorial (will use co_await internally)
    {
        auto future = actor_zeta::send(calculator.get(), actor_zeta::address_t{},
                                       &calculator_actor::factorial, 5);
        int result = std::move(future).get();
        std::cout << "Result: 5! = " << result << "\n\n";
    }

    std::cout << "--- Key observations ---\n";
    std::cout << "1. Both sync and async methods have SAME signature: unique_future<int>\n";
    std::cout << "2. Caller uses SAME code: send() and get()\n";
    std::cout << "3. Sync methods use 'return T' - implicit conversion\n";
    std::cout << "4. Async methods use 'co_await' and 'co_return'\n";
    std::cout << "5. No difference from caller's perspective!\n\n";

    std::cout << "--- Cleanup ---\n\n";

    // Actors destroyed automatically
    calculator.reset();

    std::cout << "\n=== Example completed ===\n";

    return 0;
}

#else // !HAVE_STD_COROUTINES

#include <iostream>

int main() {
    std::cout << "ERROR: This example requires C++20 coroutines support.\n";
    std::cout << "Please compile with -std=c++20 and ensure your compiler supports coroutines.\n";
    return 1;
}

#endif // HAVE_STD_COROUTINES
// Mixed Sync/Async Actor Example
// Demonstrates that actor can have BOTH sync and async methods with unified interface
//
// ⚠️ PHASE 1 LIMITATION: Recursive/nested coroutines NOT supported
// This example uses only non-recursive async methods

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
        , square_(actor_zeta::make_behavior(resource(), this, &calculator_actor::square)) {
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

    /// @brief Square function - asynchronous coroutine method
    /// Uses co_await to call multiply() without blocking
    /// NOTE: This is NON-RECURSIVE - safe in Phase 1
    actor_zeta::unique_future<int> square(int x) {
        std::cout << "[Calculator] ASYNC square(" << x << ") - START\n";

        // Call multiply() asynchronously - this will suspend without blocking!
        auto future = actor_zeta::send(this, address(), &calculator_actor::multiply, x, x);

        std::cout << "[Calculator] ASYNC square - awaiting multiply...\n";
        int result = co_await future;  // ✅ Suspend here, thread is free for other actors
        std::cout << "[Calculator] ASYNC square - multiply completed: " << result << "\n";

        co_return result;
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &calculator_actor::add,
        &calculator_actor::multiply,
        &calculator_actor::square
    >;

    void behavior(actor_zeta::mailbox::message* msg) override {
        // ✅ NEW: Resume any suspended coroutines before processing message
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

    std::cout << "--- Testing ASYNC method (coroutine) ---\n\n";

    // Call async method - square (will use co_await internally)
    {
        auto future = actor_zeta::send(calculator.get(), actor_zeta::address_t{},
                                       &calculator_actor::square, 5);
        int result = std::move(future).get();
        std::cout << "Result: 5^2 = " << result << "\n\n";
    }

    std::cout << "--- Key observations ---\n";
    std::cout << "1. Both sync and async methods have SAME signature: unique_future<int>\n";
    std::cout << "2. Caller uses SAME code: send() and get()\n";
    std::cout << "3. Sync methods use 'return T' - implicit conversion\n";
    std::cout << "4. Async methods use 'co_await' and 'co_return'\n";
    std::cout << "5. No difference from caller's perspective!\n";
    std::cout << "\n⚠️ NOTE: Recursive coroutines (like factorial, power) are NOT supported in Phase 1\n";
    std::cout << "   Use iterative algorithms instead, or wait for Phase 5.\n\n";

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
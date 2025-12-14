#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

#include <actor-zeta.hpp>

// Forward declaration
class calculator_actor;

/// @brief Calculator actor with BOTH sync and async methods
/// Demonstrates unified interface - caller doesn't need to know which is which
class calculator_actor final : public actor_zeta::basic_actor<calculator_actor> {
public:
    explicit calculator_actor(std::pmr::memory_resource* ptr)
        : actor_zeta::basic_actor<calculator_actor>(ptr) {
        std::cout << "[Calculator " << id() << "] Created\n";
    }

    // NOTE: No explicit destructor needed!
    // shutdown_guard_t automatically calls begin_shutdown() before base class destructor.
    // This prevents race condition between main thread destroying actor and worker threads accessing it.
    ~calculator_actor() = default;

    // ========================================
    // All methods must be coroutines (use co_return)
    // ========================================

    /// @brief Simple addition - coroutine method
    actor_zeta::unique_future<int> add(int a, int b) {
        std::cout << "[Calculator] add(" << a << ", " << b << ")\n";
        co_return a + b;
    }

    /// @brief Simple multiplication - coroutine method
    actor_zeta::unique_future<int> multiply(int a, int b) {
        std::cout << "[Calculator] multiply(" << a << ", " << b << ")\n";
        co_return a * b;
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
        switch (msg->command()) {
            case actor_zeta::msg_id<calculator_actor, &calculator_actor::add>:
                actor_zeta::dispatch(this, &calculator_actor::add, msg);
                break;
            case actor_zeta::msg_id<calculator_actor, &calculator_actor::multiply>:
                actor_zeta::dispatch(this, &calculator_actor::multiply, msg);
                break;
            case actor_zeta::msg_id<calculator_actor, &calculator_actor::square>: {
                // CRITICAL: Store pending coroutine!
                // If destroyed immediately, coroutine is destroyed → use-after-free
                auto future = actor_zeta::dispatch(this, &calculator_actor::square, msg);
                if (!future.available()) {
                    pending_.push_back(std::move(future));
                }
                break;
            }
            default:
                std::cerr << "[Calculator] Unknown message\n";
                break;
        }
    }

    /// @brief Clean up completed pending futures
    /// @return true if there are still pending coroutines
    /// With auto-resume in set_value(), coroutines resume automatically
    bool poll_pending() {
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->available()) {
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
        return !pending_.empty();
    }

private:
    std::vector<actor_zeta::unique_future<int>> pending_;
};

int main() {
    std::cout << "=== Mixed Sync/Async Actor Example ===\n\n";

    // Create PMR resource
    auto* resource =std::pmr::get_default_resource();

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

        // Process messages and poll pending coroutines until complete
        while (!future.available()) {
            calculator->resume(100);        // Process mailbox messages
            calculator->poll_pending();     // Resume pending coroutines
        }

        int result = std::move(future).get();
        std::cout << "Result: 5^2 = " << result << "\n\n";
    }

    std::cout << "--- Key observations ---\n";
    std::cout << "1. All methods have SAME signature: unique_future<T>\n";
    std::cout << "2. Caller uses SAME code: send() and get()\n";
    std::cout << "3. All methods must be coroutines using 'co_return'\n";
    std::cout << "4. Async methods can use 'co_await' to suspend\n";
    std::cout << "5. Coroutines with co_await need poll_pending() to resume\n";
    std::cout << "\n NOTE: Recursive coroutines (like factorial, power) are NOT supported\n";
    std::cout << "   Use iterative algorithms instead.\n\n";

    std::cout << "--- Cleanup ---\n\n";

    // Actors destroyed automatically
    calculator.reset();

    std::cout << "\n=== Example completed ===\n";

    return 0;
}
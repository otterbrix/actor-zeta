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

        auto future = actor_zeta::send(this, address(), &calculator_actor::multiply, x, x);

        std::cout << "[Calculator] ASYNC square - awaiting multiply...\n";
        int result = co_await std::move(future);
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
                // Store pending coroutine to prevent premature destruction
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

    auto* resource = std::pmr::get_default_resource();
    auto calculator = actor_zeta::spawn<calculator_actor>(resource);

    std::cout << "\n--- Testing SYNC methods ---\n\n";

    {
        auto future = actor_zeta::send(calculator.get(), calculator->address(),
                                       &calculator_actor::add, 10, 20);
        calculator->resume(100);
        int result = std::move(future).get();
        std::cout << "Result: 10 + 20 = " << result << "\n\n";
    }

    {
        auto future = actor_zeta::send(calculator.get(), calculator->address(),
                                       &calculator_actor::multiply, 7, 8);
        calculator->resume(100);
        int result = std::move(future).get();
        std::cout << "Result: 7 * 8 = " << result << "\n\n";
    }

    std::cout << "--- Testing ASYNC method (coroutine) ---\n\n";

    {
        auto future = actor_zeta::send(calculator.get(), calculator->address(),
                                       &calculator_actor::square, 5);

        while (!future.available()) {
            calculator->resume(100);
            calculator->poll_pending();
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

    calculator.reset();

    std::cout << "\n=== Example completed ===\n";

    return 0;
}
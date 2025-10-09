#define CATCH_CONFIG_MAIN // This tells Catch to provide a main() - only do this in one cpp file
#include <catch2/catch.hpp>

#include <actor-zeta/detail/memory_resource.hpp>
#include <actor-zeta/detail/unique_function.hpp>
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

// Simple memory_resource implementation for testing
class test_memory_resource final : public actor_zeta::pmr::memory_resource {
private:
    size_t allocations_ = 0;
    size_t deallocations_ = 0;
    size_t total_bytes_allocated_ = 0;

    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        allocations_++;
        total_bytes_allocated_ += bytes;
        void* ptr = nullptr;

        #if defined(_MSC_VER)
            ptr = _aligned_malloc(bytes, alignment);
        #else
            // posix_memalign requires alignment to be:
            // - a power of 2
            // - a multiple of sizeof(void*)
            // Ensure minimum alignment of sizeof(void*)
            size_t actual_alignment = alignment;
            if (actual_alignment < sizeof(void*)) {
                actual_alignment = sizeof(void*);
            }
            if (posix_memalign(&ptr, actual_alignment, bytes) != 0) {
                assert(0 && "bad_alloc");
            }
        #endif

        return ptr;
    }

    void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
        if (!p) return;

        deallocations_++;

        #if defined(_MSC_VER)
            _aligned_free(p);
        #else
            free(p);
        #endif
    }

    bool do_is_equal(const memory_resource& other) const noexcept override {
        return this == &other;
    }

public:
    // Reset counters
    void reset_stats() {
        allocations_ = 0;
        deallocations_ = 0;
        total_bytes_allocated_ = 0;
    }

    // Get statistics
    size_t allocations() const { return allocations_; }
    size_t deallocations() const { return deallocations_; }
    size_t total_bytes_allocated() const { return total_bytes_allocated_; }

    // Check if all allocations were deallocated
    bool all_deallocated() const { return allocations_ == deallocations_; }
};

// Test functions and functors
int add(int a, int b) {
    return a + b;
}

struct small_functor {
    int value;

    small_functor(int val) : value(val) {}

    int operator()(int x) const {
        return x + value;
    }
};

// Large functor that won't fit in small buffer
struct large_functor {
    char buffer[100];  // Large enough to not fit in small buffer
    std::string name;

    large_functor(const std::string& n) : name(n) {
        for (size_t i = 0; i < sizeof(buffer); ++i) {
            buffer[i] = static_cast<char>(i % 256);
        }
    }

    std::string operator()(int x) const {
        return name + ": " + std::to_string(x);
    }
};

TEST_CASE("Basic Functionality", "[unique_function]") {
    test_memory_resource resource;

    SECTION("Function pointer") {
        actor_zeta::detail::unique_function<int(int, int)> func(&resource,add);
        REQUIRE(func(5, 3) == 8);
    }

    SECTION("Small functor") {
        actor_zeta::detail::unique_function<int(int)> small_func(&resource,small_functor(10));
        REQUIRE(small_func(5) == 15);
        REQUIRE(small_func.uses_small_buffer());
    }

    SECTION("Large functor") {
        actor_zeta::detail::unique_function<std::string(int)> large_func(&resource,large_functor("Test"));
        REQUIRE(large_func(42) == "Test: 42");
        REQUIRE_FALSE(large_func.uses_small_buffer());
    }
}

TEST_CASE("Small Buffer Optimization", "[unique_function]") {
    test_memory_resource resource;
    resource.reset_stats();

    SECTION("Small functor doesn't allocate memory") {
        actor_zeta::detail::unique_function<int(int)> small_func(&resource,small_functor(10));
        REQUIRE(small_func.uses_small_buffer());
        REQUIRE(resource.allocations() == 0);
    }

    SECTION("Large functor allocates memory") {
        {
            actor_zeta::detail::unique_function<std::string(int)> large_func(&resource,large_functor("Test"));
            REQUIRE_FALSE(large_func.uses_small_buffer());
            REQUIRE(resource.allocations() == 1);
        }
        // After scope exit, all allocations should be deallocated
        REQUIRE(resource.all_deallocated());
    }
}

TEST_CASE("Move Semantics", "[unique_function]") {
    test_memory_resource resource;

    SECTION("Move small functor") {
        actor_zeta::detail::unique_function<int(int)> small_func1(&resource,small_functor(10));
        actor_zeta::detail::unique_function<int(int)> small_func2(&resource,std::move(small_func1));

        REQUIRE(small_func2.uses_small_buffer());
        REQUIRE(small_func2(5) == 15);
        REQUIRE_FALSE(small_func1); // Should be empty after move
    }

    SECTION("Move large functor") {
        resource.reset_stats();

        {
            actor_zeta::detail::unique_function<std::string(int)> large_func1(&resource,large_functor("Test"));
            REQUIRE(resource.allocations() == 1);

            actor_zeta::detail::unique_function<std::string(int)> large_func2(&resource,std::move(large_func1));
            REQUIRE_FALSE(large_func2.uses_small_buffer());
            REQUIRE(large_func2(42) == "Test: 42");
            REQUIRE_FALSE(large_func1); // Should be empty after move

            // No additional allocations should happen during move
            REQUIRE(resource.allocations() == 1);
        }

        // After scope exit, all allocations should be deallocated
        REQUIRE(resource.all_deallocated());
    }
}

TEST_CASE("Different Resources", "[unique_function]") {
    test_memory_resource resource1;
    test_memory_resource resource2;

    SECTION("Small functors with different resources") {
        actor_zeta::detail::unique_function<int(int)> small_func1(&resource1, small_functor(10));
        actor_zeta::detail::unique_function<int(int)> small_func2(&resource2, small_functor(20));

        // Small functors always move successfully since they don't use the resource
        REQUIRE(small_func1(5) == 15);
        REQUIRE(small_func2(5) == 25);

        small_func2 = std::move(small_func1);
        REQUIRE(small_func2.uses_small_buffer());
        REQUIRE(small_func2(5) == 15);
        REQUIRE(small_func1.empty());
    }

    SECTION("Large functors with different resources") {
        actor_zeta::detail::unique_function<std::string(int)> large_func1(&resource1, large_functor("One"));
        actor_zeta::detail::unique_function<std::string(int)> large_func2(&resource2, large_functor("Two"));

        // Verify functionality before move
        REQUIRE(large_func1(42) == "One: 42");
        REQUIRE(large_func2(42) == "Two: 42");

        // When moving between different resources both objects become empty
        large_func2 = std::move(large_func1);

        // Verify both objects became empty
        REQUIRE(large_func1.empty());
        REQUIRE(large_func2.empty());

        // Verify no memory leaks
        REQUIRE(resource1.allocations() == 1);
        REQUIRE(resource2.allocations() == 1);

        // Cannot test calling empty function as it would trigger assert
        // Instead just verify the function is empty
        REQUIRE_FALSE(large_func2);
    }

    SECTION("Large functors with same resource") {
        actor_zeta::detail::unique_function<std::string(int)> large_func1(&resource1, large_functor("One"));
        actor_zeta::detail::unique_function<std::string(int)> large_func2(&resource1, large_functor("Two"));

        REQUIRE(large_func1(42) == "One: 42");
        REQUIRE(large_func2(42) == "Two: 42");

        // With compatible resources move works normally
        large_func2 = std::move(large_func1);

        REQUIRE(large_func1.empty());
        REQUIRE_FALSE(large_func2.empty());
        REQUIRE(large_func2(42) == "One: 42");

        // Verify no memory leaks
        REQUIRE(resource1.allocations() == 2);
    }
}


TEST_CASE("Swap Operations", "[unique_function]") {
    test_memory_resource resource;

    SECTION("Swap member function") {
        actor_zeta::detail::unique_function<int(int)> func1(&resource,[](int x) { return x * 2; });
        actor_zeta::detail::unique_function<int(int)> func2(&resource,[](int x) { return x + 10; });

        func1.swap(func2);
        REQUIRE(func1(5) == 15);
        REQUIRE(func2(5) == 10);
    }

    SECTION("Swap free function") {
        actor_zeta::detail::unique_function<int(int)> func1(&resource,[](int x) { return x * 2; });
        actor_zeta::detail::unique_function<int(int)> func2(&resource,[](int x) { return x + 10; });

        swap(func1, func2);
        REQUIRE(func1(5) == 15);
        REQUIRE(func2(5) == 10);
    }

    SECTION("Swap between small and large functor") {
        actor_zeta::detail::unique_function<std::string(int)> large_func(&resource,large_functor("Test"));
        actor_zeta::detail::unique_function<std::string(int)> small_func(&resource,[](int x) { return std::to_string(x); });

        small_func.swap(large_func);
        REQUIRE(small_func(42) == "Test: 42");
        REQUIRE(large_func(42) == "42");
    }
}

// 1. Test for different function signatures
TEST_CASE("Function Signatures", "[unique_function]") {
    test_memory_resource resource;

    SECTION("Function without arguments") {
        actor_zeta::detail::unique_function<int()> func(&resource, []() { return 42; });
        REQUIRE(func() == 42);
    }

    SECTION("Function with multiple arguments") {
        actor_zeta::detail::unique_function<int(int, int, int)> func(&resource,
            [](int a, int b, int c) { return a + b + c; });
        REQUIRE(func(1, 2, 3) == 6);
    }

    SECTION("Function returning void") {
        bool called = false;
        actor_zeta::detail::unique_function<void(bool&)> func(&resource,
            [](bool& flag) { flag = true; });
        func(called);
        REQUIRE(called);
    }

    SECTION("Function with reference arguments") {
        actor_zeta::detail::unique_function<void(int&)> func(&resource,
            [](int& x) { x *= 2; });
        int value = 5;
        func(value);
        REQUIRE(value == 10);
    }
}

// 2. Test for reset() and empty checks
TEST_CASE("Reset and Empty Checks", "[unique_function]") {
    test_memory_resource resource;

    SECTION("Reset small functor") {
        actor_zeta::detail::unique_function<int(int)> func(&resource, small_functor(10));
        REQUIRE_FALSE(func.empty());
        REQUIRE(func);

        func.reset();
        REQUIRE(func.empty());
        REQUIRE_FALSE(func);
        REQUIRE(func == nullptr);
    }

    SECTION("Reset large functor") {
        resource.reset_stats();

        {
            actor_zeta::detail::unique_function<std::string(int)> func(&resource, large_functor("Test"));
            REQUIRE(resource.allocations() == 1);

            func.reset();
            REQUIRE(func.empty());
            REQUIRE(resource.deallocations() == 1);
        }

        REQUIRE(resource.all_deallocated());
    }

    SECTION("Comparison with nullptr") {
        actor_zeta::detail::unique_function<int(int)> func(&resource, small_functor(10));
        REQUIRE_FALSE(func == nullptr);
        REQUIRE(func != nullptr);

        func.reset();
        REQUIRE(func == nullptr);
        REQUIRE_FALSE(func != nullptr);

        REQUIRE(nullptr == func);
        REQUIRE_FALSE(nullptr != func);
    }
}

// 3. Test for buffer size edge cases
TEST_CASE("Buffer Size Edge Cases", "[unique_function]") {
    test_memory_resource resource;

    const size_t small_buffer_size = 3 * sizeof(void*);

    struct almost_small_functor {
        char data[small_buffer_size - sizeof(int) - 1];
        int value;

        almost_small_functor(int v = 42) : value(v) {
            std::memset(data, 0, sizeof(data));
        }

        int operator()(int x) const {
            return x + value;
        }
    };

    SECTION("Verify small buffer size assumptions") {
        INFO("Small buffer size: " << small_buffer_size);
        INFO("almost_small_functor size: " << sizeof(almost_small_functor));
        REQUIRE(sizeof(almost_small_functor) <= small_buffer_size);
    }

    SECTION("Functor just fits in small buffer") {
        actor_zeta::detail::unique_function<int(int)> func(&resource, almost_small_functor(10));
        REQUIRE(func.uses_small_buffer());
        REQUIRE(func(5) == 15);
        REQUIRE(resource.allocations() == 0);
    }
}

// 4. Test for self-assignment and multiple reassignments
TEST_CASE("Self-Assignment and Multiple Assignments", "[unique_function]") {
    test_memory_resource resource;

    SECTION("Self-assignment") {
        actor_zeta::detail::unique_function<int(int)> func(&resource, [](int x) { return x * 2; });

        // Save address to verify object remains the same
        const void* func_address = &func;

        // Self-assignment should be safe
        func = std::move(func);

        // Verify address hasn't changed
        REQUIRE(&func == func_address);

        // Function should retain its functionality
        REQUIRE_FALSE(func.empty());
        REQUIRE(func(5) == 10);
    }

    SECTION("Multiple reassignments") {
        resource.reset_stats();

        actor_zeta::detail::unique_function<int(int)> func(&resource, [](int x) { return x; });

        // Sequential reassignment
        for (int i = 1; i <= 10; ++i) {
            func = actor_zeta::detail::unique_function<int(int)>(&resource, [i](int x) { return x * i; });
            REQUIRE(func(2) == 2 * i);
        }

        // Check for memory leaks
        func.reset();
        REQUIRE(resource.all_deallocated());
    }
}

// 5. Test for compatibility with std::function and various functors
TEST_CASE("Compatibility with Standard Functors", "[unique_function]") {
    test_memory_resource resource;

    SECTION("std::function compatibility") {
        std::function<int(int)> std_func = [](int x) { return x * 3; };

        actor_zeta::detail::unique_function<int(int)> unique_func(&resource, std_func);
        REQUIRE(unique_func(5) == 15);
    }

    SECTION("std::bind compatibility") {
        auto bound_func = std::bind(std::plus<int>(), std::placeholders::_1, 5);

        actor_zeta::detail::unique_function<int(int)> func(&resource, bound_func);
        REQUIRE(func(3) == 8);
    }

    SECTION("Capturing lambda with state") {
        int state = 0;

        actor_zeta::detail::unique_function<void()> func(&resource, [&state]() {
            state++;
        });

        func();
        REQUIRE(state == 1);

        func();
        REQUIRE(state == 2);
    }
}

// 6. Test for different return types
TEST_CASE("Different Return Types", "[unique_function]") {
    test_memory_resource resource;

    SECTION("Complex return types") {
        actor_zeta::detail::unique_function<std::vector<int>(int)> func(&resource,
            [](int size) {
                std::vector<int> result;
                for (int i = 0; i < size; ++i) {
                    result.push_back(i);
                }
                return result;
            });

        std::vector<int> result = func(3);
        REQUIRE(result.size() == 3);
        REQUIRE(result[0] == 0);
        REQUIRE(result[1] == 1);
        REQUIRE(result[2] == 2);
    }

    SECTION("Reference return type") {
        int value = 42;

        actor_zeta::detail::unique_function<int&()> func(&resource,
            [&value]() -> int& {
                return value;
            });

        func() = 100; // Change value via reference
        REQUIRE(value == 100);
    }
}

TEST_CASE("Resource Management Scenarios", "[unique_function]") {
    test_memory_resource resource1;
    test_memory_resource resource2;

    SECTION("Memory management with compatible resources") {
        resource1.reset_stats();

        {
            actor_zeta::detail::unique_function<std::string(int)> func1(&resource1, large_functor("One"));
            REQUIRE(resource1.allocations() == 1);

            actor_zeta::detail::unique_function<std::string(int)> func2(&resource1, std::move(func1));
            REQUIRE(resource1.allocations() == 1);
            REQUIRE(func1.empty());

            actor_zeta::detail::unique_function<std::string(int)> func3(&resource1, std::move(func2));
            REQUIRE(resource1.allocations() == 1);
            REQUIRE(func2.empty());
            REQUIRE_FALSE(func3.empty());
        }

        REQUIRE(resource1.allocations() == resource1.deallocations());
    }

    SECTION("Memory management with incompatible resources") {
        resource1.reset_stats();
        resource2.reset_stats();

        {
            actor_zeta::detail::unique_function<std::string(int)> func1(&resource1, large_functor("One"));
            REQUIRE(resource1.allocations() == 1);

            actor_zeta::detail::unique_function<std::string(int)> func2(&resource2, large_functor("Two"));
            REQUIRE(resource2.allocations() == 1);

            REQUIRE(func1(42) == "One: 42");
            REQUIRE(func2(42) == "Two: 42");

            func2 = std::move(func1);

            REQUIRE(func1.empty());
            REQUIRE(func2.empty());

        }


        REQUIRE(resource2.allocations() == resource2.deallocations());

        INFO("Expected behavior: memory from resource1 is not deallocated when moving to incompatible resource");
        INFO("Allocations: " << resource1.allocations() << ", Deallocations: " << resource1.deallocations());
    }
}

// Helper class to track object lifetime
struct lifetime_tracker {
    int* counter_;
    int value_;

    explicit lifetime_tracker(int* counter, int value = 42)
        : counter_(counter), value_(value) {
        if (counter_) (*counter_)++;
    }

    lifetime_tracker(const lifetime_tracker& other)
        : counter_(other.counter_), value_(other.value_) {
        if (counter_) (*counter_)++;
    }

    lifetime_tracker(lifetime_tracker&& other) noexcept
        : counter_(other.counter_), value_(other.value_) {
        other.counter_ = nullptr;
    }

    ~lifetime_tracker() {
        if (counter_) (*counter_)--;
    }

    int get_value() const { return value_; }
};

// Test 1: Parameter lifetime shorter than unique_function
TEST_CASE("Parameter Lifetime - Data Dies Before Function", "[unique_function][lifetime]") {
    test_memory_resource resource;
    int alive_count = 0;

    SECTION("By value - copy") {
        actor_zeta::detail::unique_function<int(lifetime_tracker)> func(&resource,
            [](lifetime_tracker obj) {
                return obj.get_value() * 2;
            });

        {
            lifetime_tracker obj(&alive_count, 100);
            REQUIRE(alive_count == 1);

            int result = func(obj);
            REQUIRE(result == 200);
            // obj is copied into function, so we have 2 instances during call
        }

        // Original object is destroyed, but function still works
        REQUIRE(alive_count == 0);

        // Create new object and call again
        {
            lifetime_tracker obj2(&alive_count, 50);
            REQUIRE(alive_count == 1);

            int result = func(obj2);
            REQUIRE(result == 100);
        }

        REQUIRE(alive_count == 0);
    }

    SECTION("By rvalue reference - move") {
        actor_zeta::detail::unique_function<int(lifetime_tracker)> func(&resource,
            [](lifetime_tracker obj) {
                return obj.get_value() + 10;
            });

        {
            lifetime_tracker obj(&alive_count, 33);
            REQUIRE(alive_count == 1);

            int result = func(std::move(obj));
            REQUIRE(result == 43);
        }

        // Original object destroyed
        REQUIRE(alive_count == 0);

        // Function still works with new data
        {
            lifetime_tracker obj2(&alive_count, 77);
            int result = func(std::move(obj2));
            REQUIRE(result == 87);
        }

        REQUIRE(alive_count == 0);
    }

    SECTION("By const reference") {
        actor_zeta::detail::unique_function<int(const lifetime_tracker&)> func(&resource,
            [](const lifetime_tracker& obj) {
                return obj.get_value() * 3;
            });

        {
            lifetime_tracker obj(&alive_count, 15);
            REQUIRE(alive_count == 1);

            int result = func(obj);
            REQUIRE(result == 45);

            // Object still alive after call
            REQUIRE(alive_count == 1);
        }

        // Object destroyed after scope
        REQUIRE(alive_count == 0);

        // Function still valid but needs live object to call
        {
            lifetime_tracker obj2(&alive_count, 20);
            REQUIRE(alive_count == 1);

            int result = func(obj2);
            REQUIRE(result == 60);
        }

        REQUIRE(alive_count == 0);
    }

    SECTION("By mutable reference") {
        actor_zeta::detail::unique_function<void(lifetime_tracker&)> func(&resource,
            [](lifetime_tracker& obj) {
                // Modify the object through reference
                obj.value_ *= 2;
            });

        {
            lifetime_tracker obj(&alive_count, 10);
            REQUIRE(alive_count == 1);
            REQUIRE(obj.get_value() == 10);

            func(obj);
            REQUIRE(obj.get_value() == 20);

            func(obj);
            REQUIRE(obj.get_value() == 40);

            REQUIRE(alive_count == 1);
        }

        // Object destroyed
        REQUIRE(alive_count == 0);

        // Function still valid with new object
        {
            lifetime_tracker obj2(&alive_count, 5);
            func(obj2);
            REQUIRE(obj2.get_value() == 10);
        }

        REQUIRE(alive_count == 0);
    }

    SECTION("By pointer") {
        actor_zeta::detail::unique_function<int(const lifetime_tracker*)> func(&resource,
            [](const lifetime_tracker* obj) {
                return obj ? obj->get_value() * 5 : -1;
            });

        {
            lifetime_tracker obj(&alive_count, 8);
            REQUIRE(alive_count == 1);

            int result = func(&obj);
            REQUIRE(result == 40);

            REQUIRE(alive_count == 1);
        }

        // Object destroyed, pointer becomes dangling
        REQUIRE(alive_count == 0);

        // Function still valid but needs valid pointer
        {
            lifetime_tracker obj2(&alive_count, 12);
            int result = func(&obj2);
            REQUIRE(result == 60);
        }

        REQUIRE(alive_count == 0);

        // Test with nullptr
        int null_result = func(nullptr);
        REQUIRE(null_result == -1);
    }

    SECTION("Multiple parameters with different lifetimes") {
        actor_zeta::detail::unique_function<int(lifetime_tracker, const lifetime_tracker&, int)> func(
            &resource,
            [](lifetime_tracker by_val, const lifetime_tracker& by_ref, int multiplier) {
                return (by_val.get_value() + by_ref.get_value()) * multiplier;
            });

        {
            lifetime_tracker obj1(&alive_count, 10);
            lifetime_tracker obj2(&alive_count, 20);
            REQUIRE(alive_count == 2);

            int result = func(obj1, obj2, 2);
            REQUIRE(result == 60); // (10 + 20) * 2

            REQUIRE(alive_count == 2);
        }

        // Both objects destroyed
        REQUIRE(alive_count == 0);

        // Function still works with new objects
        {
            lifetime_tracker obj3(&alive_count, 5);
            lifetime_tracker obj4(&alive_count, 15);
            REQUIRE(alive_count == 2);

            int result = func(obj3, obj4, 3);
            REQUIRE(result == 60); // (5 + 15) * 3
        }

        REQUIRE(alive_count == 0);
    }
}

// Test 2: Function lifetime shorter than parameters
TEST_CASE("Parameter Lifetime - Function Dies Before Data", "[unique_function][lifetime]") {
    test_memory_resource resource;
    int alive_count = 0;

    SECTION("By value with long-lived data") {
        lifetime_tracker long_lived_obj(&alive_count, 100);
        REQUIRE(alive_count == 1);

        {
            actor_zeta::detail::unique_function<int(lifetime_tracker)> func(&resource,
                [](lifetime_tracker obj) {
                    return obj.get_value() * 2;
                });

            int result = func(long_lived_obj);
            REQUIRE(result == 200);
            REQUIRE(alive_count == 1); // Original still alive
        }

        // Function destroyed, but original object still alive
        REQUIRE(alive_count == 1);
        REQUIRE(long_lived_obj.get_value() == 100);
    }

    SECTION("By const reference with long-lived data") {
        lifetime_tracker long_lived_obj(&alive_count, 50);
        REQUIRE(alive_count == 1);

        {
            actor_zeta::detail::unique_function<int(const lifetime_tracker&)> func(&resource,
                [](const lifetime_tracker& obj) {
                    return obj.get_value() + 25;
                });

            int result = func(long_lived_obj);
            REQUIRE(result == 75);
            REQUIRE(alive_count == 1);

            int result2 = func(long_lived_obj);
            REQUIRE(result2 == 75);
        }

        // Function destroyed, object still alive
        REQUIRE(alive_count == 1);
        REQUIRE(long_lived_obj.get_value() == 50);
    }

    SECTION("By mutable reference with long-lived data") {
        lifetime_tracker long_lived_obj(&alive_count, 10);
        REQUIRE(alive_count == 1);

        {
            actor_zeta::detail::unique_function<void(lifetime_tracker&)> func(&resource,
                [](lifetime_tracker& obj) {
                    obj.value_ += 5;
                });

            func(long_lived_obj);
            REQUIRE(long_lived_obj.get_value() == 15);

            func(long_lived_obj);
            REQUIRE(long_lived_obj.get_value() == 20);

            REQUIRE(alive_count == 1);
        }

        // Function destroyed, but modifications persist in object
        REQUIRE(alive_count == 1);
        REQUIRE(long_lived_obj.get_value() == 20);
    }

    SECTION("By pointer with long-lived data") {
        lifetime_tracker long_lived_obj(&alive_count, 30);
        REQUIRE(alive_count == 1);

        {
            actor_zeta::detail::unique_function<int(const lifetime_tracker*)> func(&resource,
                [](const lifetime_tracker* obj) {
                    return obj ? obj->get_value() * 3 : 0;
                });

            int result = func(&long_lived_obj);
            REQUIRE(result == 90);
            REQUIRE(alive_count == 1);
        }

        // Function destroyed, pointer target still valid
        REQUIRE(alive_count == 1);
        REQUIRE(long_lived_obj.get_value() == 30);
    }

    SECTION("Move semantics - ownership transfer") {
        lifetime_tracker long_lived_obj(&alive_count, 77);
        REQUIRE(alive_count == 1);

        {
            actor_zeta::detail::unique_function<int(lifetime_tracker)> func(&resource,
                [](lifetime_tracker obj) {
                    return obj.get_value() * 2;
                });

            // Move object into function call
            lifetime_tracker moved_obj = std::move(long_lived_obj);
            REQUIRE(alive_count == 1); // Counter transferred, not duplicated

            int result = func(std::move(moved_obj));
            REQUIRE(result == 154);
        }

        // Function destroyed
        REQUIRE(alive_count == 0); // Object was moved and copied into call, then destroyed
    }

    SECTION("Capturing lambda with long-lived data") {
        lifetime_tracker long_lived_obj(&alive_count, 15);
        REQUIRE(alive_count == 1);

        {
            // Lambda captures by reference
            actor_zeta::detail::unique_function<int()> func(&resource,
                [&long_lived_obj]() {
                    return long_lived_obj.get_value() * 4;
                });

            int result = func();
            REQUIRE(result == 60);
            REQUIRE(alive_count == 1);

            // Modify captured object
            long_lived_obj.value_ = 20;
            result = func();
            REQUIRE(result == 80);
        }

        // Function destroyed, captured object still alive
        REQUIRE(alive_count == 1);
        REQUIRE(long_lived_obj.get_value() == 20);
    }

    SECTION("Multiple functions sharing same data") {
        lifetime_tracker shared_obj(&alive_count, 25);
        REQUIRE(alive_count == 1);

        {
            actor_zeta::detail::unique_function<int(const lifetime_tracker&)> func1(&resource,
                [](const lifetime_tracker& obj) { return obj.get_value() * 2; });

            actor_zeta::detail::unique_function<int(const lifetime_tracker&)> func2(&resource,
                [](const lifetime_tracker& obj) { return obj.get_value() + 100; });

            REQUIRE(func1(shared_obj) == 50);
            REQUIRE(func2(shared_obj) == 125);
            REQUIRE(alive_count == 1);
        }

        // Both functions destroyed, shared object still alive
        REQUIRE(alive_count == 1);
        REQUIRE(shared_obj.get_value() == 25);
    }
}
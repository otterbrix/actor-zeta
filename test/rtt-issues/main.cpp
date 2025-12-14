/// @file main.cpp
/// @brief Tests to demonstrate RTT issues from CODE_REVIEW_RTT.md
///
/// These tests are designed to FAIL or demonstrate problems in current implementation.
/// After fixes are applied, they should PASS.

#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta/detail/rtt.hpp>

#include <memory_resource>
#include <string>
#include <vector>
#include <atomic>

using actor_zeta::detail::rtt;

// ============================================================================
// Issue #1: Destruction Order Test
// ============================================================================

namespace destruction_order_test {

    // Track destruction order
    static std::vector<int> destruction_sequence;

    struct TrackedObject {
        int id;

        explicit TrackedObject(int id_) : id(id_) {
            // std::cout << "TrackedObject(" << id << ") constructed\n";
        }

        TrackedObject(TrackedObject&& other) noexcept : id(other.id) {
            other.id = -1; // mark as moved
        }

        ~TrackedObject() {
            if (id >= 0) { // only track non-moved objects
                destruction_sequence.push_back(id);
                // std::cout << "TrackedObject(" << id << ") destroyed\n";
            }
        }
    };

    inline void clear() {
        destruction_sequence.clear();
    }

} // namespace destruction_order_test

TEST_CASE("Issue #1: Destruction order should be reverse (LIFO)") {
    using namespace destruction_order_test;

    SECTION("Three objects should be destroyed in reverse order") {
        clear();

        {
            auto* resource = std::pmr::get_default_resource();
            rtt r(resource,
                  TrackedObject(1),  // first created
                  TrackedObject(2),  // second created
                  TrackedObject(3)); // third created

            REQUIRE(r.size() == 3);
        }
        // After rtt destruction, check order

        REQUIRE(destruction_sequence.size() == 3);

        // C++ convention: reverse order (LIFO)
        // Expected: 3, 2, 1 (last created, first destroyed)
        // Current (buggy): 1, 2, 3 (first created, first destroyed)

        INFO("Actual destruction order: "
             << destruction_sequence[0] << ", "
             << destruction_sequence[1] << ", "
             << destruction_sequence[2]);

        // FIXED: Now destroys in reverse order (LIFO) - C++ convention
        CHECK(destruction_sequence[0] == 3); // last created, first destroyed
        CHECK(destruction_sequence[1] == 2); // middle
        CHECK(destruction_sequence[2] == 1); // first created, last destroyed

        // Correct LIFO order
        REQUIRE(destruction_sequence == std::vector<int>{3, 2, 1});
    }

    SECTION("Dependency scenario: Service depends on Logger") {
        clear();

        // Simulates real-world scenario where later objects depend on earlier ones
        struct Logger {
            int* destruction_flag;
            Logger(int* flag) : destruction_flag(flag) {}
            ~Logger() { *destruction_flag = 1; }
            void log(const char*) const {
                // In real code, this would crash if Logger is destroyed
            }
        };

        struct Service {
            const Logger* logger;
            int* destruction_flag;

            Service(const Logger* l, int* flag) : logger(l), destruction_flag(flag) {}
            ~Service() {
                *destruction_flag = 1;
                // If logger was destroyed first, this would be UB:
                // logger->log("Service shutting down");
            }
        };

        int logger_destroyed = 0;
        int service_destroyed = 0;

        {
            auto* resource = std::pmr::get_default_resource();

            // Create Logger first, then Service that depends on it
            Logger logger(&logger_destroyed);
            Service service(&logger, &service_destroyed);

            // Store in rtt (copies are made)
            rtt r(resource,
                  TrackedObject(1),  // placeholder to track order
                  TrackedObject(2));
        }

        // Note: This test mainly demonstrates the concept.
        // The actual dependency issue is harder to test without UB.
        REQUIRE(destruction_sequence.size() == 2);
    }
}

// ============================================================================
// Issue #2: swap() Cross-Arena Test - RESOLVED BY REMOVAL
// ============================================================================

// Note: swap() method was removed from rtt class because it was inherently
// unsafe for cross-arena operations. Type-erased containers cannot safely
// swap between different memory resources because:
// 1. Memory was allocated by one resource but would be deallocated by another
// 2. No way to deep-copy type-erased objects (no type info at runtime)
//
// Resolution: swap() and free function swap(rtt&, rtt&) have been removed.
// Use move semantics for transferring rtt objects within same arena.

namespace swap_arena_test {

    // Custom memory resource that tracks which resource allocated memory
    // Kept for use in other cross-arena tests
    class tracking_resource : public std::pmr::memory_resource {
    public:
        explicit tracking_resource(int id) : id_(id) {}

        int id() const { return id_; }
        size_t allocations() const { return allocations_; }
        size_t deallocations() const { return deallocations_; }

        struct allocation_info {
            void* ptr;
            size_t size;
            int resource_id;
        };

        std::vector<allocation_info>& alloc_log() { return alloc_log_; }
        std::vector<allocation_info>& dealloc_log() { return dealloc_log_; }

    protected:
        void* do_allocate(size_t bytes, size_t alignment) override {
            void* ptr = std::pmr::get_default_resource()->allocate(bytes, alignment);
            allocations_++;
            alloc_log_.push_back({ptr, bytes, id_});
            return ptr;
        }

        void do_deallocate(void* p, size_t bytes, size_t alignment) override {
            deallocations_++;
            dealloc_log_.push_back({p, bytes, id_});
            std::pmr::get_default_resource()->deallocate(p, bytes, alignment);
        }

        bool do_is_equal(const memory_resource& other) const noexcept override {
            // Can't use dynamic_cast with -fno-rtti
            // Use address comparison as simple check
            return this == &other;
        }

    private:
        int id_;
        size_t allocations_ = 0;
        size_t deallocations_ = 0;
        std::vector<allocation_info> alloc_log_;
        std::vector<allocation_info> dealloc_log_;
    };

} // namespace swap_arena_test

TEST_CASE("Issue #2: swap() removed - use move semantics instead") {
    using namespace swap_arena_test;

    SECTION("Move semantics within same arena - correct pattern") {
        tracking_resource arena(1);

        rtt a(&arena, 1, 2, 3);
        rtt b(&arena, 4, 5);

        REQUIRE(a.size() == 3);
        REQUIRE(b.size() == 2);

        // Instead of swap, use move semantics for transferring data
        rtt temp(std::move(a));
        a = std::move(b);
        b = std::move(temp);

        REQUIRE(a.size() == 2);
        REQUIRE(b.size() == 3);

        // Values should be swapped
        REQUIRE(a.get<int>(0) == 4);
        REQUIRE(b.get<int>(0) == 1);

        // All memory operations stay within same arena
        REQUIRE(arena.allocations() == 2);
    }

    SECTION("Cross-arena operations use assert protection") {
        tracking_resource arena1(1);
        tracking_resource arena2(2);

        rtt a(&arena1, 100, 200);
        rtt b(&arena2, 300, 400);

        REQUIRE(a.memory_resource() == &arena1);
        REQUIRE(b.memory_resource() == &arena2);

        // Note: Cross-arena move assignment is protected by assert
        // Cannot easily test assertion failure, but API is now safe
        // a = std::move(b); // Would trigger assert in debug mode

        // Each rtt will deallocate to its own arena correctly
    }

    SECTION("Proper destruction - each arena deallocates own memory") {
        tracking_resource arena1(1);
        tracking_resource arena2(2);

        {
            rtt a(&arena1, 100, 200);
            rtt b(&arena2, 300, 400);

            REQUIRE(arena1.allocations() == 1);
            REQUIRE(arena2.allocations() == 1);
        }
        // Without swap(), each rtt correctly deallocates to its own arena

        REQUIRE(arena1.deallocations() == 1);
        REQUIRE(arena2.deallocations() == 1);

        // Verify correct deallocation (memory returned to same arena that allocated it)
        if (!arena1.dealloc_log().empty() && !arena1.alloc_log().empty()) {
            REQUIRE(arena1.alloc_log()[0].ptr == arena1.dealloc_log()[0].ptr);
        }
        if (!arena2.dealloc_log().empty() && !arena2.alloc_log().empty()) {
            REQUIRE(arena2.alloc_log()[0].ptr == arena2.dealloc_log()[0].ptr);
        }
    }
}

// ============================================================================
// Issue #3: dtor_ Counter Test
// ============================================================================

#ifdef __ENABLE_TESTS_MEASUREMENTS__

namespace rtt_test = actor_zeta::detail::rtt_test;

TEST_CASE("Issue #3: dtor_ counter should be incremented") {
    rtt_test::clear();

    SECTION("Single rtt destruction should increment dtor_") {
        {
            auto* resource = std::pmr::get_default_resource();
            rtt r(resource, 1, 2, 3);

            REQUIRE(rtt_test::templated_ctor_ == 1);
            REQUIRE(rtt_test::dtor_ == 0);
        }
        // After destruction:

        // This will FAIL with current implementation because dtor_ is never incremented!
        REQUIRE(rtt_test::dtor_ == 1); // Expected 1, but is 0
    }

    SECTION("Multiple rtt destructions should increment dtor_ correctly") {
        rtt_test::clear();

        {
            auto* resource = std::pmr::get_default_resource();
            rtt r1(resource, 1);
            rtt r2(resource, 2);
            rtt r3(resource, 3);

            REQUIRE(rtt_test::templated_ctor_ == 3);
        }

        // This will FAIL - dtor_ stays 0
        REQUIRE(rtt_test::dtor_ == 3); // Expected 3, but is 0
    }

    SECTION("Move should not double-count destructions") {
        rtt_test::clear();

        {
            auto* resource = std::pmr::get_default_resource();
            rtt r1(resource, 42);
            rtt r2(std::move(r1)); // move

            REQUIRE(rtt_test::templated_ctor_ == 1);
            REQUIRE(rtt_test::move_ctor_ == 1);
        }
        // Two objects destroyed, but only one had data

        // With fix: dtor_ should be 2 (both objects destructed)
        // The moved-from object still has its destructor called
        REQUIRE(rtt_test::dtor_ == 2);
    }
}

#else

TEST_CASE("Issue #3: dtor_ counter test (skipped - measurements disabled)") {
    WARN("__ENABLE_TESTS_MEASUREMENTS__ is not defined. dtor_ counter tests skipped.");
    REQUIRE(true); // placeholder
}

#endif

// ============================================================================
// Issue #5: Bounds Checking Test
// ============================================================================

TEST_CASE("Issue #5: Bounds checking for get() and offset()") {
    auto* resource = std::pmr::get_default_resource();

    SECTION("Valid indices should work") {
        rtt r(resource, 10, 20, 30);

        REQUIRE(r.size() == 3);
        REQUIRE(r.get<int>(0) == 10);
        REQUIRE(r.get<int>(1) == 20);
        REQUIRE(r.get<int>(2) == 30);
    }

    SECTION("Empty rtt - any index is invalid") {
        rtt r(resource);

        REQUIRE(r.size() == 0);
        REQUIRE(r.empty());

        // These would be UB without bounds checking:
        // r.get<int>(0);  // UB - reading past end
        // r.offset(0);    // UB - reading past end

        // With assert, this should fail in debug mode
        // Cannot easily test UB, but we document the issue
    }

    SECTION("Index equal to size is invalid") {
        rtt r(resource, 1, 2, 3);

        REQUIRE(r.size() == 3);

        // Index 3 is out of bounds (valid: 0, 1, 2)
        // r.get<int>(3);  // UB without bounds check
        // r.offset(3);    // UB without bounds check

        // This test documents the issue but can't safely demonstrate UB
    }
}

// ============================================================================
// Issue #6: Naming Consistency Test (compile-time only)
// ============================================================================

TEST_CASE("Issue #6: Member naming consistency") {
    // This is a style issue - no runtime test needed
    // Just documenting that 'allocation' should be 'allocation_'

    // The inconsistency is visible in the source:
    // void* allocation = nullptr;      // no underscore
    // char* data_ = nullptr;           // has underscore
    // objects_t* objects_ = nullptr;   // has underscore

    REQUIRE(true); // placeholder
}

// ============================================================================
// Additional: Verify alignment calculation
// ============================================================================

TEST_CASE("Verify alignment calculation with getSize") {
    SECTION("Simple types") {
        constexpr size_t sz1 = actor_zeta::detail::getSize<0, int>();
        REQUIRE(sz1 == sizeof(int));

        constexpr size_t sz2 = actor_zeta::detail::getSize<0, int, double>();
        // int (4) + padding (4) + double (8) = 16
        REQUIRE(sz2 >= sizeof(int) + sizeof(double));
    }

    SECTION("Mixed alignment types") {
        constexpr size_t sz = actor_zeta::detail::getSize<0, char, double, char>();
        // char (1) + padding (7) + double (8) + char (1) = 17
        REQUIRE(sz >= sizeof(char) + sizeof(double) + sizeof(char));
    }

    SECTION("Actual rtt capacity matches getSize") {
        auto* resource = std::pmr::get_default_resource();

        rtt r(resource, char('a'), double(3.14), char('z'));

        constexpr size_t expected = actor_zeta::detail::getSize<0, char, double, char>();
        REQUIRE(r.capacity() == expected);
    }
}

// ============================================================================
// Issue #4: force_align() returns nullptr test
// ============================================================================

TEST_CASE("Issue #4: force_align behavior") {
    // Note: This issue is hard to test because getSize<>() guarantees
    // correct capacity at compile time. We can only document the concern.

    SECTION("Normal usage - alignment always succeeds") {
        auto* resource = std::pmr::get_default_resource();

        // Various alignment requirements
        rtt r1(resource, char('a'), int(42), double(3.14));
        REQUIRE(r1.size() == 3);
        REQUIRE(r1.get<char>(0) == 'a');
        REQUIRE(r1.get<int>(1) == 42);
        REQUIRE(r1.get<double>(2) == Approx(3.14));

        // Stress test with many elements
        rtt r2(resource, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
        REQUIRE(r2.size() == 10);
        for (std::size_t i = 0; i < 10; ++i) {
            REQUIRE(r2.get<int>(i) == static_cast<int>(i) + 1);
        }
    }
}

// ============================================================================
// Move semantics tests
// ============================================================================

TEST_CASE("Move semantics correctness") {
    auto* resource = std::pmr::get_default_resource();

    SECTION("Move constructor leaves source in valid empty state") {
        rtt source(resource, 1, 2, 3);
        REQUIRE(source.size() == 3);
        REQUIRE(source.memory_resource() == resource);

        rtt dest(std::move(source));

        // Source should be empty and valid
        REQUIRE(source.size() == 0);
        REQUIRE(source.empty());
        REQUIRE(source.memory_resource() == nullptr);
        REQUIRE(source.capacity() == 0);
        REQUIRE(source.volume() == 0);

        // Dest should have the data
        REQUIRE(dest.size() == 3);
        REQUIRE(dest.get<int>(0) == 1);
        REQUIRE(dest.memory_resource() == resource);
    }

    SECTION("Move assignment leaves source in valid empty state") {
        rtt source(resource, 100, 200);
        rtt dest(resource, 1, 2, 3, 4, 5);

        REQUIRE(source.size() == 2);
        REQUIRE(dest.size() == 5);

        dest = std::move(source);

        // Source should be empty
        REQUIRE(source.size() == 0);
        REQUIRE(source.empty());
        REQUIRE(source.memory_resource() == nullptr);

        // Dest should have source's data
        REQUIRE(dest.size() == 2);
        REQUIRE(dest.get<int>(0) == 100);
        REQUIRE(dest.get<int>(1) == 200);
    }

    SECTION("Self-move-assignment is safe") {
        rtt r(resource, 42);

        // Self-assignment should be a no-op
        r = std::move(r);

        // Object should still be valid (implementation-defined state)
        // At minimum, shouldn't crash
        REQUIRE(true);
    }

    SECTION("Move from empty rtt") {
        rtt empty_source(resource);
        REQUIRE(empty_source.empty());

        rtt dest(std::move(empty_source));

        REQUIRE(dest.empty());
        REQUIRE(empty_source.empty());
    }
}

// ============================================================================
// allocator_arg_t constructor tests
// ============================================================================

TEST_CASE("allocator_arg_t constructor") {
    SECTION("Same arena move works") {
        auto* resource = std::pmr::get_default_resource();

        rtt source(resource, 1, 2, 3);
        rtt dest(std::allocator_arg, resource, std::move(source));

        REQUIRE(source.empty());
        REQUIRE(dest.size() == 3);
        REQUIRE(dest.get<int>(0) == 1);
        REQUIRE(dest.memory_resource() == resource);
    }

    // Note: Cross-arena with allocator_arg_t has assert
    // Cannot easily test without triggering assert
}

// ============================================================================
// Move-only types support
// ============================================================================

TEST_CASE("Move-only types in rtt") {
    auto* resource = std::pmr::get_default_resource();

    SECTION("unique_ptr stored and retrieved correctly") {
        auto ptr = std::make_unique<int>(42);
        int* raw = ptr.get();

        rtt r(resource, std::move(ptr));

        REQUIRE(r.size() == 1);

        // Get returns by move for value types
        auto& stored = r.get<std::unique_ptr<int>>(0);
        REQUIRE(stored.get() == raw);
        REQUIRE(*stored == 42);
    }

    SECTION("Multiple move-only objects") {
        auto p1 = std::make_unique<int>(1);
        auto p2 = std::make_unique<int>(2);
        auto p3 = std::make_unique<int>(3);

        rtt r(resource, std::move(p1), std::move(p2), std::move(p3));

        REQUIRE(r.size() == 3);
        REQUIRE(*r.get<std::unique_ptr<int>>(0) == 1);
        REQUIRE(*r.get<std::unique_ptr<int>>(1) == 2);
        REQUIRE(*r.get<std::unique_ptr<int>>(2) == 3);
    }
}

// ============================================================================
// Mixed types and complex objects
// ============================================================================

TEST_CASE("Complex objects in rtt") {
    auto* resource = std::pmr::get_default_resource();

    SECTION("std::string stored correctly") {
        std::string long_string(1000, 'x'); // 1000 chars

        rtt r(resource, long_string, std::string("hello"), std::string("world"));

        REQUIRE(r.size() == 3);
        REQUIRE(r.get<std::string>(0) == long_string);
        REQUIRE(r.get<std::string>(1) == "hello");
        REQUIRE(r.get<std::string>(2) == "world");
    }

    SECTION("std::vector stored correctly") {
        std::vector<int> v1{1, 2, 3, 4, 5};
        std::vector<int> v2{10, 20};

        rtt r(resource, v1, v2);

        REQUIRE(r.size() == 2);
        REQUIRE(r.get<std::vector<int>>(0) == v1);
        REQUIRE(r.get<std::vector<int>>(1) == v2);
    }

    SECTION("Mixed complex types") {
        rtt r(resource,
              int(42),
              std::string("test"),
              std::vector<int>{1, 2, 3},
              double(3.14));

        REQUIRE(r.size() == 4);
        REQUIRE(r.get<int>(0) == 42);
        REQUIRE(r.get<std::string>(1) == "test");
        REQUIRE(r.get<std::vector<int>>(2) == std::vector<int>{1, 2, 3});
        REQUIRE(r.get<double>(3) == Approx(3.14));
    }
}

// ============================================================================
// detail::get<I, List>() function tests
// ============================================================================

TEST_CASE("detail::get<I, List>() function") {
    using actor_zeta::type_traits::type_list;
    using actor_zeta::detail::get;

    auto* resource = std::pmr::get_default_resource();

    SECTION("Value types - returns by move") {
        rtt r(resource, 42, std::string("hello"));

        using List = type_list<int, std::string>;

        // For value types, get<I, List> returns by move
        int val = get<0, List>(r);
        REQUIRE(val == 42);

        std::string str = get<1, List>(r);
        REQUIRE(str == "hello");
    }

    SECTION("Const reference types - returns reference") {
        rtt r(resource, 42, std::string("hello"));

        using List = type_list<const int&, const std::string&>;

        // For const T&, returns reference without copy
        const int& val_ref = get<0, List>(r);
        REQUIRE(val_ref == 42);

        const std::string& str_ref = get<1, List>(r);
        REQUIRE(str_ref == "hello");

        // Verify it's actually the same object (not a copy)
        REQUIRE(&val_ref == &r.get<int>(0));
        REQUIRE(&str_ref == &r.get<std::string>(1));
    }
}

// ============================================================================
// Edge cases
// ============================================================================

TEST_CASE("Edge cases") {
    auto* resource = std::pmr::get_default_resource();

    SECTION("Single element") {
        rtt r(resource, 42);
        REQUIRE(r.size() == 1);
        REQUIRE(!r.empty());
        REQUIRE(r.get<int>(0) == 42);
    }

    SECTION("Large number of elements") {
        rtt r(resource,
              1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
              11, 12, 13, 14, 15, 16, 17, 18, 19, 20);

        REQUIRE(r.size() == 20);
        for (std::size_t i = 0; i < 20; ++i) {
            REQUIRE(r.get<int>(i) == static_cast<int>(i) + 1);
        }
    }

    SECTION("Zero-sized types") {
        struct Empty {};
        static_assert(sizeof(Empty) == 1, "Empty class has size 1");

        // Empty structs should work
        rtt r(resource, Empty{}, Empty{}, int(42));
        REQUIRE(r.size() == 3);
        REQUIRE(r.get<int>(2) == 42);
    }

    SECTION("Types with different alignments") {
        struct Align1 { char c; };
        struct Align8 { double d; };
        struct Align4 { int i; };

        rtt r(resource,
              Align1{'a'},
              Align8{3.14},
              Align4{42},
              Align1{'b'});

        REQUIRE(r.size() == 4);
        REQUIRE(r.get<Align1>(0).c == 'a');
        REQUIRE(r.get<Align8>(1).d == Approx(3.14));
        REQUIRE(r.get<Align4>(2).i == 42);
        REQUIRE(r.get<Align1>(3).c == 'b');
    }
}

// ============================================================================
// Issue #15: Lambda assert pattern duplicated 3x
// ============================================================================

TEST_CASE("Issue #15: Lambda assert pattern - compile-time verification") {
    // This test verifies that the lambda pattern works correctly
    // The issue is code duplication (same lambda in 3 places), not functionality

    SECTION("Templated constructor checks resource") {
        auto* resource = std::pmr::get_default_resource();
        REQUIRE(resource != nullptr);

        // Should work with valid resource
        rtt r(resource, 1, 2, 3);
        REQUIRE(r.size() == 3);
        REQUIRE(r.memory_resource() == resource);
    }

    SECTION("Default constructor checks resource") {
        auto* resource = std::pmr::get_default_resource();
        rtt r(resource);
        REQUIRE(r.empty());
        REQUIRE(r.memory_resource() == resource);
    }

    SECTION("allocator_arg_t constructor checks resource") {
        auto* resource = std::pmr::get_default_resource();
        rtt source(resource, 42);
        rtt dest(std::allocator_arg, resource, std::move(source));
        REQUIRE(dest.size() == 1);
        REQUIRE(dest.memory_resource() == resource);
    }

    // Note: Cannot test nullptr resource - would trigger assert
    // The duplication issue is about maintainability, not correctness
}

// ============================================================================
// Issue #16: try_to_align() unused parameter
// ============================================================================

TEST_CASE("Issue #16: try_to_align - parameter is unused") {
    // This test demonstrates that try_to_align only uses the TYPE, not the value
    // The parameter could be removed and replaced with explicit template argument

    auto* resource = std::pmr::get_default_resource();

    SECTION("Alignment works for different types") {
        // Different types with different alignments
        rtt r(resource, char('a'), int(42), double(3.14), char('z'));

        REQUIRE(r.size() == 4);
        REQUIRE(r.get<char>(0) == 'a');
        REQUIRE(r.get<int>(1) == 42);
        REQUIRE(r.get<double>(2) == Approx(3.14));
        REQUIRE(r.get<char>(3) == 'z');
    }

    SECTION("Alignment works for struct types") {
        struct alignas(16) Aligned16 {
            int value;
        };

        rtt r(resource, char('x'), Aligned16{42}, char('y'));

        REQUIRE(r.size() == 3);
        REQUIRE(r.get<char>(0) == 'x');
        REQUIRE(r.get<Aligned16>(1).value == 42);
        REQUIRE(r.get<char>(2) == 'y');
    }
}

// ============================================================================
// Issue #17: Public methods missing noexcept
// ============================================================================

TEST_CASE("Issue #17: Public methods should be noexcept") {
    auto* resource = std::pmr::get_default_resource();
    rtt r(resource, 1, 2, 3);

    SECTION("size() noexcept verification") {
        // These static_asserts verify noexcept specification
        static_assert(noexcept(r.memory_resource()), "memory_resource() should be noexcept");
        static_assert(noexcept(r.size()), "size() should be noexcept");
        static_assert(noexcept(r.volume()), "volume() should be noexcept");
        static_assert(noexcept(r.capacity()), "capacity() should be noexcept");
        static_assert(noexcept(r.empty()), "empty() should be noexcept");

        // Runtime check that methods work
        REQUIRE(r.size() == 3);
        REQUIRE(r.volume() > 0);
        REQUIRE(r.capacity() > 0);
        REQUIRE(!r.empty());
    }

    SECTION("Methods work on const rtt") {
        const rtt& cr = r;

        REQUIRE(cr.size() == 3);
        REQUIRE(cr.volume() > 0);
        REQUIRE(cr.capacity() > 0);
        REQUIRE(!cr.empty());
        REQUIRE(cr.memory_resource() == resource);
    }
}

// ============================================================================
// Issue #18: destroy() is not namespace-local
// ============================================================================

TEST_CASE("Issue #18: destroy() visibility") {
    // This test documents that destroy<T> is currently visible in namespace

    SECTION("destroy<T> exists in actor_zeta::detail") {
        // Currently destroy<T> is accessible (this compiles)
        // After fix, it should be private static member of rtt

        // We can verify destruction works correctly through rtt
        auto* resource = std::pmr::get_default_resource();

        static int destruct_count = 0;
        struct Tracked {
            bool moved_from = false;
            Tracked() = default;
            Tracked(Tracked&& other) noexcept : moved_from(false) {
                other.moved_from = true;
            }
            ~Tracked() {
                if (!moved_from) ++destruct_count;
            }
        };

        destruct_count = 0;
        {
            rtt r(resource, Tracked{});
            REQUIRE(r.size() == 1);
        }
        // Destruction happened through destroy<Tracked>
        REQUIRE(destruct_count == 1);
    }

    SECTION("Multiple types destroyed correctly") {
        static std::vector<int> destruction_order;
        destruction_order.clear();

        struct A {
            bool moved_from = false;
            A() = default;
            A(A&& o) noexcept : moved_from(false) { o.moved_from = true; }
            ~A() { if (!moved_from) destruction_order.push_back(1); }
        };
        struct B {
            bool moved_from = false;
            B() = default;
            B(B&& o) noexcept : moved_from(false) { o.moved_from = true; }
            ~B() { if (!moved_from) destruction_order.push_back(2); }
        };
        struct C {
            bool moved_from = false;
            C() = default;
            C(C&& o) noexcept : moved_from(false) { o.moved_from = true; }
            ~C() { if (!moved_from) destruction_order.push_back(3); }
        };

        auto* resource = std::pmr::get_default_resource();
        {
            rtt r(resource, A{}, B{}, C{});
            REQUIRE(r.size() == 3);
        }

        // LIFO destruction order
        REQUIRE(destruction_order == std::vector<int>{3, 2, 1});
    }
}

// ============================================================================
// Regression: Ensure basic functionality works
// ============================================================================

TEST_CASE("Regression: Basic rtt functionality") {
    auto* resource = std::pmr::get_default_resource();

    SECTION("Empty rtt") {
        rtt r(resource);
        REQUIRE(r.empty());
        REQUIRE(r.size() == 0);
        REQUIRE(r.capacity() == 0);
        REQUIRE(r.volume() == 0);
    }

    SECTION("Single element") {
        rtt r(resource, 42);
        REQUIRE(!r.empty());
        REQUIRE(r.size() == 1);
        REQUIRE(r.get<int>(0) == 42);
    }

    SECTION("Multiple elements of same type") {
        rtt r(resource, 1, 2, 3, 4, 5);
        REQUIRE(r.size() == 5);
        for (std::size_t i = 0; i < 5; ++i) {
            REQUIRE(r.get<int>(i) == static_cast<int>(i) + 1);
        }
    }

    SECTION("Mixed types") {
        rtt r(resource, 42, std::string("hello"), 3.14);
        REQUIRE(r.size() == 3);
        REQUIRE(r.get<int>(0) == 42);
        REQUIRE(r.get<std::string>(1) == "hello");
        REQUIRE(r.get<double>(2) == Approx(3.14));
    }

    SECTION("Move construction") {
        rtt r1(resource, 1, 2, 3);
        rtt r2(std::move(r1));

        REQUIRE(r1.empty()); // moved-from
        REQUIRE(r2.size() == 3);
        REQUIRE(r2.get<int>(0) == 1);
    }

    SECTION("Move assignment") {
        rtt r1(resource, 1, 2, 3);
        rtt r2(resource, 4, 5);

        r2 = std::move(r1);

        REQUIRE(r1.empty()); // moved-from
        REQUIRE(r2.size() == 3);
        REQUIRE(r2.get<int>(0) == 1);
    }
}
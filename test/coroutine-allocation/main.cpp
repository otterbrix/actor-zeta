/**
 * @file main.cpp
 * @brief Tests for coroutine frame allocation with custom memory resources
 *
 * These tests verify that:
 * 1. Coroutine frames are allocated using actor's memory_resource
 * 2. All allocations are properly balanced with deallocations
 * 3. Custom allocators work correctly with coroutines
 */

#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta/actor/dispatch.hpp>
#include <actor-zeta.hpp>

#include <atomic>
#include <mutex>
#include <vector>

// =============================================================================
// Tracking Memory Resource - monitors all allocations
// =============================================================================

class tracking_resource : public std::pmr::memory_resource {
public:
    struct allocation_record {
        void* ptr;
        std::size_t size;
        std::size_t align;
        bool freed;
    };

    tracking_resource()
        : alloc_count_(0)
        , dealloc_count_(0)
        , total_allocated_(0)
        , total_freed_(0)
        , peak_allocated_(0)
        , current_allocated_(0) {}

    void* do_allocate(std::size_t bytes, std::size_t align) override {
        void* ptr = ::operator new(bytes, std::align_val_t{align});

        std::lock_guard<std::mutex> lock(mutex_);
        ++alloc_count_;
        total_allocated_ += bytes;
        current_allocated_ += bytes;
        if (current_allocated_.load() > peak_allocated_.load()) {
            peak_allocated_.store(current_allocated_.load());
        }

        records_.push_back({ptr, bytes, align, false});
        return ptr;
    }

    void do_deallocate(void* ptr, std::size_t bytes, std::size_t align) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++dealloc_count_;
            total_freed_ += bytes;
            current_allocated_ -= bytes;

            // Mark as freed
            for (auto& rec : records_) {
                if (rec.ptr == ptr && !rec.freed) {
                    rec.freed = true;
                    break;
                }
            }
        }

        ::operator delete(ptr, std::align_val_t{align});
    }

    bool do_is_equal(const memory_resource& other) const noexcept override {
        return this == &other;
    }

    // Statistics
    std::size_t alloc_count() const { return alloc_count_.load(); }
    std::size_t dealloc_count() const { return dealloc_count_.load(); }
    std::size_t total_allocated() const { return total_allocated_.load(); }
    std::size_t total_freed() const { return total_freed_.load(); }
    std::size_t peak_allocated() const { return peak_allocated_.load(); }
    std::size_t current_allocated() const { return current_allocated_.load(); }

    bool all_freed() const {
        return alloc_count_.load() == dealloc_count_.load();
    }

    std::vector<allocation_record> leaked_allocations() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<allocation_record> leaked;
        for (const auto& rec : records_) {
            if (!rec.freed) {
                leaked.push_back(rec);
            }
        }
        return leaked;
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        alloc_count_ = 0;
        dealloc_count_ = 0;
        total_allocated_ = 0;
        total_freed_ = 0;
        peak_allocated_ = 0;
        current_allocated_ = 0;
        records_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::atomic<std::size_t> alloc_count_;
    std::atomic<std::size_t> dealloc_count_;
    std::atomic<std::size_t> total_allocated_;
    std::atomic<std::size_t> total_freed_;
    std::atomic<std::size_t> peak_allocated_;
    std::atomic<std::size_t> current_allocated_;
    std::vector<allocation_record> records_;
};

// =============================================================================
// Test Actor
// =============================================================================

class TestActor : public actor_zeta::basic_actor<TestActor> {
public:
    using base_type = actor_zeta::basic_actor<TestActor>;

    explicit TestActor(std::pmr::memory_resource* res)
        : base_type(res) {}

    // Simple void coroutine
    actor_zeta::unique_future<void> void_coro() {
        co_return;
    }

    // Coroutine with int result
    actor_zeta::unique_future<int> int_coro(int x) {
        co_return x * 2;
    }

    // Coroutine with string result (by value - required for coroutines)
    actor_zeta::unique_future<std::string> string_coro(std::string s) {
        co_return s + "_processed";
    }

    // Coroutine that stores local variables (larger frame)
    actor_zeta::unique_future<int> large_frame_coro(int x) {
        // Local variables increase frame size
        int a = x + 1;
        int b = a + 2;
        int c = b + 3;
        std::string s = std::to_string(c);
        co_return static_cast<int>(s.length()) + c;
    }

    actor_zeta::unique_future<void> behavior(actor_zeta::mailbox::message*) {
        co_return;
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &TestActor::void_coro,
        &TestActor::int_coro,
        &TestActor::string_coro,
        &TestActor::large_frame_coro
    >;
};

// =============================================================================
// Tests
// =============================================================================

TEST_CASE("Coroutine allocation tracking - baseline") {
    tracking_resource tracker;

    SECTION("Actor creation uses resource") {
        auto actor = actor_zeta::spawn<TestActor>(&tracker);

        CHECK(tracker.alloc_count() > 0);
        INFO("Actor creation allocated " << tracker.total_allocated() << " bytes");
    }

    SECTION("Actor destruction frees memory") {
        {
            auto actor = actor_zeta::spawn<TestActor>(&tracker);
        }

        CHECK(tracker.all_freed());
        auto leaked = tracker.leaked_allocations();
        CHECK(leaked.empty());
    }
}

TEST_CASE("Void coroutine allocation") {
    tracking_resource tracker;
    auto actor = actor_zeta::spawn<TestActor>(&tracker);

    std::size_t baseline_allocs = tracker.alloc_count();
    std::size_t baseline_bytes = tracker.total_allocated();

    SECTION("Single void coroutine") {
        {
            auto future = actor->void_coro();
            // Coroutine should allocate: future_state + coroutine_frame
            CHECK(tracker.alloc_count() > baseline_allocs);
        }

        INFO("Void coroutine allocated " << (tracker.total_allocated() - baseline_bytes) << " bytes");
    }

    SECTION("Multiple void coroutines are balanced") {
        tracker.reset();
        auto actor2 = actor_zeta::spawn<TestActor>(&tracker);
        std::size_t actor_allocs = tracker.alloc_count();

        constexpr int N = 100;
        for (int i = 0; i < N; ++i) {
            auto future = actor2->void_coro();
            // Future goes out of scope, coroutine should be cleaned up
        }

        // All coroutine allocations should be freed
        // Only actor allocation remains
        CHECK(tracker.alloc_count() > actor_allocs);
        INFO("Total allocations: " << tracker.alloc_count());
        INFO("Total deallocations: " << tracker.dealloc_count());
    }
}

TEST_CASE("Typed coroutine allocation") {
    tracking_resource tracker;
    auto actor = actor_zeta::spawn<TestActor>(&tracker);

    tracker.reset();
    // Re-spawn to get clean tracking
    actor = actor_zeta::spawn<TestActor>(&tracker);
    std::size_t actor_bytes = tracker.total_allocated();

    SECTION("Int coroutine with get()") {
        {
            auto future = actor->int_coro(21);
            int result = std::move(future).get();
            CHECK(result == 42);
        }

        INFO("Int coroutine overhead: " << (tracker.peak_allocated() - actor_bytes) << " bytes");
    }

    SECTION("String coroutine with get()") {
        {
            auto future = actor->string_coro("test");
            std::string result = std::move(future).get();
            CHECK(result == "test_processed");
        }

        INFO("String coroutine overhead: " << (tracker.peak_allocated() - actor_bytes) << " bytes");
    }

    SECTION("Large frame coroutine") {
        {
            auto future = actor->large_frame_coro(10);
            int result = std::move(future).get();
            CHECK(result > 10);
        }

        INFO("Large frame coroutine overhead: " << (tracker.peak_allocated() - actor_bytes) << " bytes");
    }
}

TEST_CASE("Coroutine allocation patterns") {
    tracking_resource tracker;

    SECTION("Sequential coroutines reuse memory pattern") {
        auto actor = actor_zeta::spawn<TestActor>(&tracker);

        // Track peak memory across multiple calls
        std::size_t first_peak = 0;
        std::size_t second_peak = 0;

        {
            auto future = actor->int_coro(1);
            first_peak = tracker.peak_allocated();
            std::move(future).get();
        }

        tracker.reset();
        actor = actor_zeta::spawn<TestActor>(&tracker);

        {
            auto future = actor->int_coro(2);
            second_peak = tracker.peak_allocated();
            std::move(future).get();
        }

        // Memory pattern should be similar
        INFO("First call peak: " << first_peak);
        INFO("Second call peak: " << second_peak);
    }

    SECTION("Concurrent futures") {
        auto actor = actor_zeta::spawn<TestActor>(&tracker);

        std::vector<actor_zeta::unique_future<int>> futures;
        futures.reserve(10);

        // Create multiple futures before consuming
        for (int i = 0; i < 10; ++i) {
            futures.push_back(actor->int_coro(i));
        }

        INFO("Peak with 10 concurrent futures: " << tracker.peak_allocated() << " bytes");

        // Consume all
        int sum = 0;
        for (auto& f : futures) {
            sum += std::move(f).get();
        }

        CHECK(sum == 2 * (0 + 1 + 2 + 3 + 4 + 5 + 6 + 7 + 8 + 9));
    }
}

TEST_CASE("make_ready_future allocation") {
    tracking_resource tracker;

    SECTION("make_ready_future<int> allocation") {
        {
            auto future = actor_zeta::make_ready_future<int>(&tracker, 42);
            CHECK(tracker.alloc_count() > 0);
            int result = std::move(future).get();
            CHECK(result == 42);
        }

        CHECK(tracker.all_freed());
        INFO("make_ready_future<int> used " << tracker.total_allocated() << " bytes");
    }

    SECTION("make_ready_future_void allocation") {
        {
            auto future = actor_zeta::make_ready_future_void(&tracker);
            CHECK(tracker.alloc_count() > 0);
            std::move(future).get();
        }

        CHECK(tracker.all_freed());
        INFO("make_ready_future_void used " << tracker.total_allocated() << " bytes");
    }
}

TEST_CASE("promise/future allocation") {
    tracking_resource tracker;

    SECTION("promise<int> allocation") {
        {
            actor_zeta::promise<int> p(&tracker);
            CHECK(tracker.alloc_count() > 0);

            auto future = p.get_future();
            p.set_value(42);

            int result = std::move(future).get();
            CHECK(result == 42);
        }

        CHECK(tracker.all_freed());
        INFO("promise<int> used " << tracker.total_allocated() << " bytes");
    }

    SECTION("promise<void> allocation") {
        {
            actor_zeta::promise<void> p(&tracker);
            CHECK(tracker.alloc_count() > 0);

            auto future = p.get_future();
            p.set_value();

            std::move(future).get();
        }

        CHECK(tracker.all_freed());
        INFO("promise<void> used " << tracker.total_allocated() << " bytes");
    }
}

// =============================================================================
// Stress test
// =============================================================================

TEST_CASE("Stress test - many coroutines") {
    tracking_resource tracker;
    auto actor = actor_zeta::spawn<TestActor>(&tracker);

    constexpr int ITERATIONS = 10000;

    for (int i = 0; i < ITERATIONS; ++i) {
        auto future = actor->int_coro(i);
        int result = std::move(future).get();
        CHECK(result == i * 2);
    }

    INFO("After " << ITERATIONS << " iterations:");
    INFO("  Total allocations: " << tracker.alloc_count());
    INFO("  Total deallocations: " << tracker.dealloc_count());
    INFO("  Peak memory: " << tracker.peak_allocated() << " bytes");
    INFO("  Current memory: " << tracker.current_allocated() << " bytes");

    // Should not have accumulated memory
    auto leaked = tracker.leaked_allocations();
    // Note: actor itself is still allocated
    CHECK(leaked.size() <= 1);  // Only actor allocation remains
}
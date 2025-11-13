#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta/detail/future_state.hpp>
#include <actor-zeta/detail/memory_resource.hpp>
#include <thread>
#include <vector>
#include <atomic>

using namespace actor_zeta::detail;
using namespace actor_zeta;

TEST_CASE("future_state<int> - basic construction and destruction") {
    auto* res = pmr::get_default_resource();

    SECTION("construct with initial refcount = 2") {
        // Allocate slot manually
        void* mem = res->allocate(sizeof(future_state<int>), alignof(future_state<int>));
        auto* slot = new (mem) future_state<int>(res);

        REQUIRE(!slot->is_ready());

        // Cleanup: manually release both references
        slot->release();  // First release (2 -> 1)
        slot->release();  // Second release (1 -> 0, deletes)

        // If we reach here, no crash - test passed
    }

    SECTION("set_result makes slot ready") {
        void* mem = res->allocate(sizeof(future_state<int>), alignof(future_state<int>));
        auto* slot = new (mem) future_state<int>(res);

        REQUIRE(!slot->is_ready());

        // Set result
        rtt value(res, 42);
        slot->set_result(std::move(value));

        REQUIRE(slot->is_ready());
        REQUIRE(slot->result().get<int>(0) == 42);

        // Cleanup
        slot->release();
        slot->release();
    }
}

TEST_CASE("future_state<int> - refcount increment/decrement") {
    auto* res = pmr::get_default_resource();

    SECTION("add_ref increases refcount") {
        void* mem = res->allocate(sizeof(future_state<int>), alignof(future_state<int>));
        auto* slot = new (mem) future_state<int>(res);

        // Initial refcount = 2
        // Add 3 more references
        slot->add_ref();  // 3
        slot->add_ref();  // 4
        slot->add_ref();  // 5

        // Release all 5 references
        slot->release();  // 4
        slot->release();  // 3
        slot->release();  // 2
        slot->release();  // 1
        slot->release();  // 0 - deletes
    }

    SECTION("last owner deletes slot") {
        void* mem = res->allocate(sizeof(future_state<int>), alignof(future_state<int>));
        auto* slot = new (mem) future_state<int>(res);

        // Only one release should NOT delete (refcount 2 -> 1)
        slot->release();

        // Slot still accessible
        REQUIRE(!slot->is_ready());

        // Second release should delete (refcount 1 -> 0)
        slot->release();

        // After this point, slot is deleted - no access
    }
}

TEST_CASE("future_state<int> - result storage and retrieval") {
    auto* res = pmr::get_default_resource();

    SECTION("store and retrieve int") {
        void* mem = res->allocate(sizeof(future_state<int>), alignof(future_state<int>));
        auto* slot = new (mem) future_state<int>(res);

        rtt value(res, 123);
        slot->set_result(std::move(value));

        REQUIRE(slot->is_ready());
        REQUIRE(slot->result().get<int>(0) == 123);

        slot->release();
        slot->release();
    }

    SECTION("store and retrieve string") {
        void* mem = res->allocate(sizeof(future_state<int>), alignof(future_state<int>));
        auto* slot = new (mem) future_state<int>(res);

        rtt value(res, std::string("hello world"));
        slot->set_result(std::move(value));

        REQUIRE(slot->is_ready());
        REQUIRE(slot->result().get<std::string>(0) == "hello world");

        slot->release();
        slot->release();
    }

    SECTION("ready flag atomic visibility") {
        void* mem = res->allocate(sizeof(future_state<int>), alignof(future_state<int>));
        auto* slot = new (mem) future_state<int>(res);

        REQUIRE(!slot->is_ready());

        rtt value(res, 999);
        slot->set_result(std::move(value));

        REQUIRE(slot->is_ready());

        slot->release();
        slot->release();
    }
}

TEST_CASE("future_state<int> - thread safety") {
    auto* res = pmr::get_default_resource();

    SECTION("concurrent add_ref from multiple threads") {
        void* mem = res->allocate(sizeof(future_state<int>), alignof(future_state<int>));
        auto* slot = new (mem) future_state<int>(res);

        constexpr int num_threads = 10;
        constexpr int refs_per_thread = 100;

        std::vector<std::thread> threads;

        // Spawn threads that add references
        for (int i = 0; i < num_threads; ++i) {
            threads.emplace_back([slot]() {
                for (int j = 0; j < refs_per_thread; ++j) {
                    slot->add_ref();
                }
            });
        }

        // Wait for all threads
        for (auto& t : threads) {
            t.join();
        }

        // Total refcount should be: 2 (initial) + 10 * 100 = 1002
        // Release all references
        for (int i = 0; i < num_threads * refs_per_thread + 2; ++i) {
            slot->release();
        }
    }

    SECTION("concurrent release from multiple threads") {
        constexpr int num_threads = 10;
        constexpr int releases_per_thread = 100;
        constexpr int total_refs = 2 + num_threads * releases_per_thread;

        void* mem = res->allocate(sizeof(future_state<int>), alignof(future_state<int>));
        auto* slot = new (mem) future_state<int>(res);

        // Add references first
        for (int i = 0; i < num_threads * releases_per_thread; ++i) {
            slot->add_ref();
        }

        std::vector<std::thread> threads;

        // Spawn threads that release references
        for (int i = 0; i < num_threads; ++i) {
            threads.emplace_back([slot]() {
                for (int j = 0; j < releases_per_thread; ++j) {
                    slot->release();
                }
            });
        }

        // Wait for all threads
        for (auto& t : threads) {
            t.join();
        }

        // Release remaining 2 initial references
        slot->release();
        slot->release();
    }

    SECTION("concurrent add_ref and release") {
        void* mem = res->allocate(sizeof(future_state<int>), alignof(future_state<int>));
        auto* slot = new (mem) future_state<int>(res);

        constexpr int num_threads = 8;
        constexpr int operations = 1000;

        std::atomic<bool> start{false};
        std::vector<std::thread> threads;

        // Add many references first to prevent premature deletion
        for (int i = 0; i < num_threads * operations; ++i) {
            slot->add_ref();
        }

        // Half threads add_ref, half release
        for (int i = 0; i < num_threads; ++i) {
            if (i % 2 == 0) {
                threads.emplace_back([slot, &start]() {
                    while (!start.load(std::memory_order_acquire)) {}
                    for (int j = 0; j < operations; ++j) {
                        slot->add_ref();
                    }
                });
            } else {
                threads.emplace_back([slot, &start]() {
                    while (!start.load(std::memory_order_acquire)) {}
                    for (int j = 0; j < operations; ++j) {
                        slot->release();
                    }
                });
            }
        }

        // Start all threads simultaneously
        start.store(true, std::memory_order_release);

        for (auto& t : threads) {
            t.join();
        }

        // Net effect: +4000 add_ref, -4000 release = 0 change
        // Still have initial refs (2) + pre-added refs (8000) = 8002
        for (int i = 0; i < num_threads * operations + 2; ++i) {
            slot->release();
        }
    }

    SECTION("concurrent set_result and is_ready") {
        void* mem = res->allocate(sizeof(future_state<int>), alignof(future_state<int>));
        auto* slot = new (mem) future_state<int>(res);

        std::atomic<bool> result_set{false};

        // Thread 1: sets result
        std::thread writer([slot, &result_set]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            rtt value(pmr::get_default_resource(), 777);
            // Set external flag BEFORE set_result()
            // This tests transitive happens-before: result_set → set_result() → is_ready()
            result_set.store(true, std::memory_order_release);
            slot->set_result(std::move(value));
        });

        // Thread 2: polls is_ready
        std::thread reader([slot, &result_set]() {
            while (!slot->is_ready()) {
                std::this_thread::yield();
            }

            REQUIRE(result_set.load(std::memory_order_acquire));
            REQUIRE(slot->result().get<int>(0) == 777);
        });

        writer.join();
        reader.join();

        slot->release();
        slot->release();
    }
}

TEST_CASE("future_state<int> - memory ordering guarantees") {
    auto* res = pmr::get_default_resource();

    SECTION("release-acquire semantics for ready flag") {
        void* mem = res->allocate(sizeof(future_state<int>), alignof(future_state<int>));
        auto* slot = new (mem) future_state<int>(res);

        std::atomic<int> side_effect{0};

        std::thread writer([slot, &side_effect]() {
            side_effect.store(42, std::memory_order_relaxed);
            rtt value(pmr::get_default_resource(), 100);
            slot->set_result(std::move(value));  // Release
        });

        std::thread reader([slot, &side_effect]() {
            while (!slot->is_ready()) {  // Acquire
                std::this_thread::yield();
            }

            // Due to release-acquire, we should see side_effect = 42
            REQUIRE(side_effect.load(std::memory_order_relaxed) == 42);
        });

        writer.join();
        reader.join();

        slot->release();
        slot->release();
    }
}
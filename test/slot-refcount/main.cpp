#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta/detail/future_state.hpp>
#include <memory_resource>
#include <thread>
#include <vector>
#include <atomic>

using namespace actor_zeta::detail;
using namespace actor_zeta;

TEST_CASE("future_state<int> - basic construction and destruction") {
    auto* res = std::pmr::get_default_resource();

    SECTION("construct with initial refcount = 1") {
        // Allocate slot manually
        void* mem = res->allocate(sizeof(future_state<int>), alignof(future_state<int>));
        auto* slot = new (mem) future_state<int>(res);

        REQUIRE(!slot->is_ready());

        // Cleanup: release single initial reference
        slot->release();  // Release (1 -> 0, deletes)

        // If we reach here, no crash - test passed
    }

    SECTION("set_value makes slot ready") {
        void* mem = res->allocate(sizeof(future_state<int>), alignof(future_state<int>));
        auto* slot = new (mem) future_state<int>(res);

        REQUIRE(!slot->is_ready());

        // Set result using new typed API
        slot->set_value(42);

        REQUIRE(slot->is_ready());
        REQUIRE(slot->get_value() == 42);

        // Cleanup
        slot->release();
    }
}

TEST_CASE("future_state<int> - refcount increment/decrement") {
    auto* res = std::pmr::get_default_resource();

    SECTION("add_ref increases refcount") {
        void* mem = res->allocate(sizeof(future_state<int>), alignof(future_state<int>));
        auto* slot = new (mem) future_state<int>(res);

        // Initial refcount = 1
        // Add 3 more references
        slot->add_ref();  // 2
        slot->add_ref();  // 3
        slot->add_ref();  // 4

        // Release all 4 references
        slot->release();  // 3
        slot->release();  // 2
        slot->release();  // 1
        slot->release();  // 0 - deletes
    }

    SECTION("last owner deletes slot") {
        void* mem = res->allocate(sizeof(future_state<int>), alignof(future_state<int>));
        auto* slot = new (mem) future_state<int>(res);

        // Refcount = 1, single release should delete
        slot->release();  // 0 - deletes

        // After this point, slot is deleted - no access
    }
}

TEST_CASE("future_state<int> - result storage and retrieval") {
    auto* res = std::pmr::get_default_resource();

    SECTION("store and retrieve int") {
        void* mem = res->allocate(sizeof(future_state<int>), alignof(future_state<int>));
        auto* slot = new (mem) future_state<int>(res);

        slot->set_value(123);

        REQUIRE(slot->is_ready());
        REQUIRE(slot->get_value() == 123);

        slot->release();
    }

    SECTION("store and retrieve different int") {
        void* mem = res->allocate(sizeof(future_state<int>), alignof(future_state<int>));
        auto* slot = new (mem) future_state<int>(res);

        slot->set_value(999);

        REQUIRE(slot->is_ready());
        REQUIRE(slot->get_value() == 999);

        slot->release();
    }

    SECTION("ready flag atomic visibility") {
        void* mem = res->allocate(sizeof(future_state<int>), alignof(future_state<int>));
        auto* slot = new (mem) future_state<int>(res);

        REQUIRE(!slot->is_ready());

        slot->set_value(999);

        REQUIRE(slot->is_ready());

        slot->release();
    }
}

TEST_CASE("future_state<std::string> - string storage") {
    auto* res = std::pmr::get_default_resource();

    SECTION("store and retrieve string") {
        void* mem = res->allocate(sizeof(future_state<std::string>), alignof(future_state<std::string>));
        auto* slot = new (mem) future_state<std::string>(res);

        slot->set_value(std::string("hello world"));

        REQUIRE(slot->is_ready());
        REQUIRE(slot->get_value() == "hello world");

        slot->release();
    }
}

TEST_CASE("future_state<int> - thread safety") {
    auto* res = std::pmr::get_default_resource();

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

        // Total refcount should be: 1 (initial) + 10 * 100 = 1001
        // Release all references
        for (int i = 0; i < num_threads * refs_per_thread + 1; ++i) {
            slot->release();
        }
    }

    SECTION("concurrent release from multiple threads") {
        constexpr int num_threads = 10;
        constexpr int releases_per_thread = 100;
        constexpr int total_refs = 1 + num_threads * releases_per_thread;

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

        // Release remaining 1 initial reference
        slot->release();
        actor_zeta::detail::ignore_unused(total_refs);
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
        // Still have initial refs (1) + pre-added refs (8000) = 8001
        for (int i = 0; i < num_threads * operations + 1; ++i) {
            slot->release();
        }
    }

    SECTION("concurrent set_value and is_ready") {
        void* mem = res->allocate(sizeof(future_state<int>), alignof(future_state<int>));
        auto* slot = new (mem) future_state<int>(res);

        // Thread 1: sets result
        std::thread writer([slot]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            slot->set_value(777);
        });

        // Thread 2: polls is_ready and reads result
        std::thread reader([slot]() {
            // Wait for result to be ready
            while (!slot->is_ready()) {
                std::this_thread::yield();
            }

            // If is_ready() returned true, result must be visible (release-acquire)
            REQUIRE(slot->get_value() == 777);
        });

        writer.join();
        reader.join();

        slot->release();
    }
}

TEST_CASE("future_state<int> - memory ordering guarantees") {
    auto* res = std::pmr::get_default_resource();

    SECTION("release-acquire semantics for ready flag") {
        void* mem = res->allocate(sizeof(future_state<int>), alignof(future_state<int>));
        auto* slot = new (mem) future_state<int>(res);

        std::atomic<int> side_effect{0};

        std::thread writer([slot, &side_effect]() {
            side_effect.store(42, std::memory_order_relaxed);
            slot->set_value(100);  // Release
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
    }
}
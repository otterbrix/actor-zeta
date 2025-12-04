/// @file main.cpp
/// @brief Dispatcher pattern tests with coroutines
///
/// This file contains tests demonstrating various coroutine patterns:
/// 1. Single co_await - simple request-response
/// 2. Sequential co_await - transaction with multiple steps
/// 3. Parallel requests + await all - aggregation
/// 4. Nested coroutines - depth
/// 5. Lambda INSIDE actor method - lambdas defined in class methods
///
/// File structure:
/// - common_types.hpp - Domain types
/// - test_logger.hpp - Thread-safe logging
/// - memory_storage.hpp - Storage actor (bottom level)
/// - manager_dispatcher.hpp - Dispatcher actor (middle level)
/// - client.hpp - Client actor (top level)

#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include "client.hpp"  // Includes all other headers

#include <thread>
#include <atomic>

using namespace actor_zeta;
using namespace dispatcher_test;

// ============================================================================
// Basic flow tests
// ============================================================================

TEST_CASE("dispatcher-pattern: single-thread basic flow") {
    auto* resource = pmr::get_default_resource();

    g_log.log("\n========== TEST: dispatcher-pattern: single-thread basic flow ==========");

    // Create actor chain: client -> dispatcher -> storage
    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");
    auto client = spawn<client_t>(resource, dispatcher->address(), "Client");

    session_id_t session("session-001");

    // Send request
    auto future = send(
        client.get(),
        address_t::empty_address(),
        &client_t::request_collection_size,
        session,
        std::string("test_db"),
        std::string("users"));

    // Manual execution (single-threaded):
    // poll_pending() is called inside behavior() automatically

    // 1. Client behavior -> starts coroutine, suspends on co_await send(dispatcher)
    client->resume(1);

    // 2. Dispatcher behavior -> starts coroutine, suspends on co_await send(storage)
    dispatcher->resume(1);

    // 3. Storage behavior -> executes size(), returns ready future
    storage->resume(1);

    // 4. Send poll to dispatcher to trigger behavior() and poll_pending()
    send(dispatcher.get(), address_t::empty_address(), &manager_dispatcher_t::poll);
    dispatcher->resume(1);

    // 5. Send poll to client to trigger behavior() and poll_pending()
    send(client.get(), address_t::empty_address(), &client_t::poll);
    client->resume(1);

    // Check result
    REQUIRE(future.available());
    auto result = std::move(future).get();

    REQUIRE(!result.has_error);
    REQUIRE(result.size == 100);  // test_db.users has 100 items

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("dispatcher-pattern: error handling") {
    auto* resource = pmr::get_default_resource();

    g_log.log("\n========== TEST: dispatcher-pattern: error handling ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");
    auto client = spawn<client_t>(resource, dispatcher->address(), "Client");

    session_id_t session("session-002");

    // Send request with empty database name (should trigger error)
    auto future = send(
        client.get(),
        address_t::empty_address(),
        &client_t::request_collection_size,
        session,
        std::string(""),  // Empty - triggers error
        std::string("users"));

    // Execute
    client->resume(1);

    // Dispatcher returns error immediately (co_return before co_await)
    dispatcher->resume(1);

    // Send poll to client to trigger poll_pending()
    send(client.get(), address_t::empty_address(), &client_t::poll);
    client->resume(1);

    // Check result
    REQUIRE(future.available());
    auto result = std::move(future).get();

    REQUIRE(result.has_error);
    REQUIRE(result.error_message == "Empty database or collection name");

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("dispatcher-pattern: multiple requests") {
    auto* resource = pmr::get_default_resource();

    g_log.log("\n========== TEST: dispatcher-pattern: multiple requests ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");
    auto client = spawn<client_t>(resource, dispatcher->address(), "Client");

    // Request 1: users
    auto future1 = send(
        client.get(),
        address_t::empty_address(),
        &client_t::request_collection_size,
        session_id_t("session-003"),
        std::string("test_db"),
        std::string("users"));

    client->resume(1);
    dispatcher->resume(1);
    storage->resume(1);
    send(dispatcher.get(), address_t::empty_address(), &manager_dispatcher_t::poll);
    dispatcher->resume(1);
    send(client.get(), address_t::empty_address(), &client_t::poll);
    client->resume(1);

    REQUIRE(future1.available());
    auto result1 = std::move(future1).get();
    REQUIRE(result1.size == 100);

    // Request 2: orders
    auto future2 = send(
        client.get(),
        address_t::empty_address(),
        &client_t::request_collection_size,
        session_id_t("session-004"),
        std::string("test_db"),
        std::string("orders"));

    client->resume(1);
    dispatcher->resume(1);
    storage->resume(1);
    send(dispatcher.get(), address_t::empty_address(), &manager_dispatcher_t::poll);
    dispatcher->resume(1);
    send(client.get(), address_t::empty_address(), &client_t::poll);
    client->resume(1);

    REQUIRE(future2.available());
    auto result2 = std::move(future2).get();
    REQUIRE(result2.size == 250);

    // Request 3: products
    auto future3 = send(
        client.get(),
        address_t::empty_address(),
        &client_t::request_collection_size,
        session_id_t("session-005"),
        std::string("test_db"),
        std::string("products"));

    client->resume(1);
    dispatcher->resume(1);
    storage->resume(1);
    send(dispatcher.get(), address_t::empty_address(), &manager_dispatcher_t::poll);
    dispatcher->resume(1);
    send(client.get(), address_t::empty_address(), &client_t::poll);
    client->resume(1);

    REQUIRE(future3.available());
    auto result3 = std::move(future3).get();
    REQUIRE(result3.size == 50);

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("dispatcher-pattern: non-existent collection") {
    auto* resource = pmr::get_default_resource();

    g_log.log("\n========== TEST: dispatcher-pattern: non-existent collection ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");
    auto client = spawn<client_t>(resource, dispatcher->address(), "Client");

    session_id_t session("session-006");

    // Request non-existent collection
    auto future = send(
        client.get(),
        address_t::empty_address(),
        &client_t::request_collection_size,
        session,
        std::string("test_db"),
        std::string("nonexistent"));

    client->resume(1);
    dispatcher->resume(1);
    storage->resume(1);
    send(dispatcher.get(), address_t::empty_address(), &manager_dispatcher_t::poll);
    dispatcher->resume(1);
    send(client.get(), address_t::empty_address(), &client_t::poll);
    client->resume(1);

    REQUIRE(future.available());
    auto result = std::move(future).get();

    // Non-existent collection returns 0 (not an error)
    REQUIRE(!result.has_error);
    REQUIRE(result.size == 0);

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("dispatcher-pattern: multi-thread execution") {
    auto* resource = pmr::get_default_resource();

    g_log.log("\n========== TEST: dispatcher-pattern: multi-thread execution ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");
    auto client = spawn<client_t>(resource, dispatcher->address(), "Client");

    std::atomic<bool> done{false};
    std::atomic<std::size_t> result_size{0};
    std::atomic<bool> result_has_error{true};
    std::atomic<bool> future_available{false};

    std::thread worker_thread([&]() {
        auto tid = thread_id_str();
        g_log.log("[WORKER_THREAD] Started, thread=%", tid);

        session_id_t session("session-007");

        auto future = send(
            client.get(),
            address_t::empty_address(),
            &client_t::request_collection_size,
            session,
            std::string("test_db"),
            std::string("orders"));

        // Execute entire chain in this thread
        client->resume(1);
        dispatcher->resume(1);
        storage->resume(1);
        send(dispatcher.get(), address_t::empty_address(), &manager_dispatcher_t::poll);
        dispatcher->resume(1);
        send(client.get(), address_t::empty_address(), &client_t::poll);
        client->resume(1);

        future_available = future.available();
        if (future_available) {
            auto result = std::move(future).get();
            result_size = result.size;
            result_has_error = result.has_error;
        }

        done = true;
        g_log.log("[WORKER_THREAD] Done, result_size=%", result_size.load());
    });

    worker_thread.join();

    REQUIRE(done);
    REQUIRE(future_available);
    REQUIRE(!result_has_error);
    REQUIRE(result_size == 250);

    g_log.log("========== TEST PASSED ==========");
}

// ============================================================================
// Execute plan tests
// ============================================================================

TEST_CASE("dispatcher-pattern: execute_plan with cursor") {
    auto* resource = pmr::get_default_resource();

    g_log.log("\n========== TEST: dispatcher-pattern: execute_plan with cursor ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    session_id_t session("session-exec-001");

    logical_plan_t plan("select",
                        collection_full_name_t("test_db", "users"),
                        "id > 0");

    auto future = send(
        dispatcher.get(),
        address_t::empty_address(),
        &manager_dispatcher_t::execute_plan,
        session,
        std::move(plan));

    dispatcher->resume(1);
    storage->resume(1);
    send(dispatcher.get(), address_t::empty_address(), &manager_dispatcher_t::poll);
    dispatcher->resume(1);

    REQUIRE(future.available());
    auto cursor = std::move(future).get();

    REQUIRE(!cursor->has_error);
    REQUIRE(cursor->row_count() == 10);
    REQUIRE(cursor->get_row(0) == "row_0_from_test_db.users");
    REQUIRE(cursor->is_open);

    // Cleanup
    send(dispatcher.get(), address_t::empty_address(),
         &manager_dispatcher_t::close_cursor, session);
    dispatcher->resume(1);

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("dispatcher-pattern: execute_plan with invalid plan") {
    auto* resource = pmr::get_default_resource();

    g_log.log("\n========== TEST: dispatcher-pattern: execute_plan with invalid plan ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    session_id_t session("session-exec-002");

    logical_plan_t plan("select",
                        collection_full_name_t("", "users"),  // Empty database
                        "");

    auto future = send(
        dispatcher.get(),
        address_t::empty_address(),
        &manager_dispatcher_t::execute_plan,
        session,
        std::move(plan));

    dispatcher->resume(1);

    REQUIRE(future.available());
    auto cursor = std::move(future).get();

    REQUIRE(cursor->has_error);
    REQUIRE(cursor->error_message == "Invalid plan");

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("dispatcher-pattern: execute_plan non-existent collection") {
    auto* resource = pmr::get_default_resource();

    g_log.log("\n========== TEST: dispatcher-pattern: execute_plan non-existent collection ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    session_id_t session("session-exec-003");

    logical_plan_t plan("select",
                        collection_full_name_t("test_db", "nonexistent"),
                        "");

    auto future = send(
        dispatcher.get(),
        address_t::empty_address(),
        &manager_dispatcher_t::execute_plan,
        session,
        std::move(plan));

    dispatcher->resume(1);
    storage->resume(1);
    send(dispatcher.get(), address_t::empty_address(), &manager_dispatcher_t::poll);
    dispatcher->resume(1);

    REQUIRE(future.available());
    auto cursor = std::move(future).get();

    REQUIRE(cursor->has_error);
    REQUIRE(cursor->error_message.find("not found") != std::string::npos);

    g_log.log("========== TEST PASSED ==========");
}

// ============================================================================
// Transaction tests (sequential co_await)
// ============================================================================

TEST_CASE("dispatcher-pattern: transaction - sequential co_await") {
    auto* resource = pmr::get_default_resource();

    g_log.log("\n========== TEST: transaction - sequential co_await ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    session_id_t session("session-tx-001");

    auto future = send(
        dispatcher.get(),
        address_t::empty_address(),
        &manager_dispatcher_t::execute_transaction,
        session,
        std::string("users"),
        std::string("orders"));

    // Execute: dispatcher -> storage (step 1)
    dispatcher->resume(1);
    storage->resume(1);

    // Poll to resume after first co_await
    send(dispatcher.get(), address_t::empty_address(), &manager_dispatcher_t::poll);
    dispatcher->resume(1);

    // Step 2
    storage->resume(1);

    // Poll to complete
    send(dispatcher.get(), address_t::empty_address(), &manager_dispatcher_t::poll);
    dispatcher->resume(1);

    REQUIRE(future.available());
    auto result = std::move(future).get();

    REQUIRE(!result.has_error);
    REQUIRE(result.committed);
    REQUIRE(result.total_rows == 20);  // 10 + 10 rows

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("dispatcher-pattern: transaction - error in step 1") {
    auto* resource = pmr::get_default_resource();

    g_log.log("\n========== TEST: transaction - error in step 1 ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    session_id_t session("session-tx-002");

    auto future = send(
        dispatcher.get(),
        address_t::empty_address(),
        &manager_dispatcher_t::execute_transaction,
        session,
        std::string("nonexistent"),  // Does not exist
        std::string("orders"));

    dispatcher->resume(1);
    storage->resume(1);
    send(dispatcher.get(), address_t::empty_address(), &manager_dispatcher_t::poll);
    dispatcher->resume(1);

    REQUIRE(future.available());
    auto result = std::move(future).get();

    REQUIRE(result.has_error);
    REQUIRE(!result.committed);
    REQUIRE(result.error_message.find("Step 1 failed") != std::string::npos);

    g_log.log("========== TEST PASSED ==========");
}

// ============================================================================
// Aggregation tests (parallel + nested coroutine)
// ============================================================================

TEST_CASE("dispatcher-pattern: aggregate - parallel requests + nested coroutine") {
    auto* resource = pmr::get_default_resource();

    g_log.log("\n========== TEST: aggregate - parallel requests + nested coroutine ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    session_id_t session("session-agg-001");

    auto future = send(
        dispatcher.get(),
        address_t::empty_address(),
        &manager_dispatcher_t::aggregate_sizes,
        session,
        std::vector<std::string>{"users", "orders", "products"});

    dispatcher->resume(1);

    // Storage receives 3 parallel requests
    storage->resume(1);
    storage->resume(1);
    storage->resume(1);

    // Poll - first co_await ready
    send(dispatcher.get(), address_t::empty_address(), &manager_dispatcher_t::poll);
    dispatcher->resume(1);

    // Poll - second co_await
    send(dispatcher.get(), address_t::empty_address(), &manager_dispatcher_t::poll);
    dispatcher->resume(1);

    // Poll - third co_await
    send(dispatcher.get(), address_t::empty_address(), &manager_dispatcher_t::poll);
    dispatcher->resume(1);

    // Nested coroutine get_aggregate_detail
    // total = 400 > 200, so extra co_await
    storage->resume(1);

    send(dispatcher.get(), address_t::empty_address(), &manager_dispatcher_t::poll);
    dispatcher->resume(1);

    REQUIRE(future.available());
    auto result = std::move(future).get();

    REQUIRE(!result.has_error);
    REQUIRE(result.total_size == 400);
    REQUIRE(result.collection_count == 3);
    REQUIRE(result.detail_info.find("large_dataset") != std::string::npos);
    REQUIRE(result.detail_info.find("total=400") != std::string::npos);

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("dispatcher-pattern: aggregate - small dataset (no extra request)") {
    auto* resource = pmr::get_default_resource();

    g_log.log("\n========== TEST: aggregate - small dataset ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    session_id_t session("session-agg-002");

    // Only products (50) - small dataset, total < 200
    auto future = send(
        dispatcher.get(),
        address_t::empty_address(),
        &manager_dispatcher_t::aggregate_sizes,
        session,
        std::vector<std::string>{"products"});

    dispatcher->resume(1);
    storage->resume(1);

    send(dispatcher.get(), address_t::empty_address(), &manager_dispatcher_t::poll);
    dispatcher->resume(1);

    REQUIRE(future.available());
    auto result = std::move(future).get();

    REQUIRE(!result.has_error);
    REQUIRE(result.total_size == 50);
    REQUIRE(result.collection_count == 1);
    REQUIRE(result.detail_info.find("small_dataset") != std::string::npos);

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("dispatcher-pattern: aggregate - empty collection list") {
    auto* resource = pmr::get_default_resource();

    g_log.log("\n========== TEST: aggregate - empty collection list ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    session_id_t session("session-agg-003");

    auto future = send(
        dispatcher.get(),
        address_t::empty_address(),
        &manager_dispatcher_t::aggregate_sizes,
        session,
        std::vector<std::string>{});  // Empty!

    dispatcher->resume(1);

    REQUIRE(future.available());
    auto result = std::move(future).get();

    REQUIRE(result.has_error);
    REQUIRE(result.error_message == "No collections provided");

    g_log.log("========== TEST PASSED ==========");
}

// ============================================================================
// Parallel clients test
// ============================================================================

TEST_CASE("dispatcher-pattern: parallel clients (separate chains)") {
    auto* resource = pmr::get_default_resource();

    g_log.log("\n========== TEST: dispatcher-pattern: parallel clients ==========");

    std::atomic<int> success_count{0};
    constexpr int NUM_THREADS = 4;
    std::vector<std::thread> threads;

    struct ThreadResult {
        std::atomic<std::size_t> size{0};
        std::atomic<bool> has_error{true};
        std::atomic<bool> available{false};
    };

    std::vector<ThreadResult> results(NUM_THREADS);

    for (std::size_t i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back([resource, &results, &success_count, i]() {
            auto tid = thread_id_str();
            g_log.log("[THREAD %] Started, thread=%", i, tid);

            // Each thread creates its own actor chain
            auto storage = spawn<memory_storage_t>(resource, "Storage" + std::to_string(i));
            auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher" + std::to_string(i));
            auto client = spawn<client_t>(resource, dispatcher->address(), "Client" + std::to_string(i));

            std::string collection;
            std::size_t expected_size{0};
            switch (i % 3) {
                case 0: collection = "users"; expected_size = 100; break;
                case 1: collection = "orders"; expected_size = 250; break;
                case 2: collection = "products"; expected_size = 50; break;
                default: break;
            }

            session_id_t session("session-thread-" + std::to_string(i));

            auto future = send(
                client.get(),
                address_t::empty_address(),
                &client_t::request_collection_size,
                session,
                std::string("test_db"),
                collection);

            client->resume(1);
            dispatcher->resume(1);
            storage->resume(1);
            send(dispatcher.get(), address_t::empty_address(), &manager_dispatcher_t::poll);
            dispatcher->resume(1);
            send(client.get(), address_t::empty_address(), &client_t::poll);
            client->resume(1);

            results[i].available = future.available();
            if (results[i].available) {
                auto result = std::move(future).get();
                results[i].size = result.size;
                results[i].has_error = result.has_error;

                if (!result.has_error && result.size == expected_size) {
                    ++success_count;
                }
            }

            g_log.log("[THREAD %] Done, size=%", i, results[i].size.load());
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    REQUIRE(success_count == NUM_THREADS);

    for (std::size_t i = 0; i < NUM_THREADS; ++i) {
        REQUIRE(results[i].available);
        REQUIRE(!results[i].has_error);
    }

    g_log.log("========== TEST PASSED ==========");
}

// ============================================================================
// LAMBDA EXAMPLE - Using lambdas INSIDE existing actor methods
// Tests for manager_dispatcher_t::transform_with_lambda,
//              manager_dispatcher_t::compute_with_lambda_and_state,
//              manager_dispatcher_t::async_transform_with_lambda
// ============================================================================

TEST_CASE("lambda-inside: simple lambda in method (transform_with_lambda)") {
    auto* resource = pmr::get_default_resource();

    g_log.log("\n========== TEST: lambda-inside: simple lambda in method ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    // Test transform_with_lambda(int value, int factor)
    // result = value * factor + 100
    auto future = send(
        dispatcher.get(),
        address_t::empty_address(),
        &manager_dispatcher_t::transform_with_lambda,
        5, 10);  // value=5, factor=10

    dispatcher->resume(1);

    REQUIRE(future.available());
    auto result = std::move(future).get();
    REQUIRE(result == 150);  // 5 * 10 + 100 = 150

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("lambda-inside: lambda capturing this and state (compute_with_lambda_and_state)") {
    auto* resource = pmr::get_default_resource();

    g_log.log("\n========== TEST: lambda-inside: lambda capturing this and state ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "TestDispatcher");

    // Test compute_with_lambda_and_state(const std::string& prefix)
    // result = prefix + "_from_" + name_
    auto future = send(
        dispatcher.get(),
        address_t::empty_address(),
        &manager_dispatcher_t::compute_with_lambda_and_state,
        std::string("query"));

    dispatcher->resume(1);

    REQUIRE(future.available());
    auto result = std::move(future).get();
    REQUIRE(result == "query_from_TestDispatcher");

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("lambda-inside: lambda + coroutine (async_transform_with_lambda)") {
    auto* resource = pmr::get_default_resource();

    g_log.log("\n========== TEST: lambda-inside: lambda + coroutine ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    session_id_t session("lambda-session");

    // Test async_transform_with_lambda - calls storage, then uses lambda to format
    // result = "Collection " + coll + " in " + name_ + " has " + size + " items"
    auto future = send(
        dispatcher.get(),
        address_t::empty_address(),
        &manager_dispatcher_t::async_transform_with_lambda,
        session,
        std::string("users"));

    // Dispatcher suspends on co_await, storage processes
    dispatcher->resume(1);
    storage->resume(1);

    // Poll to resume dispatcher after storage returns
    send(dispatcher.get(), address_t::empty_address(), &manager_dispatcher_t::poll);
    dispatcher->resume(1);

    REQUIRE(future.available());
    auto result = std::move(future).get();
    REQUIRE(result == "Collection users in Dispatcher has 100 items");

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("lambda-inside: lambda + coroutine with different collection") {
    auto* resource = pmr::get_default_resource();

    g_log.log("\n========== TEST: lambda-inside: lambda + coroutine with orders ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "OrderDispatcher");

    session_id_t session("orders-session");

    // Test with 'orders' collection which has 250 items
    auto future = send(
        dispatcher.get(),
        address_t::empty_address(),
        &manager_dispatcher_t::async_transform_with_lambda,
        session,
        std::string("orders"));

    dispatcher->resume(1);
    storage->resume(1);
    send(dispatcher.get(), address_t::empty_address(), &manager_dispatcher_t::poll);
    dispatcher->resume(1);

    REQUIRE(future.available());
    auto result = std::move(future).get();
    REQUIRE(result == "Collection orders in OrderDispatcher has 250 items");

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("lambda-inside: coroutine lambda (execute_with_coroutine_lambda)") {
    auto* resource = pmr::get_default_resource();

    g_log.log("\n========== TEST: lambda-inside: coroutine lambda ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    session_id_t session("coroutine-lambda-session");

    // Test execute_with_coroutine_lambda
    // Lambda INSIDE has co_await -> sends to storage -> gets size -> multiplies
    // users collection has 100 items, multiplier = 3 -> result = 300
    auto future = send(
        dispatcher.get(),
        address_t::empty_address(),
        &manager_dispatcher_t::execute_with_coroutine_lambda,
        session,
        std::string("users"),
        3);  // multiplier

    // Dispatcher suspends on co_await (outer), then lambda suspends on co_await (inner)
    dispatcher->resume(1);
    storage->resume(1);

    // Poll to resume lambda-coroutine, then outer coroutine
    send(dispatcher.get(), address_t::empty_address(), &manager_dispatcher_t::poll);
    dispatcher->resume(1);

    REQUIRE(future.available());
    auto result = std::move(future).get();
    REQUIRE(result == 300);  // 100 * 3

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("lambda-inside: coroutine lambda with orders") {
    auto* resource = pmr::get_default_resource();

    g_log.log("\n========== TEST: lambda-inside: coroutine lambda with orders ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    session_id_t session("orders-lambda-session");

    // orders collection has 250 items, multiplier = 2 -> result = 500
    auto future = send(
        dispatcher.get(),
        address_t::empty_address(),
        &manager_dispatcher_t::execute_with_coroutine_lambda,
        session,
        std::string("orders"),
        2);

    dispatcher->resume(1);
    storage->resume(1);
    send(dispatcher.get(), address_t::empty_address(), &manager_dispatcher_t::poll);
    dispatcher->resume(1);

    REQUIRE(future.available());
    auto result = std::move(future).get();
    REQUIRE(result == 500);  // 250 * 2

    g_log.log("========== TEST PASSED ==========");
}

// ============================================================================
// Extended database operations tests
// ============================================================================

TEST_CASE("database: create_cursor_from_query - lambda-coroutine returns unique_ptr") {
    auto* resource = pmr::get_default_resource();
    g_log.log("\n========== TEST: create_cursor_from_query ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    session_id_t session("cursor-session");
    auto future = send(dispatcher.get(), address_t::empty_address(),
        &manager_dispatcher_t::create_cursor_from_query, session, std::string("users"));

    dispatcher->resume(1);
    storage->resume(1);
    send(dispatcher.get(), address_t::empty_address(), &manager_dispatcher_t::poll);
    dispatcher->resume(1);

    REQUIRE(future.available());
    auto cursor = std::move(future).get();
    REQUIRE(cursor != nullptr);
    REQUIRE(cursor->row_count() == 10);  // min(100, 10) rows from users

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("database: validate_and_execute - chained lambda-coroutines") {
    auto* resource = pmr::get_default_resource();
    g_log.log("\n========== TEST: validate_and_execute ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    session_id_t session("validate-session");
    logical_plan_t plan("select", collection_full_name_t("test_db", "users"), "id > 0");

    auto future = send(dispatcher.get(), address_t::empty_address(),
        &manager_dispatcher_t::validate_and_execute, session, std::move(plan));

    dispatcher->resume(1);
    storage->resume(1);
    send(dispatcher.get(), address_t::empty_address(), &manager_dispatcher_t::poll);
    dispatcher->resume(1);

    REQUIRE(future.available());
    auto cursor = std::move(future).get();
    REQUIRE(cursor != nullptr);
    REQUIRE(!cursor->has_error);
    REQUIRE(cursor->row_count() == 10);

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("database: validate_and_execute - validation failure") {
    auto* resource = pmr::get_default_resource();
    g_log.log("\n========== TEST: validate_and_execute - validation failure ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    session_id_t session("validate-fail-session");
    logical_plan_t plan("select", collection_full_name_t("", ""), "");  // Invalid plan

    auto future = send(dispatcher.get(), address_t::empty_address(),
        &manager_dispatcher_t::validate_and_execute, session, std::move(plan));

    dispatcher->resume(1);

    REQUIRE(future.available());
    auto cursor = std::move(future).get();
    REQUIRE(cursor != nullptr);
    REQUIRE(cursor->has_error);
    REQUIRE(cursor->error_message == "Validation failed");

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("database: get_database_statistics - parallel lambda-coroutines") {
    auto* resource = pmr::get_default_resource();
    g_log.log("\n========== TEST: get_database_statistics ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    session_id_t session("stats-session");
    auto future = send(dispatcher.get(), address_t::empty_address(),
        &manager_dispatcher_t::get_database_statistics, session);

    dispatcher->resume(1);
    // Process 3 parallel storage requests
    storage->resume(1);
    storage->resume(1);
    storage->resume(1);
    // Poll after each storage completes to resume lambda-coroutines
    send(dispatcher.get(), address_t::empty_address(), &manager_dispatcher_t::poll);
    dispatcher->resume(1);
    send(dispatcher.get(), address_t::empty_address(), &manager_dispatcher_t::poll);
    dispatcher->resume(1);
    send(dispatcher.get(), address_t::empty_address(), &manager_dispatcher_t::poll);
    dispatcher->resume(1);

    REQUIRE(future.available());
    auto result = std::move(future).get();
    // users(100) + orders(250) + products(50) = 400
    REQUIRE(result.total_size == 400);
    REQUIRE(result.collection_count == 3);
    REQUIRE(result.detail_info.find("stats:users+orders+products=400") != std::string::npos);

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("database: process_batch_buffer - move-only argument") {
    auto* resource = pmr::get_default_resource();
    g_log.log("\n========== TEST: process_batch_buffer ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    session_id_t session("batch-session");
    auto batch = std::make_unique<std::vector<std::string>>();
    batch->push_back("item1");
    batch->push_back("item2");
    batch->push_back("item3");

    auto future = send(dispatcher.get(), address_t::empty_address(),
        &manager_dispatcher_t::process_batch_buffer, session, std::move(batch));

    dispatcher->resume(1);

    REQUIRE(future.available());
    auto result = std::move(future).get();
    REQUIRE(!result.has_error);
    REQUIRE(result.committed);
    REQUIRE(result.total_rows == 3);

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("database: process_batch_buffer - empty batch") {
    auto* resource = pmr::get_default_resource();
    g_log.log("\n========== TEST: process_batch_buffer - empty batch ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    session_id_t session("empty-batch-session");
    auto batch = std::make_unique<std::vector<std::string>>();  // Empty

    auto future = send(dispatcher.get(), address_t::empty_address(),
        &manager_dispatcher_t::process_batch_buffer, session, std::move(batch));

    dispatcher->resume(1);

    REQUIRE(future.available());
    auto result = std::move(future).get();
    REQUIRE(result.has_error);
    REQUIRE(result.error_message == "Empty batch");

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("database: get_cached_value - promise direct manipulation") {
    auto* resource = pmr::get_default_resource();
    g_log.log("\n========== TEST: get_cached_value ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    session_id_t session("cache-session");

    // Test cached values for different collections
    auto future1 = send(dispatcher.get(), address_t::empty_address(),
        &manager_dispatcher_t::get_cached_value, session, std::string("users"));
    dispatcher->resume(1);
    REQUIRE(future1.available());
    REQUIRE(std::move(future1).get() == 100);

    auto future2 = send(dispatcher.get(), address_t::empty_address(),
        &manager_dispatcher_t::get_cached_value, session, std::string("orders"));
    dispatcher->resume(1);
    REQUIRE(future2.available());
    REQUIRE(std::move(future2).get() == 250);

    auto future3 = send(dispatcher.get(), address_t::empty_address(),
        &manager_dispatcher_t::get_cached_value, session, std::string("products"));
    dispatcher->resume(1);
    REQUIRE(future3.available());
    REQUIRE(std::move(future3).get() == 50);

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("database: execute_with_retry - success without retry") {
    auto* resource = pmr::get_default_resource();
    g_log.log("\n========== TEST: execute_with_retry - success ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    session_id_t session("retry-session");
    // max_retries=0 -> no simulated error, direct success
    auto future = send(dispatcher.get(), address_t::empty_address(),
        &manager_dispatcher_t::execute_with_retry, session, std::string("users"), 0);

    dispatcher->resume(1);
    storage->resume(1);
    send(dispatcher.get(), address_t::empty_address(), &manager_dispatcher_t::poll);
    dispatcher->resume(1);

    REQUIRE(future.available());
    auto result = std::move(future).get();
    REQUIRE(!result.has_error);
    REQUIRE(result.size == 100);

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("database: execute_with_retry - retry after failure") {
    auto* resource = pmr::get_default_resource();
    g_log.log("\n========== TEST: execute_with_retry - retry ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    session_id_t session("retry-session");
    // max_retries=1 -> first attempt fails, retry succeeds
    auto future = send(dispatcher.get(), address_t::empty_address(),
        &manager_dispatcher_t::execute_with_retry, session, std::string("orders"), 1);

    dispatcher->resume(1);
    storage->resume(1);
    send(dispatcher.get(), address_t::empty_address(), &manager_dispatcher_t::poll);
    dispatcher->resume(1);

    REQUIRE(future.available());
    auto result = std::move(future).get();
    REQUIRE(!result.has_error);
    REQUIRE(result.size == 250);

    g_log.log("========== TEST PASSED ==========");
}

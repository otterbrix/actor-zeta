/// @file main.cpp
/// Driving the actors: resume() hands back a verdict the caller is obliged to act
/// on, so the tests never call it directly. Every step enqueues the actor into a
/// scheduler_test_t, which owns the verdict -- run_once() re-queues a job that
/// asked to be resumed and drops one that parked. The drain call is stop(), not
/// run(): a behavior suspended on a still-pending co_await keeps answering
/// `resume` with zero messages handled, so "queue empty" (run()'s condition) is
/// never reached, while stop() ends on a full sweep that made no progress.
///
/// The scheduler is declared after the actors in every test: its deque holds raw
/// job pointers, so it has to be destroyed first.

#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include "client.hpp"  // Includes all other headers

#include <test/tooltestsuites/scheduler_test.hpp>

#include <thread>
#include <atomic>

using namespace actor_zeta;
using namespace dispatcher_test;

// Drive steps follow one shape: `send(...).second.detach(); sched.enqueue(target);`.
// The future is detached because these messages carry no result the test reads,
// and the enqueue is unconditional because a manual driver runs the actor
// regardless of what send() reports -- the actors hold no scheduler, so these
// enqueues ARE the discharge of the obligation.
//
// `poll` is a no-op message: it exists so the scheduler resumes the actor, and the
// resume entry path drains a handler whose awaited future has since become ready.

TEST_CASE("dispatcher-pattern: single-thread basic flow") {
    auto* resource = std::pmr::get_default_resource();

    g_log.log("\n========== TEST: dispatcher-pattern: single-thread basic flow ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");
    auto client = spawn<client_t>(resource, dispatcher->address(), "Client");

    actor_zeta::test::scheduler_test_t sched(1, 100);

    session_id_t session("session-001");

    auto [needs_sched, future] = send(
        client.get(),
        &client_t::request_collection_size,
        session,
        std::string("test_db"),
        std::string("users"));

    // Five stages: client suspends on the dispatcher; dispatcher suspends on
    // storage; storage answers (flag-only, nobody is woken); a poll resumes the
    // dispatcher, which drains its await and answers the client; a poll resumes
    // the client.
    sched.enqueue(client.get());
    sched.stop();

    sched.enqueue(dispatcher.get());
    sched.stop();

    sched.enqueue(storage.get());
    sched.stop();

    send(dispatcher.get(), &manager_dispatcher_t::poll).second.detach();
    sched.enqueue(dispatcher.get());
    sched.stop();

    send(client.get(), &client_t::poll).second.detach();
    sched.enqueue(client.get());
    sched.stop();

    REQUIRE(future.is_ready());
    auto result = std::move(future).take_ready();

    REQUIRE(!result.has_error);
    REQUIRE(result.size == 100);  // test_db.users has 100 items

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("dispatcher-pattern: error handling") {
    auto* resource = std::pmr::get_default_resource();

    g_log.log("\n========== TEST: dispatcher-pattern: error handling ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");
    auto client = spawn<client_t>(resource, dispatcher->address(), "Client");

    actor_zeta::test::scheduler_test_t sched(1, 100);

    session_id_t session("session-002");

    auto [needs_sched, future] = send(
        client.get(),
        &client_t::request_collection_size,
        session,
        std::string(""),  // Empty - triggers error
        std::string("users"));

    sched.enqueue(client.get());
    sched.stop();

    // The dispatcher co_returns the error before any co_await, so no storage stage.
    sched.enqueue(dispatcher.get());
    sched.stop();

    send(client.get(), &client_t::poll).second.detach();
    sched.enqueue(client.get());
    sched.stop();

    REQUIRE(future.is_ready());
    auto result = std::move(future).take_ready();

    REQUIRE(result.has_error);
    REQUIRE(result.error_message == "Empty database or collection name");

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("dispatcher-pattern: multiple requests") {
    auto* resource = std::pmr::get_default_resource();

    g_log.log("\n========== TEST: dispatcher-pattern: multiple requests ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");
    auto client = spawn<client_t>(resource, dispatcher->address(), "Client");

    actor_zeta::test::scheduler_test_t sched(1, 100);

    auto [needs_sched1, future1] = send(
        client.get(),
        &client_t::request_collection_size,
        session_id_t("session-003"),
        std::string("test_db"),
        std::string("users"));

    sched.enqueue(client.get());
    sched.stop();
    sched.enqueue(dispatcher.get());
    sched.stop();
    sched.enqueue(storage.get());
    sched.stop();
    send(dispatcher.get(), &manager_dispatcher_t::poll).second.detach();
    sched.enqueue(dispatcher.get());
    sched.stop();
    send(client.get(), &client_t::poll).second.detach();
    sched.enqueue(client.get());
    sched.stop();

    REQUIRE(future1.is_ready());
    auto result1 = std::move(future1).take_ready();
    REQUIRE(result1.size == 100);

    auto [needs_sched2, future2] = send(
        client.get(),
        &client_t::request_collection_size,
        session_id_t("session-004"),
        std::string("test_db"),
        std::string("orders"));

    sched.enqueue(client.get());
    sched.stop();
    sched.enqueue(dispatcher.get());
    sched.stop();
    sched.enqueue(storage.get());
    sched.stop();
    send(dispatcher.get(), &manager_dispatcher_t::poll).second.detach();
    sched.enqueue(dispatcher.get());
    sched.stop();
    send(client.get(), &client_t::poll).second.detach();
    sched.enqueue(client.get());
    sched.stop();

    REQUIRE(future2.is_ready());
    auto result2 = std::move(future2).take_ready();
    REQUIRE(result2.size == 250);

    auto [needs_sched3, future3] = send(
        client.get(),
        &client_t::request_collection_size,
        session_id_t("session-005"),
        std::string("test_db"),
        std::string("products"));

    sched.enqueue(client.get());
    sched.stop();
    sched.enqueue(dispatcher.get());
    sched.stop();
    sched.enqueue(storage.get());
    sched.stop();
    send(dispatcher.get(), &manager_dispatcher_t::poll).second.detach();
    sched.enqueue(dispatcher.get());
    sched.stop();
    send(client.get(), &client_t::poll).second.detach();
    sched.enqueue(client.get());
    sched.stop();

    REQUIRE(future3.is_ready());
    auto result3 = std::move(future3).take_ready();
    REQUIRE(result3.size == 50);

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("dispatcher-pattern: non-existent collection") {
    auto* resource = std::pmr::get_default_resource();

    g_log.log("\n========== TEST: dispatcher-pattern: non-existent collection ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");
    auto client = spawn<client_t>(resource, dispatcher->address(), "Client");

    actor_zeta::test::scheduler_test_t sched(1, 100);

    session_id_t session("session-006");

    auto [needs_sched, future] = send(
        client.get(),
        &client_t::request_collection_size,
        session,
        std::string("test_db"),
        std::string("nonexistent"));

    sched.enqueue(client.get());
    sched.stop();
    sched.enqueue(dispatcher.get());
    sched.stop();
    sched.enqueue(storage.get());
    sched.stop();
    send(dispatcher.get(), &manager_dispatcher_t::poll).second.detach();
    sched.enqueue(dispatcher.get());
    sched.stop();
    send(client.get(), &client_t::poll).second.detach();
    sched.enqueue(client.get());
    sched.stop();

    REQUIRE(future.is_ready());
    auto result = std::move(future).take_ready();

    // Non-existent collection returns 0 (not an error)
    REQUIRE(!result.has_error);
    REQUIRE(result.size == 0);

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("dispatcher-pattern: multi-thread execution") {
    auto* resource = std::pmr::get_default_resource();

    g_log.log("\n========== TEST: dispatcher-pattern: multi-thread execution ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");
    auto client = spawn<client_t>(resource, dispatcher->address(), "Client");

    actor_zeta::test::scheduler_test_t sched(1, 100);

    std::atomic<bool> done{false};
    std::atomic<std::size_t> result_size{0};
    std::atomic<bool> result_has_error{true};
    std::atomic<bool> future_available{false};

    std::thread worker_thread([&]() {
        auto tid = thread_id_str();
        g_log.log("[WORKER_THREAD] Started, thread=%", tid);

        session_id_t session("session-007");

        auto [needs_sched_wt, future] = send(
        client.get(),
        &client_t::request_collection_size,
            session,
            std::string("test_db"),
            std::string("orders"));

        sched.enqueue(client.get());
        sched.stop();
        sched.enqueue(dispatcher.get());
        sched.stop();
        sched.enqueue(storage.get());
        sched.stop();
        send(dispatcher.get(), &manager_dispatcher_t::poll).second.detach();
        sched.enqueue(dispatcher.get());
        sched.stop();
        send(client.get(), &client_t::poll).second.detach();
        sched.enqueue(client.get());
        sched.stop();

        future_available = future.is_ready();
        if (future_available) {
            auto result = std::move(future).take_ready();
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

TEST_CASE("dispatcher-pattern: execute_plan with cursor") {
    auto* resource = std::pmr::get_default_resource();

    g_log.log("\n========== TEST: dispatcher-pattern: execute_plan with cursor ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    actor_zeta::test::scheduler_test_t sched(1, 100);

    session_id_t session("session-exec-001");

    logical_plan_t plan("select",
                        collection_full_name_t("test_db", "users"),
                        "id > 0");

    auto [needs_sched, future] = send(
        dispatcher.get(),
        &manager_dispatcher_t::execute_plan,
        session,
        std::move(plan));

    sched.enqueue(dispatcher.get());
    sched.stop();
    sched.enqueue(storage.get());
    sched.stop();
    send(dispatcher.get(), &manager_dispatcher_t::poll).second.detach();
    sched.enqueue(dispatcher.get());
    sched.stop();

    REQUIRE(future.is_ready());
    auto cursor = std::move(future).take_ready();

    REQUIRE(!cursor->has_error);
    REQUIRE(cursor->row_count() == 10);
    REQUIRE(cursor->get_row(0) == "row_0_from_test_db.users");
    REQUIRE(cursor->is_open);

    send(dispatcher.get(), &manager_dispatcher_t::close_cursor, session).second.detach();
    sched.enqueue(dispatcher.get());
    sched.stop();

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("dispatcher-pattern: execute_plan with invalid plan") {
    auto* resource = std::pmr::get_default_resource();

    g_log.log("\n========== TEST: dispatcher-pattern: execute_plan with invalid plan ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    actor_zeta::test::scheduler_test_t sched(1, 100);

    session_id_t session("session-exec-002");

    logical_plan_t plan("select",
                        collection_full_name_t("", "users"),  // Empty database
                        "");

    auto [needs_sched, future] = send(
        dispatcher.get(),
        &manager_dispatcher_t::execute_plan,
        session,
        std::move(plan));

    sched.enqueue(dispatcher.get());
    sched.stop();

    REQUIRE(future.is_ready());
    auto cursor = std::move(future).take_ready();

    REQUIRE(cursor->has_error);
    REQUIRE(cursor->error_message == "Invalid plan");

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("dispatcher-pattern: execute_plan non-existent collection") {
    auto* resource = std::pmr::get_default_resource();

    g_log.log("\n========== TEST: dispatcher-pattern: execute_plan non-existent collection ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    actor_zeta::test::scheduler_test_t sched(1, 100);

    session_id_t session("session-exec-003");

    logical_plan_t plan("select",
                        collection_full_name_t("test_db", "nonexistent"),
                        "");

    auto [needs_sched, future] = send(
        dispatcher.get(),
        &manager_dispatcher_t::execute_plan,
        session,
        std::move(plan));

    sched.enqueue(dispatcher.get());
    sched.stop();
    sched.enqueue(storage.get());
    sched.stop();
    send(dispatcher.get(), &manager_dispatcher_t::poll).second.detach();
    sched.enqueue(dispatcher.get());
    sched.stop();

    REQUIRE(future.is_ready());
    auto cursor = std::move(future).take_ready();

    REQUIRE(cursor->has_error);
    REQUIRE(cursor->error_message.find("not found") != std::string::npos);

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("dispatcher-pattern: transaction - sequential co_await") {
    auto* resource = std::pmr::get_default_resource();

    g_log.log("\n========== TEST: transaction - sequential co_await ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    actor_zeta::test::scheduler_test_t sched(1, 100);

    session_id_t session("session-tx-001");

    auto [needs_sched, future] = send(
        dispatcher.get(),
        &manager_dispatcher_t::execute_transaction,
        session,
        std::string("users"),
        std::string("orders"));

    // Step 1: dispatcher -> storage, then a poll to drain the first await.
    sched.enqueue(dispatcher.get());
    sched.stop();
    sched.enqueue(storage.get());
    sched.stop();

    send(dispatcher.get(), &manager_dispatcher_t::poll).second.detach();
    sched.enqueue(dispatcher.get());
    sched.stop();

    // Step 2: the second request is only sent after the first came back.
    sched.enqueue(storage.get());
    sched.stop();

    send(dispatcher.get(), &manager_dispatcher_t::poll).second.detach();
    sched.enqueue(dispatcher.get());
    sched.stop();

    REQUIRE(future.is_ready());
    auto result = std::move(future).take_ready();

    REQUIRE(!result.has_error);
    REQUIRE(result.committed);
    REQUIRE(result.total_rows == 20);  // 10 + 10 rows

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("dispatcher-pattern: transaction - error in step 1") {
    auto* resource = std::pmr::get_default_resource();

    g_log.log("\n========== TEST: transaction - error in step 1 ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    actor_zeta::test::scheduler_test_t sched(1, 100);

    session_id_t session("session-tx-002");

    auto [needs_sched, future] = send(
        dispatcher.get(),
        &manager_dispatcher_t::execute_transaction,
        session,
        std::string("nonexistent"),  // Does not exist
        std::string("orders"));

    sched.enqueue(dispatcher.get());
    sched.stop();
    sched.enqueue(storage.get());
    sched.stop();
    send(dispatcher.get(), &manager_dispatcher_t::poll).second.detach();
    sched.enqueue(dispatcher.get());
    sched.stop();

    REQUIRE(future.is_ready());
    auto result = std::move(future).take_ready();

    REQUIRE(result.has_error);
    REQUIRE(!result.committed);
    REQUIRE(result.error_message.find("Step 1 failed") != std::string::npos);

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("dispatcher-pattern: aggregate - parallel requests + nested coroutine") {
    auto* resource = std::pmr::get_default_resource();

    g_log.log("\n========== TEST: aggregate - parallel requests + nested coroutine ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    actor_zeta::test::scheduler_test_t sched(1, 100);

    session_id_t session("session-agg-001");

    auto [needs_sched, future] = send(
        dispatcher.get(),
        &manager_dispatcher_t::aggregate_sizes,
        session,
        std::vector<std::string>{"users", "orders", "products"});

    sched.enqueue(dispatcher.get());
    sched.stop();

    // Storage receives 3 parallel requests. Drive them ONE MESSAGE AT A TIME:
    // sched.stop() drains to quiescence, so a single enqueue+stop would handle
    // all three and the next two stages would process nothing at all. Asserting
    // messages_processed is what keeps a stage from silently becoming a no-op.
    for (int i = 0; i < 3; ++i) {
        const auto info = storage->resume(1);
        REQUIRE(info.messages_processed == 1);
    }

    // One poll per awaited future, in order.
    send(dispatcher.get(), &manager_dispatcher_t::poll).second.detach();
    sched.enqueue(dispatcher.get());
    sched.stop();

    send(dispatcher.get(), &manager_dispatcher_t::poll).second.detach();
    sched.enqueue(dispatcher.get());
    sched.stop();

    send(dispatcher.get(), &manager_dispatcher_t::poll).second.detach();
    sched.enqueue(dispatcher.get());
    sched.stop();

    // total = 400 > 200, so get_aggregate_detail() makes one more request.
    {
        const auto info = storage->resume(1);
        REQUIRE(info.messages_processed == 1);
    }

    send(dispatcher.get(), &manager_dispatcher_t::poll).second.detach();
    sched.enqueue(dispatcher.get());
    sched.stop();

    REQUIRE(future.is_ready());
    auto result = std::move(future).take_ready();

    REQUIRE(!result.has_error);
    REQUIRE(result.total_size == 400);
    REQUIRE(result.collection_count == 3);
    REQUIRE(result.detail_info.find("large_dataset") != std::string::npos);
    REQUIRE(result.detail_info.find("total=400") != std::string::npos);

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("dispatcher-pattern: aggregate - small dataset (no extra request)") {
    auto* resource = std::pmr::get_default_resource();

    g_log.log("\n========== TEST: aggregate - small dataset ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    actor_zeta::test::scheduler_test_t sched(1, 100);

    session_id_t session("session-agg-002");

    // products alone is 50 < 200, so no extra request from get_aggregate_detail().
    auto [needs_sched, future] = send(
        dispatcher.get(),
        &manager_dispatcher_t::aggregate_sizes,
        session,
        std::vector<std::string>{"products"});

    sched.enqueue(dispatcher.get());
    sched.stop();
    sched.enqueue(storage.get());
    sched.stop();

    send(dispatcher.get(), &manager_dispatcher_t::poll).second.detach();
    sched.enqueue(dispatcher.get());
    sched.stop();

    REQUIRE(future.is_ready());
    auto result = std::move(future).take_ready();

    REQUIRE(!result.has_error);
    REQUIRE(result.total_size == 50);
    REQUIRE(result.collection_count == 1);
    REQUIRE(result.detail_info.find("small_dataset") != std::string::npos);

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("dispatcher-pattern: aggregate - empty collection list") {
    auto* resource = std::pmr::get_default_resource();

    g_log.log("\n========== TEST: aggregate - empty collection list ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    actor_zeta::test::scheduler_test_t sched(1, 100);

    session_id_t session("session-agg-003");

    auto [needs_sched, future] = send(
        dispatcher.get(),
        &manager_dispatcher_t::aggregate_sizes,
        session,
        std::vector<std::string>{});  // Empty!

    sched.enqueue(dispatcher.get());
    sched.stop();

    REQUIRE(future.is_ready());
    auto result = std::move(future).take_ready();

    REQUIRE(result.has_error);
    REQUIRE(result.error_message == "No collections provided");

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("dispatcher-pattern: parallel clients (separate chains)") {
    auto* resource = std::pmr::get_default_resource();

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

            auto storage = spawn<memory_storage_t>(resource, "Storage" + std::to_string(i));
            auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher" + std::to_string(i));
            auto client = spawn<client_t>(resource, dispatcher->address(), "Client" + std::to_string(i));

            actor_zeta::test::scheduler_test_t sched(1, 100);

            std::string collection;
            std::size_t expected_size{0};
            switch (i % 3) {
                case 0: collection = "users"; expected_size = 100; break;
                case 1: collection = "orders"; expected_size = 250; break;
                case 2: collection = "products"; expected_size = 50; break;
                default: break;
            }

            session_id_t session("session-thread-" + std::to_string(i));

            auto [needs_sched_th, future] = send(
        client.get(),
        &client_t::request_collection_size,
                session,
                std::string("test_db"),
                collection);

            sched.enqueue(client.get());
            sched.stop();
            sched.enqueue(dispatcher.get());
            sched.stop();
            sched.enqueue(storage.get());
            sched.stop();
            send(dispatcher.get(), &manager_dispatcher_t::poll).second.detach();
            sched.enqueue(dispatcher.get());
            sched.stop();
            send(client.get(), &client_t::poll).second.detach();
            sched.enqueue(client.get());
            sched.stop();

            results[i].available = future.is_ready();
            if (results[i].available) {
                auto result = std::move(future).take_ready();
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

TEST_CASE("lambda-inside: simple lambda in method (transform_with_lambda)") {
    auto* resource = std::pmr::get_default_resource();

    g_log.log("\n========== TEST: lambda-inside: simple lambda in method ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    actor_zeta::test::scheduler_test_t sched(1, 100);

    auto [needs_sched, future] = send(
        dispatcher.get(),
        &manager_dispatcher_t::transform_with_lambda,
        5, 10);

    sched.enqueue(dispatcher.get());
    sched.stop();

    REQUIRE(future.is_ready());
    auto result = std::move(future).take_ready();
    REQUIRE(result == 150);  // 5 * 10 + 100 = 150

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("lambda-inside: lambda capturing this and state (compute_with_lambda_and_state)") {
    auto* resource = std::pmr::get_default_resource();

    g_log.log("\n========== TEST: lambda-inside: lambda capturing this and state ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "TestDispatcher");

    actor_zeta::test::scheduler_test_t sched(1, 100);

    auto [needs_sched, future] = send(
        dispatcher.get(),
        &manager_dispatcher_t::compute_with_lambda_and_state,
        std::string("query"));

    sched.enqueue(dispatcher.get());
    sched.stop();

    REQUIRE(future.is_ready());
    auto result = std::move(future).take_ready();
    REQUIRE(result == "query_from_TestDispatcher");

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("lambda-inside: lambda + coroutine (async_transform_with_lambda)") {
    auto* resource = std::pmr::get_default_resource();

    g_log.log("\n========== TEST: lambda-inside: lambda + coroutine ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    actor_zeta::test::scheduler_test_t sched(1, 100);

    session_id_t session("lambda-session");

    auto [needs_sched, future] = send(
        dispatcher.get(),
        &manager_dispatcher_t::async_transform_with_lambda,
        session,
        std::string("users"));

    sched.enqueue(dispatcher.get());
    sched.stop();
    sched.enqueue(storage.get());
    sched.stop();

    send(dispatcher.get(), &manager_dispatcher_t::poll).second.detach();
    sched.enqueue(dispatcher.get());
    sched.stop();

    REQUIRE(future.is_ready());
    auto result = std::move(future).take_ready();
    REQUIRE(result == "Collection users in Dispatcher has 100 items");

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("lambda-inside: lambda + coroutine with different collection") {
    auto* resource = std::pmr::get_default_resource();

    g_log.log("\n========== TEST: lambda-inside: lambda + coroutine with orders ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "OrderDispatcher");

    actor_zeta::test::scheduler_test_t sched(1, 100);

    session_id_t session("orders-session");

    auto [needs_sched, future] = send(
        dispatcher.get(),
        &manager_dispatcher_t::async_transform_with_lambda,
        session,
        std::string("orders"));

    sched.enqueue(dispatcher.get());
    sched.stop();
    sched.enqueue(storage.get());
    sched.stop();
    send(dispatcher.get(), &manager_dispatcher_t::poll).second.detach();
    sched.enqueue(dispatcher.get());
    sched.stop();

    REQUIRE(future.is_ready());
    auto result = std::move(future).take_ready();
    REQUIRE(result == "Collection orders in OrderDispatcher has 250 items");

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("lambda-inside: coroutine lambda (execute_with_coroutine_lambda)") {
    auto* resource = std::pmr::get_default_resource();

    g_log.log("\n========== TEST: lambda-inside: coroutine lambda ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    actor_zeta::test::scheduler_test_t sched(1, 100);

    session_id_t session("coroutine-lambda-session");

    auto [needs_sched, future] = send(
        dispatcher.get(),
        &manager_dispatcher_t::execute_with_coroutine_lambda,
        session,
        std::string("users"),
        3);

    sched.enqueue(dispatcher.get());
    sched.stop();
    sched.enqueue(storage.get());
    sched.stop();

    // One poll drains the inner lambda-coroutine and, through it, the outer one.
    send(dispatcher.get(), &manager_dispatcher_t::poll).second.detach();
    sched.enqueue(dispatcher.get());
    sched.stop();

    REQUIRE(future.is_ready());
    auto result = std::move(future).take_ready();
    REQUIRE(result == 300);  // 100 * 3

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("lambda-inside: coroutine lambda with orders") {
    auto* resource = std::pmr::get_default_resource();

    g_log.log("\n========== TEST: lambda-inside: coroutine lambda with orders ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    actor_zeta::test::scheduler_test_t sched(1, 100);

    session_id_t session("orders-lambda-session");

    auto [needs_sched, future] = send(
        dispatcher.get(),
        &manager_dispatcher_t::execute_with_coroutine_lambda,
        session,
        std::string("orders"),
        2);

    sched.enqueue(dispatcher.get());
    sched.stop();
    sched.enqueue(storage.get());
    sched.stop();
    send(dispatcher.get(), &manager_dispatcher_t::poll).second.detach();
    sched.enqueue(dispatcher.get());
    sched.stop();

    REQUIRE(future.is_ready());
    auto result = std::move(future).take_ready();
    REQUIRE(result == 500);  // 250 * 2

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("database: create_cursor_from_query - lambda-coroutine returns unique_ptr") {
    auto* resource = std::pmr::get_default_resource();
    g_log.log("\n========== TEST: create_cursor_from_query ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    actor_zeta::test::scheduler_test_t sched(1, 100);

    session_id_t session("cursor-session");
    auto [needs_sched, future] = send(dispatcher.get(),
        &manager_dispatcher_t::create_cursor_from_query, session, std::string("users"));

    sched.enqueue(dispatcher.get());
    sched.stop();
    sched.enqueue(storage.get());
    sched.stop();
    send(dispatcher.get(), &manager_dispatcher_t::poll).second.detach();
    sched.enqueue(dispatcher.get());
    sched.stop();

    REQUIRE(future.is_ready());
    auto cursor = std::move(future).take_ready();
    REQUIRE(cursor != nullptr);
    REQUIRE(cursor->row_count() == 10);  // min(100, 10) rows from users

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("database: validate_and_execute - chained lambda-coroutines") {
    auto* resource = std::pmr::get_default_resource();
    g_log.log("\n========== TEST: validate_and_execute ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    actor_zeta::test::scheduler_test_t sched(1, 100);

    session_id_t session("validate-session");
    logical_plan_t plan("select", collection_full_name_t("test_db", "users"), "id > 0");

    auto [needs_sched, future] = send(dispatcher.get(),
        &manager_dispatcher_t::validate_and_execute, session, std::move(plan));

    sched.enqueue(dispatcher.get());
    sched.stop();
    sched.enqueue(storage.get());
    sched.stop();
    send(dispatcher.get(), &manager_dispatcher_t::poll).second.detach();
    sched.enqueue(dispatcher.get());
    sched.stop();

    REQUIRE(future.is_ready());
    auto cursor = std::move(future).take_ready();
    REQUIRE(cursor != nullptr);
    REQUIRE(!cursor->has_error);
    REQUIRE(cursor->row_count() == 10);

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("database: validate_and_execute - validation failure") {
    auto* resource = std::pmr::get_default_resource();
    g_log.log("\n========== TEST: validate_and_execute - validation failure ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    actor_zeta::test::scheduler_test_t sched(1, 100);

    session_id_t session("validate-fail-session");
    logical_plan_t plan("select", collection_full_name_t("", ""), "");  // Invalid plan

    auto [needs_sched, future] = send(dispatcher.get(),
        &manager_dispatcher_t::validate_and_execute, session, std::move(plan));

    sched.enqueue(dispatcher.get());
    sched.stop();

    REQUIRE(future.is_ready());
    auto cursor = std::move(future).take_ready();
    REQUIRE(cursor != nullptr);
    REQUIRE(cursor->has_error);
    REQUIRE(cursor->error_message == "Validation failed");

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("database: get_database_statistics - parallel lambda-coroutines") {
    auto* resource = std::pmr::get_default_resource();
    g_log.log("\n========== TEST: get_database_statistics ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    actor_zeta::test::scheduler_test_t sched(1, 100);

    session_id_t session("stats-session");
    auto [needs_sched, future] = send(dispatcher.get(),
        &manager_dispatcher_t::get_database_statistics, session);

    sched.enqueue(dispatcher.get());
    sched.stop();
    sched.enqueue(storage.get());
    sched.stop();
    sched.enqueue(storage.get());
    sched.stop();
    sched.enqueue(storage.get());
    sched.stop();
    send(dispatcher.get(), &manager_dispatcher_t::poll).second.detach();
    sched.enqueue(dispatcher.get());
    sched.stop();
    send(dispatcher.get(), &manager_dispatcher_t::poll).second.detach();
    sched.enqueue(dispatcher.get());
    sched.stop();
    send(dispatcher.get(), &manager_dispatcher_t::poll).second.detach();
    sched.enqueue(dispatcher.get());
    sched.stop();

    REQUIRE(future.is_ready());
    auto result = std::move(future).take_ready();
    // users(100) + orders(250) + products(50) = 400
    REQUIRE(result.total_size == 400);
    REQUIRE(result.collection_count == 3);
    REQUIRE(result.detail_info.find("stats:users+orders+products=400") != std::string::npos);

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("database: process_batch_buffer - move-only argument") {
    auto* resource = std::pmr::get_default_resource();
    g_log.log("\n========== TEST: process_batch_buffer ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    actor_zeta::test::scheduler_test_t sched(1, 100);

    session_id_t session("batch-session");
    auto batch = std::make_unique<std::vector<std::string>>();
    batch->push_back("item1");
    batch->push_back("item2");
    batch->push_back("item3");

    auto [needs_sched, future] = send(dispatcher.get(),
        &manager_dispatcher_t::process_batch_buffer, session, std::move(batch));

    sched.enqueue(dispatcher.get());
    sched.stop();

    REQUIRE(future.is_ready());
    auto result = std::move(future).take_ready();
    REQUIRE(!result.has_error);
    REQUIRE(result.committed);
    REQUIRE(result.total_rows == 3);

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("database: process_batch_buffer - empty batch") {
    auto* resource = std::pmr::get_default_resource();
    g_log.log("\n========== TEST: process_batch_buffer - empty batch ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    actor_zeta::test::scheduler_test_t sched(1, 100);

    session_id_t session("empty-batch-session");
    auto batch = std::make_unique<std::vector<std::string>>();  // Empty

    auto [needs_sched, future] = send(dispatcher.get(),
        &manager_dispatcher_t::process_batch_buffer, session, std::move(batch));

    sched.enqueue(dispatcher.get());
    sched.stop();

    REQUIRE(future.is_ready());
    auto result = std::move(future).take_ready();
    REQUIRE(result.has_error);
    REQUIRE(result.error_message == "Empty batch");

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("database: get_cached_value - promise direct manipulation") {
    auto* resource = std::pmr::get_default_resource();
    g_log.log("\n========== TEST: get_cached_value ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    actor_zeta::test::scheduler_test_t sched(1, 100);

    session_id_t session("cache-session");

    auto [needs_sched1, future1] = send(dispatcher.get(),
        &manager_dispatcher_t::get_cached_value, session, std::string("users"));
    sched.enqueue(dispatcher.get());
    sched.stop();
    REQUIRE(future1.is_ready());
    REQUIRE(std::move(future1).take_ready() == 100);

    auto [needs_sched2, future2] = send(dispatcher.get(),
        &manager_dispatcher_t::get_cached_value, session, std::string("orders"));
    sched.enqueue(dispatcher.get());
    sched.stop();
    REQUIRE(future2.is_ready());
    REQUIRE(std::move(future2).take_ready() == 250);

    auto [needs_sched3, future3] = send(dispatcher.get(),
        &manager_dispatcher_t::get_cached_value, session, std::string("products"));
    sched.enqueue(dispatcher.get());
    sched.stop();
    REQUIRE(future3.is_ready());
    REQUIRE(std::move(future3).take_ready() == 50);

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("database: execute_with_retry - success without retry") {
    auto* resource = std::pmr::get_default_resource();
    g_log.log("\n========== TEST: execute_with_retry - success ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    actor_zeta::test::scheduler_test_t sched(1, 100);

    session_id_t session("retry-session");
    // max_retries=0: no simulated failure.
    auto [needs_sched, future] = send(dispatcher.get(),
        &manager_dispatcher_t::execute_with_retry, session, std::string("users"), 0);

    sched.enqueue(dispatcher.get());
    sched.stop();
    sched.enqueue(storage.get());
    sched.stop();
    send(dispatcher.get(), &manager_dispatcher_t::poll).second.detach();
    sched.enqueue(dispatcher.get());
    sched.stop();

    REQUIRE(future.is_ready());
    auto result = std::move(future).take_ready();
    REQUIRE(!result.has_error);
    REQUIRE(result.size == 100);

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("database: execute_with_retry - retry after failure") {
    auto* resource = std::pmr::get_default_resource();
    g_log.log("\n========== TEST: execute_with_retry - retry ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    actor_zeta::test::scheduler_test_t sched(1, 100);

    session_id_t session("retry-session");
    // max_retries=1: the first attempt is made to fail, the retry succeeds.
    auto [needs_sched2, future] = send(dispatcher.get(),
        &manager_dispatcher_t::execute_with_retry, session, std::string("orders"), 1);

    sched.enqueue(dispatcher.get());
    sched.stop();
    sched.enqueue(storage.get());
    sched.stop();
    send(dispatcher.get(), &manager_dispatcher_t::poll).second.detach();
    sched.enqueue(dispatcher.get());
    sched.stop();

    REQUIRE(future.is_ready());
    auto result = std::move(future).take_ready();
    REQUIRE(!result.has_error);
    REQUIRE(result.size == 250);

    g_log.log("========== TEST PASSED ==========");
}
// The rows themselves are asserted, manager prefix included, so the forwarding
// hop is covered rather than merely reaching a handle.
TEST_CASE("dispatcher-pattern: fetch_row_batch forwards prefixed rows") {
    auto* resource = std::pmr::get_default_resource();

    g_log.log("\n========== TEST: fetch_row_batch forwards prefixed rows ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    actor_zeta::test::scheduler_test_t sched(1, 100);

    session_id_t session("session-batch-001");

    auto [needs_sched, future] = send(
        dispatcher.get(),
        &manager_dispatcher_t::fetch_row_batch,
        session,
        std::string("users"));

    sched.enqueue(dispatcher.get());   // starts the handler, suspends on the storage await
    sched.stop();
    sched.enqueue(storage.get());      // completes the storage future (flag-only)
    sched.stop();
    send(dispatcher.get(), &manager_dispatcher_t::poll).second.detach();
    sched.enqueue(dispatcher.get());
    sched.stop();

    REQUIRE(future.is_ready());
    auto rows = std::move(future).take_ready();

    REQUIRE(rows.size() == 10);
    REQUIRE(rows[0] == "[manager] row_0_from_test_db.users");
    REQUIRE(rows[9] == "[manager] row_9_from_test_db.users");

    g_log.log("========== TEST PASSED ==========");
}

TEST_CASE("dispatcher-pattern: fetch_row_batch on an empty collection name") {
    auto* resource = std::pmr::get_default_resource();

    g_log.log("\n========== TEST: fetch_row_batch empty collection ==========");

    auto storage = spawn<memory_storage_t>(resource, "Storage");
    auto dispatcher = spawn<manager_dispatcher_t>(resource, storage->address(), "Dispatcher");

    actor_zeta::test::scheduler_test_t sched(1, 100);

    session_id_t session("session-batch-002");

    auto [needs_sched, future] = send(
        dispatcher.get(),
        &manager_dispatcher_t::fetch_row_batch,
        session,
        std::string(""));

    // Rejected before any storage round trip, so one resume is enough.
    sched.enqueue(dispatcher.get());
    sched.stop();

    REQUIRE(future.is_ready());
    auto rows = std::move(future).take_ready();
    REQUIRE(rows.empty());

    g_log.log("========== TEST PASSED ==========");
}

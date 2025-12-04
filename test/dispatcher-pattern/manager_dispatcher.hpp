#pragma once

/// @file manager_dispatcher.hpp
/// @brief Dispatcher actor - middle level coordinator
///
/// manager_dispatcher_t demonstrates advanced coroutine patterns:
/// 1. Single co_await: size() waits for storage response
/// 2. Sequential co_await: execute_transaction() does two sequential queries
/// 3. Parallel requests + await all: aggregate_sizes() sends parallel requests
/// 4. Nested coroutines: get_aggregate_detail() called from aggregate_sizes()
/// 5. Branching based on results
/// 6. Pending coroutine management with poll_pending()
///
/// Type System Integration (from make_message.hpp, dispatch_traits.hpp):
/// - is_valid_rtt_type<T>: Validates message argument types
/// - dispatch_traits<&Method...>: Compile-time method->ID mapping
/// - msg_id<Actor, &Method>: Get message ID for method pointer
/// - dispatch(self, &method, msg): Unpack args and call method

#include <actor-zeta.hpp>
#include <actor-zeta/dispatch.hpp>

#include "common_types.hpp"
#include "test_logger.hpp"
#include "memory_storage.hpp"

#include <vector>
#include <unordered_map>
#include <string>

namespace dispatcher_test {

using namespace actor_zeta;

/// @brief Dispatcher actor - coordinates queries between client and storage
///
/// Architecture position: MIDDLE level
/// Receives: requests from client_t (or direct tests)
/// Sends: requests to memory_storage_t
/// Returns: results back to caller via unique_future
///
/// OLD (session-based) pattern:
///   - session_to_address_ map to track senders
///   - size() saves sender, sends fire-and-forget to storage
///   - size_finish() callback finds sender, sends response
///
/// NEW (promise/future) pattern:
///   - No session_to_address_ needed!
///   - size() returns unique_future<size_result_t>
///   - Uses co_await to wait for storage response
class manager_dispatcher_t final : public basic_actor<manager_dispatcher_t> {
public:
    explicit manager_dispatcher_t(
            pmr::memory_resource* mr,
            address_t memory_storage,
            const std::string& name)
        : basic_actor<manager_dispatcher_t>(mr)
        , memory_storage_(std::move(memory_storage))
        , name_(name) {
    }

    // =========================================================================
    // Simple methods
    // =========================================================================

    /// @brief Trigger behavior() to process pending coroutines
    void poll() {
        g_log.log("[%::poll] called", name_);
    }

    /// @brief Close and cleanup cursor for session
    void close_cursor(const session_id_t& session) {
        auto tid = thread_id_str();
        g_log.log("[%::close_cursor] thread=% session=%", name_, tid, session.data());

        auto it = result_storage_.find(session);
        if (it != result_storage_.end()) {
            g_log.log("[%::close_cursor] Removing cursor from storage", name_);
            result_storage_.erase(it);
        } else {
            g_log.log("[%::close_cursor] Cursor not found in storage", name_);
        }
    }

    // =========================================================================
    // PATTERN 1: Single co_await
    // Method waits for one response from storage
    // =========================================================================

    /// @brief Get collection size
    ///
    /// Demonstrates: Single co_await pattern
    /// - Validate input
    /// - Send request to storage
    /// - co_await response
    /// - Return result
    unique_future<size_result_t> size(
            session_id_t session,
            std::string database_name,
            std::string collection) {

        auto tid = thread_id_str();
        g_log.log("[%::size] thread=% session=% db=% coll=%",
                  name_, tid, session.data(), database_name, collection);

        // Branching: validate input
        if (database_name.empty() || collection.empty()) {
            g_log.log("[%::size] Error: empty database or collection name", name_);
            co_return size_result_t::error("Empty database or collection name");
        }

        g_log.log("[%::size] Sending request to memory_storage...", name_);

        // Single co_await - wait for storage response
        auto result = co_await send(
            memory_storage_,
            address(),
            &memory_storage_t::size,
            session,
            collection_full_name_t{database_name, collection});

        g_log.log("[%::size] Got result from storage: %", name_, result);

        co_return size_result_t(result);
    }

    // =========================================================================
    // PATTERN 2: Execute plan with cursor result
    // =========================================================================

    /// @brief Execute query plan and return cursor
    /// @param session Session ID (BY VALUE - message destroyed after co_await)
    unique_future<cursor_t_ptr> execute_plan(
            session_id_t session,
            logical_plan_t plan) {

        auto tid = thread_id_str();
        g_log.log("[%::execute_plan] thread=% session=% plan=%",
                  name_, tid, session.data(), plan.to_string());

        // Branching: validate plan
        if (plan.collection.database.empty()) {
            g_log.log("[%::execute_plan] Error: invalid plan", name_);
            auto error_cursor = std::make_unique<cursor_t>();
            error_cursor->has_error = true;
            error_cursor->error_message = "Invalid plan";
            co_return std::move(error_cursor);
        }

        g_log.log("[%::execute_plan] Sending request to memory_storage...", name_);

        auto cursor = co_await send(
            memory_storage_,
            address(),
            &memory_storage_t::execute_plan,
            session,
            std::move(plan));

        g_log.log("[%::execute_plan] Got cursor with % rows, error=%",
                  name_, cursor->row_count(), cursor->has_error);

        // Track cursor by session for later cleanup
        result_storage_[session] = cursor.get();

        co_return std::move(cursor);
    }

    // =========================================================================
    // PATTERN 3: Transaction - Sequential co_await
    // Multiple dependent operations in sequence
    // =========================================================================

    /// @brief Execute transaction with two sequential queries
    ///
    /// Demonstrates: Sequential co_await pattern
    /// - Step 1: Execute first query, co_await result
    /// - Check result, branch on error
    /// - Step 2: Execute second query, co_await result
    /// - Combine results
    ///
    /// IMPORTANT: Local variables may be corrupted after co_await!
    /// Save values you need BEFORE the next co_await.
    /// @param session Session ID (BY VALUE - message destroyed after co_await)
    unique_future<transaction_result_t> execute_transaction(
            session_id_t session,
            std::string collection1,
            std::string collection2) {

        auto tid = thread_id_str();
        g_log.log("[%::execute_transaction] thread=% session=% coll1=% coll2=%",
                  name_, tid, session.data(), collection1, collection2);

        // Validation
        if (collection1.empty() || collection2.empty()) {
            g_log.log("[%::execute_transaction] Error: empty collection name", name_);
            co_return transaction_result_t::error("Empty collection name");
        }

        g_log.log("[%::execute_transaction] Step 1: Execute plan for %", name_, collection1);

        // STEP 1: First query
        auto cursor1 = co_await send(
            memory_storage_,
            address(),
            &memory_storage_t::execute_plan,
            session,
            logical_plan_t("select", collection_full_name_t("test_db", collection1)));

        if (!cursor1) {
            g_log.log("[%::execute_transaction] ERROR: cursor1 is NULL after first co_await!", name_);
            co_return transaction_result_t::error("cursor1 is NULL");
        }

        // Branching: check step 1 result
        if (cursor1->has_error) {
            g_log.log("[%::execute_transaction] Step 1 FAILED: %", name_, cursor1->error_message);
            co_return transaction_result_t::error("Step 1 failed: " + cursor1->error_message);
        }

        g_log.log("[%::execute_transaction] Step 1 OK: % rows. Step 2: Execute plan for %",
                  name_, cursor1->row_count(), collection2);

        // IMPORTANT: Save cursor1 data BEFORE second co_await
        // cursor1 may be corrupted during second suspend
        std::size_t cursor1_row_count = cursor1->row_count();

        // STEP 2: Second query (depends on step 1 success)
        auto cursor2 = co_await send(
            memory_storage_,
            address(),
            &memory_storage_t::execute_plan,
            session,
            logical_plan_t("select", collection_full_name_t("test_db", collection2)));

        if (!cursor2) {
            g_log.log("[%::execute_transaction] ERROR: cursor2 is NULL!", name_);
            co_return transaction_result_t::error("cursor2 is NULL");
        }

        if (cursor2->has_error) {
            g_log.log("[%::execute_transaction] Step 2 FAILED: %", name_, cursor2->error_message);
            co_return transaction_result_t::error("Step 2 failed: " + cursor2->error_message);
        }

        g_log.log("[%::execute_transaction] Step 2 OK: % rows. COMMIT",
                  name_, cursor2->row_count());

        // Use saved row count from step 1
        std::size_t total = cursor1_row_count + cursor2->row_count();
        co_return transaction_result_t(total, true);
    }

    // =========================================================================
    // PATTERN 4: Aggregation - Parallel requests + Nested coroutine
    // =========================================================================

    /// @brief Aggregate sizes from multiple collections
    ///
    /// Demonstrates:
    /// - Parallel requests: send ALL requests before waiting
    /// - Await all: collect results in order
    /// - Nested coroutine: call get_aggregate_detail()
    /// - Branching based on total size
    ///
    /// @param session Session ID (BY VALUE - message destroyed after first suspend)
    /// @param collections Collection names to aggregate
    unique_future<aggregate_result_t> aggregate_sizes(
            session_id_t session,  // BY VALUE - critical for coroutine safety
            std::vector<std::string> collections) {

        auto tid = thread_id_str();
        g_log.log("[%::aggregate_sizes] thread=% session=% collections=%",
                  name_, tid, session.data(), collections.size());

        // A) Branching: validate input
        if (collections.empty()) {
            g_log.log("[%::aggregate_sizes] Error: no collections provided", name_);
            co_return aggregate_result_t::error("No collections provided");
        }

        // B) Parallel requests - send ALL without waiting
        g_log.log("[%::aggregate_sizes] Sending % parallel requests...", name_, collections.size());

        std::vector<unique_future<std::size_t>> futures;
        futures.reserve(collections.size());  // CRITICAL: reserve to avoid reallocation!

        for (const auto& coll : collections) {
            auto f = send(
                memory_storage_,
                address(),
                &memory_storage_t::size,
                session,
                collection_full_name_t("test_db", coll));
            futures.push_back(std::move(f));
        }

        // C) Await all - collect results in order
        g_log.log("[%::aggregate_sizes] Awaiting % futures...", name_, futures.size());

        std::size_t total = 0;
        for (std::size_t i = 0; i < futures.size(); ++i) {
            auto size = co_await std::move(futures[i]);
            g_log.log("[%::aggregate_sizes] Collection '%' size: %",
                      name_, collections[i], size);
            total += size;
        }

        g_log.log("[%::aggregate_sizes] Total: %. Calling nested coroutine...", name_, total);

        // D) Nested coroutine call
        auto detail = co_await get_aggregate_detail(session, total, collections.size());

        g_log.log("[%::aggregate_sizes] Done. Detail: %", name_, detail);

        co_return aggregate_result_t(total, collections.size(), std::move(detail));
    }

    /// @brief Nested coroutine - called from aggregate_sizes()
    ///
    /// Demonstrates:
    /// - Coroutine depth (coroutine calling coroutine)
    /// - Conditional co_await (only for large datasets)
    ///
    /// @param session Session ID (BY VALUE - coroutine may outlive caller)
    unique_future<std::string> get_aggregate_detail(
            session_id_t session,  // BY VALUE
            std::size_t total,
            std::size_t count) {

        auto tid = thread_id_str();
        g_log.log("[%::get_aggregate_detail] thread=% session=% total=% count=%",
                  name_, tid, session.data(), total, count);

        std::string detail;

        // Branching inside nested coroutine
        if (total > 200) {
            g_log.log("[%::get_aggregate_detail] Large dataset, getting extra info...", name_);

            // Additional co_await inside nested coroutine
            auto extra_size = co_await send(
                memory_storage_,
                address(),
                &memory_storage_t::size,
                session,
                collection_full_name_t("test_db", "users"));

            detail = "large_dataset:total=" + std::to_string(total) +
                     ",count=" + std::to_string(count) +
                     ",users=" + std::to_string(extra_size);
        } else {
            detail = "small_dataset:total=" + std::to_string(total) +
                     ",count=" + std::to_string(count);
        }

        g_log.log("[%::get_aggregate_detail] Returning: %", name_, detail);
        co_return detail;
    }

    // =========================================================================
    // PATTERN 5: Lambda INSIDE actor method
    // =========================================================================

    /// @brief Pattern 5a: Simple lambda inside method
    /// Lambda is defined and used entirely within the method
    unique_future<int> transform_with_lambda(int value, int factor) {
        auto tid = thread_id_str();
        g_log.log("[%::transform_with_lambda] value=% factor=%", name_, value, factor);

        // Lambda defined INSIDE actor method
        auto transform = [](int v, int f) {
            return v * f + 100;
        };

        int result = transform(value, factor);
        g_log.log("[%::transform_with_lambda] result=%", name_, result);
        co_return result;
    }

    /// @brief Pattern 5b: Lambda capturing `this` to access actor state
    /// Lambda can modify actor member variables
    unique_future<std::string> compute_with_lambda_and_state(std::string prefix) {
        auto tid = thread_id_str();
        g_log.log("[%::compute_with_lambda_and_state] prefix=%", name_, prefix);

        // Lambda captures `this` to access actor state (name_)
        auto format_with_name = [this](const std::string& p) {
            return p + "_from_" + name_;
        };

        std::string result = format_with_name(prefix);
        g_log.log("[%::compute_with_lambda_and_state] result=%", name_, result);
        co_return result;
    }

    /// @brief Pattern 5c: Lambda + coroutine - use lambda after co_await
    /// Lambda processes result from async operation
    ///
    /// IMPORTANT: Parameters must be BY VALUE if used after co_await!
    /// References become invalid after first suspend.
    ///
    /// @param session Session ID (BY VALUE - message destroyed after first suspend)
    /// @param collection Collection name (BY VALUE - must survive co_await!)
    unique_future<std::string> async_transform_with_lambda(
            session_id_t session,       // BY VALUE - survives co_await
            std::string collection) {   // BY VALUE - survives co_await

        auto tid = thread_id_str();
        g_log.log("[%::async_transform_with_lambda] session=% collection=%",
                  name_, session.data(), collection);

        // Lambda to transform result after async operation
        auto format_result = [this](std::size_t size, const std::string& coll) {
            return "Collection " + coll + " in " + name_ + " has " + std::to_string(size) + " items";
        };

        // co_await - wait for storage response
        auto size = co_await send(
            memory_storage_,
            address(),
            &memory_storage_t::size,
            session,
            collection_full_name_t("test_db", collection));

        // Use lambda to process result AFTER co_await
        // collection is valid here because it was passed BY VALUE
        std::string result = format_result(size, collection);
        g_log.log("[%::async_transform_with_lambda] result=%", name_, result);
        co_return result;
    }

    /// @brief Pattern 5d: Lambda that IS a coroutine (has co_await inside)
    ///
    /// CRITICAL: Lambda-coroutine needs resource as FIRST parameter!
    /// The promise extracts resource from first arg via extract_resource_impl().
    ///
    /// @param session Session ID (BY VALUE)
    /// @param collection Collection name (BY VALUE)
    /// @param multiplier Multiplier for result (BY VALUE)
    unique_future<int> execute_with_coroutine_lambda(
            session_id_t session,
            std::string collection,
            int multiplier) {

        auto tid = thread_id_str();
        g_log.log("[%::execute_with_coroutine_lambda] session=% collection=% mult=%",
                  name_, session.data(), collection, multiplier);

        // Lambda that IS a coroutine - has co_await/co_return INSIDE
        // CRITICAL: First parameter MUST be pmr::memory_resource* for promise!
        // Capture everything else BY VALUE (not by reference!)
        auto async_get_size = [this,
                               session_copy = std::move(session),         // MOVE
                               collection_copy = std::move(collection),   // MOVE
                               multiplier_copy = multiplier               // copy (int is cheap)
                              ](pmr::memory_resource* /*res*/) -> unique_future<int> {
            // ^^ resource passed as first param for promise_type constructor

            g_log.log("[%::coroutine_lambda] Starting async operation...", name_);

            // co_await INSIDE lambda - this makes lambda a coroutine
            auto size = co_await send(
                memory_storage_,
                address(),
                &memory_storage_t::size,
                session_copy,
                collection_full_name_t("test_db", collection_copy));

            g_log.log("[%::coroutine_lambda] Got size=%, applying multiplier=%",
                      name_, size, multiplier_copy);

            // co_return INSIDE lambda
            co_return static_cast<int>(size) * multiplier_copy;
        };

        // Execute the lambda-coroutine - pass resource() as first argument!
        g_log.log("[%::execute_with_coroutine_lambda] Calling lambda-coroutine...", name_);
        auto result_future = async_get_size(resource());

        // co_await the result of lambda-coroutine
        int result = co_await std::move(result_future);

        g_log.log("[%::execute_with_coroutine_lambda] Lambda-coroutine returned: %", name_, result);
        co_return result;
    }

    // =========================================================================
    // Extended database operations with lambda-coroutines
    // =========================================================================

    /// @brief Create cursor from async query - lambda-coroutine returns unique_ptr
    unique_future<cursor_t_ptr> create_cursor_from_query(
            session_id_t session,
            std::string collection) {
        auto build_cursor = [this, s = std::move(session), coll = std::move(collection)]
                (pmr::memory_resource*) -> unique_future<cursor_t_ptr> {
            auto size = co_await send(memory_storage_, address(), &memory_storage_t::size,
                s, collection_full_name_t("test_db", coll));
            auto cursor = std::make_unique<cursor_t>();
            for (std::size_t i = 0; i < std::min(size, std::size_t(10)); ++i)
                cursor->data.push_back("row_" + std::to_string(i) + "_from_" + coll);
            g_log.log("[%::create_cursor_from_query] Built cursor with % rows", name_, cursor->row_count());
            co_return std::move(cursor);
        };
        co_return co_await build_cursor(resource());
    }

    /// @brief Validate query then execute - chained lambda-coroutines
    unique_future<cursor_t_ptr> validate_and_execute(
            session_id_t session,
            logical_plan_t plan) {
        // Step 1: Validate plan
        auto validate = [](pmr::memory_resource*, const logical_plan_t& p) -> unique_future<bool> {
            co_return !p.collection.database.empty() && !p.collection.collection.empty();
        };
        // Step 2: Execute if valid
        auto execute = [this, &validate, s = std::move(session)]
                (pmr::memory_resource* res, logical_plan_t p) -> unique_future<cursor_t_ptr> {
            bool is_valid = co_await validate(res, p);
            if (!is_valid) {
                auto err = std::make_unique<cursor_t>();
                err->has_error = true;
                err->error_message = "Validation failed";
                co_return std::move(err);
            }
            auto cursor = co_await send(memory_storage_, address(),
                &memory_storage_t::execute_plan, s, std::move(p));
            co_return std::move(cursor);
        };
        co_return co_await execute(resource(), std::move(plan));
    }

    /// @brief Get statistics across all collections - parallel lambda-coroutines
    unique_future<aggregate_result_t> get_database_statistics(session_id_t session) {
        auto fetch_size = [this, s = session](pmr::memory_resource*, std::string coll)
                -> unique_future<std::size_t> {
            co_return co_await send(memory_storage_, address(), &memory_storage_t::size,
                s, collection_full_name_t("test_db", coll));
        };
        // Parallel fetch all collection sizes
        std::vector<unique_future<std::size_t>> futures;
        futures.reserve(3);
        futures.push_back(fetch_size(resource(), "users"));
        futures.push_back(fetch_size(resource(), "orders"));
        futures.push_back(fetch_size(resource(), "products"));

        std::size_t total = 0;
        for (auto& f : futures)
            total += co_await std::move(f);

        aggregate_result_t result;
        result.total_size = total;
        result.collection_count = 3;
        result.detail_info = "stats:users+orders+products=" + std::to_string(total);
        co_return result;
    }

    /// @brief Process batch buffer with move-only data
    unique_future<transaction_result_t> process_batch_buffer(
            session_id_t session,
            std::unique_ptr<std::vector<std::string>> batch) {
        auto process = [this, s = std::move(session), b = std::move(batch)]
                (pmr::memory_resource*) -> unique_future<transaction_result_t> {
            transaction_result_t result;
            if (!b || b->empty()) {
                result.has_error = true;
                result.error_message = "Empty batch";
                co_return result;
            }
            g_log.log("[%::process_batch_buffer] Processing % items", name_, b->size());
            result.committed = true;
            result.total_rows = static_cast<int>(b->size());
            co_return result;
        };
        co_return co_await process(resource());
    }

    /// @brief Get cached value - ready immediately via promise
    unique_future<std::size_t> get_cached_value(
            session_id_t session,
            std::string collection) {
        // Simulate cache hit - value is ready immediately
        promise<std::size_t> cache_promise(resource());
        auto future = cache_promise.get_future();

        // Lookup in "cache" (simulated)
        std::size_t cached = 0;
        if (collection == "users") cached = 100;
        else if (collection == "orders") cached = 250;
        else if (collection == "products") cached = 50;

        cache_promise.set_value(cached);
        g_log.log("[%::get_cached_value] Cache hit for %: %", name_, collection, cached);
        co_return co_await std::move(future);
    }

    /// @brief Execute with retry on error
    unique_future<size_result_t> execute_with_retry(
            session_id_t session,
            std::string collection,
            int max_retries) {
        auto try_fetch = [this, s = session, coll = collection]
                (pmr::memory_resource*, bool simulate_error) -> unique_future<size_result_t> {
            if (simulate_error)
                co_return size_result_t::error("connection_timeout");
            auto size = co_await send(memory_storage_, address(), &memory_storage_t::size,
                s, collection_full_name_t("test_db", coll));
            co_return size_result_t(size);
        };
        // First attempt fails if max_retries > 0 (simulate retry scenario)
        auto result = co_await try_fetch(resource(), max_retries > 0);
        if (result.has_error && max_retries > 0) {
            g_log.log("[%::execute_with_retry] Retry after error: %", name_, result.error_message);
            result = co_await try_fetch(resource(), false);  // Retry succeeds
        }
        co_return result;
    }

    // =========================================================================
    // dispatch_traits - compile-time method->ID mapping
    // =========================================================================

    using dispatch_traits = actor_zeta::dispatch_traits<
        &manager_dispatcher_t::poll,
        &manager_dispatcher_t::size,
        &manager_dispatcher_t::execute_plan,
        &manager_dispatcher_t::close_cursor,
        &manager_dispatcher_t::execute_transaction,
        &manager_dispatcher_t::aggregate_sizes,
        &manager_dispatcher_t::get_aggregate_detail,
        &manager_dispatcher_t::transform_with_lambda,
        &manager_dispatcher_t::compute_with_lambda_and_state,
        &manager_dispatcher_t::async_transform_with_lambda,
        &manager_dispatcher_t::execute_with_coroutine_lambda,
        // Extended database operations
        &manager_dispatcher_t::create_cursor_from_query,
        &manager_dispatcher_t::validate_and_execute,
        &manager_dispatcher_t::get_database_statistics,
        &manager_dispatcher_t::process_batch_buffer,
        &manager_dispatcher_t::get_cached_value,
        &manager_dispatcher_t::execute_with_retry
    >;

    // =========================================================================
    // behavior() - message dispatch with pending coroutine management
    // =========================================================================

    unique_future<void> behavior(mailbox::message* msg) {
        auto tid = thread_id_str();
        g_log.log("[%::behavior] thread=% command=%", name_, tid, msg->command());

        switch (msg->command()) {
            case msg_id<manager_dispatcher_t, &manager_dispatcher_t::poll>:
                poll();
                break;
            case msg_id<manager_dispatcher_t, &manager_dispatcher_t::size>: {
                auto future = dispatch(this, &manager_dispatcher_t::size, msg);
                if (!future.available()) {
                    g_log.log("[%::behavior] size() suspended, storing pending", name_);
                    pending_size_.push_back(std::move(future));
                }
                break;
            }
            case msg_id<manager_dispatcher_t, &manager_dispatcher_t::execute_plan>: {
                auto future = dispatch(this, &manager_dispatcher_t::execute_plan, msg);
                if (!future.available()) {
                    g_log.log("[%::behavior] execute_plan() suspended, storing pending", name_);
                    pending_execute_.push_back(std::move(future));
                }
                break;
            }
            case msg_id<manager_dispatcher_t, &manager_dispatcher_t::close_cursor>:
                dispatch(this, &manager_dispatcher_t::close_cursor, msg);
                break;
            case msg_id<manager_dispatcher_t, &manager_dispatcher_t::execute_transaction>: {
                auto future = dispatch(this, &manager_dispatcher_t::execute_transaction, msg);
                if (!future.available()) {
                    g_log.log("[%::behavior] execute_transaction() suspended, storing pending", name_);
                    pending_transaction_.push_back(std::move(future));
                }
                break;
            }
            case msg_id<manager_dispatcher_t, &manager_dispatcher_t::aggregate_sizes>: {
                auto future = dispatch(this, &manager_dispatcher_t::aggregate_sizes, msg);
                if (!future.available()) {
                    g_log.log("[%::behavior] aggregate_sizes() suspended, storing pending", name_);
                    pending_aggregate_.push_back(std::move(future));
                }
                break;
            }
            case msg_id<manager_dispatcher_t, &manager_dispatcher_t::get_aggregate_detail>: {
                auto future = dispatch(this, &manager_dispatcher_t::get_aggregate_detail, msg);
                if (!future.available()) {
                    g_log.log("[%::behavior] get_aggregate_detail() suspended, storing pending", name_);
                    pending_detail_.push_back(std::move(future));
                }
                break;
            }
            // PATTERN 5: Lambda methods
            case msg_id<manager_dispatcher_t, &manager_dispatcher_t::transform_with_lambda>: {
                auto future = dispatch(this, &manager_dispatcher_t::transform_with_lambda, msg);
                if (!future.available()) {
                    g_log.log("[%::behavior] transform_with_lambda() suspended, storing pending", name_);
                    pending_transform_.push_back(std::move(future));
                }
                break;
            }
            case msg_id<manager_dispatcher_t, &manager_dispatcher_t::compute_with_lambda_and_state>: {
                auto future = dispatch(this, &manager_dispatcher_t::compute_with_lambda_and_state, msg);
                if (!future.available()) {
                    g_log.log("[%::behavior] compute_with_lambda_and_state() suspended, storing pending", name_);
                    pending_detail_.push_back(std::move(future));
                }
                break;
            }
            case msg_id<manager_dispatcher_t, &manager_dispatcher_t::async_transform_with_lambda>: {
                auto future = dispatch(this, &manager_dispatcher_t::async_transform_with_lambda, msg);
                if (!future.available()) {
                    g_log.log("[%::behavior] async_transform_with_lambda() suspended, storing pending", name_);
                    pending_detail_.push_back(std::move(future));
                }
                break;
            }
            case msg_id<manager_dispatcher_t, &manager_dispatcher_t::execute_with_coroutine_lambda>: {
                auto future = dispatch(this, &manager_dispatcher_t::execute_with_coroutine_lambda, msg);
                if (!future.available()) {
                    g_log.log("[%::behavior] execute_with_coroutine_lambda() suspended, storing pending", name_);
                    pending_transform_.push_back(std::move(future));
                }
                break;
            }
            // Extended database operations
            case msg_id<manager_dispatcher_t, &manager_dispatcher_t::create_cursor_from_query>: {
                auto future = dispatch(this, &manager_dispatcher_t::create_cursor_from_query, msg);
                if (!future.available()) pending_execute_.push_back(std::move(future));
                break;
            }
            case msg_id<manager_dispatcher_t, &manager_dispatcher_t::validate_and_execute>: {
                auto future = dispatch(this, &manager_dispatcher_t::validate_and_execute, msg);
                if (!future.available()) pending_execute_.push_back(std::move(future));
                break;
            }
            case msg_id<manager_dispatcher_t, &manager_dispatcher_t::get_database_statistics>: {
                auto future = dispatch(this, &manager_dispatcher_t::get_database_statistics, msg);
                if (!future.available()) pending_aggregate_.push_back(std::move(future));
                break;
            }
            case msg_id<manager_dispatcher_t, &manager_dispatcher_t::process_batch_buffer>: {
                auto future = dispatch(this, &manager_dispatcher_t::process_batch_buffer, msg);
                if (!future.available()) pending_transaction_.push_back(std::move(future));
                break;
            }
            case msg_id<manager_dispatcher_t, &manager_dispatcher_t::get_cached_value>: {
                // get_cached_value uses promise direct manipulation - always ready immediately
                dispatch(this, &manager_dispatcher_t::get_cached_value, msg);
                break;
            }
            case msg_id<manager_dispatcher_t, &manager_dispatcher_t::execute_with_retry>: {
                auto future = dispatch(this, &manager_dispatcher_t::execute_with_retry, msg);
                if (!future.available()) pending_size_.push_back(std::move(future));
                break;
            }
            default:
                g_log.log("[%::behavior] Unknown command!", name_);
                break;
        }

        // Process pending coroutines after each message
        poll_pending();

        return make_ready_future_void(resource());
    }

    // =========================================================================
    // Pending coroutine management
    // =========================================================================

    bool has_pending() const {
        return !pending_size_.empty() ||
               !pending_execute_.empty() ||
               !pending_transaction_.empty() ||
               !pending_aggregate_.empty() ||
               !pending_detail_.empty();
    }

    /// @brief Recursively resume awaiting chain
    ///
    /// KEY INSIGHT: The coroutine handle is stored in the AWAITED state, not the awaiter's state!
    ///
    /// When coroutine X does `co_await future_Y`:
    /// - X's coroutine handle is set on Y's state (via awaiter::await_suspend)
    /// - X's state gets awaiting_on = Y's state
    ///
    /// So: state.awaiting_on.coro_handle_ = the coroutine that's waiting on awaiting_on
    ///
    /// Chain example:
    /// - outer_state.awaiting_on -> lambda_state (holds outer's coroutine)
    /// - lambda_state.awaiting_on -> storage_state (holds lambda's coroutine)
    ///
    /// When storage_state is ready:
    /// 1. Resume storage_state's coroutine (which is lambda's coroutine)
    /// 2. Lambda completes, lambda_state becomes ready
    /// 3. Resume lambda_state's coroutine (which is outer's coroutine)
    ///
    /// @return true if any coroutine was resumed
    bool resume_awaiting_chain(detail::future_state_base* state) {
        if (!state) return false;

        auto* awaiting = state->get_awaiting_on();
        if (!awaiting) return false;

        // If awaiting is NOT ready, check if IT is waiting on something ready
        if (!awaiting->is_ready() && awaiting->get_awaiting_on()) {
            // Recurse: try to resume awaiting's awaiting first
            if (resume_awaiting_chain(awaiting)) {
                // After recursion, check if awaiting is now ready
                // If so, resume the coroutine stored in awaiting (which is state's coroutine)
                if (awaiting->is_ready() && awaiting->has_coroutine()) {
                    g_log.log("[%::resume_awaiting_chain] Inner completed, resuming from awaiting", name_);
                    awaiting->resume_coroutine();
                    return true;
                }
                return true;  // Something was resumed in recursion
            }
        }

        // Base case: awaiting is ready, resume the coroutine stored IN awaiting
        // (which is the coroutine that was waiting on awaiting)
        if (awaiting->is_ready() && awaiting->has_coroutine()) {
            g_log.log("[%::resume_awaiting_chain] Awaiting ready, resuming coroutine from awaiting", name_);
            awaiting->resume_coroutine();
            return true;
        }

        return false;
    }

    /// @brief Poll and resume pending coroutines
    void poll_pending() {
        // Poll size futures
        for (auto it = pending_size_.begin(); it != pending_size_.end();) {
            auto* state = it->get_state();
            if (state) {
                resume_awaiting_chain(state);
            }
            if (it->awaiting_ready()) {
                g_log.log("[%::poll_pending] size awaiting ready, resuming coroutine", name_);
                it->resume();
                if (it->available()) {
                    g_log.log("[%::poll_pending] size coroutine completed", name_);
                    it = pending_size_.erase(it);
                    continue;
                }
            }
            ++it;
        }
        // Poll execute futures
        for (auto it = pending_execute_.begin(); it != pending_execute_.end();) {
            auto* state = it->get_state();
            if (state) {
                resume_awaiting_chain(state);
            }
            if (it->awaiting_ready()) {
                g_log.log("[%::poll_pending] execute awaiting ready, resuming coroutine", name_);
                it->resume();
                if (it->available()) {
                    g_log.log("[%::poll_pending] execute coroutine completed", name_);
                    it = pending_execute_.erase(it);
                    continue;
                }
            }
            ++it;
        }
        // Poll transaction futures
        for (auto it = pending_transaction_.begin(); it != pending_transaction_.end();) {
            auto* state = it->get_state();
            if (state) {
                resume_awaiting_chain(state);
            }
            if (it->awaiting_ready()) {
                g_log.log("[%::poll_pending] transaction awaiting ready, resuming coroutine", name_);
                it->resume();
                if (it->available()) {
                    g_log.log("[%::poll_pending] transaction coroutine completed", name_);
                    it = pending_transaction_.erase(it);
                    continue;
                }
            }
            ++it;
        }
        // Poll aggregate futures
        for (auto it = pending_aggregate_.begin(); it != pending_aggregate_.end();) {
            auto* state = it->get_state();
            if (state) {
                resume_awaiting_chain(state);
            }
            if (it->awaiting_ready()) {
                g_log.log("[%::poll_pending] aggregate awaiting ready, resuming coroutine", name_);
                it->resume();
                if (it->available()) {
                    g_log.log("[%::poll_pending] aggregate coroutine completed", name_);
                    it = pending_aggregate_.erase(it);
                    continue;
                }
            }
            ++it;
        }
        // Poll detail futures
        for (auto it = pending_detail_.begin(); it != pending_detail_.end();) {
            auto* state = it->get_state();
            if (state) {
                resume_awaiting_chain(state);
            }
            if (it->awaiting_ready()) {
                g_log.log("[%::poll_pending] detail awaiting ready, resuming coroutine", name_);
                it->resume();
                if (it->available()) {
                    g_log.log("[%::poll_pending] detail coroutine completed", name_);
                    it = pending_detail_.erase(it);
                    continue;
                }
            }
            ++it;
        }
        // Poll transform futures (lambda-coroutines)
        for (auto it = pending_transform_.begin(); it != pending_transform_.end();) {
            auto* state = it->get_state();
            if (state) {
                resume_awaiting_chain(state);
            }
            if (it->awaiting_ready()) {
                g_log.log("[%::poll_pending] transform awaiting ready, resuming coroutine", name_);
                it->resume();
                if (it->available()) {
                    g_log.log("[%::poll_pending] transform coroutine completed", name_);
                    it = pending_transform_.erase(it);
                    continue;
                }
            }
            ++it;
        }
    }

    const std::string& name() const { return name_; }

    ~manager_dispatcher_t() = default;

private:
    address_t memory_storage_;
    std::string name_;

    // Pending coroutines by type
    std::vector<unique_future<size_result_t>> pending_size_;
    std::vector<unique_future<cursor_t_ptr>> pending_execute_;
    std::vector<unique_future<transaction_result_t>> pending_transaction_;
    std::vector<unique_future<aggregate_result_t>> pending_aggregate_;
    std::vector<unique_future<std::string>> pending_detail_;
    std::vector<unique_future<int>> pending_transform_;  // for transform_with_lambda

    // Cursor tracking by session
    std::unordered_map<session_id_t, cursor_t*, session_id_hash> result_storage_;
};

} // namespace dispatcher_test
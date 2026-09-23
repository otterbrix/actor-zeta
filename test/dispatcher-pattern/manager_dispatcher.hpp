#pragma once

/// @file manager_dispatcher.hpp
/// Middle-level actor of the dispatcher-pattern test. Every handler returns
/// unique_future<T> and co_awaits storage directly -- single, sequential, parallel
/// and nested awaits, plus lambda-coroutines -- so there is no sender map and no
/// *_finish() callback hop.

#include <actor-zeta.hpp>

#include "common_types.hpp"
#include "memory_storage.hpp"
#include "test_logger.hpp"

#include <vector>
#include <atomic>
#include <unordered_map>
#include <string>

namespace dispatcher_test {

using namespace actor_zeta;

class manager_dispatcher_t final : public basic_actor<manager_dispatcher_t> {
public:
    /// Holds an address_t: all a peer needs to send. Launching actors is the supervisor's job.
    explicit manager_dispatcher_t(
            std::pmr::memory_resource* mr,
            address_t memory_storage,
            const std::string& name)
        : basic_actor<manager_dispatcher_t>(mr)
        , memory_storage_(std::move(memory_storage))
        , name_(name) {
    }

    /// send() hands back an obligation this actor cannot discharge -- it has an
    /// address, not a scheduler. Dropping it would strand storage for good, so it
    /// is recorded here and the supervisor driving this test claims it below.
    void owe_storage(bool needs_sched) {
        if (needs_sched) {
            storage_owed_.fetch_add(1, std::memory_order_release);
        }
    }

    /// Claimed once by the owner, which then enqueues storage.
    std::size_t take_storage_obligations() {
        return storage_owed_.exchange(0, std::memory_order_acq_rel);
    }

    /// Returns whether a cursor was actually removed, so a test can tell the work
    /// from a handler that merely ran to completion.
    unique_future<bool> close_cursor(session_id_t session) {
        auto tid = thread_id_str();
        g_log.log("[%::close_cursor] thread=% session=%", name_, tid, session.data());

        auto it = result_storage_.find(session);
        if (it == result_storage_.end()) {
            g_log.log("[%::close_cursor] Cursor not found in storage", name_);
            co_return false;
        }

        g_log.log("[%::close_cursor] Removing cursor from storage", name_);
        result_storage_.erase(it);
        co_return true;
    }

    unique_future<size_result_t> size(
            session_id_t session,
            std::string database_name,
            std::string collection) {

        auto tid = thread_id_str();
        g_log.log("[%::size] thread=% session=% db=% coll=%",
                  name_, tid, session.data(), database_name, collection);

        if (database_name.empty() || collection.empty()) {
            g_log.log("[%::size] Error: empty database or collection name", name_);
            co_return size_result_t::error("Empty database or collection name");
        }

        g_log.log("[%::size] Sending request to memory_storage...", name_);

        auto sent_result = send(memory_storage_,
            &memory_storage_t::size,
            session,
            collection_full_name_t{database_name, collection});
        owe_storage(sent_result.first);
        auto result = co_await std::move(sent_result.second);

        g_log.log("[%::size] Got result from storage: %", name_, result);

        co_return size_result_t(result);
    }

    /// Parameters by value: the message they came from is gone after the co_await.
    unique_future<cursor_t_ptr> execute_plan(
            session_id_t session,
            logical_plan_t plan) {

        auto tid = thread_id_str();
        g_log.log("[%::execute_plan] thread=% session=% plan=%",
                  name_, tid, session.data(), plan.to_string());

        if (plan.collection.database.empty()) {
            g_log.log("[%::execute_plan] Error: invalid plan", name_);
            auto error_cursor = std::make_unique<cursor_t>();
            error_cursor->has_error = true;
            error_cursor->error_message = "Invalid plan";
            co_return std::move(error_cursor);
        }

        g_log.log("[%::execute_plan] Sending request to memory_storage...", name_);

        auto sent_cursor = send(memory_storage_,
            &memory_storage_t::execute_plan,
            session,
            std::move(plan));
        owe_storage(sent_cursor.first);
        auto cursor = co_await std::move(sent_cursor.second);

        g_log.log("[%::execute_plan] Got cursor with % rows, error=%",
                  name_, cursor->row_count(), cursor->has_error);

        result_storage_[session] = cursor.get();

        co_return std::move(cursor);
    }

    /// Two sequential awaits, the second issued only if the first succeeded.
    unique_future<transaction_result_t> execute_transaction(
            session_id_t session,
            std::string collection1,
            std::string collection2) {

        auto tid = thread_id_str();
        g_log.log("[%::execute_transaction] thread=% session=% coll1=% coll2=%",
                  name_, tid, session.data(), collection1, collection2);

        if (collection1.empty() || collection2.empty()) {
            g_log.log("[%::execute_transaction] Error: empty collection name", name_);
            co_return transaction_result_t::error("Empty collection name");
        }

        g_log.log("[%::execute_transaction] Step 1: Execute plan for %", name_, collection1);

        auto sent_cursor1 = send(memory_storage_,
            &memory_storage_t::execute_plan,
            session,
            logical_plan_t("select", collection_full_name_t("test_db", collection1)));
        owe_storage(sent_cursor1.first);
        auto cursor1 = co_await std::move(sent_cursor1.second);

        if (!cursor1) {
            g_log.log("[%::execute_transaction] ERROR: cursor1 is NULL after first co_await!", name_);
            co_return transaction_result_t::error("cursor1 is NULL");
        }

        if (cursor1->has_error) {
            g_log.log("[%::execute_transaction] Step 1 FAILED: %", name_, cursor1->error_message);
            co_return transaction_result_t::error("Step 1 failed: " + cursor1->error_message);
        }

        g_log.log("[%::execute_transaction] Step 1 OK: % rows. Step 2: Execute plan for %",
                  name_, cursor1->row_count(), collection2);

        std::size_t cursor1_row_count = cursor1->row_count();

        auto sent_cursor2 = send(memory_storage_,
            &memory_storage_t::execute_plan,
            session,
            logical_plan_t("select", collection_full_name_t("test_db", collection2)));
        owe_storage(sent_cursor2.first);
        auto cursor2 = co_await std::move(sent_cursor2.second);

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

        std::size_t total = cursor1_row_count + cursor2->row_count();
        co_return transaction_result_t(total, true);
    }

    /// Sends ALL requests before awaiting any, then awaits a nested coroutine.
    unique_future<aggregate_result_t> aggregate_sizes(
            session_id_t session,
            std::vector<std::string> collections) {

        auto tid = thread_id_str();
        g_log.log("[%::aggregate_sizes] thread=% session=% collections=%",
                  name_, tid, session.data(), collections.size());

        if (collections.empty()) {
            g_log.log("[%::aggregate_sizes] Error: no collections provided", name_);
            co_return aggregate_result_t::error("No collections provided");
        }

        g_log.log("[%::aggregate_sizes] Sending % parallel requests...", name_, collections.size());

        std::vector<unique_future<std::size_t>> futures;
        futures.reserve(collections.size());

        for (const auto& coll : collections) {

            auto sent = send(memory_storage_,
                &memory_storage_t::size,
                session,
                collection_full_name_t("test_db", coll));
            owe_storage(sent.first);
            futures.push_back(std::move(sent.second));
        }

        g_log.log("[%::aggregate_sizes] Awaiting % futures...", name_, futures.size());

        std::size_t total = 0;
        for (std::size_t i = 0; i < futures.size(); ++i) {
            auto size = co_await std::move(futures[i]);
            g_log.log("[%::aggregate_sizes] Collection '%' size: %",
                      name_, collections[i], size);
            total += size;
        }

        g_log.log("[%::aggregate_sizes] Total: %. Calling nested coroutine...", name_, total);

        auto detail = co_await get_aggregate_detail(session, total, collections.size());

        g_log.log("[%::aggregate_sizes] Done. Detail: %", name_, detail);

        co_return aggregate_result_t(total, collections.size(), std::move(detail));
    }

    /// Nested: awaited from aggregate_sizes(), with a co_await only on one branch.
    unique_future<std::string> get_aggregate_detail(
            session_id_t session,
            std::size_t total,
            std::size_t count) {

        auto tid = thread_id_str();
        g_log.log("[%::get_aggregate_detail] thread=% session=% total=% count=%",
                  name_, tid, session.data(), total, count);

        std::string detail;

        if (total > 200) {
            g_log.log("[%::get_aggregate_detail] Large dataset, getting extra info...", name_);

            auto sent_extra_size = send(memory_storage_,
                &memory_storage_t::size,
                session,
                collection_full_name_t("test_db", "users"));
            owe_storage(sent_extra_size.first);
            auto extra_size = co_await std::move(sent_extra_size.second);

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

    unique_future<int> transform_with_lambda(int value, int factor) {
        auto tid = thread_id_str();
        g_log.log("[%::transform_with_lambda] value=% factor=%", name_, value, factor);

        auto transform = [](int v, int f) {
            return v * f + 100;
        };

        int result = transform(value, factor);
        g_log.log("[%::transform_with_lambda] result=%", name_, result);
        co_return result;
    }

    unique_future<std::string> compute_with_lambda_and_state(std::string prefix) {
        auto tid = thread_id_str();
        g_log.log("[%::compute_with_lambda_and_state] prefix=%", name_, prefix);

        auto format_with_name = [this](const std::string& p) {
            return p + "_from_" + name_;
        };

        std::string result = format_with_name(prefix);
        g_log.log("[%::compute_with_lambda_and_state] result=%", name_, result);
        co_return result;
    }

    /// `collection` is used after the co_await, so it is by value: a reference
    /// into the message would dangle once the frame suspends.
    unique_future<std::string> async_transform_with_lambda(
            session_id_t session,
            std::string collection) {

        auto tid = thread_id_str();
        g_log.log("[%::async_transform_with_lambda] session=% collection=%",
                  name_, session.data(), collection);

        auto format_result = [this](std::size_t size, const std::string& coll) {
            return "Collection " + coll + " in " + name_ + " has " + std::to_string(size) + " items";
        };

        auto sent_size = send(memory_storage_,
            &memory_storage_t::size,
            session,
            collection_full_name_t("test_db", collection));
        owe_storage(sent_size.first);
        auto size = co_await std::move(sent_size.second);

        std::string result = format_result(size, collection);
        g_log.log("[%::async_transform_with_lambda] result=%", name_, result);
        co_return result;
    }

    /// A lambda that is itself a coroutine. Its first parameter must be the
    /// memory_resource*: the promise takes its allocator from the first argument
    /// (extract_resource_impl()), and a lambda has no actor `this` to take it from.
    /// Everything else is captured by value.
    unique_future<int> execute_with_coroutine_lambda(
            session_id_t session,
            std::string collection,
            int multiplier) {

        auto tid = thread_id_str();
        g_log.log("[%::execute_with_coroutine_lambda] session=% collection=% mult=%",
                  name_, session.data(), collection, multiplier);

        auto async_get_size = [this,
                               session_copy = std::move(session),
                               collection_copy = std::move(collection),
                               multiplier_copy = multiplier
                              ](std::pmr::memory_resource* /*res*/) -> unique_future<int> {
            g_log.log("[%::coroutine_lambda] Starting async operation...", name_);

            auto sent_size = send(memory_storage_,
                &memory_storage_t::size,
                session_copy,
                collection_full_name_t("test_db", collection_copy));
            owe_storage(sent_size.first);
            auto size = co_await std::move(sent_size.second);

            g_log.log("[%::coroutine_lambda] Got size=%, applying multiplier=%",
                      name_, size, multiplier_copy);

            co_return static_cast<int>(size) * multiplier_copy;
        };

        g_log.log("[%::execute_with_coroutine_lambda] Calling lambda-coroutine...", name_);
        auto result_future = async_get_size(resource());

        int result = co_await std::move(result_future);

        g_log.log("[%::execute_with_coroutine_lambda] Lambda-coroutine returned: %", name_, result);
        co_return result;
    }

    /// Lambda-coroutine returning a move-only unique_ptr.
    unique_future<cursor_t_ptr> create_cursor_from_query(
            session_id_t session,
            std::string collection) {
        auto build_cursor = [this, s = std::move(session), coll = std::move(collection)]
                (std::pmr::memory_resource*) -> unique_future<cursor_t_ptr> {
            auto sent_size = send(memory_storage_, &memory_storage_t::size,
                s, collection_full_name_t("test_db", coll));
            owe_storage(sent_size.first);
            auto size = co_await std::move(sent_size.second);
            auto cursor = std::make_unique<cursor_t>();
            for (std::size_t i = 0; i < std::min(size, std::size_t(10)); ++i)
                cursor->data.push_back("row_" + std::to_string(i) + "_from_" + coll);
            g_log.log("[%::create_cursor_from_query] Built cursor with % rows", name_, cursor->row_count());
            co_return std::move(cursor);
        };
        co_return co_await build_cursor(resource());
    }

    /// One lambda-coroutine awaiting another.
    unique_future<cursor_t_ptr> validate_and_execute(
            session_id_t session,
            logical_plan_t plan) {
        auto validate = [](std::pmr::memory_resource*, const logical_plan_t& p) -> unique_future<bool> {
            co_return !p.collection.database.empty() && !p.collection.collection.empty();
        };
        auto execute = [this, &validate, s = std::move(session)]
                (std::pmr::memory_resource* res, logical_plan_t p) -> unique_future<cursor_t_ptr> {
            bool is_valid = co_await validate(res, p);
            if (!is_valid) {
                auto err = std::make_unique<cursor_t>();
                err->has_error = true;
                err->error_message = "Validation failed";
                co_return std::move(err);
            }
            auto sent_cursor = send(memory_storage_,
                &memory_storage_t::execute_plan, s, std::move(p));
            owe_storage(sent_cursor.first);
            auto cursor = co_await std::move(sent_cursor.second);
            co_return std::move(cursor);
        };
        co_return co_await execute(resource(), std::move(plan));
    }

    /// Three lambda-coroutines started before any is awaited.
    unique_future<aggregate_result_t> get_database_statistics(session_id_t session) {
        auto fetch_size = [this, s = session](std::pmr::memory_resource*, std::string coll)
                -> unique_future<std::size_t> {
            auto sent_result = send(memory_storage_, &memory_storage_t::size,
                s, collection_full_name_t("test_db", coll));
            owe_storage(sent_result.first);
            auto result = co_await std::move(sent_result.second);
            co_return result;
        };
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

    /// Move-only argument captured into a lambda-coroutine.
    unique_future<transaction_result_t> process_batch_buffer(
            session_id_t session,
            std::unique_ptr<std::vector<std::string>> batch) {
        auto process = [this, s = std::move(session), b = std::move(batch)]
                (std::pmr::memory_resource*) -> unique_future<transaction_result_t> {
            transaction_result_t result;
            if (!b || b->empty()) {
                result.has_error = true;
                result.error_message = "Empty batch";
                co_return result;
            }
            g_log.log("[%::process_batch_buffer] Processing % items", name_, b->size());
            result.committed = true;
            result.total_rows = b->size();
            co_return result;
        };
        co_return co_await process(resource());
    }

    /// A promise filled by hand and awaited while already ready.
    unique_future<std::size_t> get_cached_value(
            [[maybe_unused]] session_id_t session,
            std::string collection) {
        promise<std::size_t> cache_promise(resource());
        auto future = cache_promise.get_future();

        std::size_t cached = 0;
        if (collection == "users") cached = 100;
        else if (collection == "orders") cached = 250;
        else if (collection == "products") cached = 50;

        cache_promise.set_value(cached);
        g_log.log("[%::get_cached_value] Cache hit for %: %", name_, collection, cached);
        co_return co_await std::move(future);
    }

    unique_future<size_result_t> execute_with_retry(
            session_id_t session,
            std::string collection,
            int max_retries) {
        auto try_fetch = [this, s = session, coll = collection]
                (std::pmr::memory_resource*, bool simulate_error) -> unique_future<size_result_t> {
            if (simulate_error)
                co_return size_result_t::error("connection_timeout");
            auto sent_size = send(memory_storage_, &memory_storage_t::size,
                s, collection_full_name_t("test_db", coll));
            owe_storage(sent_size.first);
            auto size = co_await std::move(sent_size.second);
            co_return size_result_t(size);
        };
        // The first attempt is made to fail whenever max_retries > 0.
        auto result = co_await try_fetch(resource(), max_retries > 0);
        if (result.has_error && max_retries > 0) {
            g_log.log("[%::execute_with_retry] Retry after error: %", name_, result.error_message);
            result = co_await try_fetch(resource(), false);
        }
        co_return result;
    }

    /// Row forwarding (manager -> storage) with the rows delivered as one vector,
    /// each prefixed by this actor.
    unique_future<std::vector<std::string>> fetch_row_batch(
            session_id_t session,
            std::string collection) {

        auto tid = thread_id_str();
        g_log.log("[%::fetch_row_batch] thread=% session=% collection=%",
                  name_, tid, session.data(), collection);

        std::vector<std::string> rows;

        if (collection.empty()) {
            g_log.log("[%::fetch_row_batch] Error: empty collection name", name_);
            co_return rows;
        }

        // Goes straight to storage rather than through this actor's own
        // execute_plan(), which would register the cursor in result_storage_ and
        // leave a dangling pointer once the cursor dies with this frame.
        auto sent_cursor = send(memory_storage_,
            &memory_storage_t::execute_plan,
            session,
            logical_plan_t("select", collection_full_name_t("test_db", collection)));
        owe_storage(sent_cursor.first);
        auto cursor = co_await std::move(sent_cursor.second);

        if (cursor->has_error) {
            g_log.log("[%::fetch_row_batch] Storage error: %", name_, cursor->error_message);
            co_return rows;
        }

        rows.reserve(cursor->data.size());
        for (const auto& row : cursor->data) {
            g_log.log("[%::fetch_row_batch] Forwarding: %", name_, row);
            rows.push_back("[manager] " + row);
        }

        g_log.log("[%::fetch_row_batch] Done, % rows", name_, rows.size());
        co_return rows;
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
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

        &manager_dispatcher_t::create_cursor_from_query,
        &manager_dispatcher_t::validate_and_execute,
        &manager_dispatcher_t::get_database_statistics,
        &manager_dispatcher_t::process_batch_buffer,
        &manager_dispatcher_t::get_cached_value,
        &manager_dispatcher_t::execute_with_retry,

        &manager_dispatcher_t::fetch_row_batch
    >;

    behavior_t behavior(mailbox::message* msg) {
        auto tid = thread_id_str();
        g_log.log("[%::behavior] thread=% command=%", name_, tid, msg->command());

        switch (msg->command()) {
            case msg_id<manager_dispatcher_t, &manager_dispatcher_t::size>:
                co_await dispatch(this, &manager_dispatcher_t::size, msg);
                break;
            case msg_id<manager_dispatcher_t, &manager_dispatcher_t::execute_plan>:
                co_await dispatch(this, &manager_dispatcher_t::execute_plan, msg);
                break;
            case msg_id<manager_dispatcher_t, &manager_dispatcher_t::close_cursor>:
                co_await dispatch(this, &manager_dispatcher_t::close_cursor, msg);
                break;
            case msg_id<manager_dispatcher_t, &manager_dispatcher_t::execute_transaction>:
                co_await dispatch(this, &manager_dispatcher_t::execute_transaction, msg);
                break;
            case msg_id<manager_dispatcher_t, &manager_dispatcher_t::aggregate_sizes>:
                co_await dispatch(this, &manager_dispatcher_t::aggregate_sizes, msg);
                break;
            case msg_id<manager_dispatcher_t, &manager_dispatcher_t::get_aggregate_detail>:
                co_await dispatch(this, &manager_dispatcher_t::get_aggregate_detail, msg);
                break;
            case msg_id<manager_dispatcher_t, &manager_dispatcher_t::transform_with_lambda>:
                co_await dispatch(this, &manager_dispatcher_t::transform_with_lambda, msg);
                break;
            case msg_id<manager_dispatcher_t, &manager_dispatcher_t::compute_with_lambda_and_state>:
                co_await dispatch(this, &manager_dispatcher_t::compute_with_lambda_and_state, msg);
                break;
            case msg_id<manager_dispatcher_t, &manager_dispatcher_t::async_transform_with_lambda>:
                co_await dispatch(this, &manager_dispatcher_t::async_transform_with_lambda, msg);
                break;
            case msg_id<manager_dispatcher_t, &manager_dispatcher_t::execute_with_coroutine_lambda>:
                co_await dispatch(this, &manager_dispatcher_t::execute_with_coroutine_lambda, msg);
                break;
            case msg_id<manager_dispatcher_t, &manager_dispatcher_t::create_cursor_from_query>:
                co_await dispatch(this, &manager_dispatcher_t::create_cursor_from_query, msg);
                break;
            case msg_id<manager_dispatcher_t, &manager_dispatcher_t::validate_and_execute>:
                co_await dispatch(this, &manager_dispatcher_t::validate_and_execute, msg);
                break;
            case msg_id<manager_dispatcher_t, &manager_dispatcher_t::get_database_statistics>:
                co_await dispatch(this, &manager_dispatcher_t::get_database_statistics, msg);
                break;
            case msg_id<manager_dispatcher_t, &manager_dispatcher_t::process_batch_buffer>:
                co_await dispatch(this, &manager_dispatcher_t::process_batch_buffer, msg);
                break;
            case msg_id<manager_dispatcher_t, &manager_dispatcher_t::get_cached_value>:
                co_await dispatch(this, &manager_dispatcher_t::get_cached_value, msg);
                break;
            case msg_id<manager_dispatcher_t, &manager_dispatcher_t::execute_with_retry>:
                co_await dispatch(this, &manager_dispatcher_t::execute_with_retry, msg);
                break;
            case msg_id<manager_dispatcher_t, &manager_dispatcher_t::fetch_row_batch>:
                co_await dispatch(this, &manager_dispatcher_t::fetch_row_batch, msg);
                break;
            default:
                g_log.log("[%::behavior] Unknown command!", name_);
                break;
        }
    }

private:
    address_t memory_storage_;
    std::atomic<std::size_t> storage_owed_{0};
    std::string name_;

    std::unordered_map<session_id_t, cursor_t*, session_id_hash> result_storage_;
};

} // namespace dispatcher_test
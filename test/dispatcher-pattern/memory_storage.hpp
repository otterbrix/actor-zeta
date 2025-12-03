#pragma once

/// @file memory_storage.hpp
/// @brief Storage actor - bottom level of the actor hierarchy
///
/// memory_storage_t simulates a database storage with collections.
/// It demonstrates:
/// - Simple coroutine methods returning unique_future<T>
/// - dispatch_traits for compile-time message ID mapping
/// - Behavior dispatch using switch + msg_id<Actor, &Actor::method>

#include <actor-zeta.hpp>
#include <actor-zeta/dispatch.hpp>

#include "common_types.hpp"
#include "test_logger.hpp"

#include <unordered_map>
#include <string>
#include <algorithm>

namespace dispatcher_test {

using namespace actor_zeta;

/// @brief Storage actor - simulates database storage
///
/// Architecture position: BOTTOM level
/// Receives: size(), execute_plan() requests from manager_dispatcher_t
/// Returns: collection sizes, cursors with data
class memory_storage_t final : public basic_actor<memory_storage_t> {
public:
    explicit memory_storage_t(pmr::memory_resource* mr, const std::string& name)
        : basic_actor<memory_storage_t>(mr)
        , name_(name) {
        // Initialize "database" with test data
        collections_["test_db.users"] = 100;
        collections_["test_db.orders"] = 250;
        collections_["test_db.products"] = 50;
    }

    // =========================================================================
    // Public methods (coroutines returning unique_future<T>)
    // =========================================================================

    /// @brief Get collection size
    /// @param session Session identifier for logging
    /// @param name Full collection name (database.collection)
    /// @return unique_future<std::size_t> with collection size
    unique_future<std::size_t> size(
            session_id_t session,
            collection_full_name_t name) {

        auto tid = thread_id_str();
        g_log.log("[%::size] thread=% session=% db=% coll=%",
                  name_, tid, session.data(), name.database, name.collection);

        std::string key = name.database + "." + name.collection;
        auto it = collections_.find(key);

        std::size_t result = 0;
        if (it != collections_.end()) {
            result = it->second;
        }

        g_log.log("[%::size] returning %", name_, result);
        co_return result;
    }

    /// @brief Execute logical query plan
    /// @param session Session identifier
    /// @param plan Query plan to execute
    /// @return unique_future<cursor_t_ptr> with query results
    unique_future<cursor_t_ptr> execute_plan(
            session_id_t session,
            logical_plan_t plan) {

        auto tid = thread_id_str();
        g_log.log("[%::execute_plan] thread=% session=% plan=%",
                  name_, tid, session.data(), plan.to_string());

        auto cursor = std::make_unique<cursor_t>();

        std::string key = plan.collection.database + "." + plan.collection.collection;
        auto it = collections_.find(key);

        if (it == collections_.end()) {
            g_log.log("[%::execute_plan] Collection not found: %", name_, key);
            cursor->has_error = true;
            cursor->error_message = "Collection not found: " + key;
            co_return std::move(cursor);
        }

        // Simulate data retrieval
        std::size_t count = it->second;
        for (std::size_t i = 0; i < std::min(count, std::size_t(10)); ++i) {
            cursor->data.push_back("row_" + std::to_string(i) + "_from_" + key);
        }

        g_log.log("[%::execute_plan] returning cursor with % rows", name_, cursor->row_count());
        co_return std::move(cursor);
    }

    // =========================================================================
    // dispatch_traits - compile-time message ID mapping
    // =========================================================================

    /// @brief Maps method pointers to message IDs at compile time
    ///
    /// Usage in behavior():
    ///   case msg_id<memory_storage_t, &memory_storage_t::size>:
    ///
    /// The type system from make_message.hpp validates argument types.
    using dispatch_traits = actor_zeta::dispatch_traits<
        &memory_storage_t::size,
        &memory_storage_t::execute_plan
    >;

    // =========================================================================
    // behavior() - message dispatch
    // =========================================================================

    /// @brief Main message handler
    unique_future<void> behavior(mailbox::message* msg) {
        auto tid = thread_id_str();
        g_log.log("[%::behavior] thread=% command=%", name_, tid, msg->command());

        switch (msg->command()) {
            case msg_id<memory_storage_t, &memory_storage_t::size>: {
                // dispatch() unpacks message arguments and calls method
                // Returns future that chains result to sender's result_slot
                auto future = dispatch(this, &memory_storage_t::size, msg);
                (void)future;  // Result chaining handled by dispatch()
                break;
            }
            case msg_id<memory_storage_t, &memory_storage_t::execute_plan>: {
                auto future = dispatch(this, &memory_storage_t::execute_plan, msg);
                (void)future;
                break;
            }
            default:
                g_log.log("[%::behavior] Unknown command!", name_);
                break;
        }
        return make_ready_future_void(resource());
    }

    const std::string& name() const { return name_; }

    ~memory_storage_t() = default;

private:
    std::string name_;
    std::unordered_map<std::string, std::size_t> collections_;
};

} // namespace dispatcher_test
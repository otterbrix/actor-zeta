#pragma once

/// @file client.hpp
/// @brief Client actor - top level in the actor hierarchy
///
/// client_t demonstrates:
/// - Simple client that sends requests to dispatcher
/// - Single co_await for request-response pattern
/// - Pending coroutine management
///
/// OLD (session-based) pattern:
///   - request_size() sends request with sender address
///   - size_finish() callback receives result
///
/// NEW (promise/future) pattern:
///   - request_collection_size() returns result via co_await
///   - No callback needed!

#include <actor-zeta.hpp>
#include <actor-zeta/dispatch.hpp>

#include "common_types.hpp"
#include "test_logger.hpp"
#include "manager_dispatcher.hpp"

#include <vector>
#include <string>

namespace dispatcher_test {

using namespace actor_zeta;

/// @brief Client actor - top level, sends requests to dispatcher
///
/// Architecture position: TOP level
/// Sends: requests to manager_dispatcher_t
/// Receives: responses via unique_future
class client_t final : public basic_actor<client_t> {
public:
    explicit client_t(
            pmr::memory_resource* mr,
            address_t dispatcher,
            const std::string& name)
        : basic_actor<client_t>(mr)
        , dispatcher_(std::move(dispatcher))
        , name_(name) {
    }

    // =========================================================================
    // Simple methods
    // =========================================================================

    /// @brief Trigger behavior() to process pending coroutines
    void poll() {
        g_log.log("[%::poll] called", name_);
    }

    // =========================================================================
    // Client methods
    // =========================================================================

    /// @brief Request collection size from dispatcher
    ///
    /// Demonstrates: Simple request-response with co_await
    /// - Send request to dispatcher
    /// - co_await response
    /// - Store result for test verification
    unique_future<size_result_t> request_collection_size(
            const session_id_t& session,
            std::string database,
            std::string collection) {

        auto tid = thread_id_str();
        g_log.log("[%::request_collection_size] thread=% session=% db=% coll=%",
                  name_, tid, session.data(), database, collection);

        // Send request to dispatcher, wait for response
        auto result = co_await send(
            dispatcher_,
            address(),
            &manager_dispatcher_t::size,
            session,
            database,
            collection);

        g_log.log("[%::request_collection_size] Got result: size=% error=%",
                  name_, result.size, result.has_error);

        // Store for test verification
        last_result_ = result;

        co_return result;
    }

    // =========================================================================
    // dispatch_traits
    // =========================================================================

    using dispatch_traits = actor_zeta::dispatch_traits<
        &client_t::poll,
        &client_t::request_collection_size
    >;

    // =========================================================================
    // behavior()
    // =========================================================================

    unique_future<void> behavior(mailbox::message* msg) {
        auto tid = thread_id_str();
        g_log.log("[%::behavior] thread=% command=%", name_, tid, msg->command());

        switch (msg->command()) {
            case msg_id<client_t, &client_t::poll>:
                poll();
                break;
            case msg_id<client_t, &client_t::request_collection_size>: {
                auto future = dispatch(this, &client_t::request_collection_size, msg);
                if (!future.available()) {
                    g_log.log("[%::behavior] request_collection_size() suspended, storing pending", name_);
                    pending_.push_back(std::move(future));
                }
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

    bool has_pending() const { return !pending_.empty(); }

    void poll_pending() {
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->awaiting_ready()) {
                g_log.log("[%::poll_pending] awaiting ready, resuming coroutine", name_);
                it->resume();
                if (it->available()) {
                    g_log.log("[%::poll_pending] coroutine completed", name_);
                    it = pending_.erase(it);
                    continue;
                }
            }
            ++it;
        }
    }

    // =========================================================================
    // Accessors
    // =========================================================================

    const size_result_t& last_result() const { return last_result_; }
    const std::string& name() const { return name_; }

    ~client_t() = default;

private:
    address_t dispatcher_;
    std::string name_;
    std::vector<unique_future<size_result_t>> pending_;
    size_result_t last_result_;
};

} // namespace dispatcher_test
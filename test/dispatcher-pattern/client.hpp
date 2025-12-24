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

#include "common_types.hpp"
#include "manager_dispatcher.hpp"
#include "test_logger.hpp"

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
            std::pmr::memory_resource* mr,
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
    unique_future<void> poll() {
        g_log.log("[%::poll] called", name_);
        co_return;
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
            session_id_t session,
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

    /// @brief Consume stream from dispatcher
    ///
    /// Demonstrates: Generator consumption pattern
    /// - Request generator from dispatcher
    /// - co_await inside unique_future to consume each value
    /// - Collect all rows
    /// - Handle errors via stream_error
    ///
    /// @param session Session ID (BY VALUE)
    /// @param collection Collection name (BY VALUE)
    /// @return unique_future<std::vector<std::string>> with all rows
    unique_future<std::vector<std::string>> consume_stream(
            session_id_t session,
            std::string collection) {

        auto tid = thread_id_str();
        g_log.log("[%::consume_stream] thread=% session=% collection=%",
                  name_, tid, session.data(), collection);

        std::vector<std::string> rows;

        // Get generator from dispatcher
        auto gen = send(
            dispatcher_,
            address(),
            &manager_dispatcher_t::create_row_stream,
            session,
            collection);

        g_log.log("[%::consume_stream] Got generator, consuming rows...", name_);

        // Consume generator using co_await
        while (co_await gen) {
            // Check for error
            if (gen.has_error()) {
                g_log.log("[%::consume_stream] Stream error: %",
                          name_, gen.error().message());
                last_stream_error_ = gen.error();
                co_return rows;  // Return partial results
            }

            // Get current value
            auto& row = gen.current();
            g_log.log("[%::consume_stream] Received row: %", name_, row);

            // Transform: add client prefix
            rows.push_back("[client] " + row);
        }

        g_log.log("[%::consume_stream] Stream exhausted, got % rows", name_, rows.size());
        last_streamed_rows_ = rows;

        co_return rows;
    }

    // =========================================================================
    // dispatch_traits
    // =========================================================================

    using dispatch_traits = actor_zeta::dispatch_traits<
        &client_t::poll,
        &client_t::request_collection_size,
        &client_t::consume_stream
    >;

    // =========================================================================
    // behavior()
    // =========================================================================

    void behavior(mailbox::message* msg) {
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
            case msg_id<client_t, &client_t::consume_stream>: {
                auto future = dispatch(this, &client_t::consume_stream, msg);
                if (!future.available()) {
                    g_log.log("[%::behavior] consume_stream() suspended, storing pending", name_);
                    pending_stream_.push_back(std::move(future));
                }
                break;
            }
            default:
                g_log.log("[%::behavior] Unknown command!", name_);
                break;
        }

        // Process pending coroutines after each message
        poll_pending();
    }

    // =========================================================================
    // Pending coroutine management
    // =========================================================================

    bool has_pending() const {
        return !pending_.empty() || !pending_stream_.empty();
    }

    /// @brief Clean up completed pending futures
    /// With auto-resume in set_value(), coroutines resume automatically
    void poll_pending() {
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->available()) {
                g_log.log("[%::poll_pending] size coroutine completed", name_);
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = pending_stream_.begin(); it != pending_stream_.end();) {
            if (it->available()) {
                g_log.log("[%::poll_pending] stream coroutine completed", name_);
                it = pending_stream_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // =========================================================================
    // Accessors
    // =========================================================================

    const size_result_t& last_result() const { return last_result_; }
    const std::vector<std::string>& last_streamed_rows() const { return last_streamed_rows_; }
    std::error_code last_stream_error() const { return last_stream_error_; }
    const std::string& name() const { return name_; }

    ~client_t() = default;

private:
    address_t dispatcher_;
    std::string name_;
    std::vector<unique_future<size_result_t>> pending_;
    std::vector<unique_future<std::vector<std::string>>> pending_stream_;
    size_result_t last_result_;
    std::vector<std::string> last_streamed_rows_;
    std::error_code last_stream_error_;
};

} // namespace dispatcher_test
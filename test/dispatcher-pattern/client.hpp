#pragma once

/// @file client.hpp
/// Top-level actor of the dispatcher-pattern test: sends to the dispatcher and
/// co_awaits the reply.

#include <actor-zeta.hpp>

#include "common_types.hpp"
#include "manager_dispatcher.hpp"
#include "test_logger.hpp"

#include <vector>
#include <string>

namespace dispatcher_test {

using namespace actor_zeta;

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

    /// No-op message: gives the actor a turn so a suspended handler can drain a
    /// ready await.
    unique_future<void> poll() {
        g_log.log("[%::poll] called", name_);
        co_return;
    }

    unique_future<size_result_t> request_collection_size(
            session_id_t session,
            std::string database,
            std::string collection) {

        auto tid = thread_id_str();
        g_log.log("[%::request_collection_size] thread=% session=% db=% coll=%",
                  name_, tid, session.data(), database, collection);

        auto sent_result = send(
            dispatcher_,
            &manager_dispatcher_t::size,
            session,
            database,
            collection);
        auto result = co_await std::move(sent_result.second);

        g_log.log("[%::request_collection_size] Got result: size=% error=%",
                  name_, result.size, result.has_error);

        last_result_ = result;

        co_return result;
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &client_t::poll,
        &client_t::request_collection_size
    >;

    behavior_t behavior(mailbox::message* msg) {
        auto tid = thread_id_str();
        g_log.log("[%::behavior] thread=% command=%", name_, tid, msg->command());

        switch (msg->command()) {
            case msg_id<client_t, &client_t::poll>:
                co_await poll();
                break;
            case msg_id<client_t, &client_t::request_collection_size>:
                co_await dispatch(this, &client_t::request_collection_size, msg);
                break;
            default:
                g_log.log("[%::behavior] Unknown command!", name_);
                break;
        }
    }

    bool has_pending() const {
        return !pending_.empty();
    }

    void poll_pending() {
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->is_ready()) {
                g_log.log("[%::poll_pending] size coroutine completed", name_);
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
    }

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
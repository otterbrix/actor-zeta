#pragma once

/// @file client.hpp
/// Top-level actor of the dispatcher-pattern test: sends to the dispatcher and
/// co_awaits the reply.

#include <actor-zeta.hpp>

#include "common_types.hpp"
#include "manager_dispatcher.hpp"
#include "test_logger.hpp"

#include <atomic>
#include <string>

namespace dispatcher_test {

using namespace actor_zeta;

class client_t final : public basic_actor<client_t> {
public:
    /// Holds an address_t: sending is all it does. The obligation send() reports
    /// is recorded for the supervisor, which owns the actors and schedules them.
    explicit client_t(
            std::pmr::memory_resource* mr,
            address_t dispatcher,
            const std::string& name)
        : basic_actor<client_t>(mr)
        , dispatcher_(std::move(dispatcher))
        , name_(name) {
    }

    std::size_t take_dispatcher_obligations() {
        return dispatcher_owed_.exchange(0, std::memory_order_acq_rel);
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
        if (sent_result.first) {
            dispatcher_owed_.fetch_add(1, std::memory_order_release);
        }
        auto result = co_await std::move(sent_result.second);

        g_log.log("[%::request_collection_size] Got result: size=% error=%",
                  name_, result.size, result.has_error);

        co_return result;
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &client_t::request_collection_size
    >;

    behavior_t behavior(mailbox::message* msg) {
        auto tid = thread_id_str();
        g_log.log("[%::behavior] thread=% command=%", name_, tid, msg->command());

        switch (msg->command()) {
            case msg_id<client_t, &client_t::request_collection_size>:
                co_await dispatch(this, &client_t::request_collection_size, msg);
                break;
            default:
                g_log.log("[%::behavior] Unknown command!", name_);
                break;
        }
    }

    ~client_t() = default;

private:
    address_t dispatcher_;
    std::atomic<std::size_t> dispatcher_owed_{0};
    std::string name_;
};

} // namespace dispatcher_test
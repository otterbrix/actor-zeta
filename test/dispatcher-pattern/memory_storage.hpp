#pragma once

/// @file memory_storage.hpp
/// Bottom-level actor of the dispatcher-pattern test: a fake database with three
/// fixed collections, answering size() and execute_plan() without awaiting anyone.

#include <actor-zeta.hpp>

#include "common_types.hpp"
#include "test_logger.hpp"

#include <atomic>
#include <unordered_map>
#include <string>
#include <algorithm>

namespace dispatcher_test {

using namespace actor_zeta;

class memory_storage_t final : public basic_actor<memory_storage_t> {
public:
    explicit memory_storage_t(std::pmr::memory_resource* mr, const std::string& name)
        : basic_actor<memory_storage_t>(mr)
        , name_(name) {
        collections_["test_db.users"] = 100;
        collections_["test_db.orders"] = 250;
        collections_["test_db.products"] = 50;
    }

    /// Messages actually handled, across size() and execute_plan(). Lets a test
    /// assert the round trips really happened instead of pumping the actor stage
    /// by stage -- right numbers with fewer trips no longer pass.
    std::size_t served() const noexcept { return served_.load(std::memory_order_acquire); }

    unique_future<std::size_t> size(
            session_id_t session,
            collection_full_name_t name) {

        auto tid = thread_id_str();
        g_log.log("[%::size] thread=% session=% db=% coll=%",
                  name_, tid, session.data(), name.database, name.collection);

        served_.fetch_add(1, std::memory_order_release);

        std::string key = name.database + "." + name.collection;
        auto it = collections_.find(key);

        std::size_t result = 0;
        if (it != collections_.end()) {
            result = it->second;
        }

        g_log.log("[%::size] returning %", name_, result);
        co_return result;
    }

    unique_future<cursor_t_ptr> execute_plan(
            session_id_t session,
            logical_plan_t plan) {
        served_.fetch_add(1, std::memory_order_release);

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

        std::size_t count = it->second;
        for (std::size_t i = 0; i < std::min(count, std::size_t(10)); ++i) {
            cursor->data.push_back("row_" + std::to_string(i) + "_from_" + key);
        }

        g_log.log("[%::execute_plan] returning cursor with % rows", name_, cursor->row_count());
        co_return std::move(cursor);
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &memory_storage_t::size,
        &memory_storage_t::execute_plan
    >;

    behavior_t behavior(mailbox::message* msg) {
        auto tid = thread_id_str();
        g_log.log("[%::behavior] thread=% command=%", name_, tid, msg->command());

        switch (msg->command()) {
            case msg_id<memory_storage_t, &memory_storage_t::size>: {
                co_await dispatch(this, &memory_storage_t::size, msg);
                break;
            }
            case msg_id<memory_storage_t, &memory_storage_t::execute_plan>: {
                co_await dispatch(this, &memory_storage_t::execute_plan, msg);
                break;
            }
            default:
                g_log.log("[%::behavior] Unknown command!", name_);
                break;
        }
    }

    const std::string& name() const { return name_; }

    ~memory_storage_t() = default;

private:
    std::atomic<std::size_t> served_{0};
    std::string name_;
    std::unordered_map<std::string, std::size_t> collections_;
};

} // namespace dispatcher_test
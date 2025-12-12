#define CATCH_CONFIG_MAIN // This tells Catch to provide a main() - only do this in one cpp file
#include <catch2/catch.hpp>

#include <cassert>

#include <map>
#include <set>
#include <string>
#include <list>

#include "actor-zeta/detail/memory.hpp"
#include "test/tooltestsuites/scheduler_test.hpp"
#include <actor-zeta/actor/dispatch.hpp>
#include <actor-zeta.hpp>
#include <actor-zeta/spawn.hpp>

using std::pmr::memory_resource;
class dummy_supervisor;
class storage_t;

class dummy_supervisor final {
public:
    dummy_supervisor(memory_resource* resource_ptr)
        : resource_(resource_ptr)
        , executor_(new actor_zeta::test::scheduler_test_t(1, 1)) {
        executor_->start();
    }

    std::pmr::memory_resource* resource() const noexcept {
        return resource_;
    }

    auto scheduler_test() noexcept -> actor_zeta::test::scheduler_test_t* {
        return executor_.get();
    }

    actor_zeta::unique_future<void> create();

    void behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<dummy_supervisor, &dummy_supervisor::create>) {
            dispatch(this, &dummy_supervisor::create, msg);
        }
    }

    void enqueue_impl(actor_zeta::mailbox::message_ptr msg) {
        auto tmp = std::move(msg);
        behavior(msg.get());
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &dummy_supervisor::create
    >;

private:
    std::pmr::memory_resource* resource_;
    std::unique_ptr<actor_zeta::test::scheduler_test_t> executor_;
    std::list<std::unique_ptr<storage_t, actor_zeta::pmr::deleter_t>> storage_;
    std::set<int64_t> ids_;
};

class storage_t final : public actor_zeta::basic_actor<storage_t> {
public:
    explicit storage_t(std::pmr::memory_resource* resource_ptr, dummy_supervisor* )
        : actor_zeta::basic_actor<storage_t>(resource_ptr) {
    }

    void behavior(actor_zeta::mailbox::message*) {
    }

    ~storage_t() = default;

    using dispatch_traits = actor_zeta::dispatch_traits<>;
};

actor_zeta::unique_future<void> dummy_supervisor::create() {
    auto uptr = actor_zeta::spawn<storage_t>(resource_, reinterpret_cast<dummy_supervisor*>(this));
    REQUIRE(ids_.find(reinterpret_cast<int64_t>(uptr.get())) == ids_.end());
    ids_.insert(reinterpret_cast<int64_t>(uptr.get()));
    ///scheduler_test()->enqueue(uptr.get());
    storage_.emplace_back(std::move(uptr));
    co_return;
}

TEST_CASE("actor id match") {
    auto* resource =std::pmr::get_default_resource();
    auto supervisor = std::unique_ptr<dummy_supervisor>(new dummy_supervisor(resource));
    for (auto i = 0; i < 1000; ++i) { //todo: 10000000
        supervisor->create();
    }
}

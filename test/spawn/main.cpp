#define CATCH_CONFIG_MAIN // This tells Catch to provide a main() - only do this in one cpp file
#include <catch2/catch.hpp>

#include "classes.hpp"

#include <cassert>

#include <set>
#include <string>

#include <actor-zeta.hpp>
#include <test/tooltestsuites/scheduler_test.hpp>

class dummy_supervisor;

static std::atomic_int actor_counter{0};
static std::atomic_int supervisor_counter{0};
static std::atomic_int supervisor_sub_counter{0};

constexpr static auto update_id = actor_zeta::make_message_id(0);
constexpr static auto find_id = actor_zeta::make_message_id(1);
constexpr static auto remove_id = actor_zeta::make_message_id(2);

constexpr static auto create_actor_id = actor_zeta::make_message_id(0);
constexpr static auto create_supervisor_id = actor_zeta::make_message_id(1);
constexpr static auto create_supervisor_custom_resource_id = actor_zeta::make_message_id(2);

class dummy_supervisor;

class storage_t final : public actor_zeta::basic_actor<storage_t> {
public:
    storage_t(actor_zeta::pmr::memory_resource* resource_ptr, dummy_supervisor* ptr);

    ~storage_t() override = default;

    void behavior(actor_zeta::mailbox::message* msg) {
        switch (msg->command()) {
            case update_id: {
                update_(msg);
                break;
            }
            case find_id: {
                find_(msg);
                break;
            }
            case remove_id: {
                remove_(msg);
                break;
            }
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<>;

private:
    actor_zeta::behavior_t update_;
    actor_zeta::behavior_t find_;
    actor_zeta::behavior_t remove_;
};

class dummy_supervisor_sub final : public actor_zeta::actor_abstract_t {
public:
    dummy_supervisor_sub(dummy_supervisor* ptr);

    dummy_supervisor_sub(actor_zeta::pmr::memory_resource* ptr, dummy_supervisor*)
        : actor_zeta::actor_abstract_t(ptr)
        , executor_(new actor_zeta::test::scheduler_test_t(1, 1)) {
        executor_->start();
        supervisor_sub_counter++;
    }

    dummy_supervisor_sub(actor_zeta::pmr::memory_resource* ptr)
        : actor_zeta::actor_abstract_t(ptr)
        , executor_(new actor_zeta::test::scheduler_test_t(1, 1)) {
        executor_->start();
        supervisor_sub_counter++;
    }

    auto scheduler_test() noexcept -> actor_zeta::test::scheduler_test_t* {
        return executor_.get();
    }

    void behavior(actor_zeta::mailbox::message*) {
        // Empty behavior
    }

    bool enqueue_impl(actor_zeta::message_ptr msg) override {
        auto tmp = std::move(msg);
        behavior(tmp.get());
        return true;
    }

private:
    std::unique_ptr<actor_zeta::test::scheduler_test_t> executor_;
};

class dummy_supervisor final : public actor_zeta::actor_abstract_t {
public:
    dummy_supervisor(actor_zeta::pmr::memory_resource* ptr)
        : actor_zeta::actor_abstract_t(ptr)
        , create_actor_(actor_zeta::make_behavior(resource(), this, &dummy_supervisor::create_actor))
        , create_supervisor_(actor_zeta::make_behavior(resource(), this, &dummy_supervisor::create_supervisor))
        , create_supervisor_custom_resource_(actor_zeta::make_behavior(resource(), this, &dummy_supervisor::create_supervisor_custom_resource))
        , executor_(new actor_zeta::test::scheduler_test_t(1, 1)) {
        executor_->start();
        supervisor_counter++;
    }

    auto scheduler_test() noexcept -> actor_zeta::test::scheduler_test_t* {
        return executor_.get();
    }

    void create_actor() {
        auto uptr = actor_zeta::spawn<storage_t>(resource(), this);
        actors_.emplace_back(std::move(uptr));
    }

    void create_supervisor() {
        auto uptr = actor_zeta::spawn<dummy_supervisor_sub>(resource(), this);
        supervisor_.emplace_back(std::move(uptr));
    }

    void create_supervisor_custom_resource() {
        auto uptr = actor_zeta::spawn<dummy_supervisor_sub>(resource());
        supervisor_.emplace_back(std::move(uptr));
    }

    void behavior(actor_zeta::mailbox::message* msg) {
        if (msg->command() == actor_zeta::msg_id<dummy_supervisor, &dummy_supervisor::create_actor>) {
            create_actor_(msg);
        } else if (msg->command() == actor_zeta::msg_id<dummy_supervisor, &dummy_supervisor::create_supervisor>) {
            create_supervisor_(msg);
        } else if (msg->command() == actor_zeta::msg_id<dummy_supervisor, &dummy_supervisor::create_supervisor_custom_resource>) {
            create_supervisor_custom_resource_(msg);
        }
    }

    bool enqueue_impl(actor_zeta::message_ptr msg) override {
        auto tmp = std::move(msg);
        behavior(tmp.get());
        return true;
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &dummy_supervisor::create_actor,
        &dummy_supervisor::create_supervisor,
        &dummy_supervisor::create_supervisor_custom_resource
    >;

private:
    actor_zeta::behavior_t create_actor_;
    actor_zeta::behavior_t create_supervisor_;
    actor_zeta::behavior_t create_supervisor_custom_resource_;
    std::unique_ptr<actor_zeta::test::scheduler_test_t> executor_;
    std::vector<storage_t::unique_actor> actors_;
    std::vector<std::unique_ptr<dummy_supervisor_sub, actor_zeta::pmr::deleter_t>> supervisor_;
};


 storage_t::storage_t(actor_zeta::pmr::memory_resource* resource_ptr, dummy_supervisor*): actor_zeta::basic_actor<storage_t>(resource_ptr)
        , update_(actor_zeta::make_behavior(resource(), []() -> void {}))
        , find_(actor_zeta::make_behavior(resource(), []() -> void {}))
        , remove_(actor_zeta::make_behavior(resource(), []() -> void {})) {
    actor_counter++;
}

dummy_supervisor_sub::dummy_supervisor_sub(dummy_supervisor* ptr): actor_zeta::actor_abstract_t(ptr->resource())
       , executor_(new actor_zeta::test::scheduler_test_t(1, 1)) {
    ///auto * ptr_sheduler = scheduler();
    ///ptr_sheduler->start();
    executor_->start();
    supervisor_sub_counter++;
}

TEST_CASE("spawn base supervisor") {
    supervisor_counter = 0;
    actor_counter = 0;
    auto* mr_ptr = actor_zeta::pmr::get_default_resource();
    auto supervisor = actor_zeta::spawn<dummy_supervisor>(mr_ptr);
    REQUIRE(supervisor_counter == 1);
}

TEST_CASE("spawn supervisor") {
    supervisor_counter = 0;
    supervisor_sub_counter = 0;
    actor_counter = 0;
    auto* mr_ptr = actor_zeta::pmr::get_default_resource();
    auto supervisor = actor_zeta::spawn<dummy_supervisor>(mr_ptr);
    actor_zeta::send(supervisor.get(), actor_zeta::address_t::empty_address(), &dummy_supervisor::create_supervisor);
    supervisor->scheduler_test()->run_once();
    REQUIRE(supervisor_counter == 1);
    REQUIRE(supervisor_sub_counter == 1);
}

TEST_CASE("spawn supervisor custom resource") {
    supervisor_counter = 0;
    supervisor_sub_counter = 0;
    actor_counter = 0;
    auto* mr_ptr = actor_zeta::pmr::get_default_resource();
    auto supervisor = actor_zeta::spawn<dummy_supervisor>(mr_ptr);
    actor_zeta::send(supervisor.get(), actor_zeta::address_t::empty_address(), &dummy_supervisor::create_supervisor_custom_resource);
    supervisor->scheduler_test()->run_once();
    REQUIRE(supervisor_counter == 1);
    REQUIRE(supervisor_sub_counter == 1);
}

TEST_CASE("spawn actor") {
    supervisor_counter = 0;
    actor_counter = 0;
    auto* mr_ptr = actor_zeta::pmr::get_default_resource();
    auto supervisor = actor_zeta::spawn<dummy_supervisor>(mr_ptr);
    actor_zeta::send(supervisor.get(), actor_zeta::address_t::empty_address(), &dummy_supervisor::create_actor);
    supervisor->scheduler_test()->run_once();
    REQUIRE(actor_counter == 1);
}

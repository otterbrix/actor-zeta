#define CATCH_CONFIG_MAIN // This tells Catch to provide a main() - only do this in one cpp file
#include <catch2/catch.hpp>

#include "classes.hpp"

#include <cassert>

#include <set>
#include <string>

#include <actor-zeta/actor/dispatch.hpp>
#include <actor-zeta.hpp>
#include <test/tooltestsuites/scheduler_test.hpp>

class dummy_supervisor;

static std::atomic_int actor_counter{0};
static std::atomic_int supervisor_counter{0};
static std::atomic_int supervisor_sub_counter{0};

class dummy_supervisor;

class storage_t final : public actor_zeta::basic_actor<storage_t> {
public:
    storage_t(std::pmr::memory_resource* resource_ptr, dummy_supervisor* ptr);

    ~storage_t() = default;

    actor_zeta::unique_future<void> update() { co_return; }
    actor_zeta::unique_future<void> find() { co_return; }
    actor_zeta::unique_future<void> remove() { co_return; }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &storage_t::update,
        &storage_t::find,
        &storage_t::remove
    >;

    void behavior(actor_zeta::mailbox::message* msg) {
        switch (msg->command()) {
            case actor_zeta::msg_id<storage_t, &storage_t::update>:
                actor_zeta::dispatch(this, &storage_t::update, msg);
                break;
            case actor_zeta::msg_id<storage_t, &storage_t::find>:
                actor_zeta::dispatch(this, &storage_t::find, msg);
                break;
            case actor_zeta::msg_id<storage_t, &storage_t::remove>:
                actor_zeta::dispatch(this, &storage_t::remove, msg);
                break;
        }
    }
};

class dummy_supervisor_sub final : public actor_zeta::actor::actor_mixin<dummy_supervisor_sub> {
public:
    template<typename T> using unique_future = actor_zeta::unique_future<T>;

    dummy_supervisor_sub(dummy_supervisor* ptr);

    dummy_supervisor_sub(std::pmr::memory_resource* ptr, dummy_supervisor*)
        : actor_zeta::actor::actor_mixin<dummy_supervisor_sub>()
        , resource_(ptr)
        , executor_(new actor_zeta::test::scheduler_test_t(1, 1)) {
        executor_->start();
        supervisor_sub_counter++;
    }

    dummy_supervisor_sub(std::pmr::memory_resource* ptr)
        : actor_zeta::actor::actor_mixin<dummy_supervisor_sub>()
        , resource_(ptr)
        , executor_(new actor_zeta::test::scheduler_test_t(1, 1)) {
        executor_->start();
        supervisor_sub_counter++;
    }

    std::pmr::memory_resource* resource() const noexcept { return resource_; }

    auto scheduler_test() noexcept -> actor_zeta::test::scheduler_test_t* {
        return executor_.get();
    }

    void behavior(actor_zeta::mailbox::message*) {
        // Empty behavior
    }

    template<typename ReturnType, typename... Args>
    ReturnType enqueue_impl(
        actor_zeta::actor::address_t sender,
        actor_zeta::mailbox::message_id cmd,
        Args&&... args
    ) {
        using R = typename actor_zeta::type_traits::is_unique_future<ReturnType>::value_type;
        return enqueue_sync_impl<R>(
            sender,
            cmd,
            [this](auto* msg) { behavior(msg); },
            std::forward<Args>(args)...
        );
    }

protected:

private:
    std::pmr::memory_resource* resource_;
    std::unique_ptr<actor_zeta::test::scheduler_test_t> executor_;
};

class dummy_supervisor final : public actor_zeta::actor::actor_mixin<dummy_supervisor> {
public:
    template<typename T> using unique_future = actor_zeta::unique_future<T>;

    dummy_supervisor(std::pmr::memory_resource* ptr)
        : actor_zeta::actor::actor_mixin<dummy_supervisor>()
        , resource_(ptr)
        , executor_(new actor_zeta::test::scheduler_test_t(1, 1)) {
        executor_->start();
        supervisor_counter++;
    }

    std::pmr::memory_resource* resource() const noexcept { return resource_; }

    auto scheduler_test() noexcept -> actor_zeta::test::scheduler_test_t* {
        return executor_.get();
    }

    actor_zeta::unique_future<void> create_actor() {
        auto uptr = actor_zeta::spawn<storage_t>(resource_, this);
        actors_.emplace_back(std::move(uptr));
        co_return;
    }

    actor_zeta::unique_future<void> create_supervisor() {
        auto uptr = actor_zeta::spawn<dummy_supervisor_sub>(resource_, this);
        supervisor_.emplace_back(std::move(uptr));
        co_return;
    }

    actor_zeta::unique_future<void> create_supervisor_custom_resource() {
        auto uptr = actor_zeta::spawn<dummy_supervisor_sub>(resource_);
        supervisor_.emplace_back(std::move(uptr));
        co_return;
    }

    void behavior(actor_zeta::mailbox::message* msg) {
        if (msg->command() == actor_zeta::msg_id<dummy_supervisor, &dummy_supervisor::create_actor>) {
            dispatch(this, &dummy_supervisor::create_actor, msg);
        } else if (msg->command() == actor_zeta::msg_id<dummy_supervisor, &dummy_supervisor::create_supervisor>) {
            dispatch(this, &dummy_supervisor::create_supervisor, msg);
        } else if (msg->command() == actor_zeta::msg_id<dummy_supervisor, &dummy_supervisor::create_supervisor_custom_resource>) {
            dispatch(this, &dummy_supervisor::create_supervisor_custom_resource, msg);
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &dummy_supervisor::create_actor,
        &dummy_supervisor::create_supervisor,
        &dummy_supervisor::create_supervisor_custom_resource
    >;

    template<typename ReturnType, typename... Args>
    ReturnType enqueue_impl(
        actor_zeta::actor::address_t sender,
        actor_zeta::mailbox::message_id cmd,
        Args&&... args
    ) {
        using R = typename actor_zeta::type_traits::is_unique_future<ReturnType>::value_type;
        return enqueue_sync_impl<R>(
            sender,
            cmd,
            [this](auto* msg) { behavior(msg); },
            std::forward<Args>(args)...
        );
    }

protected:

private:
    std::pmr::memory_resource* resource_;
    std::unique_ptr<actor_zeta::test::scheduler_test_t> executor_;
    std::vector<storage_t::unique_actor> actors_;
    std::vector<std::unique_ptr<dummy_supervisor_sub, actor_zeta::pmr::deleter_t>> supervisor_;
};


 storage_t::storage_t(std::pmr::memory_resource* resource_ptr, dummy_supervisor*): actor_zeta::basic_actor<storage_t>(resource_ptr) {
    actor_counter++;
}

dummy_supervisor_sub::dummy_supervisor_sub(dummy_supervisor* ptr): actor_zeta::actor::actor_mixin<dummy_supervisor_sub>()
       , resource_(ptr->resource())
       , executor_(new actor_zeta::test::scheduler_test_t(1, 1)) {
    ///auto * ptr_sheduler = scheduler();
    ///ptr_sheduler->start();
    executor_->start();
    supervisor_sub_counter++;
}

TEST_CASE("spawn base supervisor") {
    supervisor_counter = 0;
    actor_counter = 0;
    auto* mr_ptr =std::pmr::get_default_resource();
    auto supervisor = actor_zeta::spawn<dummy_supervisor>(mr_ptr);
    REQUIRE(supervisor_counter == 1);
}

TEST_CASE("spawn supervisor") {
    supervisor_counter = 0;
    supervisor_sub_counter = 0;
    actor_counter = 0;
    auto* mr_ptr =std::pmr::get_default_resource();
    auto supervisor = actor_zeta::spawn<dummy_supervisor>(mr_ptr);
    auto fut = actor_zeta::send(supervisor.get(), actor_zeta::address_t::empty_address(), &dummy_supervisor::create_supervisor);
    supervisor->scheduler_test()->run_once();
    std::move(fut).get();
    REQUIRE(supervisor_counter == 1);
    REQUIRE(supervisor_sub_counter == 1);
}

TEST_CASE("spawn supervisor custom resource") {
    supervisor_counter = 0;
    supervisor_sub_counter = 0;
    actor_counter = 0;
    auto* mr_ptr =std::pmr::get_default_resource();
    auto supervisor = actor_zeta::spawn<dummy_supervisor>(mr_ptr);
    auto fut = actor_zeta::send(supervisor.get(), actor_zeta::address_t::empty_address(), &dummy_supervisor::create_supervisor_custom_resource);
    supervisor->scheduler_test()->run_once();
    std::move(fut).get();
    REQUIRE(supervisor_counter == 1);
    REQUIRE(supervisor_sub_counter == 1);
}

TEST_CASE("spawn actor") {
    supervisor_counter = 0;
    actor_counter = 0;
    auto* mr_ptr =std::pmr::get_default_resource();
    auto supervisor = actor_zeta::spawn<dummy_supervisor>(mr_ptr);
    auto fut = actor_zeta::send(supervisor.get(), actor_zeta::address_t::empty_address(), &dummy_supervisor::create_actor);
    supervisor->scheduler_test()->run_once();
    std::move(fut).get();
    REQUIRE(actor_counter == 1);
}

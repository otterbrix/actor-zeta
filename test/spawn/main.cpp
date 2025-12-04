#define CATCH_CONFIG_MAIN // This tells Catch to provide a main() - only do this in one cpp file
#include <catch2/catch.hpp>

#include "classes.hpp"

#include <cassert>

#include <set>
#include <string>

#include <actor-zeta.hpp>
#include <actor-zeta/dispatch.hpp>
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

    ~storage_t() = default;

    actor_zeta::unique_future<void> behavior(actor_zeta::mailbox::message* msg) {
        switch (msg->command()) {
            case update_id: {
                return actor_zeta::dispatch(this, &storage_t::update, msg);
            }
            case find_id: {
                return actor_zeta::dispatch(this, &storage_t::find, msg);
            }
            case remove_id: {
                return actor_zeta::dispatch(this, &storage_t::remove, msg);
            }
        }
        return actor_zeta::make_ready_future_void(resource());
    }

    using dispatch_traits = actor_zeta::dispatch_traits<>;

private:
    void update() {}
    void find() {}
    void remove() {}
};

class dummy_supervisor_sub final : public actor_zeta::base::actor_mixin<dummy_supervisor_sub> {
public:
    template<typename T> using unique_future = actor_zeta::unique_future<T>;

    dummy_supervisor_sub(dummy_supervisor* ptr);

    dummy_supervisor_sub(actor_zeta::pmr::memory_resource* ptr, dummy_supervisor*)
        : actor_zeta::base::actor_mixin<dummy_supervisor_sub>()
        , resource_(ptr)
        , executor_(new actor_zeta::test::scheduler_test_t(1, 1)) {
        executor_->start();
        supervisor_sub_counter++;
    }

    dummy_supervisor_sub(actor_zeta::pmr::memory_resource* ptr)
        : actor_zeta::base::actor_mixin<dummy_supervisor_sub>()
        , resource_(ptr)
        , executor_(new actor_zeta::test::scheduler_test_t(1, 1)) {
        executor_->start();
        supervisor_sub_counter++;
    }

    actor_zeta::pmr::memory_resource* resource() const noexcept { return resource_; }

    auto scheduler_test() noexcept -> actor_zeta::test::scheduler_test_t* {
        return executor_.get();
    }

    actor_zeta::unique_future<void> behavior(actor_zeta::mailbox::message*) {
        // Empty behavior
        return actor_zeta::make_ready_future_void(resource());
    }

    /// @brief Override enqueue_impl для supervisor - используем helper
    template<typename R, typename... Args>
    unique_future<R> enqueue_impl(
        actor_zeta::base::address_t sender,
        actor_zeta::mailbox::message_id cmd,
        Args&&... args
    ) {
        return enqueue_sync_impl<R>(
            sender,
            cmd,
            [this](auto* msg) { behavior(msg); },
            std::forward<Args>(args)...
        );
    }

protected:

private:
    actor_zeta::pmr::memory_resource* resource_;
    std::unique_ptr<actor_zeta::test::scheduler_test_t> executor_;
};

class dummy_supervisor final : public actor_zeta::base::actor_mixin<dummy_supervisor> {
public:
    template<typename T> using unique_future = actor_zeta::unique_future<T>;

    dummy_supervisor(actor_zeta::pmr::memory_resource* ptr)
        : actor_zeta::base::actor_mixin<dummy_supervisor>()
        , resource_(ptr)
        , executor_(new actor_zeta::test::scheduler_test_t(1, 1)) {
        executor_->start();
        supervisor_counter++;
    }

    actor_zeta::pmr::memory_resource* resource() const noexcept { return resource_; }

    auto scheduler_test() noexcept -> actor_zeta::test::scheduler_test_t* {
        return executor_.get();
    }

    actor_zeta::unique_future<void> create_actor() {
        auto uptr = actor_zeta::spawn<storage_t>(resource_, this);
        actors_.emplace_back(std::move(uptr));
        return actor_zeta::make_ready_future_void(resource_);
    }

    actor_zeta::unique_future<void> create_supervisor() {
        auto uptr = actor_zeta::spawn<dummy_supervisor_sub>(resource_, this);
        supervisor_.emplace_back(std::move(uptr));
        return actor_zeta::make_ready_future_void(resource_);
    }

    actor_zeta::unique_future<void> create_supervisor_custom_resource() {
        auto uptr = actor_zeta::spawn<dummy_supervisor_sub>(resource_);
        supervisor_.emplace_back(std::move(uptr));
        return actor_zeta::make_ready_future_void(resource_);
    }

    actor_zeta::unique_future<void> behavior(actor_zeta::mailbox::message* msg) {
        if (msg->command() == actor_zeta::msg_id<dummy_supervisor, &dummy_supervisor::create_actor>) {
            return actor_zeta::dispatch(this, &dummy_supervisor::create_actor, msg);
        } else if (msg->command() == actor_zeta::msg_id<dummy_supervisor, &dummy_supervisor::create_supervisor>) {
            return actor_zeta::dispatch(this, &dummy_supervisor::create_supervisor, msg);
        } else if (msg->command() == actor_zeta::msg_id<dummy_supervisor, &dummy_supervisor::create_supervisor_custom_resource>) {
            return actor_zeta::dispatch(this, &dummy_supervisor::create_supervisor_custom_resource, msg);
        }
        return actor_zeta::make_ready_future_void(resource());
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &dummy_supervisor::create_actor,
        &dummy_supervisor::create_supervisor,
        &dummy_supervisor::create_supervisor_custom_resource
    >;

    /// @brief Override enqueue_impl для supervisor - используем helper
    template<typename R, typename... Args>
    unique_future<R> enqueue_impl(
        actor_zeta::base::address_t sender,
        actor_zeta::mailbox::message_id cmd,
        Args&&... args
    ) {
        return enqueue_sync_impl<R>(
            sender,
            cmd,
            [this](auto* msg) { behavior(msg); },
            std::forward<Args>(args)...
        );
    }

protected:

private:
    actor_zeta::pmr::memory_resource* resource_;
    std::unique_ptr<actor_zeta::test::scheduler_test_t> executor_;
    std::vector<storage_t::unique_actor> actors_;
    std::vector<std::unique_ptr<dummy_supervisor_sub, actor_zeta::pmr::deleter_t>> supervisor_;
};


 storage_t::storage_t(actor_zeta::pmr::memory_resource* resource_ptr, dummy_supervisor*): actor_zeta::basic_actor<storage_t>(resource_ptr) {
    actor_counter++;
}

dummy_supervisor_sub::dummy_supervisor_sub(dummy_supervisor* ptr): actor_zeta::base::actor_mixin<dummy_supervisor_sub>()
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
    auto* mr_ptr = actor_zeta::pmr::get_default_resource();
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
    auto* mr_ptr = actor_zeta::pmr::get_default_resource();
    auto supervisor = actor_zeta::spawn<dummy_supervisor>(mr_ptr);
    auto fut = actor_zeta::send(supervisor.get(), actor_zeta::address_t::empty_address(), &dummy_supervisor::create_actor);
    supervisor->scheduler_test()->run_once();
    std::move(fut).get();
    REQUIRE(actor_counter == 1);
}

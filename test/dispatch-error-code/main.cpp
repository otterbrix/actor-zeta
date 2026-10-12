#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <memory_resource>
#include <system_error>

#include <actor-zeta.hpp>

// A dispatched method that co_returns an std::error_code fails its caller's future with that code,
// whether it fails at once or on a later step.

using namespace actor_zeta;

namespace {

    class coder_t final : public basic_actor<coder_t> {
    public:
        explicit coder_t(std::pmr::memory_resource* res)
            : basic_actor<coder_t>(res) {}

        unique_future<int> fail_now() {
            co_return std::make_error_code(std::errc::invalid_argument);
        }

        unique_future<int> fail_later() {
            co_await std::move(gate_);
            co_return std::make_error_code(std::errc::timed_out);
        }

        unique_future<int> answer() {
            co_return 42;
        }

        using dispatch_traits = actor_zeta::dispatch_traits<
            &coder_t::fail_now,
            &coder_t::fail_later,
            &coder_t::answer>;

        behavior_t behavior(mailbox::message* msg) {
            if (msg->command() == msg_id<coder_t, &coder_t::fail_now>) {
                co_await dispatch(this, &coder_t::fail_now, msg);
            } else if (msg->command() == msg_id<coder_t, &coder_t::fail_later>) {
                co_await dispatch(this, &coder_t::fail_later, msg);
            } else if (msg->command() == msg_id<coder_t, &coder_t::answer>) {
                co_await dispatch(this, &coder_t::answer, msg);
            }
        }

        unique_future<void> gate_;
    };

} // namespace

TEST_CASE("dispatch(): a method's error code at once reaches the caller") {
    std::pmr::unsynchronized_pool_resource resource;
    auto actor = spawn<coder_t>(&resource);

    auto [needs_sched, future] = send(actor.get(), &coder_t::fail_now);
    REQUIRE(needs_sched);
    REQUIRE(actor->resume(1).result == scheduler::resume_result::awaiting);

    REQUIRE(future.is_ready());
    REQUIRE(future.failed());
    REQUIRE(future.error() == std::make_error_code(std::errc::invalid_argument));
}

TEST_CASE("dispatch(): a method's error code after a suspension reaches the caller") {
    std::pmr::unsynchronized_pool_resource resource;
    auto actor = spawn<coder_t>(&resource);
    promise<void> gate(&resource);
    actor->gate_ = gate.get_future();

    auto [needs_sched, future] = send(actor.get(), &coder_t::fail_later);
    REQUIRE(needs_sched);
    REQUIRE(actor->resume(1).result == scheduler::resume_result::resume); // suspended: keeps the turn
    REQUIRE_FALSE(future.is_ready());

    gate.set_value();
    REQUIRE(actor->resume(1).result == scheduler::resume_result::awaiting);

    REQUIRE(future.is_ready());
    REQUIRE(future.failed());
    REQUIRE(future.error() == std::make_error_code(std::errc::timed_out));
}

TEST_CASE("dispatch(): a value still reaches the caller") {
    std::pmr::unsynchronized_pool_resource resource;
    auto actor = spawn<coder_t>(&resource);

    auto [needs_sched, future] = send(actor.get(), &coder_t::answer);
    REQUIRE(needs_sched);
    REQUIRE(actor->resume(1).result == scheduler::resume_result::awaiting);

    REQUIRE(future.is_ready());
    REQUIRE_FALSE(future.failed());
    REQUIRE(std::move(future).take_ready() == 42);
}

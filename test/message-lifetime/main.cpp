/// @file
/// A message lives as long as the behavior it started: behavior() may co_await first and
/// dispatch the message later. The actor runs on a resource that scribbles over freed memory, so
/// a message freed at the behavior's first suspension shows up as a wrong answer or a crash, not
/// only under ASan.

#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta.hpp>

#include <cstddef>
#include <cstring>
#include <memory_resource>
#include <optional>

using namespace actor_zeta;

namespace {

    // Fills every freed block with 0xDD before handing it back.
    struct scribbling_resource final : std::pmr::memory_resource {
        std::pmr::memory_resource* upstream_ = std::pmr::new_delete_resource();

        void* do_allocate(std::size_t bytes, std::size_t align) override {
            return upstream_->allocate(bytes, align);
        }

        void do_deallocate(void* p, std::size_t bytes, std::size_t align) override {
            std::memset(p, 0xDD, bytes);
            upstream_->deallocate(p, bytes, align);
        }

        bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
            return this == &other;
        }
    };

    class late_dispatcher final : public basic_actor<late_dispatcher> {
    public:
        explicit late_dispatcher(std::pmr::memory_resource* res)
            : basic_actor<late_dispatcher>(res) {}

        unique_future<int> twice(int x) {
            co_return x * 2;
        }

        using dispatch_traits = actor_zeta::dispatch_traits<&late_dispatcher::twice>;

        // Waits for the gate BEFORE dispatching: the message must still be there afterwards.
        behavior_t behavior(mailbox::message* msg) {
            if (msg->command() == msg_id<late_dispatcher, &late_dispatcher::twice>) {
                co_await std::move(gate_);
                co_await dispatch(this, &late_dispatcher::twice, msg);
            }
        }

        unique_future<void> gate_;
    };

} // namespace

TEST_CASE("a message outlives the first suspension of its behavior") {
    scribbling_resource resource;
    auto actor = spawn<late_dispatcher>(&resource);

    promise<void> gate(&resource);
    actor->gate_ = gate.get_future();

    auto [needs_sched, answer] = send(actor.get(), &late_dispatcher::twice, 21);
    REQUIRE(needs_sched);
    REQUIRE(actor->resume(8).result == scheduler::resume_result::resume); // suspended on the gate

    gate.set_value();
    auto verdict = scheduler::resume_result::resume;
    for (int i = 0; i < 16 && verdict == scheduler::resume_result::resume; ++i) {
        verdict = actor->resume(8).result;
    }

    REQUIRE(answer.is_ready());
    REQUIRE_FALSE(answer.failed());
    REQUIRE(std::move(answer).take_ready() == 42);
}

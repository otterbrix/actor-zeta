#define CATCH_CONFIG_MAIN // This tells Catch to provide a main() - only do this in one cpp file
#include <catch2/catch.hpp>

#define TEST_HAS_NO_EXCEPTIONS

#include <actor-zeta/detail/queue/lifo_inbox.hpp>
#include <actor-zeta/detail/queue/singly_linked.hpp>
#include <actor-zeta/mailbox/make_message.hpp>
#include <actor-zeta/mailbox/message.hpp>

using namespace actor_zeta::detail;

namespace {

    struct inode : singly_linked<inode> {
        int value;
        explicit inode(int x = 0)
            : value(x) {}
    };

    auto to_string(const inode& x) -> std::string {
        return std::to_string(x.value);
    }

    using inbox_type = lifo_inbox<inode>;

    struct fixture {
        inbox_type inbox;

        void fill() {
        }

        template<class T, class... Ts>
        void fill(T x, Ts... xs) {
            inbox.emplace_front(x);
            fill(xs...);
        }

        auto fetch() -> std::string {
            std::string result;
            std::unique_ptr<inode> ptr{inbox.take_head()};
            while (ptr != nullptr) {
                auto next = ptr->next;
                result += to_string(*ptr);
                ptr.reset(inbox_type::promote(next));
            }
            return result;
        }

        auto close_and_fetch() -> std::string {
            std::string result;
            auto f = [&](inode* x) {
                result += to_string(*x);
                delete x;
            };
            inbox.close(f);
            return result;
        }
    };

} // namespace

TEST_CASE("lifo_inbox_tests") {
    SECTION("default_constructed") {
        fixture fix;
        REQUIRE(fix.inbox.empty());
    }

    SECTION("push_front") {
        fixture fix;
        fix.fill(1, 2, 3);
        REQUIRE(fix.close_and_fetch() == "321");
        REQUIRE(fix.inbox.closed());
    }

    SECTION("push_after_close") {
        fixture fix;
        fix.inbox.close();
        auto res = fix.inbox.push_front(new inode(0));
        REQUIRE(res == enqueue_result::queue_closed);
    }

    SECTION("unblock") {
        fixture fix;
        REQUIRE(fix.inbox.try_block());
        auto res = fix.inbox.push_front(new inode(1));
        REQUIRE(res == enqueue_result::unblocked_reader);
        res = fix.inbox.push_front(new inode(2));
        REQUIRE(res == enqueue_result::success);
        REQUIRE(fix.close_and_fetch() == "21");
    }
}

// A queue must free its leftovers with the element's OWN deleter.
//
// mailbox::message lives inside a PMR block behind a BlockHdr, so
// std::default_delete hands the global allocator a pointer that is not the
// allocation base. The one reachable path is a push into an already-closed inbox
// -- what default_mailbox_impl does when a send races shutdown -- where ASan
// reported "attempting free on address which was not malloc()-ed".
//
// The static_assert in lifo_inbox/linked_list makes the wrong instantiation a
// COMPILE error; this test pins the runtime half, and bites under ASan.

TEST_CASE("lifo_inbox: a rejected push frees the message through its own deleter") {
    auto* resource = std::pmr::get_default_resource();
    lifo_inbox<actor_zeta::mailbox::message, actor_zeta::mailbox::message_deleter> inbox;

    auto msg = actor_zeta::mailbox::pmr_make_message(
        resource, resource, actor_zeta::mailbox::make_message_id(7));
    REQUIRE(msg);

    inbox.close();

    // Ownership passes to the queue, which must reject it AND free it correctly.
    const auto result = inbox.push_front(msg.release());
    REQUIRE(result == enqueue_result::queue_closed);
}

#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>



#include <string>
#include <deque>
#include <list>
#include <map>
#include <queue>
#include <vector>
#include <memory>      // unique_ptr
#include <tuple>       // piecewise_construct
#include <type_traits>


#include <actor-zeta/detail/ignore_unused.hpp>
#include <actor-zeta/mailbox/make_message.hpp>

using actor_zeta::actor::address_t;
using actor_zeta::mailbox::message;
using actor_zeta::mailbox::message_ptr;
using actor_zeta::detail::rtt;
using actor_zeta::mailbox::make_message_id;

constexpr static auto zero  = actor_zeta::mailbox::make_message_id(0);
constexpr static auto one   = actor_zeta::mailbox::make_message_id(1);
constexpr static auto two   = actor_zeta::mailbox::make_message_id(2);
constexpr static auto three = actor_zeta::mailbox::make_message_id(3);

namespace {

// Helper functions for testing message in containers
// Note: message constructor no longer takes result_slot parameter

template<class Seq>
void check_seq_push_back(Seq& v, std::pmr::memory_resource* res) {
    v.emplace_back(res, one);
    v.emplace_back(res, two);
    v.emplace_back(res, three);
    REQUIRE(v.size() == 3);
    v.clear();
}

template<class Seq>
void check_seq_insert_at_end(Seq& v, std::pmr::memory_resource* res) {
    v.emplace(v.end(), res, one);
    v.emplace(v.end(), res, two);
    v.emplace(v.end(), res, three);
    REQUIRE(v.size() == 3);
    v.clear();
}

// For queue<message>
inline void check_queue_push(std::queue<message>& q,
                             std::pmr::memory_resource* res) {
    q.emplace(res, one);
    q.emplace(res, two);
    q.emplace(res, three);
    REQUIRE(q.size() == 3);
    while (!q.empty()) q.pop();
}

// --- For map<size_t, message> ---

inline void check_map_basic(std::map<size_t, message>& m,
                            std::pmr::memory_resource* res) {
    m.emplace(std::piecewise_construct,
              std::forward_as_tuple(0ul),
              std::forward_as_tuple(res, one));
    m.emplace(std::piecewise_construct,
              std::forward_as_tuple(1ul),
              std::forward_as_tuple(res, two));
    m.emplace(std::piecewise_construct,
              std::forward_as_tuple(2ul),
              std::forward_as_tuple(res, three));
    REQUIRE(m.size() == 3);
    m.clear();
}

inline void check_map_emplace(std::map<size_t, message>& m,
                              std::pmr::memory_resource* res) {
    m.emplace(std::piecewise_construct,
              std::forward_as_tuple(0ul),
              std::forward_as_tuple(res, one));
    m.emplace(std::piecewise_construct,
              std::forward_as_tuple(1ul),
              std::forward_as_tuple(res, two));
    m.emplace(std::piecewise_construct,
              std::forward_as_tuple(2ul),
              std::forward_as_tuple(res, three));
    REQUIRE(m.size() == 3);
    m.clear();
}

inline void check_map_zero_id(std::map<size_t, message>& m,
                              std::pmr::memory_resource* res) {
    m.emplace(std::piecewise_construct,
              std::forward_as_tuple(0ul),
              std::forward_as_tuple(res, zero));
    m.emplace(std::piecewise_construct,
              std::forward_as_tuple(1ul),
              std::forward_as_tuple(res, zero));
    m.emplace(std::piecewise_construct,
              std::forward_as_tuple(2ul),
              std::forward_as_tuple(res, zero));
    REQUIRE(m.size() == 3);
    m.clear();
}

} // namespace

TEST_CASE("message (no move/copy of message/rtt)") {
    auto* resource =std::pmr::get_default_resource();

    SECTION("vector of messages") {
        std::vector<message> v;
        check_seq_push_back(v, resource);
        check_seq_insert_at_end(v, resource);
    }

    SECTION("list of messages") {
        std::list<message> v;
        check_seq_push_back(v, resource);
        check_seq_insert_at_end(v, resource);
    }

    SECTION("deque of messages") {
        std::deque<message> v;
        check_seq_push_back(v, resource);
        check_seq_insert_at_end(v, resource);
    }

    SECTION("queue of messages") {
        std::queue<message> q;
        check_queue_push(q, resource);
    }

    SECTION("map of messages") {
        std::map<size_t, message> m;
        check_map_basic(m, resource);
        check_map_emplace(m, resource);
        check_map_zero_id(m, resource);
    }

    SECTION("simple") {
        // 1) simple message via make_message
        auto [msg, future] = actor_zeta::detail::make_message(resource, one);
        REQUIRE( static_cast<bool>(msg) ); // message_ptr has operator bool
        REQUIRE( msg->command() == actor_zeta::mailbox::make_message_id(1) );
        actor_zeta::detail::ignore_unused(future); // unused in this test

        // 2) separate payload - use specialized rtt move constructor
        rtt body(resource, int(1));
        message msg2(resource, one, std::move(body));
        REQUIRE(msg2.body().get<int>(0) == 1);
    }

    SECTION("swap messages") {
        message msg1(resource, one);
        message msg2(resource, two);

        msg1.swap(msg2);

        REQUIRE( msg1.command() == actor_zeta::mailbox::make_message_id(2) );
        REQUIRE( msg2.command() == actor_zeta::mailbox::make_message_id(1) );
    }

    SECTION("allocator-extended move constructor") {
        message msg1(resource, three);
        REQUIRE( msg1.command() == actor_zeta::mailbox::make_message_id(3) );

        // Use allocator-extended move constructor (PMR migration)
        message msg2(std::allocator_arg, resource, std::move(msg1));
        REQUIRE( msg2.command() == actor_zeta::mailbox::make_message_id(3) );
    }

    SECTION("message with multiple payload values") {
        auto [msg, future2] = actor_zeta::detail::make_message(resource, one,
                                           int(42), std::string("test"), double(3.14));
        REQUIRE( msg->body().get<int>(0) == 42 );
        REQUIRE( msg->body().get<std::string>(1) == "test" );
        REQUIRE( msg->body().get<double>(2) == Approx(3.14) );
        actor_zeta::detail::ignore_unused(future2);
    }

    SECTION("zero message id") {
        message msg(resource, zero);
        REQUIRE( msg.command() == actor_zeta::mailbox::make_message_id(0) );
    }

    // NOTE: Cross-arena RTT migration is not supported for type-erased containers
    // containing non-trivial types, as it would require proper copy construction
    // which is impossible without runtime type information
    SECTION("rtt same-arena migration") {
        // Create rtt in arena
        auto* arena =std::pmr::get_default_resource();
        rtt rtt1(arena, int(42), std::string("test"), double(3.14));
        REQUIRE( rtt1.get<int>(0) == 42 );
        REQUIRE( rtt1.get<std::string>(1) == "test" );
        REQUIRE( rtt1.get<double>(2) == Approx(3.14) );

        // Same-arena migration via allocator-extended move constructor
        rtt rtt2(std::allocator_arg, arena, std::move(rtt1));

        // Verify data is preserved after migration
        REQUIRE( rtt2.get<int>(0) == 42 );
        REQUIRE( rtt2.get<std::string>(1) == "test" );
        REQUIRE( rtt2.get<double>(2) == Approx(3.14) );
    }

    SECTION("init_future_slot and transfer_ownership") {
        // Test the new unified slot API
        message msg(resource, one);
        REQUIRE( !msg.has_result_slot() );

        // Create a shared_state and init the slot
        auto* state = actor_zeta::detail::allocate_shared_state<int>(resource);
        msg.init_future_slot<int>(state);
        REQUIRE( msg.has_result_slot() );

        // Transfer ownership
        msg.transfer_ownership();

        // After transfer, destructor should NOT call cleanup_fn_
        // (We can't directly test this, but it should not crash)

        // Clean up manually since ownership was transferred
        (void)state->release_promise();
        state->release_future();
    }
}

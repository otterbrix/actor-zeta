#define CATCH_CONFIG_MAIN // This tells Catch to provide a main() - only do this in one cpp file
#include <catch2/catch.hpp>

#include "classes.hpp"

#include <string>

#include <deque>
#include <list>
#include <map>
#include <queue>
#include <vector>


constexpr static auto zero = actor_zeta::make_message_id(0);

constexpr static auto one = actor_zeta::make_message_id(1);

constexpr static auto two = actor_zeta::make_message_id(2);

constexpr static auto three = actor_zeta::make_message_id(3);


TEST_CASE("message") {
    auto*resource =  actor_zeta::pmr::get_default_resource();
    SECTION("vector of plain messages") {
        std::vector<message> v;
        v.emplace_back(resource,address_t::empty_address(), one, rtt(resource, std::string("1"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15)));
        v.emplace_back(resource,address_t::empty_address(), two, rtt(resource, std::string("2"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15)));
        v.emplace_back(resource,address_t::empty_address(), three, rtt(resource, std::string("3"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15)));
        REQUIRE(v.size() == 3);
        v.clear();
        v.push_back(message(resource,address_t::empty_address(), one, rtt(resource, std::string("1"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15))));
        v.push_back(message(resource,address_t::empty_address(), two, rtt(resource, std::string("2"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15))));
        v.push_back(message(resource,address_t::empty_address(), three, rtt(resource, std::string("3"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15))));
        REQUIRE(v.size() == 3);
        v.clear();
        v.emplace(v.end(), message(resource,address_t::empty_address(), one, rtt(resource, std::string("1"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15))));
        v.emplace(v.end(), message(resource,address_t::empty_address(), two, rtt(resource, std::string("2"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15))));
        v.emplace(v.end(), message(resource,address_t::empty_address(), three, rtt(resource, std::string("3"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15))));
        REQUIRE(v.size() == 3);
        v.clear();
        v.insert(v.end(), message(resource,address_t::empty_address(), one, rtt(resource, std::string("1"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15))));
        v.insert(v.end(), message(resource,address_t::empty_address(), two, rtt(resource, std::string("2"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15))));
        v.insert(v.end(), message(resource,address_t::empty_address(),three, rtt(resource, std::string("3"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15))));
        REQUIRE(v.size() == 3);
    }
    SECTION("list of plain messages") {
        std::list<message> v;
        v.emplace_back(resource,address_t::empty_address(), one, rtt(resource, std::string("1"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15)));
        v.emplace_back(resource,address_t::empty_address(), two, rtt(resource, std::string("2"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15)));
        v.emplace_back(resource,address_t::empty_address(), three, rtt(resource, std::string("3"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15)));
        REQUIRE(v.size() == 3);
        v.clear();
        v.push_back(message(resource,address_t::empty_address(), one, rtt(resource, std::string("1"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15))));
        v.push_back(message(resource,address_t::empty_address(), two, rtt(resource, std::string("2"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15))));
        v.push_back(message(resource,address_t::empty_address(), three, rtt(resource, std::string("3"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15))));
        REQUIRE(v.size() == 3);
        v.clear();
        v.emplace(v.end(), message(resource,address_t::empty_address(), one, rtt(resource, std::string("1"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15))));
        v.emplace(v.end(), message(resource,address_t::empty_address(), two, rtt(resource, std::string("2"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15))));
        v.emplace(v.end(), message(resource,address_t::empty_address(), three, rtt(resource, std::string("3"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15))));
        REQUIRE(v.size() == 3);
        v.clear();
        v.insert(v.end(), message(resource,address_t::empty_address(), one, rtt(resource, std::string("1"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15))));
        v.insert(v.end(), message(resource,address_t::empty_address(), two, rtt(resource, std::string("2"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15))));
        v.insert(v.end(), message(resource,address_t::empty_address(), three, rtt(resource, std::string("3"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15))));
        REQUIRE(v.size() == 3);
    }
    SECTION("map of plain messages") {
        std::map<size_t, message> v;
        v.emplace(0, message(resource,address_t::empty_address(), one, rtt(resource, std::string("1"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15))));
        v.emplace(1, message(resource,address_t::empty_address(), two, rtt(resource, std::string("2"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15))));
        v.emplace(2, message(resource,address_t::empty_address(), three, rtt(resource, std::string("3"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15))));
        REQUIRE(v.size() == 3);
        v.clear();
        v.emplace(std::make_pair(0, message( resource,address_t::empty_address(), one, rtt(resource, std::string("1"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15)))));
        v.emplace(std::make_pair(1, message(resource,address_t::empty_address(), two, rtt(resource, std::string("2"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15)))));
        v.emplace(std::make_pair(2, message(resource,address_t::empty_address(), three, rtt(resource, std::string("3"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15)))));
        REQUIRE(v.size() == 3);
        v.clear();
        v.emplace(std::pair<size_t, message>{0, message(resource,address_t::empty_address(), one, rtt(resource, std::string("1"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15)))});
        v.emplace(std::pair<size_t, message>{1, message(resource,address_t::empty_address(), two, rtt(resource, std::string("2"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15)))});
        v.emplace(std::pair<size_t, message>{2, message(resource,address_t::empty_address(), three, rtt(resource, std::string("3"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15)))});
        REQUIRE(v.size() == 3);
        v.clear();
        v.emplace(std::piecewise_construct,
                  std::forward_as_tuple(0),
                  std::forward_as_tuple(message(resource,address_t::empty_address(), one, rtt(resource, std::string("1"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15)))));
        v.emplace(std::piecewise_construct,
                  std::forward_as_tuple(1),
                  std::forward_as_tuple(message(resource,address_t::empty_address(), two, rtt(resource, std::string("2"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15)))));
        v.emplace(std::piecewise_construct,
                  std::forward_as_tuple(2),
                  std::forward_as_tuple(message(resource,address_t::empty_address(), three, rtt(resource, std::string("3"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15)))));
        REQUIRE(v.size() == 3);
        v.clear();
        v.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(0ul),
            std::forward_as_tuple(
            resource,address_t::empty_address(), zero, rtt(resource, std::string("0"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15))
            )
        );
        v.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(1ul),
            std::forward_as_tuple(
            resource,address_t::empty_address(), zero, rtt(resource, std::string("1"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15))
            )
        );
        v.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(2ul),
            std::forward_as_tuple(
            resource,address_t::empty_address(), zero, rtt(resource, std::string("2"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15))
            )
        );

        REQUIRE(v.size() == 3);
        v.clear();
        v.insert(std::make_pair(0, message(resource,address_t::empty_address(), one, rtt(resource, std::string("1"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15)))));
        v.insert(std::make_pair(1, message(resource,address_t::empty_address(), two, rtt(resource, std::string("2"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15)))));
        v.insert(std::make_pair(2, message(resource,address_t::empty_address(), three, rtt(resource, std::string("3"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15)))));
        REQUIRE(v.size() == 3);
        v.clear();
        v.insert(std::pair<size_t, message>{0, message(resource,address_t::empty_address(), one, rtt(resource, std::string("1"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15)))});
        v.insert(std::pair<size_t, message>{1, message(resource,address_t::empty_address(), two, rtt(resource, std::string("2"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15)))});
        v.insert(std::pair<size_t, message>{2, message(resource,address_t::empty_address(), three, rtt(resource, std::string("3"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15)))});
        REQUIRE(v.size() == 3);
    }
    SECTION("deque of plain messages") {
        std::deque<message> v;
        v.emplace_back(resource,address_t::empty_address(), one, rtt(resource, std::string("1"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15)));
        v.emplace_back(resource,address_t::empty_address(), two, rtt(resource, std::string("2"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15)));
        v.emplace_back(resource,address_t::empty_address(), three, rtt(resource, std::string("3"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15)));
        REQUIRE(v.size() == 3);
        v.clear();
        v.push_back(message(resource,address_t::empty_address(), one, rtt(resource, std::string("1"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15))));
        v.push_back(message(resource,address_t::empty_address(), two, rtt(resource, std::string("2"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15))));
        v.push_back(message(resource,address_t::empty_address(), three, rtt(resource, std::string("3"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15))));
        REQUIRE(v.size() == 3);
        v.clear();
        v.emplace(v.end(), message(resource,address_t::empty_address(), one, rtt(resource, std::string("1"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15))));
        v.emplace(v.end(), message(resource,address_t::empty_address(), two, rtt(resource, std::string("2"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15))));
        v.emplace(v.end(), message(resource,address_t::empty_address(), three, rtt(resource, std::string("3"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15))));
        REQUIRE(v.size() == 3);
        v.clear();
        v.insert(v.end(), message(resource,address_t::empty_address(), one, rtt(resource, std::string("1"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15))));
        v.insert(v.end(), message(resource,address_t::empty_address(), two, rtt(resource, std::string("2"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15))));
        v.insert(v.end(), message(resource,address_t::empty_address(), three, rtt(resource, std::string("3"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15))));
        REQUIRE(v.size() == 3);
    }
    SECTION("queue of plain messages") {
        {
            std::queue<message> v;
            v.emplace(resource,address_t::empty_address(), one, rtt(resource, std::string("1"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15)));
            v.emplace(resource,address_t::empty_address(), two, rtt(resource, std::string("2"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15)));
            v.emplace(resource,address_t::empty_address(), three, rtt(resource, std::string("3"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15)));
            REQUIRE(v.size() == 3);
        }
        {
            std::queue<message> v;
            v.push(message(resource,address_t::empty_address(), one, rtt(resource, std::string("1"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15))));
            v.push(message(resource,address_t::empty_address(), two, rtt(resource, std::string("2"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15))));
            v.push(message(resource,address_t::empty_address(), three, rtt(resource, std::string("3"), static_cast<int>(12), static_cast<int>(14), static_cast<int>(15))));
            REQUIRE(v.size() == 3);
        }
    }
    SECTION("simple") {
        message msg_1(resource,address_t::empty_address(), one, rtt(resource, int(1)));
        REQUIRE(msg_1.operator bool() == true);
        REQUIRE(msg_1.command() == actor_zeta::make_message_id(1));
        REQUIRE(msg_1.body().get<int>(0) == 1);
    }
}

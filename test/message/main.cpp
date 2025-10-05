#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include "classes.hpp"

#include <string>
#include <deque>
#include <list>
#include <map>
#include <queue>
#include <vector>
#include <memory>      // unique_ptr
#include <type_traits>

using actor_zeta::base::address_t;
using actor_zeta::mailbox::message;
using actor_zeta::mailbox::message_id;
using actor_zeta::detail::rtt;

constexpr static auto zero  = actor_zeta::make_message_id(0);
constexpr static auto one   = actor_zeta::make_message_id(1);
constexpr static auto two   = actor_zeta::make_message_id(2);
constexpr static auto three = actor_zeta::make_message_id(3);

namespace {

// C++11 аналога make_unique нет — свой хелпер
template<class T, class... Args>
std::unique_ptr<T> make_unique11(Args&&... args) {
    return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}

// Фабрика сообщения: создаём rtt как lvalue, затем конструируем message на куче
inline std::unique_ptr<message>
make_msg(actor_zeta::pmr::memory_resource* res,
         message_id id,
         const std::string& s)
{
    rtt body(res, std::string(s), 12, 14, 15); // lvalue -> копирование, не move
    return make_unique11<message>(res, address_t::empty_address(), id, body);
}

// --- Общие проверки для последовательных контейнеров unique_ptr<message> ---

template<class Seq>
void check_seq_push_back(Seq& v, actor_zeta::pmr::memory_resource* res) {
    v.push_back(make_msg(res, one,   "1"));
    v.push_back(make_msg(res, two,   "2"));
    v.push_back(make_msg(res, three, "3"));
    REQUIRE(v.size() == 3);
    v.clear();
}

template<class Seq>
void check_seq_insert_at_end(Seq& v, actor_zeta::pmr::memory_resource* res) {
    v.insert(v.end(), make_msg(res, one,   "1"));
    v.insert(v.end(), make_msg(res, two,   "2"));
    v.insert(v.end(), make_msg(res, three, "3"));
    REQUIRE(v.size() == 3);
    v.clear();
}

// Перегрузки для std::queue<unique_ptr<message>>
inline void check_queue_push(std::queue<std::unique_ptr<message>>& q,
                             actor_zeta::pmr::memory_resource* res) {
    q.push(make_msg(res, one,   "1"));
    q.push(make_msg(res, two,   "2"));
    q.push(make_msg(res, three, "3"));
    REQUIRE(q.size() == 3);
    while (!q.empty()) q.pop();
}

// --- Для map<size_t, unique_ptr<message>> ---

inline void check_map_basic(std::map<size_t, std::unique_ptr<message>>& m,
                            actor_zeta::pmr::memory_resource* res) {
    m.insert(std::make_pair(0ul, make_msg(res, one,   "1")));
    m.insert(std::make_pair(1ul, make_msg(res, two,   "2")));
    m.insert(std::make_pair(2ul, make_msg(res, three, "3")));
    REQUIRE(m.size() == 3);
    m.clear();
}

inline void check_map_emplace(std::map<size_t, std::unique_ptr<message>>& m,
                              actor_zeta::pmr::memory_resource* res) {
    m.emplace(0ul, make_msg(res, one,   "1"));
    m.emplace(1ul, make_msg(res, two,   "2"));
    m.emplace(2ul, make_msg(res, three, "3"));
    REQUIRE(m.size() == 3);
    m.clear();
}

inline void check_map_zero_id(std::map<size_t, std::unique_ptr<message>>& m,
                              actor_zeta::pmr::memory_resource* res) {
    m.emplace(0ul, make_msg(res, zero, "0"));
    m.emplace(1ul, make_msg(res, zero, "1"));
    m.emplace(2ul, make_msg(res, zero, "2"));
    REQUIRE(m.size() == 3);
    m.clear();
}

} // namespace

TEST_CASE("message (no move/copy of message/rtt)") {
    auto* resource = actor_zeta::pmr::get_default_resource();

    SECTION("vector of messages (unique_ptr)") {
        std::vector<std::unique_ptr<message>> v;
        check_seq_push_back(v, resource);
        check_seq_insert_at_end(v, resource);
    }

    SECTION("list of messages (unique_ptr)") {
        std::list<std::unique_ptr<message>> v;
        check_seq_push_back(v, resource);
        check_seq_insert_at_end(v, resource);
    }

    SECTION("deque of messages (unique_ptr)") {
        std::deque<std::unique_ptr<message>> v;
        check_seq_push_back(v, resource);
        check_seq_insert_at_end(v, resource);
    }

    SECTION("queue of messages (unique_ptr)") {
        std::queue<std::unique_ptr<message>> q;
        check_queue_push(q, resource);
    }

    SECTION("map of messages (unique_ptr)") {
        std::map<size_t, std::unique_ptr<message>> m;
        check_map_basic(m, resource);
        check_map_emplace(m, resource);
        check_map_zero_id(m, resource);
    }

    SECTION("simple") {
        // 1) простое сообщение через фабрику
        auto p = make_msg(resource, one, "");
        REQUIRE( static_cast<bool>(*p) ); // оператор bool у message
        REQUIRE( p->command() == actor_zeta::make_message_id(1) );

        // 2) отдельный payload как lvalue и явное построение message
        rtt body(resource, int(1));               // lvalue, копирование в message
        message msg2(resource, address_t::empty_address(), one, body);
        REQUIRE(msg2.body().get<int>(0) == 1);
    }
}
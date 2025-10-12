#include "actor-zeta/scheduler/scheduler.hpp"

#include <cassert>

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <thread>

#include <actor-zeta.hpp>
#include <actor-zeta/scheduler/sharing_scheduler.hpp>

std::atomic_int count_collection_part{0};
std::atomic_int count_collection{0};
std::atomic_int count_balancer{0};
std::atomic_int count_insert{0};
std::atomic_int count_remove{0};
std::atomic_int count_update{0};
std::atomic_int count_find{0};

class collection_t;
class collection_part_t;

class collection_part_t final : public actor_zeta::basic_actor<collection_part_t> {
public:
    collection_part_t(actor_zeta::pmr::memory_resource* ptr)
        : actor_zeta::basic_actor<collection_part_t>(ptr)
        , insert_(actor_zeta::make_behavior(resource(), this, &collection_part_t::insert))
        , remove_(actor_zeta::make_behavior(resource(), this, &collection_part_t::remove))
        , update_(actor_zeta::make_behavior(resource(), this, &collection_part_t::update))
        , find_(actor_zeta::make_behavior(resource(), this, &collection_part_t::find)) {
        ++count_collection_part;
    }

    // Методы для обработки сообщений
    void insert(std::string& key, std::string& value) {
        data_.emplace(key, value);
        std::cerr << id() << " " << key << " " << value << std::endl;
        ++count_insert;
    }

    void remove(std::string& key) {
        data_.erase(key);
        std::cerr << id() << " remove " << key << std::endl;
        ++count_remove;
    }

    void update(std::string& key, std::string& value) {
        data_[key] = value;
        std::cerr << id() << " update " << key << " = " << value << std::endl;
        ++count_update;
    }

    std::string find(std::string& key) {
        return data_[key];
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &collection_part_t::insert,
        &collection_part_t::remove,
        &collection_part_t::update,
        &collection_part_t::find
    >;

    void behavior(actor_zeta::message* msg) {
        switch (msg->command()) {
            case actor_zeta::msg_id<collection_part_t, &collection_part_t::insert>: {
                insert_(msg);
                break;
            }
            case actor_zeta::msg_id<collection_part_t, &collection_part_t::update>: {
                update_(msg);
                break;
            }
            case actor_zeta::msg_id<collection_part_t, &collection_part_t::remove>: {
                remove_(msg);
                break;
            }
            case actor_zeta::msg_id<collection_part_t, &collection_part_t::find>: {
                find_(msg);
                break;
            }
        }
    }

private:
    actor_zeta::behavior_t insert_;
    actor_zeta::behavior_t remove_;
    actor_zeta::behavior_t update_;
    actor_zeta::behavior_t find_;
    std::unordered_map<std::string, std::string> data_;
};



class collection_t final : public actor_zeta::actor_abstract_t {
public:
    collection_t(actor_zeta::pmr::memory_resource* resource, actor_zeta::scheduler::scheduler_abstract_t* scheduler)
        : actor_zeta::actor_abstract_t(resource)
        , e_(scheduler) {
        ++count_collection;
    }

    void create() {
        auto ptr = actor_zeta::spawn<collection_part_t> (resource());
        actors_.emplace_back(std::move(ptr));

    }

protected:
    bool enqueue_impl(actor_zeta::message_ptr msg) override {
        auto tmp = std::move(msg);
            switch (tmp->command()) {
                case actor_zeta::msg_id<collection_part_t, &collection_part_t::insert>:
                case actor_zeta::msg_id<collection_part_t, &collection_part_t::update>:
                case actor_zeta::msg_id<collection_part_t, &collection_part_t::remove>:
                case actor_zeta::msg_id<collection_part_t, &collection_part_t::find>: {
                    auto index = cursor_ % actors_.size();
                    actors_[index]->enqueue(std::move(tmp));
                    e_->enqueue(actors_[index].get());
                    ++cursor_;
                    ++count_balancer;
                    return true;
                }
                default: {
                    std::cerr << "unknown command" << std::endl;
                    return false;
                }
            }

    }

private:
    actor_zeta::scheduler::scheduler_abstract_t* e_;
    uint32_t cursor_ = 0;
    std::vector<collection_part_t::unique_actor> actors_;
};



static constexpr auto sleep_time = std::chrono::milliseconds(100);



int main() {
    auto* resource = actor_zeta::pmr::get_default_resource();
    auto scheduler = actor_zeta::scheduler::make_sharing_scheduler(resource,1, 100);
    auto collection = actor_zeta::spawn<collection_t>(resource, scheduler);

    std::cerr << "=== Creating 3 collection_part actors ===" << std::endl;
    collection->create();
    collection->create();
    collection->create();

    std::cerr << "\n=== Testing INSERT operations (round-robin balancing) ===" << std::endl;
    actor_zeta::send(collection.get(), actor_zeta::address_t::empty_address(), &collection_part_t::insert, std::string("key1"), std::string("value1"));
    actor_zeta::send(collection.get(), actor_zeta::address_t::empty_address(), &collection_part_t::insert, std::string("key2"), std::string("value2"));
    actor_zeta::send(collection.get(), actor_zeta::address_t::empty_address(), &collection_part_t::insert, std::string("key3"), std::string("value3"));

    std::cerr << "\n=== Testing UPDATE operations ===" << std::endl;
    actor_zeta::send(collection.get(), actor_zeta::address_t::empty_address(), &collection_part_t::update, std::string("key1"), std::string("updated1"));
    actor_zeta::send(collection.get(), actor_zeta::address_t::empty_address(), &collection_part_t::update, std::string("key2"), std::string("updated2"));

    std::cerr << "\n=== Testing REMOVE operations ===" << std::endl;
    actor_zeta::send(collection.get(), actor_zeta::address_t::empty_address(), &collection_part_t::remove, std::string("key3"));

    scheduler->start();

    std::this_thread::sleep_for(sleep_time);

    std::cerr << "\n=== Final Statistics ===" << std::endl;
    std::cerr << "Count Collection : " << count_collection << std::endl;
    std::cerr << "Count Collection Part : " << count_collection_part << std::endl;
    std::cerr << "Count Balancer : " << count_balancer << std::endl;
    std::cerr << "Count Insert : " << count_insert << std::endl;
    std::cerr << "Count Update : " << count_update << std::endl;
    std::cerr << "Count Remove : " << count_remove << std::endl;
    std::cerr << "\nExpected: 6 messages balanced across 3 actors (round-robin)" << std::endl;

    // Verify round-robin distribution
    bool success = (count_balancer == 6) && (count_insert == 3) && (count_update == 2) && (count_remove == 1);
    std::cerr << "\nTest result: " << (success ? "PASSED ✓" : "FAILED ✗") << std::endl;

    scheduler->stop();
    return success ? 0 : 1;
}
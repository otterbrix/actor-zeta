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
std::atomic_int count_find{0};

class collection_t;

enum class collection_method : uint64_t {
    insert = 0x00,
    remove,
    update,
    find
};

class collection_part_t final : public actor_zeta::basic_actor<collection_part_t> {
public:
    collection_part_t(actor_zeta::pmr::memory_resource* ptr)
        : actor_zeta::basic_actor<collection_part_t>(ptr)
        , insert_(actor_zeta::make_behavior(resource(), [this](std::string& key, std::string& value) -> void {
            data_.emplace(key, value);
            std::cerr << id() << " " << key << " " << value << std::endl;
            ++count_insert;
        }))
        , remove_(actor_zeta::make_behavior(resource(),  [this](std::string& key) -> void {
            data_.erase(key);
        }))
        , update_(actor_zeta::make_behavior(resource(),  [this](std::string& key, std::string& value) -> void {
            data_[key] = value;
        }))
        , find_(actor_zeta::make_behavior(resource(), [this](std::string& key) -> std::string {
            return data_[key];
        })) {
        ++count_collection_part;
    }

    void behavior(actor_zeta::message* msg) {
        switch (msg->command()) {
            case actor_zeta::make_message_id(collection_method::insert): {
                insert_(msg);
                break;
            }
            case actor_zeta::make_message_id(collection_method::remove): {
                remove_(msg);
                break;
            }
            case actor_zeta::make_message_id(collection_method::update): {
                update_(msg);
                break;
            }
            case actor_zeta::make_message_id(collection_method::find): {
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
    collection_t(actor_zeta::pmr::memory_resource* resource, actor_zeta::scheduler_t* scheduler)
        : actor_zeta::actor_abstract_t(resource)
        , e_(scheduler) {
        ++count_collection;
    }

    void create() {
        auto ptr = actor_zeta::spawn<collection_part_t> (resource());
        actors_.emplace_back(std::move(ptr));

    }

protected:
    bool enqueue_impl(actor_zeta::message_ptr msg) {
        auto tmp = std::move(msg);
            switch (tmp->command()) {
                case actor_zeta::make_message_id(collection_method::insert):
                case actor_zeta::make_message_id(collection_method::remove):
                case actor_zeta::make_message_id(collection_method::update):
                case actor_zeta::make_message_id(collection_method::find): {
                    auto index = cursor_ % actors_.size();
                    actors_[index]->enqueue(std::move(tmp));
                    e_->schedule(actors_[index].get());
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
    actor_zeta::scheduler_t* e_;
    uint32_t cursor_ = 0;
    std::vector<collection_part_t::unique_actor> actors_;
};



static constexpr auto sleep_time = std::chrono::milliseconds(60);



int main() {
    auto* resource = actor_zeta::pmr::get_default_resource();
    auto scheduler = actor_zeta::scheduler::make_sharing_scheduler(resource,1, 100);
    auto collection = actor_zeta::spawn<collection_t>(resource, scheduler.get());
    collection->create();
    collection->create();
    collection->create();
    actor_zeta::send(collection.get(), actor_zeta::address_t::empty_address(), collection_method::insert, std::string("1"), std::string("5"));
    actor_zeta::send(collection.get(), actor_zeta::address_t::empty_address(), collection_method::insert, std::string("1"), std::string("5"));
    actor_zeta::send(collection.get(), actor_zeta::address_t::empty_address(), collection_method::insert, std::string("1"), std::string("5"));

    scheduler->start();

    std::this_thread::sleep_for(sleep_time);

    std::cerr << "Count Collection : " << count_collection << std::endl;
    std::cerr << "Count Collection Part : " << count_collection_part << std::endl;
    std::cerr << "Count Balancer : " << count_balancer << std::endl;
    std::cerr << "Count Insert : " << count_balancer << std::endl;
    scheduler->stop();
    return 0;
}
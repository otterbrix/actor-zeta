#include <cassert>

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <thread>

#include <actor-zeta.hpp>

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
    collection_part_t(std::pmr::memory_resource* ptr)
        : actor_zeta::basic_actor<collection_part_t>(ptr) {
        ++count_collection_part;
    }
    actor_zeta::unique_future<void> insert(std::string key, std::string value) {
        data_.emplace(std::move(key), std::move(value));
        std::cerr << id() << " insert" << std::endl;
        ++count_insert;
        co_return;
    }

    actor_zeta::unique_future<void> remove(std::string key) {
        data_.erase(key);
        std::cerr << id() << " remove " << key << std::endl;
        ++count_remove;
        co_return;
    }

    actor_zeta::unique_future<void> update(std::string key, std::string value) {
        data_[std::move(key)] = std::move(value);
        std::cerr << id() << " update" << std::endl;
        ++count_update;
        co_return;
    }

    actor_zeta::unique_future<std::string> find(std::string key) {
        auto it = data_.find(key);
        ++count_find;
        if (it != data_.end()) {
            co_return it->second;
        } else {
            co_return std::string{};
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &collection_part_t::insert,
        &collection_part_t::remove,
        &collection_part_t::update,
        &collection_part_t::find
    >;

    actor_zeta::behavior_t behavior(actor_zeta::mailbox::message* msg) {
        switch (msg->command()) {
            case actor_zeta::msg_id<collection_part_t, &collection_part_t::insert>:
                co_await actor_zeta::dispatch(this, &collection_part_t::insert, msg);
                break;
            case actor_zeta::msg_id<collection_part_t, &collection_part_t::update>:
                co_await actor_zeta::dispatch(this, &collection_part_t::update, msg);
                break;
            case actor_zeta::msg_id<collection_part_t, &collection_part_t::remove>:
                co_await actor_zeta::dispatch(this, &collection_part_t::remove, msg);
                break;
            case actor_zeta::msg_id<collection_part_t, &collection_part_t::find>:
                co_await actor_zeta::dispatch(this, &collection_part_t::find, msg);
                break;
        }
    }

private:
    std::unordered_map<std::string, std::string> data_;
};



class collection_t final : public actor_zeta::actor::actor_mixin<collection_t> {
public:
    template<typename T> using unique_future = actor_zeta::unique_future<T>;

    collection_t(std::pmr::memory_resource* resource, actor_zeta::sharing_scheduler*)
        : actor_zeta::actor::actor_mixin<collection_t>()
        , resource_(resource) {
        ++count_collection;
    }

    std::pmr::memory_resource* resource() const noexcept { return resource_; }

    void create() {
        auto ptr = actor_zeta::spawn<collection_part_t>(resource_);
        actors_.emplace_back(std::move(ptr));
    }

    // Interface methods - signatures define the contract
    actor_zeta::unique_future<void> insert(std::string, std::string) { co_return; }
    actor_zeta::unique_future<void> remove(std::string) { co_return; }
    actor_zeta::unique_future<void> update(std::string, std::string) { co_return; }
    actor_zeta::unique_future<std::string> find(std::string) { co_return std::string{}; }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &collection_t::insert,
        &collection_t::remove,
        &collection_t::update,
        &collection_t::find
    >;

    // Balancer behavior: forwards messages to child actors
    actor_zeta::behavior_t behavior(actor_zeta::mailbox::message* msg) {
        if (actors_.empty()) {
            std::cerr << "Error: No child actors available" << std::endl;
            co_return;
        }

        auto index = cursor_ % actors_.size();
        ++cursor_;
        ++count_balancer;

        auto& child = actors_[index];
        auto cmd = msg->command();
        auto& args = msg->body();

        using insert_args = actor_zeta::type_traits::type_list<std::string, std::string>;
        using remove_args = actor_zeta::type_traits::type_list<std::string>;
        using update_args = actor_zeta::type_traits::type_list<std::string, std::string>;
        using find_args = actor_zeta::type_traits::type_list<std::string>;

        // Forward based on command
        switch (cmd) {
            case actor_zeta::msg_id<collection_t, &collection_t::insert>: {
                auto [needs_sched, future] = actor_zeta::send(child.get(),
                    &collection_part_t::insert,
                    actor_zeta::detail::get<0, insert_args>(args),
                    actor_zeta::detail::get<1, insert_args>(args));
                while (!future.available()) {
                    child->resume(1);
                }
                msg->get_result_promise<void>().set_value();
                break;
            }
            case actor_zeta::msg_id<collection_t, &collection_t::remove>: {
                auto [needs_sched, future] = actor_zeta::send(child.get(),
                    &collection_part_t::remove,
                    actor_zeta::detail::get<0, remove_args>(args));
                while (!future.available()) {
                    child->resume(1);
                }
                msg->get_result_promise<void>().set_value();
                break;
            }
            case actor_zeta::msg_id<collection_t, &collection_t::update>: {
                auto [needs_sched, future] = actor_zeta::send(child.get(),
                    &collection_part_t::update,
                    actor_zeta::detail::get<0, update_args>(args),
                    actor_zeta::detail::get<1, update_args>(args));
                while (!future.available()) {
                    child->resume(1);
                }
                msg->get_result_promise<void>().set_value();
                break;
            }
            case actor_zeta::msg_id<collection_t, &collection_t::find>: {
                auto [needs_sched, future] = actor_zeta::send(child.get(),
                    &collection_part_t::find,
                    actor_zeta::detail::get<0, find_args>(args));
                while (!future.available()) {
                    child->resume(1);
                }
                auto result = std::move(future).get();
                msg->get_result_promise<std::string>().set_value(std::move(result));
                break;
            }
        }
    }

private:
    std::pmr::memory_resource* resource_;
    uint32_t cursor_ = 0;
    std::vector<collection_part_t::unique_actor> actors_;
};



static constexpr auto sleep_time = std::chrono::milliseconds(100);



int main() {
    auto* resource =std::pmr::get_default_resource();
    std::unique_ptr<actor_zeta::scheduler::sharing_scheduler> scheduler(
        new actor_zeta::scheduler::sharing_scheduler(1, 100));
    auto collection = actor_zeta::spawn<collection_t>(resource, scheduler.get());

    std::cerr << "=== Creating 3 collection_part actors ===" << std::endl;
    collection->create();
    collection->create();
    collection->create();

    std::cerr << "\n=== Testing INSERT operations (round-robin balancing) ===" << std::endl;
    { auto [ns, f] = actor_zeta::send(collection.get(), &collection_t::insert, std::string("key1"), std::string("value1")); std::move(f).get(); }
    { auto [ns, f] = actor_zeta::send(collection.get(), &collection_t::insert, std::string("key2"), std::string("value2")); std::move(f).get(); }
    { auto [ns, f] = actor_zeta::send(collection.get(), &collection_t::insert, std::string("key3"), std::string("value3")); std::move(f).get(); }

    std::cerr << "\n=== Testing UPDATE operations ===" << std::endl;
    { auto [ns, f] = actor_zeta::send(collection.get(), &collection_t::update, std::string("key1"), std::string("updated1")); std::move(f).get(); }
    { auto [ns, f] = actor_zeta::send(collection.get(), &collection_t::update, std::string("key2"), std::string("updated2")); std::move(f).get(); }

    std::cerr << "\n=== Testing REMOVE operations ===" << std::endl;
    { auto [ns, f] = actor_zeta::send(collection.get(), &collection_t::remove, std::string("key3")); std::move(f).get(); }

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

    bool success = (count_balancer == 6) && (count_insert == 3) && (count_update == 2) && (count_remove == 1);
    std::cerr << "\nTest result: " << (success ? "PASSED" : "FAILED") << std::endl;

    scheduler->stop();
    return success ? 0 : 1;
}
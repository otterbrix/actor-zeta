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
#include <actor-zeta/dispatch.hpp>
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
        : actor_zeta::basic_actor<collection_part_t>(ptr) {
        ++count_collection_part;
    }

    // Методы для обработки сообщений
    // NOTE: By-value parameters required for coroutines (const& becomes dangling after co_await)
    actor_zeta::unique_future<void> insert(std::string key, std::string value) {
        data_.emplace(std::move(key), std::move(value));
        std::cerr << id() << " insert" << std::endl;
        ++count_insert;
        return actor_zeta::make_ready_future_void(resource());
    }

    actor_zeta::unique_future<void> remove(std::string key) {
        data_.erase(key);
        std::cerr << id() << " remove " << key << std::endl;
        ++count_remove;
        return actor_zeta::make_ready_future_void(resource());
    }

    actor_zeta::unique_future<void> update(std::string key, std::string value) {
        data_[std::move(key)] = std::move(value);
        std::cerr << id() << " update" << std::endl;
        ++count_update;
        return actor_zeta::make_ready_future_void(resource());
    }

    actor_zeta::unique_future<std::string> find(std::string key) {
        auto it = data_.find(key);
        ++count_find;
        if (it != data_.end()) {
            return actor_zeta::make_ready_future<std::string>(resource(), it->second);
        } else {
            return actor_zeta::make_ready_future<std::string>(resource(), std::string{});
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &collection_part_t::insert,
        &collection_part_t::remove,
        &collection_part_t::update,
        &collection_part_t::find
    >;

    actor_zeta::unique_future<void> behavior(actor_zeta::mailbox::message* msg) {
        switch (msg->command()) {
            case actor_zeta::msg_id<collection_part_t, &collection_part_t::insert>: {
                return actor_zeta::dispatch(this, &collection_part_t::insert, msg);
            }
            case actor_zeta::msg_id<collection_part_t, &collection_part_t::update>: {
                return actor_zeta::dispatch(this, &collection_part_t::update, msg);
            }
            case actor_zeta::msg_id<collection_part_t, &collection_part_t::remove>: {
                return actor_zeta::dispatch(this, &collection_part_t::remove, msg);
            }
            case actor_zeta::msg_id<collection_part_t, &collection_part_t::find>: {
                return actor_zeta::dispatch(this, &collection_part_t::find, msg);
            }
        }
        return actor_zeta::make_ready_future_void(resource());
    }

private:
    std::unordered_map<std::string, std::string> data_;
};



class collection_t final : public actor_zeta::base::actor_mixin<collection_t> {
public:
    template<typename T> using unique_future = actor_zeta::unique_future<T>;

    collection_t(actor_zeta::pmr::memory_resource* resource, actor_zeta::scheduler::sharing_scheduler*)
        : actor_zeta::base::actor_mixin<collection_t>()
        , resource_(resource) {
        ++count_collection;
    }

    actor_zeta::pmr::memory_resource* resource() const noexcept { return resource_; }

    void create() {
        auto ptr = actor_zeta::spawn<collection_part_t> (resource_);
        actors_.emplace_back(std::move(ptr));
    }

    // Dummy methods - just for dispatch_traits, never actually called
    // NOTE: By-value parameters required for coroutines (const& becomes dangling after co_await)
    actor_zeta::unique_future<void> insert(std::string, std::string) { return actor_zeta::make_ready_future_void(resource_); }
    actor_zeta::unique_future<void> remove(std::string) { return actor_zeta::make_ready_future_void(resource_); }
    actor_zeta::unique_future<void> update(std::string, std::string) { return actor_zeta::make_ready_future_void(resource_); }
    actor_zeta::unique_future<std::string> find(std::string) { return actor_zeta::make_ready_future<std::string>(resource_, std::string("")); }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &collection_t::insert,
        &collection_t::remove,
        &collection_t::update,
        &collection_t::find
    >;

    // NEW API: Forward arguments to child actor (message created in child's resource)
    template<typename R, typename... Args>
    unique_future<R> enqueue_impl(
        actor_zeta::base::address_t sender,
        actor_zeta::mailbox::message_id cmd,
        Args&&... args
    ) {
        // Check if we have any child actors
        if (actors_.empty()) {
            std::cerr << "Error: No child actors available" << std::endl;
            return actor_zeta::make_error_future<R>(resource_);
        }

        // Balancer logic: forward to child actor using round-robin
        auto index = cursor_ % actors_.size();
        ++cursor_;
        ++count_balancer;

        // Forward arguments to child actor (child creates message in its own resource)
        auto child_future = actors_[index]->template enqueue_impl<R>(
            sender,
            cmd,
            std::forward<Args>(args)...
        );

        // Execute child immediately if needed (inline execution to avoid deadlock)
        if (child_future.needs_scheduling()) {
            auto& child = actors_[index];
            while (!child_future.available()) {
                child->resume(1);  // Process one message
            }
        }

        // Create future_state<R> for balancer's return value
        void* mem = resource_->allocate(sizeof(actor_zeta::detail::future_state<R>),
                                         alignof(actor_zeta::detail::future_state<R>));
        auto* state = new (mem) actor_zeta::detail::future_state<R>(resource_);

        // Extract result from child and set immediately
        if constexpr (std::is_same_v<R, void>) {
            std::move(child_future).get();  // Wait for completion
            state->set_ready();  // Mark as ready for void
        } else {
            R result = std::move(child_future).get();  // Get result from child
            state->set_value(std::move(result));  // Set value directly (ZERO ALLOCATION!)
        }

        // Return async future (already in ready state)
        return unique_future<R>(state, false);  // needs_scheduling=false (sync execution)
    }

protected:

private:
    actor_zeta::pmr::memory_resource* resource_;
    uint32_t cursor_ = 0;
    std::vector<collection_part_t::unique_actor> actors_;
};



static constexpr auto sleep_time = std::chrono::milliseconds(100);



int main() {
    auto* resource = actor_zeta::pmr::get_default_resource();
    std::unique_ptr<actor_zeta::scheduler::sharing_scheduler> scheduler(
        new actor_zeta::scheduler::sharing_scheduler(1, 100));
    auto collection = actor_zeta::spawn<collection_t>(resource, scheduler.get());

    std::cerr << "=== Creating 3 collection_part actors ===" << std::endl;
    collection->create();
    collection->create();
    collection->create();

    std::cerr << "\n=== Testing INSERT operations (round-robin balancing) ===" << std::endl;
    actor_zeta::send(collection.get(), actor_zeta::address_t::empty_address(), &collection_t::insert, std::string("key1"), std::string("value1")).get();
    actor_zeta::send(collection.get(), actor_zeta::address_t::empty_address(), &collection_t::insert, std::string("key2"), std::string("value2")).get();
    actor_zeta::send(collection.get(), actor_zeta::address_t::empty_address(), &collection_t::insert, std::string("key3"), std::string("value3")).get();

    std::cerr << "\n=== Testing UPDATE operations ===" << std::endl;
    actor_zeta::send(collection.get(), actor_zeta::address_t::empty_address(), &collection_t::update, std::string("key1"), std::string("updated1")).get();
    actor_zeta::send(collection.get(), actor_zeta::address_t::empty_address(), &collection_t::update, std::string("key2"), std::string("updated2")).get();

    std::cerr << "\n=== Testing REMOVE operations ===" << std::endl;
    actor_zeta::send(collection.get(), actor_zeta::address_t::empty_address(), &collection_t::remove, std::string("key3")).get();

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
    std::cerr << "\nTest result: " << (success ? "PASSED" : "FAILED") << std::endl;

    scheduler->stop();
    return success ? 0 : 1;
}
#pragma once

#include "test/tooltestsuites/scheduler_test.hpp"
#include <actor-zeta.hpp>
#include <iostream>
#include <list>

#define TRACE(msg) \
    { std::cout << __FILE__ << ":" << __LINE__ << "::" << __func__ << " : " << msg << std::endl; }

class storage_t;
class test_handlers;

class dummy_supervisor final : public actor_zeta::actor_abstract_t {
public:
    static uint64_t constructor_counter;
    static uint64_t destructor_counter;
    static uint64_t executor_impl_counter;
    static uint64_t add_actor_impl_counter;
    static uint64_t add_supervisor_impl_counter;
    static uint64_t enqueue_base_counter;

    dummy_supervisor(actor_zeta::pmr::memory_resource* resource, uint64_t threads, uint64_t throughput)
        : actor_abstract_t(resource)
        , create_storage_(actor_zeta::make_behavior(resource, this, &dummy_supervisor::create_storage))
        , create_test_handlers_(actor_zeta::make_behavior(resource, this, &dummy_supervisor::create_test_handlers))
        , executor_(new actor_zeta::test::scheduler_test_t(threads, throughput)) {
        scheduler_test()->start();
        constructor_counter++;
    }

    dummy_supervisor(const dummy_supervisor&) = delete;
    dummy_supervisor& operator=(const dummy_supervisor&) = delete;

    ~dummy_supervisor() {
        destructor_counter++;
    }

    void behavior(actor_zeta::mailbox::message* msg) {
        switch (msg->command()) {
            case actor_zeta::msg_id<dummy_supervisor, &dummy_supervisor::create_storage>: {
                create_storage_(msg);
                break;
            }
            case actor_zeta::msg_id<dummy_supervisor, &dummy_supervisor::create_test_handlers>: {
                create_test_handlers_(msg);
                break;
            }
        }
    }

    auto scheduler_test() noexcept -> actor_zeta::test::scheduler_test_t* {
        TRACE("+++");
        executor_impl_counter++;
        return executor_.get();
    }


    void create_storage();
    void create_test_handlers();

    auto actors_count() const -> size_t {
        return storages_.size()+test_handlers_.size();
    }

    bool enqueue_impl(actor_zeta::message_ptr msg) override  {
        enqueue_base_counter++;
        auto tmp_msg =  (std::move(msg));
        behavior(tmp_msg.get());
        return true;
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &dummy_supervisor::create_storage,
        &dummy_supervisor::create_test_handlers
    >;

private:
    actor_zeta::behavior_t create_storage_;
    actor_zeta::behavior_t create_test_handlers_;
    std::unique_ptr<actor_zeta::test::scheduler_test_t> executor_;
    std::list<std::unique_ptr<storage_t,actor_zeta::pmr::deleter_t>> storages_;
    std::list<std::unique_ptr<test_handlers,actor_zeta::pmr::deleter_t>> test_handlers_;
};

uint64_t dummy_supervisor::constructor_counter = 0;
uint64_t dummy_supervisor::destructor_counter = 0;

uint64_t dummy_supervisor::executor_impl_counter = 0;

uint64_t dummy_supervisor::add_actor_impl_counter = 0;
uint64_t dummy_supervisor::add_supervisor_impl_counter = 0;

uint64_t dummy_supervisor::enqueue_base_counter = 0;

class storage_t final : public actor_zeta::basic_actor<storage_t> {
public:
    static uint64_t constructor_counter;
    static uint64_t destructor_counter;

    static uint64_t init_counter;
    static uint64_t search_counter;
    static uint64_t add_counter;
    static uint64_t delete_table_counter;
    static uint64_t create_table_counter;

public:
    explicit storage_t(actor_zeta::pmr::memory_resource* resource_)
        : actor_zeta::basic_actor<storage_t>(resource_)
        , init_(actor_zeta::make_behavior(
              resource(),
              this,
              &storage_t::init))
        , search_(actor_zeta::make_behavior(
              resource(),
              this,
              &storage_t::search))
        , add_(actor_zeta::make_behavior(
              resource(),
              this,
              &storage_t::add))
        , delete_table_(actor_zeta::make_behavior(
              resource(),
              this,
              &storage_t::delete_table))
        , create_table_(actor_zeta::make_behavior(
              resource(),
              this,
              &storage_t::create_table)) {
        constructor_counter++;
    }

    void behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<storage_t, &storage_t::init>) {
            init_(msg);
        } else if (cmd == actor_zeta::msg_id<storage_t, &storage_t::search>) {
            search_(msg);
        } else if (cmd == actor_zeta::msg_id<storage_t, &storage_t::add>) {
            add_(msg);
        } else if (cmd == actor_zeta::msg_id<storage_t, &storage_t::delete_table>) {
            delete_table_(msg);
        } else if (cmd == actor_zeta::msg_id<storage_t, &storage_t::create_table>) {
            create_table_(msg);
        }
    }

    ~storage_t() override {
        destructor_counter++;
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &storage_t::init,
        &storage_t::search,
        &storage_t::add,
        &storage_t::delete_table,
        &storage_t::create_table
    >;

private:
    void init() {
        init_counter++;
        TRACE("+++");
    }

    void search(std::string& key) {
        search_counter++;
        std::cerr << __func__ << " :: "
                  << "key: " << key
                  << std::endl;
    }

    void add(const std::string& key, const std::string& value) {
        add_counter++;
        std::cerr << __func__ << " :: "
                  << "key: " << key << " | "
                  << "value: " << value << " | "
                  << std::endl;
    }

    void delete_table(const std::string& name, const std::string& path, int type) {
        delete_table_counter++;
        std::cerr << __func__ << " :: "
                  << "table name: " << name << " | "
                  << "path: " << path << " | "
                  << "type: " << type << " | "
                  << std::endl;
    }

    void create_table(const std::string& name, const std::string& path, int type, int time_sync) {
        create_table_counter++;
        std::cerr << __func__ << " :: "
                  << "table name: " << name << " | "
                  << "path: " << path << " | "
                  << "type: " << type << " | "
                  << "time_sync: " << time_sync << " | "
                  << std::endl;
    }

private:
    actor_zeta::behavior_t init_;
    actor_zeta::behavior_t search_;
    actor_zeta::behavior_t add_;
    actor_zeta::behavior_t delete_table_;
    actor_zeta::behavior_t create_table_;
};

uint64_t storage_t::constructor_counter = 0;
uint64_t storage_t::destructor_counter = 0;

uint64_t storage_t::init_counter = 0;
uint64_t storage_t::search_counter = 0;
uint64_t storage_t::add_counter = 0;
uint64_t storage_t::delete_table_counter = 0;
uint64_t storage_t::create_table_counter = 0;

class test_handlers final : public actor_zeta::basic_actor<test_handlers> {
public:
    static uint64_t init_counter;

    static uint64_t ptr_0_counter;
    static uint64_t ptr_1_counter;
    static uint64_t ptr_2_counter;
    static uint64_t ptr_3_counter;
    static uint64_t ptr_4_counter;

public:
    test_handlers(actor_zeta::pmr::memory_resource* ptr)
        : actor_zeta::basic_actor<test_handlers>(ptr)
        , ptr_0_(actor_zeta::make_behavior(resource(), this, &test_handlers::ptr_0_handler))
        , ptr_1_(actor_zeta::make_behavior(resource(), this, &test_handlers::ptr_1_handler))
        , ptr_2_(actor_zeta::make_behavior(resource(), this, &test_handlers::ptr_2_handler))
        , ptr_3_(actor_zeta::make_behavior(resource(), this, &test_handlers::ptr_3_handler))
        , ptr_4_(actor_zeta::make_behavior(resource(), this, &test_handlers::ptr_4_handler)) {
        init();
    }


    void behavior(actor_zeta::mailbox::message* msg) {
        switch (msg->command()) {
            case actor_zeta::msg_id<test_handlers, &test_handlers::ptr_0_handler>: {
                ptr_0_(msg);
                break;
            }
            case actor_zeta::msg_id<test_handlers, &test_handlers::ptr_1_handler>: {
                ptr_1_(msg);
                break;
            }
            case actor_zeta::msg_id<test_handlers, &test_handlers::ptr_2_handler>: {
                ptr_2_(msg);
                break;
            }
            case actor_zeta::msg_id<test_handlers, &test_handlers::ptr_3_handler>: {
                ptr_3_(msg);
                break;
            }
            case actor_zeta::msg_id<test_handlers, &test_handlers::ptr_4_handler>: {
                ptr_4_(msg);
                break;
            }
            default: {
                TRACE("+++");
                break;
            }
        }
    }



    // Forward declarations for dispatch_traits
    void ptr_0_handler();
    void ptr_1_handler();
    void ptr_2_handler(int&);
    void ptr_3_handler(int data_1, int& data_2);
    void ptr_4_handler(int data_1, int& data_2, const std::string& data_3);

    using dispatch_traits = actor_zeta::dispatch_traits<
        &test_handlers::ptr_0_handler,
        &test_handlers::ptr_1_handler,
        &test_handlers::ptr_2_handler,
        &test_handlers::ptr_3_handler,
        &test_handlers::ptr_4_handler
    >;

private:
    void init() {
        TRACE("private init");
        init_counter++;
    }

    actor_zeta::behavior_t ptr_0_;
    actor_zeta::behavior_t ptr_1_;
    actor_zeta::behavior_t ptr_2_;
    actor_zeta::behavior_t ptr_3_;
    actor_zeta::behavior_t ptr_4_;
};

uint64_t test_handlers::init_counter = 0;

uint64_t test_handlers::ptr_0_counter = 0;
uint64_t test_handlers::ptr_1_counter = 0;
uint64_t test_handlers::ptr_2_counter = 0;
uint64_t test_handlers::ptr_3_counter = 0;
uint64_t test_handlers::ptr_4_counter = 0;

void test_handlers::ptr_0_handler() {
    TRACE("+++");
    ptr_0_counter++;
}

void test_handlers::ptr_1_handler() {
    TRACE("+++");
    ptr_1_counter++;
}

void test_handlers::ptr_2_handler(int&) {
    TRACE("+++");
    ptr_2_counter++;
}

void test_handlers::ptr_3_handler(int data_1, int& data_2) {
    TRACE("+++");
    std::cerr << "ptr_3 : " << data_1 << " : " << data_2 << std::endl;
    ptr_3_counter++;
}

void test_handlers::ptr_4_handler(int data_1, int& data_2, const std::string& data_3) {
    TRACE("+++");
    std::cerr << "ptr_4 : " << data_1 << " : " << data_2 << " : " << data_3 << std::endl;
    ptr_4_counter++;
}

void dummy_supervisor::create_storage() {
    TRACE("+++");
    auto uptr = actor_zeta::spawn<storage_t>(resource());
    storages_.emplace_back(std::move(uptr));
    add_actor_impl_counter++;
}

void dummy_supervisor::create_test_handlers() {
    TRACE("+++");
    auto uptr = actor_zeta::spawn<test_handlers>(resource());
    test_handlers_.emplace_back(std::move(uptr));
    add_actor_impl_counter++;

}
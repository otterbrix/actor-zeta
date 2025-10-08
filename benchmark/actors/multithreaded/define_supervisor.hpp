#pragma once

#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <actor-zeta.hpp>

auto thread_pool_deleter = [](actor_zeta::scheduler_abstract_t* ptr) {
    ptr->stop();
    delete ptr;
};

#define DEFINE_SUPERVISOR(supervisor_name, actor_name, num_worker_threads)                                                                              \
    class supervisor_name final : public actor_zeta::cooperative_supervisor<supervisor_name> {                                                          \
    public:                                                                                                                                             \
        supervisor_name(memory_resource* ptr)                                                                                                           \
            : actor_zeta::cooperative_supervisor<supervisor_name>(ptr, names::supervisor)                                                               \
            , sync_()                                                                                                                                   \
            , e_(new actor_zeta::scheduler_t<actor_zeta::work_sharing>(                                                                                 \
                     num_worker_threads,                                                                                                                \
                     100),                                                                                                                              \
                 thread_pool_deleter) {                                                                                                                 \
            e_->start();                                                                                                                                \
            add_handler(command_t::prepare, &supervisor_name::prepare);                                                                                 \
            add_handler(command_t::send, &supervisor_name::send);                                                                                       \
            add_handler(command_t::done, &supervisor_name::done);                                                                                       \
        }                                                                                                                                               \
                                                                                                                                                        \
        void prepare() {                                                                                                                                \
            auto address_0 = spawn_actor([this](actor_name* ptr) {                                                                                      \
                actors_.emplace(name_t::actor_0, ptr);                                                                                                  \
            });                                                                                                                                         \
            auto address_1 = spawn_actor([this](actor_name* ptr) {                                                                                      \
                actors_.emplace(name_t::actor_1, ptr);                                                                                                  \
            });                                                                                                                                         \
            address_book_.emplace(name_t::actor_0, std::move(address_0));                                                                               \
            address_book_.emplace(name_t::actor_1, std::move(address_1));                                                                               \
                                                                                                                                                        \
            actor_zeta::send(address_book_.at(name_t::actor_0), address(), command_t::add_address, address_book_.at(name_t::actor_1), name_t::actor_1); \
            actor_zeta::send(address_book_.at(name_t::actor_1), address(), command_t::add_address, address_book_.at(name_t::actor_0), name_t::actor_0); \
        }                                                                                                                                               \
                                                                                                                                                        \
        void send() {                                                                                                                                   \
            assert(actors_.size() == 2);                                                                                                                \
            actors_.at(name_t::actor_0)->start();                                                                                                       \
            sync_.sem_wait_();                                                                                                                          \
            std::cout << std::this_thread::get_id() << " :: sem_wait_ RETURNED " << reinterpret_cast<void*>(this) << std::endl;                         \
        }                                                                                                                                               \
        void done() {                                                                                                                                   \
            sync_.sem_post_();                                                                                                                          \
            std::cout << std::this_thread::get_id() << " :: sem_post_ RETURNED " << reinterpret_cast<void*>(this) << std::endl;                         \
        }                                                                                                                                               \
                                                                                                                                                        \
    protected:                                                                                                                                          \
        auto scheduler_impl() noexcept -> actor_zeta::scheduler_abstract_t* override {                                                                  \
            return e_.get();                                                                                                                            \
        }                                                                                                                                               \
                                                                                                                                                        \
        auto enqueue_impl(actor_zeta::message_ptr msg, actor_zeta::execution_unit*) -> void final {                                                     \
            {                                                                                                                                           \
                auto ptr = msg.get();                                                                                                                   \
                set_current_message(std::move(msg));                                                                                                    \
                execute(this, current_message());                                                                                                       \
                delete ptr;                                                                                                                             \
            }                                                                                                                                           \
        }                                                                                                                                               \
                                                                                                                                                        \
    private:                                                                                                                                            \
        base_single_t sync_;                                                                                                                            \
        std::unique_ptr<actor_zeta::scheduler_abstract_t, decltype(thread_pool_deleter)> e_;                                                            \
        std::map<name_t, actor_zeta::address_t> address_book_;                                                                                          \
        std::map<name_t, actor_name*> actors_;                                                                                                          \
    };                                                                                                                                                  \
    auto set_done(supervisor_name* sup_ptr_)->void {                                                                                                    \
        std::cout << std::this_thread::get_id() << " :: Going to sem done ... .. . " << reinterpret_cast<void*>(sup_ptr_) << std::endl;                 \
        sup_ptr_->done();                                                                                                                               \
    }

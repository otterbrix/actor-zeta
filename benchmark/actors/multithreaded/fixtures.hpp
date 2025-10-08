#pragma once

#include <condition_variable>
#include <iostream>
#include <memory>
#include <semaphore.h>
#include <string>
#include <unordered_map>
#include <vector>

#include <benchmark/benchmark.h>

#include <actor-zeta.hpp>
#include <actor-zeta/detail/memory_resource.hpp>

#include "define_actor.hpp"
#include "define_supervisor.hpp"
#include "counters.h"

using actor_zeta::detail::pmr::memory_resource;

enum class command_t : uint64_t {
    add_link,
    add_address,
    start,
    ping,
    pong,
    prepare,
    send,
    done,
};

enum class name_t : uint64_t {
    actor_0,
    actor_1,
    sup,
};

namespace names {

    static const std::string actor("storage");
    static const std::string supervisor("supervisor");

} // namespace names

struct sync_t {
    sem_t prodToRoutine;
};

struct base_single_t {
    sync_t sync_;

    base_single_t() {
        sem_init_();
    }
    virtual ~base_single_t() {
        sem_destroy_();
    }

    auto sem_init_() -> int {
        return sem_init(&sync_.prodToRoutine, 0, 0);
    }

    auto sem_post_() -> int {
        return sem_post(&sync_.prodToRoutine);
    }

    auto sem_wait_() -> int {
        return sem_wait(&sync_.prodToRoutine);
    }

    auto sem_destroy_() -> int {
        return sem_destroy(&sync_.prodToRoutine);
    }
};

#define DEFINE_SET(actor, supervisor, num_worker_threads, fixture, ...)                         \
    class supervisor;                                                                           \
    DEFINE_ACTOR(actor, supervisor, __VA_ARGS__);                                               \
    DEFINE_SUPERVISOR(supervisor, actor, num_worker_threads);                                   \
    class fixture : public ::benchmark::Fixture {                                               \
    public:                                                                                     \
        fixture()                                                                               \
            : sup_(nullptr, actor_zeta::deleter(nullptr)) {}                                    \
        virtual void SetUp(__attribute__((unused)) const ::benchmark::State& state) final {     \
            auto* resource = actor_zeta::detail::pmr::get_default_resource();                   \
            sup_ = actor_zeta::spawn_supervisor<supervisor>(resource);                          \
            actor_zeta::send(sup_, actor_zeta::address_t::empty_address(), command_t::prepare); \
            std::this_thread::sleep_for(std::chrono::milliseconds(15));                         \
            counter_g = 0;                                                                      \
        }                                                                                       \
        virtual void SetUp(::benchmark::State& state) final {                                   \
            SetUp(static_cast<const ::benchmark::State&>(state));                               \
        }                                                                                       \
        virtual void TearDown(__attribute__((unused)) const ::benchmark::State& state) final {  \
            sup_.reset();                                                                       \
        }                                                                                       \
        virtual void TearDown(::benchmark::State& state) final {                                \
            TearDown(static_cast<const ::benchmark::State&>(state));                            \
        }                                                                                       \
                                                                                                \
        std::unique_ptr<supervisor, actor_zeta::deleter> sup_;                                  \
    }

#define REGISTER_BENCHMARK(fixture, bm_name)                                   \
    BENCHMARK_DEFINE_F(fixture, bm_name)                                       \
    (benchmark::State & state) {                                               \
        while (state.KeepRunning()) {                                          \
            std::cout << "+++ STARTED +++" << std::endl;                       \
            counter_g = 0;                                                     \
            sup_->send();                                                      \
            /*std::this_thread::sleep_for(std::chrono::milliseconds(5));*/     \
            std::cout << "--- STOPPED counter_g = " << counter_g << std::endl; \
            assert(counter_g == 2);                                            \
        }                                                                      \
    }                                                                          \
    BENCHMARK_REGISTER_F(fixture, bm_name)->DenseRange(0, 32, 8)

#define REGISTER_BENCHMARKS(type, num_worker_threads)                                                                                                     \
    DEFINE_SET(actor_0_##type, supervisor_0_##type, num_worker_threads, fixture_0_##type);                                                                \
    DEFINE_SET(actor_1_##type, supervisor_1_##type, num_worker_threads, fixture_1_##type, type);                                                          \
    DEFINE_SET(actor_2_##type, supervisor_2_##type, num_worker_threads, fixture_2_##type, type, type);                                                    \
    DEFINE_SET(actor_3_##type, supervisor_3_##type, num_worker_threads, fixture_3_##type, type, type, type);                                              \
    DEFINE_SET(actor_4_##type, supervisor_4_##type, num_worker_threads, fixture_4_##type, type, type, type, type);                                        \
    DEFINE_SET(actor_5_##type, supervisor_5_##type, num_worker_threads, fixture_5_##type, type, type, type, type, type);                                  \
    DEFINE_SET(actor_6_##type, supervisor_6_##type, num_worker_threads, fixture_6_##type, type, type, type, type, type, type);                            \
    DEFINE_SET(actor_7_##type, supervisor_7_##type, num_worker_threads, fixture_7_##type, type, type, type, type, type, type, type);                      \
    DEFINE_SET(actor_8_##type, supervisor_8_##type, num_worker_threads, fixture_8_##type, type, type, type, type, type, type, type, type);                \
    DEFINE_SET(actor_9_##type, supervisor_9_##type, num_worker_threads, fixture_9_##type, type, type, type, type, type, type, type, type, type);          \
    DEFINE_SET(actor_10_##type, supervisor_10_##type, num_worker_threads, fixture_10_##type, type, type, type, type, type, type, type, type, type, type); \
    REGISTER_BENCHMARK(fixture_0_##type, ping_pong_single_threaded_0_##type);                                                                             \
    REGISTER_BENCHMARK(fixture_1_##type, ping_pong_single_threaded_1_##type);                                                                             \
    REGISTER_BENCHMARK(fixture_2_##type, ping_pong_single_threaded_2_##type);                                                                             \
    REGISTER_BENCHMARK(fixture_3_##type, ping_pong_single_threaded_3_##type);                                                                             \
    REGISTER_BENCHMARK(fixture_4_##type, ping_pong_single_threaded_4_##type);                                                                             \
    REGISTER_BENCHMARK(fixture_5_##type, ping_pong_single_threaded_5_##type);                                                                             \
    REGISTER_BENCHMARK(fixture_6_##type, ping_pong_single_threaded_6_##type);                                                                             \
    REGISTER_BENCHMARK(fixture_7_##type, ping_pong_single_threaded_7_##type);                                                                             \
    REGISTER_BENCHMARK(fixture_8_##type, ping_pong_single_threaded_8_##type);                                                                             \
    REGISTER_BENCHMARK(fixture_9_##type, ping_pong_single_threaded_9_##type);                                                                             \
    REGISTER_BENCHMARK(fixture_10_##type, ping_pong_single_threaded_10_##type)

#pragma once

#include <benchmark/benchmark.h>
#include <actor-zeta.hpp>
#include <actor-zeta/scheduler/scheduler.hpp>
#include <actor-zeta/scheduler/policy/work_sharing.hpp>
#include <actor-zeta/config.hpp>
#include <thread>
#include <chrono>

#include "define_actor.hpp"
#include "define_supervisor.hpp"

constexpr size_t NUM_WORKERS = 4;

#define REGISTER_BENCHMARK(type, count, ...)                                                 \
    class Fixture_##type##_##count : public benchmark::Fixture {                            \
        using Actor = coro_ping_pong_actor<__VA_ARGS__>;                                    \
        using Supervisor = coro_supervisor<Actor>;                                          \
                                                                                             \
        std::unique_ptr<actor_zeta::scheduler::sharing_scheduler> scheduler_;               \
        std::unique_ptr<Supervisor, actor_zeta::pmr::deleter_t> supervisor_;                \
        std::pmr::memory_resource* resource_;                                        \
                                                                                             \
    public:                                                                                  \
        Fixture_##type##_##count()                                                          \
            : supervisor_(nullptr, actor_zeta::pmr::deleter_t(std::pmr::get_default_resource())) { \
        }                                                                                    \
                                                                                             \
        void SetUp(const benchmark::State&) override {                                      \
            resource_ =std::pmr::get_default_resource();                            \
            scheduler_.reset(new actor_zeta::scheduler::scheduler_t<actor_zeta::scheduler::work_sharing>(NUM_WORKERS, 1000)); \
            scheduler_->start();                                                             \
            supervisor_ = actor_zeta::spawn<Supervisor>(resource_, scheduler_.get());       \
            actor_zeta::detail::ignore_unused(actor_zeta::send(supervisor_.get(), actor_zeta::address_t::empty_address(), &Supervisor::prepare)); \
        }                                                                                    \
                                                                                             \
        void TearDown(const benchmark::State&) override {                                   \
            scheduler_->stop();                                                              \
            supervisor_.reset();                                                             \
            scheduler_.reset();                                                              \
        }                                                                                    \
                                                                                             \
        void DoPingPong() {                                                                 \
            actor_zeta::detail::ignore_unused(actor_zeta::send(supervisor_.get(), actor_zeta::address_t::empty_address(), &Supervisor::send)); \
            std::this_thread::sleep_for(std::chrono::microseconds(100));                    \
        }                                                                                    \
    };                                                                                       \
                                                                                             \
    BENCHMARK_DEFINE_F(Fixture_##type##_##count, PingPong)(benchmark::State & st) {         \
        for (auto _ : st) {                                                                  \
            DoPingPong();                                                                    \
        }                                                                                    \
    }                                                                                        \
    BENCHMARK_REGISTER_F(Fixture_##type##_##count, PingPong);

#define REGISTER_BENCHMARKS(type)                                        \
    REGISTER_BENCHMARK(type, 0);                                         \
    REGISTER_BENCHMARK(type, 1, type);                                   \
    REGISTER_BENCHMARK(type, 2, type, type);                             \
    REGISTER_BENCHMARK(type, 3, type, type, type);                       \
    REGISTER_BENCHMARK(type, 4, type, type, type, type);                 \
    REGISTER_BENCHMARK(type, 5, type, type, type, type, type);           \
    REGISTER_BENCHMARK(type, 6, type, type, type, type, type, type);     \
    REGISTER_BENCHMARK(type, 7, type, type, type, type, type, type, type)
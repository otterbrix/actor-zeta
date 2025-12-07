#pragma once

#include <benchmark/benchmark.h>
#include <actor-zeta.hpp>

#include "define_actor.hpp"
#include "define_supervisor.hpp"

// Macro to register benchmarks for different argument counts
#define REGISTER_BENCHMARK(type, count, ...)                                                 \
    class Fixture_##type##_##count : public benchmark::Fixture {                            \
        using Actor = ping_pong_actor<__VA_ARGS__>;                                         \
        using Supervisor = simple_supervisor<Actor>;                                        \
                                                                                             \
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
            supervisor_ = actor_zeta::spawn<Supervisor>(resource_);                         \
            actor_zeta::send(supervisor_.get(), actor_zeta::address_t::empty_address(), &Supervisor::prepare); \
        }                                                                                    \
                                                                                             \
        void TearDown(const benchmark::State&) override {                                   \
            supervisor_.reset();                                                            \
        }                                                                                    \
                                                                                             \
        void DoPingPong() {                                                                 \
            actor_zeta::send(supervisor_.get(), actor_zeta::address_t::empty_address(), &Supervisor::send); \
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
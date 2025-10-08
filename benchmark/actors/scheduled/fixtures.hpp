#pragma once

#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <benchmark/benchmark.h>

#include <actor-zeta.hpp>

#include "define_actor.hpp"
#include "define_supervisor.hpp"
#include "counters.h"

template<class... Params_local>
class fixture_t : public ::benchmark::Fixture {
protected:
    std::unique_ptr<supervisor_t<Params_local...>, actor_zeta::pmr::deleter_t> sup_;

public:
    fixture_t()
        : sup_(nullptr, actor_zeta::pmr::deleter_t(nullptr)) {}

    virtual void SetUp(__attribute__((unused)) const ::benchmark::State& state) final {     
        auto* resource = actor_zeta::pmr::get_default_resource();
        sup_ = actor_zeta::spawn_supervisor<supervisor_t<Params_local...>>(resource);
        sup_->prepare();
        counter_g = 0;
    }
    virtual void SetUp(::benchmark::State& state) final {
        SetUp(static_cast<const ::benchmark::State&>(state));
    }
    virtual void TearDown(__attribute__((unused)) const ::benchmark::State& state) final {
        sup_.reset();
    }
    virtual void TearDown(::benchmark::State& state) final {
        TearDown(static_cast<const ::benchmark::State&>(state));
    }
};

#define REGISTER_TEMPLATE_BENCHMARK(fixture, bm_name, ...)      \
    BENCHMARK_TEMPLATE_DEFINE_F(fixture, bm_name, __VA_ARGS__)  \
    (benchmark::State& st) {                                    \
        while (st.KeepRunning()) {                              \
            counter_g = 0;                                      \
            sup_->send();                                       \
            assert(counter_g == 2);                             \
        }                                                       \
    }                                                           \
    BENCHMARK_REGISTER_F(fixture, bm_name)->DenseRange(0, 32, 8);

#define REGISTER_BENCHMARKS__(name, ...) \
    REGISTER_TEMPLATE_BENCHMARK(fixture_t, ping_pong_single_threaded_##name, __VA_ARGS__);

#define REGISTER_BENCHMARKS(type) \
    REGISTER_BENCHMARKS__(_1_##type,  type) \
    REGISTER_BENCHMARKS__(_2_##type,  type, type) \
    REGISTER_BENCHMARKS__(_3_##type,  type, type, type) \
    REGISTER_BENCHMARKS__(_4_##type,  type, type, type, type) \
    REGISTER_BENCHMARKS__(_5_##type,  type, type, type, type, type) \
    REGISTER_BENCHMARKS__(_6_##type,  type, type, type, type, type, type) \
    REGISTER_BENCHMARKS__(_7_##type,  type, type, type, type, type, type, type) \
    REGISTER_BENCHMARKS__(_8_##type,  type, type, type, type, type, type, type, type) \
    REGISTER_BENCHMARKS__(_9_##type,  type, type, type, type, type, type, type, type, type) \
    REGISTER_BENCHMARKS__(_10_##type, type, type, type, type, type, type, type, type, type, type)

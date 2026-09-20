#include <benchmark/benchmark.h>

#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <actor-zeta.hpp>

#include "fixtures.hpp"
#include "register_benchmark.hpp"

namespace benchmark_messages {

    static volatile int64_t message_sz = 0;

    using raw_t = actor_zeta::mailbox::message*;
    using smart_t = actor_zeta::mailbox::message_ptr;

    namespace by_name {

        BENCHMARK_TEMPLATE_DEFINE_F(fixture_t, RawPtrMessage_Name, raw_t)
        (benchmark::State& state) {
            auto* resource =std::pmr::get_default_resource();
            while (state.KeepRunning()) {
                auto [message, future] = actor_zeta::detail::make_message(
                    resource,
                    name_);
                auto tmp = sizeof(*message);
                if (static_cast<int64_t>(tmp) > static_cast<int64_t>(benchmark_messages::message_sz))
                    benchmark_messages::message_sz = static_cast<int64_t>(tmp);
            }
        }

        BENCHMARK_TEMPLATE_DEFINE_F(fixture_t, SmartPtrMessage_Name, smart_t)
        (benchmark::State& state) {
            auto* resource =std::pmr::get_default_resource();
            while (state.KeepRunning()) {
                auto [message, future] = actor_zeta::detail::make_message(
                    resource,
                    name_);
                auto tmp = sizeof(*message);
                if (static_cast<int64_t>(tmp) > static_cast<int64_t>(benchmark_messages::message_sz))
                    benchmark_messages::message_sz = static_cast<int64_t>(tmp);
            }
        }

        BENCHMARK_REGISTER_F(fixture_t, RawPtrMessage_Name)->DenseRange(0, 64, 8);
        BENCHMARK_REGISTER_F(fixture_t, SmartPtrMessage_Name)->DenseRange(0, 64, 8);

    } // namespace by_name

    namespace by_args {

        template<typename... Args>
        auto message_arg_tmpl(uint64_t  name_, Args&&... args) -> void;

        namespace raw_ptr {

            template<typename... Args>
            auto message_arg_tmpl(uint64_t name_, Args&&... args) -> void {
                auto* resource =std::pmr::get_default_resource();
                auto [message, future] = actor_zeta::detail::make_message(
                    resource,
                    name_,
                    std::forward<Args>(args)...);
                auto tmp = sizeof(*message);
                if (static_cast<int64_t>(tmp) > static_cast<int64_t>(benchmark_messages::message_sz))
                    benchmark_messages::message_sz = static_cast<int64_t>(tmp);
            }

            template<typename T, std::size_t... I>
            auto call_message_arg_tmpl(uint64_t  name_, T& packed_tuple, actor_zeta::type_traits::index_sequence<I...>) -> void {
                message_arg_tmpl(name_, (std::get<I>(packed_tuple))...);
            }

        } // namespace raw_ptr

        namespace smart_ptr {

            template<typename... Args>
            auto message_arg_tmpl(uint64_t name_, Args&&... args) -> void {
                auto* resource =std::pmr::get_default_resource();
                auto [message, future] = actor_zeta::detail::make_message(
                    resource,
                    name_,
                    std::forward<Args>(args)...);
                auto tmp = sizeof(*message);
                if (static_cast<int64_t>(tmp) > static_cast<int64_t>(benchmark_messages::message_sz))
                    benchmark_messages::message_sz = static_cast<int64_t>(tmp);
            }

            template<typename T, std::size_t... I>
            auto call_message_arg_tmpl(uint64_t name_, T& packed_tuple, actor_zeta::type_traits::index_sequence<I...>) -> void {
                message_arg_tmpl(name_, (std::get<I>(packed_tuple))...);
            }

        } // namespace smart_ptr

#define REGISTER_MESSAGE_BENCHMARK_FOR_RAWPTR_ARGS(fixture, bm_name, type) REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture, bm_name, raw_t, benchmark_messages::by_args::raw_ptr, type)
#define REGISTER_MESSAGE_BENCHMARK_FOR_SMARTPTR_ARGS(fixture, bm_name, type) REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture, bm_name, smart_t, benchmark_messages::by_args::smart_ptr, type)

        namespace trivial_args {

            REGISTER_MESSAGE_BENCHMARK_FOR_RAWPTR_ARGS(fixture_t, RawPtrMessage_Args_int, int);

            REGISTER_MESSAGE_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_t, SmartPtrMessage_Args_int, int);

        } // namespace trivial_args

        namespace smart_pointer_args {
        } // namespace smart_pointer_args

        namespace container_args {
        } // namespace container_args

        namespace custom_args {
        } // namespace custom_args

    } // namespace by_args

} // namespace benchmark_messages

int main(int argc, char** argv) {
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv))
        return 1;
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    benchmark::RegisterMemoryManager(nullptr);
    return 0;
}

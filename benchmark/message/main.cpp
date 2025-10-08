#include <benchmark/benchmark.h>

#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <actor-zeta/make_message.hpp>

#include "fixtures.hpp"

namespace benchmark_messages {

    static volatile int64_t message_sz = 0;

    namespace by_name {

        BENCHMARK_TEMPLATE_DEFINE_F(fixture_t, RawPtrMessage_Name, actor_zeta::base::message*)
        (benchmark::State& state) {
            while (state.KeepRunning()) {
                auto message = actor_zeta::make_message_ptr(
                    actor_zeta::base::address_t::empty_address(),
                    name_);
                auto tmp = sizeof(*message);
                delete message;
                if (static_cast<int64_t>(tmp) > static_cast<int64_t>(benchmark_messages::message_sz))
                    benchmark_messages::message_sz = static_cast<int64_t>(tmp);
            }
        }

        BENCHMARK_TEMPLATE_DEFINE_F(fixture_t, SmartPtrMessage_Name, actor_zeta::base::message_ptr)
        (benchmark::State& state) {
            while (state.KeepRunning()) {
                auto message = actor_zeta::make_message(
                    actor_zeta::base::address_t::empty_address(),
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
        auto message_arg_tmpl(std::string& name_, Args&&... args) -> void;

        namespace raw_ptr {

            template<typename... Args>
            auto message_arg_tmpl(std::string& name_, Args&&... args) -> void {
                auto message = actor_zeta::make_message_ptr(
                    actor_zeta::base::address_t::empty_address(),
                    name_,
                    std::forward<Args>(args)...);
                auto tmp = sizeof(*message);
                delete message;
                if (static_cast<int64_t>(tmp) > static_cast<int64_t>(benchmark_messages::message_sz))
                    benchmark_messages::message_sz = static_cast<int64_t>(tmp);
            }

            template<typename T, std::size_t... I>
            auto call_message_arg_tmpl(std::string& name_, T& packed_tuple, actor_zeta::type_traits::index_sequence<I...>) -> void {
                message_arg_tmpl(name_, (std::get<I>(packed_tuple))...);
            }

        } // namespace raw_ptr

        namespace smart_ptr {

            template<typename... Args>
            auto message_arg_tmpl(std::string& name_, Args&&... args) -> void {
                auto message = actor_zeta::make_message(
                    actor_zeta::base::address_t::empty_address(),
                    name_,
                    std::forward<Args>(args)...);
                auto tmp = sizeof(*message);
                if (static_cast<int64_t>(tmp) > static_cast<int64_t>(benchmark_messages::message_sz))
                    benchmark_messages::message_sz = static_cast<int64_t>(tmp);
            }

            template<typename T, std::size_t... I>
            auto call_message_arg_tmpl(std::string& name_, T& packed_tuple, actor_zeta::type_traits::index_sequence<I...>) -> void {
                message_arg_tmpl(name_, (std::get<I>(packed_tuple))...);
            }

        } // namespace smart_ptr

#define REGISTER_BENCHMARK(fixture, bm_name, p, ns, ...)                                                            \
    BENCHMARK_TEMPLATE_DEFINE_F(fixture, bm_name, p, __VA_ARGS__)                                                   \
    (benchmark::State & state) {                                                                                    \
        while (state.KeepRunning()) {                                                                               \
            ns::call_message_arg_tmpl(name_, arguments_, actor_zeta::type_traits::make_index_sequence<counter_>{}); \
        }                                                                                                           \
    }                                                                                                               \
    BENCHMARK_REGISTER_F(fixture, bm_name)->DenseRange(0, 64, 8)

#define REGISTER_BENCHMARK_FOR_RAWPTR(fixture, bm_name, ...) REGISTER_BENCHMARK(fixture, bm_name, actor_zeta::base::message*, benchmark_messages::by_args::raw_ptr, __VA_ARGS__)
#define REGISTER_BENCHMARK_FOR_SMARTPTR(fixture, bm_name, ...) REGISTER_BENCHMARK(fixture, bm_name, actor_zeta::base::message_ptr, benchmark_messages::by_args::smart_ptr, __VA_ARGS__)

#define REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture, bm_name, type)                                                                            \
    REGISTER_BENCHMARK_FOR_RAWPTR(fixture, bm_name##_1, type);                                                                                \
    REGISTER_BENCHMARK_FOR_RAWPTR(fixture, bm_name##_2, type, type);                                                                          \
    REGISTER_BENCHMARK_FOR_RAWPTR(fixture, bm_name##_3, type, type, type);                                                                    \
    REGISTER_BENCHMARK_FOR_RAWPTR(fixture, bm_name##_4, type, type, type, type);                                                              \
    REGISTER_BENCHMARK_FOR_RAWPTR(fixture, bm_name##_5, type, type, type, type, type);                                                        \
    REGISTER_BENCHMARK_FOR_RAWPTR(fixture, bm_name##_6, type, type, type, type, type, type);                                                  \
    REGISTER_BENCHMARK_FOR_RAWPTR(fixture, bm_name##_7, type, type, type, type, type, type, type);                                            \
    REGISTER_BENCHMARK_FOR_RAWPTR(fixture, bm_name##_8, type, type, type, type, type, type, type, type);                                      \
    REGISTER_BENCHMARK_FOR_RAWPTR(fixture, bm_name##_9, type, type, type, type, type, type, type, type, type);                                \
    REGISTER_BENCHMARK_FOR_RAWPTR(fixture, bm_name##_10, type, type, type, type, type, type, type, type, type, type);                         \
    REGISTER_BENCHMARK_FOR_RAWPTR(fixture, bm_name##_11, type, type, type, type, type, type, type, type, type, type, type);                   \
    REGISTER_BENCHMARK_FOR_RAWPTR(fixture, bm_name##_12, type, type, type, type, type, type, type, type, type, type, type, type);             \
    REGISTER_BENCHMARK_FOR_RAWPTR(fixture, bm_name##_13, type, type, type, type, type, type, type, type, type, type, type, type, type);       \
    REGISTER_BENCHMARK_FOR_RAWPTR(fixture, bm_name##_14, type, type, type, type, type, type, type, type, type, type, type, type, type, type); \
    REGISTER_BENCHMARK_FOR_RAWPTR(fixture, bm_name##_15, type, type, type, type, type, type, type, type, type, type, type, type, type, type, type)

#define REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture, bm_name, type)                                                                            \
    REGISTER_BENCHMARK_FOR_SMARTPTR(fixture, bm_name##_1, type);                                                                                \
    REGISTER_BENCHMARK_FOR_SMARTPTR(fixture, bm_name##_2, type, type);                                                                          \
    REGISTER_BENCHMARK_FOR_SMARTPTR(fixture, bm_name##_3, type, type, type);                                                                    \
    REGISTER_BENCHMARK_FOR_SMARTPTR(fixture, bm_name##_4, type, type, type, type);                                                              \
    REGISTER_BENCHMARK_FOR_SMARTPTR(fixture, bm_name##_5, type, type, type, type, type);                                                        \
    REGISTER_BENCHMARK_FOR_SMARTPTR(fixture, bm_name##_6, type, type, type, type, type, type);                                                  \
    REGISTER_BENCHMARK_FOR_SMARTPTR(fixture, bm_name##_7, type, type, type, type, type, type, type);                                            \
    REGISTER_BENCHMARK_FOR_SMARTPTR(fixture, bm_name##_8, type, type, type, type, type, type, type, type);                                      \
    REGISTER_BENCHMARK_FOR_SMARTPTR(fixture, bm_name##_9, type, type, type, type, type, type, type, type, type);                                \
    REGISTER_BENCHMARK_FOR_SMARTPTR(fixture, bm_name##_10, type, type, type, type, type, type, type, type, type, type);                         \
    REGISTER_BENCHMARK_FOR_SMARTPTR(fixture, bm_name##_11, type, type, type, type, type, type, type, type, type, type, type);                   \
    REGISTER_BENCHMARK_FOR_SMARTPTR(fixture, bm_name##_12, type, type, type, type, type, type, type, type, type, type, type, type);             \
    REGISTER_BENCHMARK_FOR_SMARTPTR(fixture, bm_name##_13, type, type, type, type, type, type, type, type, type, type, type, type, type);       \
    REGISTER_BENCHMARK_FOR_SMARTPTR(fixture, bm_name##_14, type, type, type, type, type, type, type, type, type, type, type, type, type, type); \
    REGISTER_BENCHMARK_FOR_SMARTPTR(fixture, bm_name##_15, type, type, type, type, type, type, type, type, type, type, type, type, type, type, type)

        namespace trivial_args {

            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_t, RawPtrMessage_Args_int, int);
            /*REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_t, RawPtrMessage_Args_int8, int8_t);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_t, RawPtrMessage_Args_int16, int16_t);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_t, RawPtrMessage_Args_int32, int32_t);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_t, RawPtrMessage_Args_int64, int64_t);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_t, RawPtrMessage_Args_short, short);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_t, RawPtrMessage_Args_unsigned_short, unsigned short);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_t, RawPtrMessage_Args_unsigned_int, unsigned int);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_t, RawPtrMessage_Args_unsigned_long_int, unsigned long int);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_t, RawPtrMessage_Args_long_int, long int);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_t, RawPtrMessage_Args_long_long_int, long long int);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_t, RawPtrMessage_Args_long_long, long long);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_t, RawPtrMessage_Args_long, long);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_t, RawPtrMessage_Args_float, float);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_t, RawPtrMessage_Args_double, double);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_t, RawPtrMessage_Args_long_double, long double);*/

            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_t, SmartPtrMessage_Args_int, int);
            /*REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_t, SmartPtrMessage_Args_int8, int8_t);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_t, SmartPtrMessage_Args_int16, int16_t);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_t, SmartPtrMessage_Args_int32, int32_t);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_t, SmartPtrMessage_Args_int64, int64_t);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_t, SmartPtrMessage_Args_short, short);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_t, SmartPtrMessage_Args_unsigned_short, unsigned short);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_t, SmartPtrMessage_Args_unsigned_int, unsigned int);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_t, SmartPtrMessage_Args_unsigned_long_int, unsigned long int);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_t, SmartPtrMessage_Args_long_int, long int);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_t, SmartPtrMessage_Args_long_long_int, long long int);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_t, SmartPtrMessage_Args_long_long, long long);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_t, SmartPtrMessage_Args_long, long);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_t, SmartPtrMessage_Args_float, float);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_t, SmartPtrMessage_Args_double, double);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_t, SmartPtrMessage_Args_long_double, long double);*/

        } // namespace trivial_args

        namespace smart_pointer_args {

            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_t, RawPtrMessage_Args_shared_ptr_int, int);
            /*REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_t, RawPtrMessage_Args_shared_ptr_int8, int8_t);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_t, RawPtrMessage_Args_shared_ptr_int16, int16_t);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_t, RawPtrMessage_Args_shared_ptr_int32, int32_t);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_t, RawPtrMessage_Args_shared_ptr_int64, int64_t);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_t, RawPtrMessage_Args_shared_ptr_short, short);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_t, RawPtrMessage_Args_shared_ptr_unsigned_short, unsigned short);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_t, RawPtrMessage_Args_shared_ptr_unsigned_int, unsigned int);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_t, RawPtrMessage_Args_shared_ptr_unsigned_long_int, unsigned long int);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_t, RawPtrMessage_Args_shared_ptr_long_int, long int);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_t, RawPtrMessage_Args_shared_ptr_long_long_int, long long int);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_t, RawPtrMessage_Args_shared_ptr_long_long, long long);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_t, RawPtrMessage_Args_shared_ptr_long, long);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_t, RawPtrMessage_Args_shared_ptr_float, float);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_t, RawPtrMessage_Args_shared_ptr_double, double);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_t, RawPtrMessage_Args_shared_ptr_long_double, long double);*/

            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_t, SmartPtrMessage_Args_shared_ptr_int, int);
            /*REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_t, SmartPtrMessage_Args_shared_ptr_int8, int8_t);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_t, SmartPtrMessage_Args_shared_ptr_int16, int16_t);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_t, SmartPtrMessage_Args_shared_ptr_int32, int32_t);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_t, SmartPtrMessage_Args_shared_ptr_int64, int64_t);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_t, SmartPtrMessage_Args_shared_ptr_short, short);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_t, SmartPtrMessage_Args_shared_ptr_unsigned_short, unsigned short);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_t, SmartPtrMessage_Args_shared_ptr_unsigned_int, unsigned int);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_t, SmartPtrMessage_Args_shared_ptr_unsigned_long_int, unsigned long int);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_t, SmartPtrMessage_Args_shared_ptr_long_int, long int);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_t, SmartPtrMessage_Args_shared_ptr_long_long_int, long long int);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_t, SmartPtrMessage_Args_shared_ptr_long_long, long long);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_t, SmartPtrMessage_Args_shared_ptr_long, long);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_t, SmartPtrMessage_Args_shared_ptr_float, float);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_t, SmartPtrMessage_Args_shared_ptr_double, double);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_t, SmartPtrMessage_Args_shared_ptr_long_double, long double);*/

        } // namespace smart_pointer_args

        namespace container_args {

            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_vec_t, RawPtrMessage_Args_std_vector_int, int);
            /*REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_vec_t, RawPtrMessage_Args_std_vector_int8, int8_t);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_vec_t, RawPtrMessage_Args_std_vector_int16, int16_t);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_vec_t, RawPtrMessage_Args_std_vector_int32, int32_t);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_vec_t, RawPtrMessage_Args_std_vector_int64, int64_t);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_vec_t, RawPtrMessage_Args_std_vector_short, short);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_vec_t, RawPtrMessage_Args_std_vector_unsigned_short, unsigned short);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_vec_t, RawPtrMessage_Args_std_vector_unsigned_int, unsigned int);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_vec_t, RawPtrMessage_Args_std_vector_unsigned_long_int, unsigned long int);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_vec_t, RawPtrMessage_Args_std_vector_long_int, long int);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_vec_t, RawPtrMessage_Args_std_vector_long_long_int, long long int);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_vec_t, RawPtrMessage_Args_std_vector_long_long, long long);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_vec_t, RawPtrMessage_Args_std_vector_long, long);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_vec_t, RawPtrMessage_Args_std_vector_float, float);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_vec_t, RawPtrMessage_Args_std_vector_double, double);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_vec_t, RawPtrMessage_Args_std_vector_long_double, long double);*/

            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_list_t, RawPtrMessage_Args_std_list_int, int);
            /*REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_list_t, RawPtrMessage_Args_std_list_int8, int8_t);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_list_t, RawPtrMessage_Args_std_list_int16, int16_t);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_list_t, RawPtrMessage_Args_std_list_int32, int32_t);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_list_t, RawPtrMessage_Args_std_list_int64, int64_t);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_list_t, RawPtrMessage_Args_std_list_short, short);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_list_t, RawPtrMessage_Args_std_list_unsigned_short, unsigned short);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_list_t, RawPtrMessage_Args_std_list_unsigned_int, unsigned int);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_list_t, RawPtrMessage_Args_std_list_unsigned_long_int, unsigned long int);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_list_t, RawPtrMessage_Args_std_list_long_int, long int);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_list_t, RawPtrMessage_Args_std_list_long_long_int, long long int);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_list_t, RawPtrMessage_Args_std_list_long_long, long long);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_list_t, RawPtrMessage_Args_std_list_long, long);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_list_t, RawPtrMessage_Args_std_list_float, float);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_list_t, RawPtrMessage_Args_std_list_double, double);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_list_t, RawPtrMessage_Args_std_list_long_double, long double);*/

            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_map_t, RawPtrMessage_Args_std_map_int, int);
            /*REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_map_t, RawPtrMessage_Args_std_map_int8, int8_t);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_map_t, RawPtrMessage_Args_std_map_int16, int16_t);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_map_t, RawPtrMessage_Args_std_map_int32, int32_t);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_map_t, RawPtrMessage_Args_std_map_int64, int64_t);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_map_t, RawPtrMessage_Args_std_map_short, short);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_map_t, RawPtrMessage_Args_std_map_unsigned_short, unsigned short);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_map_t, RawPtrMessage_Args_std_map_unsigned_int, unsigned int);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_map_t, RawPtrMessage_Args_std_map_unsigned_long_int, unsigned long int);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_map_t, RawPtrMessage_Args_std_map_long_int, long int);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_map_t, RawPtrMessage_Args_std_map_long_long_int, long long int);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_map_t, RawPtrMessage_Args_std_map_long_long, long long);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_map_t, RawPtrMessage_Args_std_map_long, long);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_map_t, RawPtrMessage_Args_std_map_float, float);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_map_t, RawPtrMessage_Args_std_map_double, double);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_map_t, RawPtrMessage_Args_std_map_long_double, long double);*/

            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_set_t, RawPtrMessage_Args_std_set_int, int);
            /*REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_set_t, RawPtrMessage_Args_std_set_int8, int8_t);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_set_t, RawPtrMessage_Args_std_set_int16, int16_t);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_set_t, RawPtrMessage_Args_std_set_int32, int32_t);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_set_t, RawPtrMessage_Args_std_set_int64, int64_t);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_set_t, RawPtrMessage_Args_std_set_short, short);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_set_t, RawPtrMessage_Args_std_set_unsigned_short, unsigned short);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_set_t, RawPtrMessage_Args_std_set_unsigned_int, unsigned int);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_set_t, RawPtrMessage_Args_std_set_unsigned_long_int, unsigned long int);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_set_t, RawPtrMessage_Args_std_set_long_int, long int);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_set_t, RawPtrMessage_Args_std_set_long_long_int, long long int);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_set_t, RawPtrMessage_Args_std_set_long_long, long long);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_set_t, RawPtrMessage_Args_std_set_long, long);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_set_t, RawPtrMessage_Args_std_set_float, float);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_set_t, RawPtrMessage_Args_std_set_double, double);
            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_set_t, RawPtrMessage_Args_std_set_long_double, long double);*/

            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_vec_t, SmartPtrMessage_Args_std_vector_int, int);
            /*REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_vec_t, SmartPtrMessage_Args_std_vector_int8, int8_t);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_vec_t, SmartPtrMessage_Args_std_vector_int16, int16_t);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_vec_t, SmartPtrMessage_Args_std_vector_int32, int32_t);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_vec_t, SmartPtrMessage_Args_std_vector_int64, int64_t);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_vec_t, SmartPtrMessage_Args_std_vector_short, short);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_vec_t, SmartPtrMessage_Args_std_vector_unsigned_short, unsigned short);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_vec_t, SmartPtrMessage_Args_std_vector_unsigned_int, unsigned int);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_vec_t, SmartPtrMessage_Args_std_vector_unsigned_long_int, unsigned long int);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_vec_t, SmartPtrMessage_Args_std_vector_long_int, long int);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_vec_t, SmartPtrMessage_Args_std_vector_long_long_int, long long int);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_vec_t, SmartPtrMessage_Args_std_vector_long_long, long long);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_vec_t, SmartPtrMessage_Args_std_vector_long, long);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_vec_t, SmartPtrMessage_Args_std_vector_float, float);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_vec_t, SmartPtrMessage_Args_std_vector_double, double);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_vec_t, SmartPtrMessage_Args_std_vector_long_double, long double);*/

            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_list_t, SmartPtrMessage_Args_std_list_int, int);
            /*REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_list_t, SmartPtrMessage_Args_std_list_int8, int8_t);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_list_t, SmartPtrMessage_Args_std_list_int16, int16_t);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_list_t, SmartPtrMessage_Args_std_list_int32, int32_t);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_list_t, SmartPtrMessage_Args_std_list_int64, int64_t);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_list_t, SmartPtrMessage_Args_std_list_short, short);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_list_t, SmartPtrMessage_Args_std_list_unsigned_short, unsigned short);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_list_t, SmartPtrMessage_Args_std_list_unsigned_int, unsigned int);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_list_t, SmartPtrMessage_Args_std_list_unsigned_long_int, unsigned long int);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_list_t, SmartPtrMessage_Args_std_list_long_int, long int);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_list_t, SmartPtrMessage_Args_std_list_long_long_int, long long int);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_list_t, SmartPtrMessage_Args_std_list_long_long, long long);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_list_t, SmartPtrMessage_Args_std_list_long, long);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_list_t, SmartPtrMessage_Args_std_list_float, float);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_list_t, SmartPtrMessage_Args_std_list_double, double);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_list_t, SmartPtrMessage_Args_std_list_long_double, long double);*/

            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_map_t, SmartPtrMessage_Args_std_map_int, int);
            /*REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_map_t, SmartPtrMessage_Args_std_map_int8, int8_t);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_map_t, SmartPtrMessage_Args_std_map_int16, int16_t);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_map_t, SmartPtrMessage_Args_std_map_int32, int32_t);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_map_t, SmartPtrMessage_Args_std_map_int64, int64_t);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_map_t, SmartPtrMessage_Args_std_map_short, short);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_map_t, SmartPtrMessage_Args_std_map_unsigned_short, unsigned short);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_map_t, SmartPtrMessage_Args_std_map_unsigned_int, unsigned int);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_map_t, SmartPtrMessage_Args_std_map_unsigned_long_int, unsigned long int);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_map_t, SmartPtrMessage_Args_std_map_long_int, long int);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_map_t, SmartPtrMessage_Args_std_map_long_long_int, long long int);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_map_t, SmartPtrMessage_Args_std_map_long_long, long long);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_map_t, SmartPtrMessage_Args_std_map_long, long);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_map_t, SmartPtrMessage_Args_std_map_float, float);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_map_t, SmartPtrMessage_Args_std_map_double, double);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_map_t, SmartPtrMessage_Args_std_map_long_double, long double);*/

            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_set_t, SmartPtrMessage_Args_std_set_int, int);
            /*REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_set_t, SmartPtrMessage_Args_std_set_int8, int8_t);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_set_t, SmartPtrMessage_Args_std_set_int16, int16_t);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_set_t, SmartPtrMessage_Args_std_set_int32, int32_t);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_set_t, SmartPtrMessage_Args_std_set_int64, int64_t);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_set_t, SmartPtrMessage_Args_std_set_short, short);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_set_t, SmartPtrMessage_Args_std_set_unsigned_short, unsigned short);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_set_t, SmartPtrMessage_Args_std_set_unsigned_int, unsigned int);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_set_t, SmartPtrMessage_Args_std_set_unsigned_long_int, unsigned long int);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_set_t, SmartPtrMessage_Args_std_set_long_int, long int);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_set_t, SmartPtrMessage_Args_std_set_long_long_int, long long int);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_set_t, SmartPtrMessage_Args_std_set_long_long, long long);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_set_t, SmartPtrMessage_Args_std_set_long, long);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_set_t, SmartPtrMessage_Args_std_set_float, float);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_set_t, SmartPtrMessage_Args_std_set_double, double);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_set_t, SmartPtrMessage_Args_std_set_long_double, long double);*/

        } // namespace container_args

        namespace custom_args {

            REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture_t, RawPtrMessage_Args_custom_1, custom_1_t);
            REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture_t, SmartPtrMessage_Args_custom_1, custom_1_t);

        } // namespace custom_args

    } // namespace by_args

    class memory_manager_t : public benchmark::MemoryManager {
        void Start() BENCHMARK_OVERRIDE {}
        void Stop(Result* result) BENCHMARK_OVERRIDE {
            result->max_bytes_used = message_sz;
        }
    };

} // namespace benchmark_messages

// Run the benchmark
int main(int argc, char** argv) {
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv))
        return 1;
    std::unique_ptr<benchmark::MemoryManager> mm(new benchmark_messages::memory_manager_t());
    benchmark::RegisterMemoryManager(mm.get());
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    benchmark::RegisterMemoryManager(nullptr);
    return 0;
}

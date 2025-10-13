#include <benchmark/benchmark.h>

#include <actor-zeta/detail/type_traits.hpp>

namespace benchmark_messages {

#define REGISTER_BENCHMARK(fixture, bm_name, p, ns, ...)                                                            \
    BENCHMARK_TEMPLATE_DEFINE_F(fixture, bm_name, p, __VA_ARGS__)                                                   \
    (benchmark::State & state) {                                                                                    \
        while (state.KeepRunning()) {                                                                               \
            ns::call_message_arg_tmpl(name_, arguments_, actor_zeta::type_traits::make_index_sequence<counter_>{}); \
        }                                                                                                           \
    }                                                                                                               \
    BENCHMARK_REGISTER_F(fixture, bm_name)->DenseRange(0, 32, 8)

#define REGISTER_MEMR_BENCHMARK(fixture, bm_name, p, ns, ...)                                                    \
    BENCHMARK_TEMPLATE_DEFINE_F(fixture, bm_name, p, __VA_ARGS__)                                                \
    (benchmark::State & state) {                                                                                 \
        while (state.KeepRunning()) {                                                                            \
            ns::call_message_arg_tmpl(arguments_, mr, actor_zeta::type_traits::make_index_sequence<counter_>{}); \
        }                                                                                                        \
    }                                                                                                            \
    BENCHMARK_REGISTER_F(fixture, bm_name)->DenseRange(0, 32, 8)

#define REGISTER_BENCHMARK_FOR_RAWPTR(fixture, bm_name, type_name, namespace_name, ...) REGISTER_BENCHMARK(fixture, bm_name, type_name, namespace_name, __VA_ARGS__)
#define REGISTER_BENCHMARK_FOR_SMARTPTR(fixture, bm_name, type_name, namespace_name, ...) REGISTER_BENCHMARK(fixture, bm_name, type_name, namespace_name, __VA_ARGS__)
#define REGISTER_BENCHMARK_WITH_MEMR(fixture, bm_name, type_name, namespace_name, ...) REGISTER_MEMR_BENCHMARK(fixture, bm_name, type_name, namespace_name, __VA_ARGS__)

#define REGISTER_BENCHMARK_FOR_RAWPTR_ARGS(fixture, bm_name, type_name, namespace_name, type)                                                                            \
    REGISTER_BENCHMARK_FOR_RAWPTR(fixture, bm_name##_1, type_name, namespace_name, type);                                                                                \
    REGISTER_BENCHMARK_FOR_RAWPTR(fixture, bm_name##_2, type_name, namespace_name, type, type);                                                                          \
    REGISTER_BENCHMARK_FOR_RAWPTR(fixture, bm_name##_3, type_name, namespace_name, type, type, type);                                                                    \
    REGISTER_BENCHMARK_FOR_RAWPTR(fixture, bm_name##_4, type_name, namespace_name, type, type, type, type);                                                              \
    REGISTER_BENCHMARK_FOR_RAWPTR(fixture, bm_name##_5, type_name, namespace_name, type, type, type, type, type);                                                        \
    REGISTER_BENCHMARK_FOR_RAWPTR(fixture, bm_name##_6, type_name, namespace_name, type, type, type, type, type, type);                                                  \
    REGISTER_BENCHMARK_FOR_RAWPTR(fixture, bm_name##_7, type_name, namespace_name, type, type, type, type, type, type, type);                                            \
    REGISTER_BENCHMARK_FOR_RAWPTR(fixture, bm_name##_8, type_name, namespace_name, type, type, type, type, type, type, type, type);                                      \
    REGISTER_BENCHMARK_FOR_RAWPTR(fixture, bm_name##_9, type_name, namespace_name, type, type, type, type, type, type, type, type, type);                                \
    REGISTER_BENCHMARK_FOR_RAWPTR(fixture, bm_name##_10, type_name, namespace_name, type, type, type, type, type, type, type, type, type, type);                         \
    REGISTER_BENCHMARK_FOR_RAWPTR(fixture, bm_name##_11, type_name, namespace_name, type, type, type, type, type, type, type, type, type, type, type);                   \
    REGISTER_BENCHMARK_FOR_RAWPTR(fixture, bm_name##_12, type_name, namespace_name, type, type, type, type, type, type, type, type, type, type, type, type);             \
    REGISTER_BENCHMARK_FOR_RAWPTR(fixture, bm_name##_13, type_name, namespace_name, type, type, type, type, type, type, type, type, type, type, type, type, type);       \
    REGISTER_BENCHMARK_FOR_RAWPTR(fixture, bm_name##_14, type_name, namespace_name, type, type, type, type, type, type, type, type, type, type, type, type, type, type); \
    REGISTER_BENCHMARK_FOR_RAWPTR(fixture, bm_name##_15, type_name, namespace_name, type, type, type, type, type, type, type, type, type, type, type, type, type, type, type)

#define REGISTER_BENCHMARK_FOR_SMARTPTR_ARGS(fixture, bm_name, type_name, namespace_name, type)                                                                            \
    REGISTER_BENCHMARK_FOR_SMARTPTR(fixture, bm_name##_1, type_name, namespace_name, type);                                                                                \
    REGISTER_BENCHMARK_FOR_SMARTPTR(fixture, bm_name##_2, type_name, namespace_name, type, type);                                                                          \
    REGISTER_BENCHMARK_FOR_SMARTPTR(fixture, bm_name##_3, type_name, namespace_name, type, type, type);                                                                    \
    REGISTER_BENCHMARK_FOR_SMARTPTR(fixture, bm_name##_4, type_name, namespace_name, type, type, type, type);                                                              \
    REGISTER_BENCHMARK_FOR_SMARTPTR(fixture, bm_name##_5, type_name, namespace_name, type, type, type, type, type);                                                        \
    REGISTER_BENCHMARK_FOR_SMARTPTR(fixture, bm_name##_6, type_name, namespace_name, type, type, type, type, type, type);                                                  \
    REGISTER_BENCHMARK_FOR_SMARTPTR(fixture, bm_name##_7, type_name, namespace_name, type, type, type, type, type, type, type);                                            \
    REGISTER_BENCHMARK_FOR_SMARTPTR(fixture, bm_name##_8, type_name, namespace_name, type, type, type, type, type, type, type, type);                                      \
    REGISTER_BENCHMARK_FOR_SMARTPTR(fixture, bm_name##_9, type_name, namespace_name, type, type, type, type, type, type, type, type, type);                                \
    REGISTER_BENCHMARK_FOR_SMARTPTR(fixture, bm_name##_10, type_name, namespace_name, type, type, type, type, type, type, type, type, type, type);                         \
    REGISTER_BENCHMARK_FOR_SMARTPTR(fixture, bm_name##_11, type_name, namespace_name, type, type, type, type, type, type, type, type, type, type, type);                   \
    REGISTER_BENCHMARK_FOR_SMARTPTR(fixture, bm_name##_12, type_name, namespace_name, type, type, type, type, type, type, type, type, type, type, type, type);             \
    REGISTER_BENCHMARK_FOR_SMARTPTR(fixture, bm_name##_13, type_name, namespace_name, type, type, type, type, type, type, type, type, type, type, type, type, type);       \
    REGISTER_BENCHMARK_FOR_SMARTPTR(fixture, bm_name##_14, type_name, namespace_name, type, type, type, type, type, type, type, type, type, type, type, type, type, type); \
    REGISTER_BENCHMARK_FOR_SMARTPTR(fixture, bm_name##_15, type_name, namespace_name, type, type, type, type, type, type, type, type, type, type, type, type, type, type, type)

#define REGISTER_BENCHMARK_WITH_MEMR_ARGS(fixture, bm_name, type_name, namespace_name, type)                                                                      \
    REGISTER_BENCHMARK_WITH_MEMR(fixture, bm_name##_1, type_name, namespace_name, type);                                                                          \
    REGISTER_BENCHMARK_WITH_MEMR(fixture, bm_name##_2, type_name, namespace_name, type, type);                                                                    \
    REGISTER_BENCHMARK_WITH_MEMR(fixture, bm_name##_3, type_name, namespace_name, type, type, type);                                                              \
    REGISTER_BENCHMARK_WITH_MEMR(fixture, bm_name##_4, type_name, namespace_name, type, type, type, type);                                                        \
    REGISTER_BENCHMARK_WITH_MEMR(fixture, bm_name##_5, type_name, namespace_name, type, type, type, type, type);                                                  \
    REGISTER_BENCHMARK_WITH_MEMR(fixture, bm_name##_6, type_name, namespace_name, type, type, type, type, type, type);                                            \
    REGISTER_BENCHMARK_WITH_MEMR(fixture, bm_name##_7, type_name, namespace_name, type, type, type, type, type, type, type);                                      \
    REGISTER_BENCHMARK_WITH_MEMR(fixture, bm_name##_8, type_name, namespace_name, type, type, type, type, type, type, type, type);                                \
    REGISTER_BENCHMARK_WITH_MEMR(fixture, bm_name##_9, type_name, namespace_name, type, type, type, type, type, type, type, type, type);                          \
    REGISTER_BENCHMARK_WITH_MEMR(fixture, bm_name##_10, type_name, namespace_name, type, type, type, type, type, type, type, type, type, type);                   \
    REGISTER_BENCHMARK_WITH_MEMR(fixture, bm_name##_11, type_name, namespace_name, type, type, type, type, type, type, type, type, type, type, type);             \
    REGISTER_BENCHMARK_WITH_MEMR(fixture, bm_name##_12, type_name, namespace_name, type, type, type, type, type, type, type, type, type, type, type, type);       \
    REGISTER_BENCHMARK_WITH_MEMR(fixture, bm_name##_13, type_name, namespace_name, type, type, type, type, type, type, type, type, type, type, type, type, type); \
    REGISTER_BENCHMARK_WITH_MEMR(fixture, bm_name##_14, type_name, namespace_name, type, type, type, type, type, type, type, type, type, type, type, type, type, type)
    //REGISTER_BENCHMARK_WITH_MEMR(fixture, bm_name##_15, type_name, namespace_name, type, type, type, type, type, type, type, type, type, type, type, type, type, type, type)

} // namespace benchmark_messages

#pragma once

#include <actor-zeta/detail/pmr/memory_resource.hpp>

#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <benchmark/benchmark.h>

namespace benchmark_messages {

#if HAVE_STD_PMR==1
    std::unique_ptr<unsigned char[]> buffer = std::make_unique<unsigned char[]>(static_cast<size_t>(1073741824) * 1);
    std::unique_ptr<unsigned char[]> backup = std::make_unique<unsigned char[]>(static_cast<size_t>(1073741824) * 2);
#endif

    namespace by_name {

        template<typename P>
        class fixture_t : public benchmark::Fixture {
        public:
            virtual void SetUp(const ::benchmark::State& state) final {
                for (int i = 0; i < state.range(0); ++i) {
                    name_ = static_cast<uint64_t>(i) % 10;
                }
            }

            virtual void SetUp(::benchmark::State& state) final {
                SetUp(static_cast<const ::benchmark::State&>(state));
            }

            virtual void TearDown(__attribute__((unused)) const ::benchmark::State& state) final {
                name_ = 0;
            }

            virtual void TearDown(::benchmark::State& state) final {
                TearDown(static_cast<const ::benchmark::State&>(state));
            }

            uint64_t name_;
        };

    } // namespace by_name

#define DEFINE_FIXTURE_CLASS_1(class_name, construct_name)                                                                                    \
    template<class P, typename... CustomArgs>                                                                                                 \
    class class_name : public ::benchmark::Fixture {                                                                                          \
    public:                                                                                                                                   \
        virtual void SetUp(const ::benchmark::State& state) final {                                                                           \
            arguments_ = std::make_tuple(std::forward<CustomArgs>(construct_name<CustomArgs>::get(static_cast<int64_t>(state.range(0))))...); \
            name_ = 0;                                                                                                                        \
        }                                                                                                                                     \
        virtual void SetUp(::benchmark::State& state) final {                                                                                 \
            SetUp(static_cast<const ::benchmark::State&>(state));                                                                             \
        }                                                                                                                                     \
        virtual void TearDown(__attribute__((unused)) const ::benchmark::State& state) final {                                                \
            name_ = 0;                                                                                                                        \
        }                                                                                                                                     \
        virtual void TearDown(::benchmark::State& state) final {                                                                              \
            TearDown(static_cast<const ::benchmark::State&>(state));                                                                          \
        }                                                                                                                                     \
        uint64_t name_;                                                                                                                       \
        std::tuple<CustomArgs...> arguments_;                                                                                                 \
        static constexpr size_t counter_ = sizeof...(CustomArgs);                                                                             \
    }

    template<size_t N>
    constexpr size_t getSize() {
        return N;
    }

    template<size_t N, class Head, class... Args>
    constexpr size_t getSize() {
        return getSize<((N + alignof(Head) - 1) & -(alignof(Head))) + sizeof(Head), Args...>();
    }

#if HAVE_STD_PMR==1
#define DEFINE_FIXTURE_CLASS_MEM_RESOURCE_1(class_name, construct_name)                                                                       \
    template<class P, typename... CustomArgs>                                                                                                 \
    class class_name : public ::benchmark::Fixture {                                                                                          \
        using monotonic_buffer_resource = actor_zeta::pmr::monotonic_buffer_resource;\
                                                                                                                                              \
    public:                                                                                                                                   \
        virtual void                                                                                                                          \
        SetUp(const ::benchmark::State& state) final {                                                                                        \
            arguments_ = std::make_tuple(std::forward<CustomArgs>(construct_name<CustomArgs>::get(static_cast<int64_t>(state.range(0))))...); \
            static monotonic_buffer_resource upstr_tmp(benchmark_messages::backup.get(), size);                                               \
            static monotonic_buffer_resource mr_tmp(benchmark_messages::buffer.get(), size, &upstr_tmp);                                      \
            upstr = &upstr_tmp;                                                                                                               \
            mr = &mr_tmp;                                                                                                                     \
        }                                                                                                                                     \
        virtual void SetUp(::benchmark::State& state) final {                                                                                 \
            SetUp(static_cast<const ::benchmark::State&>(state));                                                                             \
        }                                                                                                                                     \
        virtual void TearDown(__attribute__((unused)) const ::benchmark::State& state) final {                                                \
            mr->~monotonic_buffer_resource();                                                                                                 \
            upstr->~monotonic_buffer_resource();                                                                                              \
        }                                                                                                                                     \
        virtual void TearDown(::benchmark::State& state) final {                                                                              \
            TearDown(static_cast<const ::benchmark::State&>(state));                                                                          \
        }                                                                                                                                     \
        std::tuple<CustomArgs...> arguments_;                                                                                                 \
        static constexpr size_t counter_ = sizeof...(CustomArgs);                                                                             \
                                                                                                                                              \
        static constexpr size_t size = getSize<0, CustomArgs...>() * 3;                                                                       \
        monotonic_buffer_resource* upstr;                                                                                                     \
        monotonic_buffer_resource* mr;                                                                                                        \
    }
#else
#define DEFINE_FIXTURE_CLASS_MEM_RESOURCE_1(class_name, construct_name) DEFINE_FIXTURE_CLASS_1(class_name, construct_name)
#endif

    namespace by_args {

        namespace trivial_args {

            template<typename... CustomArgs>
            struct construct {};

            template<typename T>
            struct construct<T> {
                static T get(int64_t value) {
                    return T(value);
                }
            };

            DEFINE_FIXTURE_CLASS_1(fixture_t, construct);
            DEFINE_FIXTURE_CLASS_MEM_RESOURCE_1(fixture_mem_resource_t, construct);

        } // namespace trivial_args

#define DEFINE_APPEND_TO_TYPE_SEC_1(append_to_type_seq, type_name) \
    template<typename, typename>                                   \
    struct append_to_type_seq {};                                  \
    template<typename T, typename... Ts>                           \
    struct append_to_type_seq<T, std::tuple<Ts...>> {              \
        using type = std::tuple<Ts..., type_name<T>>;              \
    }

#define DEFINE_APPEND_TO_TYPE_SEC_2(append_to_type_seq, type_name) \
    template<typename, typename>                                   \
    struct append_to_type_seq {};                                  \
    template<typename T, typename... Ts>                           \
    struct append_to_type_seq<T, std::tuple<Ts...>> {              \
        using type = std::tuple<Ts..., type_name<T, T>>;           \
    }

#define DEFINE_REVERT_TYPE_SEC(append_to_type_seq, revert_type_seq) \
    template<typename... Ts>                                        \
    struct revert_type_seq {                                        \
        using type = std::tuple<>;                                  \
    };                                                              \
    template<typename T, typename... Ts>                            \
    struct revert_type_seq<T, Ts...> {                              \
        using type = typename append_to_type_seq<                   \
            T,                                                      \
            typename revert_type_seq<Ts...>::type>::type;           \
    }

#define DEFINE_FIXTURE_CLASS_2(class_name, construct_name, revert_type_seq)                                           \
    template<class P, typename... CustomArgs>                                                                         \
    class class_name : public ::benchmark::Fixture {                                                                  \
    public:                                                                                                           \
        virtual void SetUp(const ::benchmark::State& state) final {                                                   \
            arguments_ = std::make_tuple((construct_name<CustomArgs>::get(static_cast<int64_t>(state.range(0))))...); \
            name_ = 0;                                                                                          \
        }                                                                                                             \
        virtual void SetUp(::benchmark::State& state) final {                                                         \
            SetUp(static_cast<const ::benchmark::State&>(state));                                                     \
        }                                                                                                             \
        virtual void TearDown(__attribute__((unused)) const ::benchmark::State& state) final {                        \
            name_ = 0;                                                                                            \
        }                                                                                                             \
        virtual void TearDown(::benchmark::State& state) final {                                                      \
            TearDown(static_cast<const ::benchmark::State&>(state));                                                  \
        }                                                                                                             \
        uint64_t name_;                                                                                            \
        typename revert_type_seq<CustomArgs...>::type arguments_;                                                     \
        static constexpr size_t counter_ = sizeof...(CustomArgs);                                                     \
    }

#if HAVE_STD_PMR==1
#define DEFINE_FIXTURE_CLASS_MEM_RESOURCE_2(class_name, construct_name, revert_type_seq)                              \
    template<class P, typename... CustomArgs>                                                                         \
    class class_name : public ::benchmark::Fixture {                                                                  \
        using monotonic_buffer_resource = actor_zeta::pmr::monotonic_buffer_resource;                         \
                                                                                                                      \
    public:                                                                                                           \
        virtual void                                                                                                  \
        SetUp(const ::benchmark::State& state) final {                                                                \
            arguments_ = std::make_tuple((construct_name<CustomArgs>::get(static_cast<int64_t>(state.range(0))))...); \
            static monotonic_buffer_resource upstr_tmp(benchmark_messages::backup.get(), size);                       \
            static monotonic_buffer_resource mr_tmp(benchmark_messages::buffer.get(), size, &upstr_tmp);              \
            upstr = &upstr_tmp;                                                                                       \
            mr = &mr_tmp;                                                                                             \
        }                                                                                                             \
        virtual void SetUp(::benchmark::State& state) final {                                                         \
            SetUp(static_cast<const ::benchmark::State&>(state));                                                     \
        }                                                                                                             \
        virtual void TearDown(__attribute__((unused)) const ::benchmark::State& state) final {                        \
            mr->~monotonic_buffer_resource();                                                                         \
            upstr->~monotonic_buffer_resource();                                                                      \
        }                                                                                                             \
        virtual void TearDown(::benchmark::State& state) final {                                                      \
            TearDown(static_cast<const ::benchmark::State&>(state));                                                  \
        }                                                                                                             \
        typename revert_type_seq<CustomArgs...>::type arguments_;                                                     \
        static constexpr size_t counter_ = sizeof...(CustomArgs);                                                     \
                                                                                                                      \
        static constexpr size_t size = getSize<0, CustomArgs...>() * 3;                                               \
        monotonic_buffer_resource* upstr;                                                                             \
        monotonic_buffer_resource* mr;                                                                                \
    }
#else
#define DEFINE_FIXTURE_CLASS_MEM_RESOURCE_2(class_name, construct_name, revert_type_seq) DEFINE_FIXTURE_CLASS_2(class_name, construct_name, revert_type_seq)
#endif

        namespace smart_pointer_args {

            template<typename... CustomArgs>
            struct construct {};

            template<typename T>
            struct construct<T> {
                static std::shared_ptr<T> get(int64_t value) {
                    return std::shared_ptr<T>(new T(static_cast<T>(value)));
                }
            };

            DEFINE_APPEND_TO_TYPE_SEC_1(append_to_type_seq, std::shared_ptr);
            DEFINE_REVERT_TYPE_SEC(append_to_type_seq, revert_type_seq);

            DEFINE_FIXTURE_CLASS_2(fixture_t, construct, revert_type_seq);

        } // namespace smart_pointer_args

        namespace container_args {

            template<typename... CustomArgs>
            struct construct_vec {};

            template<typename... CustomArgs>
            struct construct_list {};

            template<typename... CustomArgs>
            struct construct_map {};

            template<typename... CustomArgs>
            struct construct_set {};

            template<typename T>
            struct construct_vec<T> {
                static std::vector<T> get(int64_t value) {
                    std::vector<T> vec;
                    for (auto i = 0; i < value; ++i) {
                        vec.push_back(static_cast<T>(value));
                    }
                    return vec;
                }
            };

            template<typename T>
            struct construct_list<T> {
                static std::list<T> get(int64_t value) {
                    std::list<T> list;
                    for (auto i = 0; i < value; ++i) {
                        list.emplace_back(static_cast<T>(value));
                    }
                    return list;
                }
            };

            template<typename T>
            struct construct_map<T> {
                static std::map<T, T> get(int64_t value) {
                    std::map<T, T> map;
                    for (auto i = 0; i < value; ++i) {
                        map[static_cast<T>(value)] = static_cast<T>(value);
                    }
                    return map;
                }
            };

            template<typename T>
            struct construct_set<T> {
                static std::set<T> get(int64_t value) {
                    std::set<T> set;
                    for (auto i = 0; i < value; ++i) {
                        set.emplace(static_cast<T>(value));
                    }
                    return set;
                }
            };

            DEFINE_APPEND_TO_TYPE_SEC_1(append_to_type_seq_vec, std::vector);
            DEFINE_REVERT_TYPE_SEC(append_to_type_seq_vec, revert_type_seq_vec);

            DEFINE_APPEND_TO_TYPE_SEC_1(append_to_type_seq_list, std::list);
            DEFINE_REVERT_TYPE_SEC(append_to_type_seq_list, revert_type_seq_list);

            DEFINE_APPEND_TO_TYPE_SEC_2(append_to_type_seq_map, std::map);
            DEFINE_REVERT_TYPE_SEC(append_to_type_seq_map, revert_type_seq_map);

            DEFINE_APPEND_TO_TYPE_SEC_1(append_to_type_seq_set, std::set);
            DEFINE_REVERT_TYPE_SEC(append_to_type_seq_set, revert_type_seq_set);

            DEFINE_FIXTURE_CLASS_2(fixture_vec_t, construct_vec, revert_type_seq_vec);
            DEFINE_FIXTURE_CLASS_2(fixture_list_t, construct_list, revert_type_seq_list);
            DEFINE_FIXTURE_CLASS_2(fixture_map_t, construct_map, revert_type_seq_map);
            DEFINE_FIXTURE_CLASS_2(fixture_set_t, construct_set, revert_type_seq_set);

            DEFINE_FIXTURE_CLASS_MEM_RESOURCE_2(fixture_mr_vec_t, construct_vec, revert_type_seq_vec);
            DEFINE_FIXTURE_CLASS_MEM_RESOURCE_2(fixture_mr_list_t, construct_list, revert_type_seq_list);
            DEFINE_FIXTURE_CLASS_MEM_RESOURCE_2(fixture_mr_map_t, construct_map, revert_type_seq_map);
            DEFINE_FIXTURE_CLASS_MEM_RESOURCE_2(fixture_mr_set_t, construct_set, revert_type_seq_set);

        } // namespace container_args

        namespace custom_args {

            struct custom_1_t {
                custom_1_t(int64_t value) {
                    std::string tmp;
                    for (int64_t i = 0; i < value; ++i) {
                        tmp += std::to_string(i % 10);
                        map_[i] = tmp;
                        vec_.push_back(tmp);
                    }
                }
                custom_1_t() = default;
                custom_1_t(custom_1_t&) = default;
                custom_1_t(const custom_1_t&) = default;
                custom_1_t(custom_1_t&&) = default;
                ~custom_1_t() = default;

                custom_1_t& operator=(custom_1_t&) = default;
                custom_1_t& operator=(const custom_1_t&) = default;
                custom_1_t& operator=(custom_1_t&&) = default;

                std::map<int64_t, std::string> map_;
                std::vector<std::string> vec_;
            };

            template<typename... CustomArgs>
            struct construct {};

            template<typename T>
            struct construct<T> {
                static T get(int64_t value) {
                    return T(value);
                }
            };

            DEFINE_APPEND_TO_TYPE_SEC_1(append_to_type_seq, std::shared_ptr);
            DEFINE_REVERT_TYPE_SEC(append_to_type_seq, revert_type_seq);

            DEFINE_FIXTURE_CLASS_1(fixture_t, construct);

        } // namespace custom_args

    } // namespace by_args

} // namespace benchmark_messages

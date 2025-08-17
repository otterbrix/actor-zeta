#define CATCH_CONFIG_MAIN // This tells Catch to provide a main() - only do this in one cpp file
#include <catch2/catch.hpp>

#include <actor-zeta/detail/rtt.hpp>

#include "classes.hpp"
#include "../utils/unused.hpp"

#include <actor-zeta/detail/callable_trait.hpp>
#include <actor-zeta/detail/type_list.hpp>

#include <memory>

using actor_zeta::detail::rtt;

#ifdef __ENABLE_TESTS_MEASUREMENTS__
namespace rtt_test = actor_zeta::detail::rtt_test;
#endif

TEST_CASE("rt_tuple") {
    SECTION("actor_zeta::detail::get f1") {
        auto f = [](
                     UNUSED int8_t a1,
                     UNUSED int16_t a2,
                     UNUSED char a3,
                     UNUSED std::string a4) {};
        rtt r(nullptr, int8_t(5), int16_t(10), char(3), std::string("dfkjgksdjgkasdg"));
        using call_trait = actor_zeta::type_traits::get_callable_trait_t<actor_zeta::type_traits::remove_reference_t<decltype(f)>>;
        using args_type_list = typename call_trait::args_types;
        REQUIRE(actor_zeta::detail::get<0, args_type_list>(r) == int8_t(5));
        REQUIRE(actor_zeta::detail::get<1, args_type_list>(r) == int16_t(10));
        REQUIRE(actor_zeta::detail::get<2, args_type_list>(r) == char(3));
        REQUIRE(actor_zeta::detail::get<3, args_type_list>(r) == std::string("dfkjgksdjgkasdg"));
        REQUIRE(std::is_same<decltype(actor_zeta::detail::get<0, args_type_list>(r)), int8_t>::value);
        REQUIRE(std::is_same<decltype(actor_zeta::detail::get<1, args_type_list>(r)), int16_t>::value);
        REQUIRE(std::is_same<decltype(actor_zeta::detail::get<2, args_type_list>(r)), char>::value);
        REQUIRE(std::is_same<decltype(actor_zeta::detail::get<3, args_type_list>(r)), std::string>::value);
    }

    SECTION("actor_zeta::detail::get f2") { // @TODO !!! here is a type_traits mismatch: const type == type !!!
        auto f = [](
                     UNUSED const int8_t a1,
                     UNUSED const int16_t a2,
                     UNUSED const char a3,
                     UNUSED const std::string a4) {};
        rtt r(nullptr, int8_t(5), int16_t(10), char(3), std::string("dfkjgksdjgkasdg"));
        using call_trait = actor_zeta::type_traits::get_callable_trait_t<actor_zeta::type_traits::remove_reference_t<decltype(f)>>;
        using args_type_list = typename call_trait::args_types;
        REQUIRE(actor_zeta::detail::get<0, args_type_list>(r) == int8_t(5));
        REQUIRE(actor_zeta::detail::get<1, args_type_list>(r) == int16_t(10));
        REQUIRE(actor_zeta::detail::get<2, args_type_list>(r) == char(3));
        REQUIRE(actor_zeta::detail::get<3, args_type_list>(r) == std::string("dfkjgksdjgkasdg"));
        REQUIRE(std::is_same<typename actor_zeta::type_traits::type_list_at_t<args_type_list, 0>, int8_t>::value);
        REQUIRE(std::is_same<typename actor_zeta::type_traits::type_list_at_t<args_type_list, 1>, int16_t>::value);
        REQUIRE(std::is_same<typename actor_zeta::type_traits::type_list_at_t<args_type_list, 2>, char>::value);
        REQUIRE(std::is_same<typename actor_zeta::type_traits::type_list_at_t<args_type_list, 3>, std::string>::value);
        REQUIRE(std::is_same<decltype(actor_zeta::detail::get<0, args_type_list>(r)), int8_t>::value);
        REQUIRE(std::is_same<decltype(actor_zeta::detail::get<1, args_type_list>(r)), int16_t>::value);
        REQUIRE(std::is_same<decltype(actor_zeta::detail::get<2, args_type_list>(r)), char>::value);
        REQUIRE(std::is_same<decltype(actor_zeta::detail::get<3, args_type_list>(r)), std::string>::value);
    }

    SECTION("actor_zeta::detail::get f3") {
        auto f = [](
                     UNUSED int8_t& a1,
                     UNUSED int16_t& a2,
                     UNUSED char& a3,
                     UNUSED std::string& a4) {};
        rtt r(nullptr, int8_t(5), int16_t(10), char(3), std::string("dfkjgksdjgkasdg"));
        using call_trait = actor_zeta::type_traits::get_callable_trait_t<actor_zeta::type_traits::remove_reference_t<decltype(f)>>;
        using args_type_list = typename call_trait::args_types;
        REQUIRE(actor_zeta::detail::get<0, args_type_list>(r) == int8_t(5));
        REQUIRE(actor_zeta::detail::get<1, args_type_list>(r) == int16_t(10));
        REQUIRE(actor_zeta::detail::get<2, args_type_list>(r) == char(3));
        REQUIRE(actor_zeta::detail::get<3, args_type_list>(r) == std::string("dfkjgksdjgkasdg"));
        REQUIRE(std::is_same<decltype(actor_zeta::detail::get<0, args_type_list>(r)), int8_t&>::value);
        REQUIRE(std::is_same<decltype(actor_zeta::detail::get<1, args_type_list>(r)), int16_t&>::value);
        REQUIRE(std::is_same<decltype(actor_zeta::detail::get<2, args_type_list>(r)), char&>::value);
        REQUIRE(std::is_same<decltype(actor_zeta::detail::get<3, args_type_list>(r)), std::string&>::value);
    }

    SECTION("actor_zeta::detail::get f4") {
        auto f = [](
                     UNUSED const int8_t& a1,
                     UNUSED const int16_t& a2,
                     UNUSED const char& a3,
                     UNUSED const std::string& a4) {};
        rtt r(nullptr, int8_t(5), int16_t(10), char(3), std::string("dfkjgksdjgkasdg"));
        using call_trait = actor_zeta::type_traits::get_callable_trait_t<actor_zeta::type_traits::remove_reference_t<decltype(f)>>;
        using args_type_list = typename call_trait::args_types;
        REQUIRE(actor_zeta::detail::get<0, args_type_list>(r) == int8_t(5));
        REQUIRE(actor_zeta::detail::get<1, args_type_list>(r) == int16_t(10));
        REQUIRE(actor_zeta::detail::get<2, args_type_list>(r) == char(3));
        REQUIRE(actor_zeta::detail::get<3, args_type_list>(r) == std::string("dfkjgksdjgkasdg"));
        REQUIRE(std::is_same<decltype(actor_zeta::detail::get<0, args_type_list>(r)), const int8_t&>::value);
        REQUIRE(std::is_same<decltype(actor_zeta::detail::get<1, args_type_list>(r)), const int16_t&>::value);
        REQUIRE(std::is_same<decltype(actor_zeta::detail::get<2, args_type_list>(r)), const char&>::value);
        REQUIRE(std::is_same<decltype(actor_zeta::detail::get<3, args_type_list>(r)), const std::string&>::value);
    }

    SECTION("actor_zeta::detail::get f5") {
        auto f = [](
                     UNUSED std::unique_ptr<int64_t>&& up1,
                     UNUSED std::unique_ptr<int64_t>&& up2,
                     UNUSED const int16_t* p2) {};

        std::unique_ptr<int64_t> uptr = std::unique_ptr<int64_t>(new int64_t(64));
        int16_t i16 = 16;
        int16_t* p_i16 = &i16;

        rtt r(nullptr, std::move(uptr),std::unique_ptr<int64_t>(new int64_t(64)), p_i16);
        REQUIRE_FALSE(uptr);
        using call_trait = actor_zeta::type_traits::get_callable_trait_t<actor_zeta::type_traits::remove_reference_t<decltype(f)>>;
        using args_type_list = typename call_trait::args_types;

        REQUIRE(actor_zeta::detail::get<2, args_type_list>(r) == p_i16);

        REQUIRE(*actor_zeta::detail::get<0, args_type_list>(r) == 64);
        REQUIRE(*actor_zeta::detail::get<1, args_type_list>(r) == 64);
        REQUIRE(*actor_zeta::detail::get<2, args_type_list>(r) == i16);


        REQUIRE(std::is_same<decltype(actor_zeta::detail::get<0, args_type_list>(r)), std::unique_ptr<int64_t>&& >::value);
        REQUIRE(std::is_same<decltype(actor_zeta::detail::get<1, args_type_list>(r)), std::unique_ptr<int64_t>&& >::value);
        REQUIRE(std::is_same<decltype(actor_zeta::detail::get<2, args_type_list>(r)), const int16_t*>::value);
    }

    SECTION("actor_zeta::detail::get f6") {
        struct pointer_test {
                size_t d1;
                size_t d2;
                size_t d3;

                bool operator==(const pointer_test& rhs) const {
                        return d1 == rhs.d1 && d2 == rhs.d2 && d3 == rhs.d3;
                }
        };

        auto f = [](
                     UNUSED std::string* p1,
                     UNUSED pointer_test* p2) {};

        std::string* p_str = new std::string {"123456"};
        pointer_test* p_dummy = new pointer_test{11111, 22222, 33333};


        rtt r(nullptr, p_str, p_dummy);
        using call_trait = actor_zeta::type_traits::get_callable_trait_t<actor_zeta::type_traits::remove_reference_t<decltype(f)>>;
        using args_type_list = typename call_trait::args_types;

        REQUIRE(actor_zeta::detail::get<0, args_type_list>(r) == p_str);
        REQUIRE(actor_zeta::detail::get<1, args_type_list>(r) == p_dummy);

        REQUIRE(*actor_zeta::detail::get<0, args_type_list>(r) == *p_str);
        REQUIRE(*actor_zeta::detail::get<1, args_type_list>(r) == *p_dummy);


        REQUIRE(std::is_same<decltype(actor_zeta::detail::get<0, args_type_list>(r)), std::string*>::value);
        REQUIRE(std::is_same<decltype(actor_zeta::detail::get<1, args_type_list>(r)), pointer_test*>::value);

        delete p_str;
        delete p_dummy;
    }

    SECTION("test_getSize") {
        test_getSize(
            static_cast<int8_t>(3457),
            static_cast<int64_t>(57),
            static_cast<int32_t>(45),
            static_cast<long>(63),
            static_cast<long long>(6348),
            static_cast<long double>(6754.9),
            static_cast<int16_t>(4563),
            static_cast<float>(5653),
            static_cast<double>(675.49),
            static_cast<unsigned long>(45645),
            static_cast<unsigned long long>(5573),
            static_cast<char>(57));

        std::map<std::string, std::string> map_strstr;
        for (int i = 0; i < 1024; ++i) {
            for (int j = 0; j < 4096; ++j)
                map_strstr["base64_" + std::to_string(i)].push_back(static_cast<char>(j % 8));
        }

        test_getSize(
            map_strstr,
            static_cast<long long>(6348),
            static_cast<long double>(6754.9),
            static_cast<int16_t>(4563),
            map_strstr,
            static_cast<float>(5653),
            static_cast<double>(675.49),
            map_strstr);

        test_getSize(
            map_strstr,
            static_cast<long long>(6348),
            static_cast<long double>(6754.9),
            std::string("sekrjghkjsh8"),
            std::string("slkdjfghlkajshglkjsdfhglkjshglkjhsdflkgjhsdflkgh"),
            static_cast<int16_t>(4563),
            map_strstr,
            std::string("slkdjfghlkajshglkjsddf,ghkjy5o876394673984769834569843589734563465fhglkjshglkjhsdflkgjhsdflkgh"),
            static_cast<float>(5653),
            std::string("slkdjfghlkajshglkjsdsfgs3465345634563465fhglkjshglkjhsdflkgjhsdflkgh"),
            static_cast<double>(675.49),
            std::string("35673673467345756756856875679659567956"),
            map_strstr,
            std::string("slkdjfghlkajshglkjsdfhglkjshglkjhsdflkgjhsdflkgsdfihgshdfgosfogskjfgkshfghsdkfjhgh"));

        test_getSize(
            align_example_t{},
            align_example_t{{1, 2, 3}, 15, 12, {6, 7, 8}},
            align_example_t{{1, 2, 3}, 15, 12, {6, 7, 8}},
            align_example_t{{1, 2, 3}, 15, 12, {6, 7, 8}},
            align_example_t{{1, 2, 3}, 15, 12, {6, 7, 8}},
            align_example_t{{1, 2, 3}, 15, 12, {6, 7, 8}});
    }

    SECTION("test_getSize many strings") {
        std::vector<int> idxs;
        for (int i = 0; i < 1024; ++i) {
            idxs.push_back(i);
        }

        auto idx = GENERATE_REF(from_range(idxs));
        std::string test_str;
        for (int i = 0; i < idx; ++i)
            test_str.push_back(static_cast<char>(i % 8));

        test_getSize(
            test_str);

        test_getSize(
            test_str,
            test_str,
            test_str);

        test_getSize(
            test_str,
            test_str,
            test_str,
            test_str,
            test_str,
            test_str,
            test_str);

        test_getSize(
            test_str,
            test_str,
            test_str,
            test_str,
            test_str,
            test_str,
            test_str,
            test_str,
            test_str,
            test_str,
            test_str,
            test_str,
            test_str,
            test_str,
            test_str,
            test_str);
    }

    SECTION("test_getSize many maps of string string") {
        std::vector<int> idxs;
        for (int i = 0; i < 1024; ++i) {
            idxs.push_back(i);
        }

        auto idx = GENERATE_REF(from_range(idxs));
        std::map<std::string, std::string> test_map;
        for (int i = 0; i < idx; ++i)
            test_map[std::to_string(i)] = "kfjhgkjsgkhsha";

        test_getSize(
            test_map);

        test_getSize(
            test_map,
            test_map,
            test_map);

        test_getSize(
            test_map,
            test_map,
            test_map,
            test_map,
            test_map,
            test_map,
            test_map);

        test_getSize(
            test_map,
            test_map,
            test_map,
            test_map,
            test_map,
            test_map,
            test_map,
            test_map,
            test_map,
            test_map,
            test_map,
            test_map,
            test_map,
            test_map,
            test_map,
            test_map);
    }

#ifdef __ENABLE_TESTS_MEASUREMENTS__

    SECTION("ctors") {
        rtt_test::clear();
        {
            auto templated_rtt_ = rtt(nullptr, 87645);
            REQUIRE(rtt_test::default_ctor_ == 0);
            REQUIRE(rtt_test::templated_ctor_ == 1);
            REQUIRE(rtt_test::move_ctor_ == 0);

            REQUIRE(rtt_test::move_operator_ == 0);
        }
        REQUIRE(rtt_test::dtor_ == 1);
        rtt_test::clear();

        {
            auto templated_rtt_ = rtt(nullptr, 87645, 1, 3, 356356, "aljehrgiauhg", std::vector<int>{1, 2, 3, 4, 5});
            REQUIRE(rtt_test::default_ctor_ == 0);
            REQUIRE(rtt_test::templated_ctor_ == 1);
            REQUIRE(rtt_test::move_ctor_ == 0);

            REQUIRE(rtt_test::move_operator_ == 0);
        }
        REQUIRE(rtt_test::dtor_ == 1);
        rtt_test::clear();

        {
            auto default_rtt_ = rtt();
            REQUIRE(rtt_test::default_ctor_ == 1);
            REQUIRE(rtt_test::templated_ctor_ == 0);
            REQUIRE(rtt_test::move_ctor_ == 0);

            REQUIRE(rtt_test::move_operator_ == 0);
        }
        REQUIRE(rtt_test::dtor_ == 1);
        rtt_test::clear();

        {
            auto default_rtt_ = rtt();            // defaulted (+1)
            auto copied_rtt_ = rtt(default_rtt_); // non-const lvalue
            REQUIRE(rtt_test::default_ctor_ == 1);
            REQUIRE(rtt_test::templated_ctor_ == 0);
            REQUIRE(rtt_test::move_ctor_ == 0);

            REQUIRE(rtt_test::move_operator_ == 0);
        }
        REQUIRE(rtt_test::dtor_ == 2);
        rtt_test::clear();

        {
            const auto default_rtt_ = rtt();            // defaulted (+1)
            auto const_copied_rtt_ = rtt(default_rtt_); // const lvalue
            REQUIRE(rtt_test::default_ctor_ == 1);
            REQUIRE(rtt_test::templated_ctor_ == 0);
            REQUIRE(rtt_test::move_ctor_ == 0);

            REQUIRE(rtt_test::move_operator_ == 0);
        }
        REQUIRE(rtt_test::dtor_ == 2);
        rtt_test::clear();

        {
            auto default_rtt_ = rtt();                      // defaulted (+1)
            auto moved_rtt_ = rtt(std::move(default_rtt_)); // move rvalue
            REQUIRE(rtt_test::default_ctor_ == 1);
            REQUIRE(rtt_test::templated_ctor_ == 0);
            REQUIRE(rtt_test::move_ctor_ == 1);

            REQUIRE(rtt_test::move_operator_ == 0);
        }
        REQUIRE(rtt_test::dtor_ == 2);
        rtt_test::clear();

        /* many ctors */

        {
            auto default_rtt_ = rtt();
            auto templated_1_rtt_ = rtt(nullptr, std::vector<int>{1, 2, 3, 4, 5}, 767, 5);
            auto templated_2_rtt_ = rtt(nullptr, 87645, 1, 3, 356356, "aljehrgiauhg", std::vector<int>{1, 2, 3, 4, 5}, 457, 4567);

            REQUIRE(rtt_test::default_ctor_ == 1);
            REQUIRE(rtt_test::templated_ctor_ == 2);
            REQUIRE(rtt_test::move_ctor_ == 0);

            REQUIRE(rtt_test::move_operator_ == 0);
        }
        REQUIRE(rtt_test::dtor_ == 3);
        rtt_test::clear();

        {
            auto default_rtt_ = rtt(); // defaulted (+1)
            auto templated_1_rtt_ = rtt(nullptr, std::vector<int>{1, 2, 3, 4, 5}, 767, 5);
            auto templated_2_rtt_ = rtt(nullptr, 87645, 1, 3, 356356, "aljehrgiauhg", std::vector<int>{1, 2, 3, 4, 5}, 457, 4567);
            auto moved_1_rtt_ = rtt(std::move(templated_1_rtt_)); // move rvalue
            auto moved_2_rtt_ = rtt(std::move(templated_2_rtt_)); // move rvalue
            auto moved_3_rtt_ = rtt(std::move(default_rtt_));     // move rvalue

            REQUIRE(rtt_test::default_ctor_ == 1);
            REQUIRE(rtt_test::templated_ctor_ == 2);
            REQUIRE(rtt_test::move_ctor_ == 3);

            REQUIRE(rtt_test::move_operator_ == 0);
        }
        REQUIRE(rtt_test::dtor_ == 6);
        rtt_test::clear();

        {
            auto default_rtt_ = rtt();                        // defaulted (+1)
            const auto const_default_rtt_ = rtt();            // const defaulted (+1)
            auto copied_rtt_ = rtt(default_rtt_);             // non-const lvalue
            auto const_copied_rtt_ = rtt(const_default_rtt_); // const lvalue
            auto templated_1_rtt_ = rtt(nullptr, std::vector<int>{1, 2, 3, 4, 5}, 767, 5);
            auto templated_2_rtt_ = rtt(nullptr, 87645, 1, 3, 356356, "aljehrgiauhg", std::vector<int>{1, 2, 3, 4, 5}, 457, 4567);

            REQUIRE(rtt_test::default_ctor_ == 2);
            REQUIRE(rtt_test::templated_ctor_ == 2);
            REQUIRE(rtt_test::move_ctor_ == 0);

            REQUIRE(rtt_test::move_operator_ == 0);
        }
        REQUIRE(rtt_test::dtor_ == 6);
        rtt_test::clear();

        {
            auto default_rtt_ = rtt();                                           // defaulted (+1)
            auto copied_rtt_ = rtt(default_rtt_);                                // non-const lvalue
            auto const_copied_rtt_ = rtt(static_cast<const rtt&>(default_rtt_)); // const lvalue
            auto templated_1_rtt_ = rtt(nullptr, std::vector<int>{1, 2, 3, 4, 5}, 767, 5);
            auto templated_2_rtt_ = rtt(nullptr, 87645, 1, 3, 356356, "aljehrgiauhg", std::vector<int>{1, 2, 3, 4, 5}, 457, 4567);

            REQUIRE(rtt_test::default_ctor_ == 1);
            REQUIRE(rtt_test::templated_ctor_ == 2);
            REQUIRE(rtt_test::move_ctor_ == 0);

            REQUIRE(rtt_test::move_operator_ == 0);
        }
        REQUIRE(rtt_test::dtor_ == 5);
        rtt_test::clear();

        {
            auto templated_1_rtt_ = rtt(nullptr, std::vector<int>{1, 2, 3, 4, 5});
            auto templated_2_rtt_ = rtt(nullptr, 87645, 1, 3, 356356, "aljehrgiauhg", std::vector<int>{1, 2, 3, 4, 5});
            auto default_rtt_ = rtt();                                           // defaulted (+1)
            auto copied_rtt_ = rtt(default_rtt_);                                // non-const lvalue
            auto const_copied_rtt_ = rtt(static_cast<const rtt&>(default_rtt_)); // const lvalue
            auto moved_1_rtt_ = rtt(std::move(default_rtt_));                    // move rvalue
            auto moved_2_rtt_ = rtt(std::move(templated_1_rtt_));                // move rvalue

            REQUIRE(rtt_test::default_ctor_ == 1);
            REQUIRE(rtt_test::templated_ctor_ == 2);
            REQUIRE(rtt_test::move_ctor_ == 2);
            REQUIRE(rtt_test::move_operator_ == 0);
        }
        REQUIRE(rtt_test::dtor_ == 7);
        rtt_test::clear();

        {
            auto templated_rtt_ = rtt(nullptr, 87645);
            auto o1 = templated_rtt_;
            auto o2 = std::move(o1);
            REQUIRE(rtt_test::default_ctor_ == 0);
            REQUIRE(rtt_test::templated_ctor_ == 1);
            REQUIRE(rtt_test::move_ctor_ == 1);

            REQUIRE(rtt_test::move_operator_ == 0);
        }
        REQUIRE(rtt_test::dtor_ == 3);
        rtt_test::clear();
    }

#endif

    SECTION("rtt created from an empty set of elements is empty") {
        const auto t = rtt{};
        REQUIRE(t.empty());
    }

    SECTION("The size of a rtt created from an empty set of elements is zero ") {
        const auto t = rtt{};
        REQUIRE(t.size() == 0);
    }

    SECTION("The volume of a rtt created from an empty set of elements is zero ") {
        const auto t = rtt{};
        REQUIRE(t.volume() == 0);
    }

    SECTION("The capacity of a rtt created from an empty set of elements is zero ") {
        const auto t = rtt{};
        REQUIRE(t.capacity() == 0);
    }

    SECTION("rtt created from a non-empty set of elements is not empty ") {
        const auto t = rtt(nullptr, 42, 3.14);
        REQUIRE(not t.empty());
    }

    SECTION("The size of a rtt created from a non-empty set of elements is equal to the number of these elements ") {
        const auto t = rtt(nullptr, std::string("123"), 42, 2.71, false);
        REQUIRE(t.size() == 4);
    }

    SECTION("The volume of a rtt created from a set of elements is equal to the volume of a rtt created by sequential insertion of the same elements into the end of an empty rtt. ") {
        const auto constructed = rtt(nullptr, 42, 3.14, std::string("123"), true);

        auto pushed_back = rtt{nullptr, 42, 3.14, std::string("123"), true};

        REQUIRE(constructed.volume() == pushed_back.volume());
    }

    SECTION("The capacity of a recreation center created from a set of elements is greater than or equal to the volume of this recreation center ") {
        const auto t = rtt(nullptr, std::vector<std::size_t>{}, 42, X{}, 3.14);
        REQUIRE(t.capacity() >= t.volume());
    }

    SECTION("The size of the rtt is equal to the number of elements it contains") {
        const auto some_vector = std::vector<std::size_t>{};
        const auto some_integer = 42;
        const auto some_struct = X{};
        const auto some_floating = 3.14;

        rtt t(nullptr, some_vector, some_integer, some_struct, some_floating);
        REQUIRE(t.size() == 4);
    }

    SECTION("rtt can be created from an arbitrary set of objects ") {
        rtt t(nullptr, std::string("123"), 42, true, X{17, std::vector<int>{1, 2, 3}});
        REQUIRE(t.size() == 4);
        REQUIRE(t.get<std::string>(0) == std::string("123"));
        REQUIRE(t.get<int>(1) == 42);
        REQUIRE(t.get<bool>(2) == true);
        REQUIRE(t.get<X>(3).a == 17);
        REQUIRE(t.get<X>(3).b == std::vector<int>{1, 2, 3});
    }

    SECTION("Arbitrary objects can be inserted at the end of the rtt") {
        rtt t(nullptr, X{17, std::vector<int>{1, 2, 3}}, true, std::string("123"));

        REQUIRE(t.size() == 3);
        REQUIRE(t.get<X>(0).a == 17);
        REQUIRE(t.get<X>(0).b == std::vector<int>{1, 2, 3});
        REQUIRE(t.get<bool>(1) == true);
        REQUIRE(t.get<std::string>(2) == std::string("123"));
    }

    SECTION("Vectors of int8_t") {
        rtt t(nullptr,
              std::vector<int8_t>{1, 2, 3, 4, 5, 6, 7, 8},
              std::vector<int8_t>{1, 2, 3, 4, 5, 6, 7, 8},
              std::vector<int8_t>{1, 2, 3, 4, 5, 6, 7, 8},
              std::vector<int8_t>{1, 2, 3, 4, 5, 6, 7, 8},
              std::vector<int8_t>{1, 2, 3, 4, 5, 6, 7, 8});

        REQUIRE(t.size() == 5);
        REQUIRE(t.get<std::vector<int8_t>>(0) == std::vector<int8_t>{1, 2, 3, 4, 5, 6, 7, 8});
        REQUIRE(t.get<std::vector<int8_t>>(1) == std::vector<int8_t>{1, 2, 3, 4, 5, 6, 7, 8});
        REQUIRE(t.get<std::vector<int8_t>>(2) == std::vector<int8_t>{1, 2, 3, 4, 5, 6, 7, 8});
        REQUIRE(t.get<std::vector<int8_t>>(3) == std::vector<int8_t>{1, 2, 3, 4, 5, 6, 7, 8});
        REQUIRE(t.get<std::vector<int8_t>>(4) == std::vector<int8_t>{1, 2, 3, 4, 5, 6, 7, 8});
    }

#if 0
#ifndef __EXCEPTIONS_DISABLE__

    SECTION("The exception thrown by the inserted object does not result in leaks ") {
        REQUIRE(kamikaze::instances_count == 0);
        REQUIRE_THROWS_AS(rtt(nullptr, kamikaze{}, kamikaze{}, kamikaze{}), std::runtime_error);
        REQUIRE(kamikaze::instances_count == 0);
    }

    /*SECTION("Exception when moving internal objects does not result in leaks") {
        const throw_on_move thrower{};

        REQUIRE(dummy::instances_count == 0);
        {
            rtt t;
            t.push_back(dummy{}); // non implemented method
            t.push_back(thrower); // non implemented method
            REQUIRE(dummy::instances_count == 1);

            REQUIRE_THROWS_AS(t.reserve(t.capacity() + 1), std::runtime_error);
            REQUIRE(dummy::instances_count == 1);
        }
        REQUIRE(dummy::instances_count == 0);
    }*/
    /*
duckstax/actor-zeta/test/rtt/main.cpp:262:36:   required from here
duckstax/actor-zeta/header/actor-zeta/detail/rtt_management.hpp:55:24: error: no matching function for call to ‘copy<std::unique_ptr<int, std::default_delete<int> > >(const void*&, void*&)’
   55 |                 copy<T>(source, destination);
duckstax/actor-zeta/header/actor-zeta/detail/rtt_management.hpp:20:10: note: candidate: ‘template<class T> std::enable_if_t<std::is_copy_constructible<_Tp>::value> actor_zeta::detail::management::copy(const void*, void*)’
   20 |     auto copy(const void* source, void* destination)
    SECTION("Throws an exception when trying to copy a non-copyable object") {
        std::unique_ptr<int> uptr(new int(5));
        rtt initial(std::move(uptr));
        rtt copied;
        CHECK_THROWS_AS(copied = initial, std::runtime_error);
    }

    SECTION("Exception when copying objects stored in the rtt does not lead to leaks") {
        REQUIRE(dummy::instances_count == 0);
        {
            std::unique_ptr<int> uptr(new int(17));
            rtt t(dummy{}, std::move(uptr), dummy{});
            REQUIRE(dummy::instances_count == 2);
            rtt copied;
            REQUIRE_THROWS_AS(copied = t, std::runtime_error);
            REQUIRE(dummy::instances_count == 2);
        }
        REQUIRE(dummy::instances_count == 0);
    }

    SECTION("Exception during copy assignment of objects stored in the rtt does not lead to leaks") {
        REQUIRE(dummy::instances_count == 0);
        {
            std::unique_ptr<int> uptr(new int(17));
            rtt t(dummy{}, std::move(uptr));
            rtt copy(dummy{});
            REQUIRE(dummy::instances_count == 2);

            REQUIRE_THROWS_AS(copy = t, std::runtime_error);
        }
        REQUIRE(dummy::instances_count == 0);
    }*/

#endif
#endif

    SECTION("Objects stored in the rtt can be changed externally ") {
        rtt t(nullptr, true, std::vector<char>{'a', 'b', 'c'}, 10);

        auto& vector = t.get<std::vector<char>>(1);

        vector.push_back('q');
        REQUIRE(t.get<std::vector<char>>(1) == std::vector<char>{'a', 'b', 'c', 'q'});

        vector.clear();
        REQUIRE(t.get<std::vector<char>>(1).empty());
    }

    // MOVE RTT

    SECTION("The rtt transfer constructor transfers elements of one rtt into another, rather than inserting one rtt inside another rtt") {
        rtt moved(nullptr, 2.71, 3, false);
        rtt constructed(std::move(moved));
        REQUIRE(constructed.size() == 3);
    }

    SECTION("rtt may contain rtt") {
        rtt t(nullptr, rtt(nullptr, 1, 2), rtt(nullptr, 3, 4));
        REQUIRE(t.size() == 2);
    }

    SECTION("Objects stored in the rtt are transferred when transferring the rtt") {
        REQUIRE(dummy::instances_count == 0);
        {
            rtt one(nullptr, dummy{});
            REQUIRE(dummy::instances_count == 1);

            rtt another(std::move(one));
            REQUIRE(dummy::instances_count == 1);

            one = std::move(another);
            REQUIRE(dummy::instances_count == 1);
        }
        REQUIRE(dummy::instances_count == 0);
    }

    SECTION("Objects stored in the rtt are destroyed during transfer assignment") {
        REQUIRE(dummy::instances_count == 0);
        rtt initial(nullptr, dummy{});
        REQUIRE(dummy::instances_count == 1);

        rtt other(nullptr, 1, 3.14, true);
        initial = std::move(other);
        REQUIRE(dummy::instances_count == 0);
    }

    SECTION("The transfer constructor leaves the portable rtt instance empty") {
        rtt source(nullptr, 13, 3.14, true);
        rtt destination(std::move(source));
        REQUIRE(source.empty());
    }

    SECTION("The takeover assignment operator leaves the portable copy of the rtt empty") {
        rtt source(nullptr, std::string("move me"));
        rtt destination(nullptr, std::string("some items here"));
        destination = std::move(source);
        REQUIRE(source.empty());
    }

    SECTION("The size of the rtt source after transfer is equal to zero") {
        rtt source(nullptr, 13, 3.14, true);
        rtt destination(std::move(source));
        REQUIRE(source.size() == 0);
    }

    SECTION("The size of the rtt-source after carrying takeover assignment is equal to zero ") {
        rtt source(nullptr, std::string("move me"));
        rtt destination(nullptr, std::string("some items here"));
        destination = std::move(source);
        REQUIRE(source.size() == 0);
    }

    SECTION("The volume of the rtt source after transfer is equal to zero") {
        rtt source(nullptr, 13, 3.14, true);
        rtt destination(nullptr, std::move(source));
        REQUIRE(source.volume() == 0);
    }

    SECTION("The volume of the rtt-source after carrying takeover assignment is equal to zero") {
        rtt source(nullptr, std::string("move me"));
        rtt destination(nullptr, std::string("some items here"));
        destination = std::move(source);
        REQUIRE(source.volume() == 0);
    }

    SECTION("Transferring rtt assignment does not lead to leaks") {
        REQUIRE(dummy::instances_count == 0);
        {
            rtt t(nullptr, dummy{});
            REQUIRE(dummy::instances_count == 1);

            rtt copy(nullptr, dummy{});
            REQUIRE(dummy::instances_count == 2);

            copy = std::move(t);
        }
        REQUIRE(dummy::instances_count == 0);
    }

    SECTION("movable only objects"){
        movable_only mv_only;
        auto t = rtt(nullptr, std::move(mv_only));
        REQUIRE_FALSE(mv_only.alive);
        REQUIRE(t.get<movable_only>(0).alive);
    }

    SECTION("actor_zeta::detail::get custom uptr instances_count check") {
        using type_t = dummy;
        using type_ptr = std::unique_ptr<type_t>;
        type_t::created_times = 0;
        type_t::destroyed_times = 0;

        auto f = [](
                     UNUSED type_ptr&& up1,
                     UNUSED const int16_t* p2) {};
        using call_trait = actor_zeta::type_traits::get_callable_trait_t<actor_zeta::type_traits::remove_reference_t<decltype(f)>>;
        using args_type_list = typename call_trait::args_types;

        REQUIRE(type_t::instances_count == 0);
        type_ptr uptr = type_ptr(new type_t());
        REQUIRE(type_t::instances_count == 1);

        REQUIRE(type_t::created_times == 1);
        REQUIRE(type_t::destroyed_times == 0);

        int16_t i16 = 16;
        int16_t* p_i16 = &i16;

        { // scoped in
            rtt r(nullptr, std::move(uptr), p_i16);
            REQUIRE(type_t::instances_count == 1);
            REQUIRE(type_t::created_times == 1);
            REQUIRE(type_t::destroyed_times == 0);
            REQUIRE_FALSE(uptr);

            REQUIRE(actor_zeta::detail::get<1, args_type_list>(r) == p_i16);

            REQUIRE(type_t::instances_count == 1);
            REQUIRE(type_t::created_times == 1);
            REQUIRE(type_t::destroyed_times == 0);

            REQUIRE(actor_zeta::detail::get<0, args_type_list>(r)->alive == true);
            REQUIRE(type_t::instances_count == 1);
            REQUIRE(type_t::created_times == 1);
            REQUIRE(type_t::destroyed_times == 0);
            REQUIRE(*actor_zeta::detail::get<1, args_type_list>(r) == i16);

            REQUIRE(std::is_same<decltype(actor_zeta::detail::get<0, args_type_list>(r)), type_ptr&& >::value);
            REQUIRE(std::is_same<decltype(actor_zeta::detail::get<1, args_type_list>(r)), const int16_t*>::value);
        } // scoped out

        REQUIRE(type_t::instances_count == 0);
        REQUIRE(type_t::created_times == 1);
        REQUIRE(type_t::destroyed_times == 1);
    }

    // REFERENCES

    SECTION("Knows how to give references to immutable values by their indentation in the rtt") {
        const rtt t(nullptr, std::string("123"), 42, true);
        auto int_offset = t.offset(1);
        REQUIRE(t.get_by_offset<int>(int_offset) == 42);
    }

    SECTION("Knows how to give links to mutable values by their indentation in rtt") {
        auto old_value = 42;
        auto new_value = 99;

        rtt t(nullptr, std::string("123"), old_value, true);
        auto int_offset = t.offset(1);

        REQUIRE(t.get<int>(1) == old_value);
        t.get_by_offset<int>(int_offset) = new_value;

        REQUIRE(t.get<int>(1) == new_value);
    }

    SECTION("swap") {
        auto t = rtt(nullptr, 1, 3.14);
        auto u = rtt(nullptr, std::string("qwe"), true);

        actor_zeta::detail::swap(t, u);
    }
}

#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta/mailbox/make_message.hpp>
#include <actor-zeta.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace actor_zeta;

struct test_session_t {
    std::string id;
    test_session_t() = default;
    explicit test_session_t(std::string s) : id(std::move(s)) {}
};

struct test_request_t {
    int value;
    std::string name;
    test_request_t() = default;
    test_request_t(int v, std::string n) : value(v), name(std::move(n)) {}
};

struct test_response_t {
    bool success;
    std::string message;
    test_response_t() = default;
    test_response_t(bool s, std::string m) : success(s), message(std::move(m)) {}
};

struct abstract_type_t {
    virtual ~abstract_type_t() = default;
    virtual void foo() = 0;
};

TEST_CASE("is_valid_rtt_type: basic types") {
    static_assert(detail::is_valid_rtt_type_v<int>, "int");
    static_assert(detail::is_valid_rtt_type_v<long>, "long");
    static_assert(detail::is_valid_rtt_type_v<unsigned>, "unsigned");
    static_assert(detail::is_valid_rtt_type_v<char>, "char");
    static_assert(detail::is_valid_rtt_type_v<bool>, "bool");

    static_assert(detail::is_valid_rtt_type_v<float>, "float");
    static_assert(detail::is_valid_rtt_type_v<double>, "double");

    REQUIRE(true);
}

TEST_CASE("is_valid_rtt_type: standard library types") {
    static_assert(detail::is_valid_rtt_type_v<std::string>, "string");

    static_assert(detail::is_valid_rtt_type_v<std::vector<int>>, "vector<int>");
    static_assert(detail::is_valid_rtt_type_v<std::vector<std::string>>, "vector<string>");

    static_assert(detail::is_valid_rtt_type_v<std::unique_ptr<int>>, "unique_ptr<int>");
    static_assert(detail::is_valid_rtt_type_v<std::shared_ptr<int>>, "shared_ptr<int>");

    REQUIRE(true);
}

TEST_CASE("is_valid_rtt_type: custom types") {
    static_assert(detail::is_valid_rtt_type_v<test_session_t>, "test_session_t");
    static_assert(detail::is_valid_rtt_type_v<test_request_t>, "test_request_t");
    static_assert(detail::is_valid_rtt_type_v<test_response_t>, "test_response_t");

    REQUIRE(true);
}

TEST_CASE("is_valid_rtt_type: invalid types") {
    static_assert(!detail::is_valid_rtt_type_v<abstract_type_t>, "abstract type should be invalid");

    static_assert(!detail::is_valid_rtt_type_v<int&>, "int& should be invalid");
    static_assert(!detail::is_valid_rtt_type_v<std::string&>, "string& should be invalid");

    REQUIRE(true);
}

TEST_CASE("is_convertible_arg: same types") {
    static_assert(detail::is_convertible_arg_v<int, int>, "int -> int");
    static_assert(detail::is_convertible_arg_v<std::string, std::string>, "string -> string");
    static_assert(detail::is_convertible_arg_v<test_session_t, test_session_t>, "session -> session");

    REQUIRE(true);
}

TEST_CASE("is_convertible_arg: const ref to value") {
    static_assert(detail::is_convertible_arg_v<const int&, int>, "const int& <- int");
    static_assert(detail::is_convertible_arg_v<const std::string&, std::string>, "const string& <- string");
    static_assert(detail::is_convertible_arg_v<const test_session_t&, test_session_t>, "const session& <- session");

    REQUIRE(true);
}

TEST_CASE("is_convertible_arg: value to const ref") {
    static_assert(detail::is_convertible_arg_v<int, const int&>, "int <- const int&");
    static_assert(detail::is_convertible_arg_v<std::string, const std::string&>, "string <- const string&");

    REQUIRE(true);
}

TEST_CASE("is_convertible_arg: rvalue references") {
    static_assert(detail::is_convertible_arg_v<std::string, std::string&&>, "string <- string&&");
    static_assert(detail::is_convertible_arg_v<std::string&&, std::string>, "string&& <- string");

    REQUIRE(true);
}

TEST_CASE("is_convertible_arg: numeric conversions") {
    static_assert(detail::is_convertible_arg_v<int, long>, "int <- long");
    static_assert(detail::is_convertible_arg_v<long, int>, "long <- int");
    static_assert(detail::is_convertible_arg_v<float, double>, "float <- double");
    static_assert(detail::is_convertible_arg_v<double, float>, "double <- float");

    REQUIRE(true);
}

TEST_CASE("is_convertible_arg: incompatible types") {
    static_assert(!detail::is_convertible_arg_v<int, std::string>, "int !<- string");
    static_assert(!detail::is_convertible_arg_v<std::string, int>, "string !<- int");
    static_assert(!detail::is_convertible_arg_v<test_session_t, test_request_t>, "session !<- request");

    REQUIRE(true);
}

TEST_CASE("args_compatible: empty lists") {
    using type_traits::type_list;

    static_assert(detail::args_compatible_v<type_list<>, type_list<>>, "empty lists");

    REQUIRE(true);
}

TEST_CASE("args_compatible: single argument") {
    using type_traits::type_list;

    static_assert(detail::args_compatible_v<type_list<int>, type_list<int>>, "int");
    static_assert(detail::args_compatible_v<type_list<const std::string&>, type_list<std::string>>,
                  "const string& <- string");

    REQUIRE(true);
}

TEST_CASE("args_compatible: multiple arguments") {
    using type_traits::type_list;

    static_assert(
        detail::args_compatible_v<
            type_list<int, std::string, double>,
            type_list<int, std::string, double>
        >,
        "int, string, double"
    );

    static_assert(
        detail::args_compatible_v<
            type_list<const int&, const std::string&>,
            type_list<int, std::string>
        >,
        "const refs <- values"
    );

    static_assert(
        detail::args_compatible_v<
            type_list<const test_session_t&, test_request_t>,
            type_list<test_session_t, test_request_t>
        >,
        "session, request"
    );

    REQUIRE(true);
}

TEST_CASE("args_compatible: mismatched count") {
    using type_traits::type_list;

    static_assert(!detail::args_compatible_v<type_list<int>, type_list<>>, "1 vs 0");
    static_assert(!detail::args_compatible_v<type_list<>, type_list<int>>, "0 vs 1");
    static_assert(!detail::args_compatible_v<type_list<int, int>, type_list<int>>, "2 vs 1");

    REQUIRE(true);
}

TEST_CASE("args_compatible: mismatched types") {
    using type_traits::type_list;

    static_assert(!detail::args_compatible_v<type_list<int>, type_list<std::string>>, "int vs string");
    static_assert(
        !detail::args_compatible_v<
            type_list<int, std::string>,
            type_list<std::string, int>
        >,
        "wrong order"
    );

    REQUIRE(true);
}

TEST_CASE("all_args_storable: basic types") {
    static_assert(detail::all_args_storable_v<int>, "int");
    static_assert(detail::all_args_storable_v<int, double>, "int, double");
    static_assert(detail::all_args_storable_v<int, double, std::string>, "int, double, string");

    REQUIRE(true);
}

TEST_CASE("all_args_storable: custom types") {
    static_assert(detail::all_args_storable_v<test_session_t>, "session");
    static_assert(detail::all_args_storable_v<test_session_t, test_request_t>, "session, request");
    static_assert(detail::all_args_storable_v<test_session_t, test_request_t, test_response_t>,
                  "session, request, response");

    REQUIRE(true);
}

TEST_CASE("all_args_storable: containers") {
    static_assert(detail::all_args_storable_v<std::vector<int>>, "vector<int>");
    static_assert(detail::all_args_storable_v<std::vector<std::string>, std::string>, "vector<string>, string");

    REQUIRE(true);
}

TEST_CASE("all_valid_rtt_types: basic types") {
    static_assert(detail::all_valid_rtt_types_v<int>, "int");
    static_assert(detail::all_valid_rtt_types_v<int, double, std::string>, "int, double, string");

    REQUIRE(true);
}

TEST_CASE("all_valid_rtt_types: custom types") {
    static_assert(detail::all_valid_rtt_types_v<test_session_t, test_request_t, test_response_t>,
                  "session, request, response");

    REQUIRE(true);
}

TEST_CASE("all_valid_rtt_types: mixed valid types") {
    static_assert(
        detail::all_valid_rtt_types_v<
            int,
            std::string,
            std::vector<int>,
            test_session_t,
            std::unique_ptr<int>
        >,
        "mixed valid types"
    );

    REQUIRE(true);
}

TEST_CASE("integration: method signature validation") {
    using type_traits::type_list;

    using method_args = type_list<const test_session_t&, std::string, int>;

    using caller_args = type_list<test_session_t, std::string, int>;

    static_assert(detail::args_compatible_v<method_args, caller_args>,
                  "method signature should accept caller args");

    static_assert(detail::all_args_storable_v<test_session_t, std::string, int>,
                  "all args should be storable");

    REQUIRE(true);
}

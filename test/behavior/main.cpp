#define CATCH_CONFIG_MAIN // This tells Catch to provide a main() - only do this in one cpp file
#include <catch2/catch.hpp>

#include <actor-zeta.hpp>
#include <actor-zeta/core.hpp>


struct foo_t {
    size_t d1;
    size_t d2;
    size_t d3;
};

static int free_function_args_0_counter = 0;
static int free_function_args_1_counter = 0;
static int free_function_args_1_ptr_counter = 0;
static int free_function_args_n_counter = 0;

void free_function_args_0() {
    free_function_args_0_counter = 1;
}

void free_function_args_1(int arg_0) {
    free_function_args_1_counter = 1;
    REQUIRE(arg_0 == 42);
}

void free_function_args_ptr(foo_t* arg_ptr) {
    free_function_args_1_ptr_counter = 1;
    REQUIRE(arg_ptr->d1 == 42);
    REQUIRE(arg_ptr->d2 == 420);
    REQUIRE(arg_ptr->d3 == 4200);
}

void free_function_args_n(const std::string& arg_0, int arg_1) {
    free_function_args_n_counter = 1;
    REQUIRE(arg_0 == "42");
    REQUIRE(arg_1 == 42);
}

enum class size_args : uint64_t {
    args_0 = 0x00,
    args_1,
    args_n
};

class class_method final {
public:
    int args_0_counter{0};
    int args_1_counter{0};
    int args_1_ptr_counter{0};
    int args_n_counter{0};

    class_method() = default;
    void class_method_args_0() {
        args_0_counter = 1;
    }

    void class_method_args_1(int arg_0) {
        args_1_counter = 1;
        REQUIRE(arg_0 == 42);
    }

    void class_method_args_ptr(foo_t* arg_ptr) {
        args_1_ptr_counter = 1;
        REQUIRE(arg_ptr->d1 == 42);
        REQUIRE(arg_ptr->d2 == 420);
        REQUIRE(arg_ptr->d3 == 4200);
    }

    void class_method_args_n(const std::string& arg_0, int arg_1) {
        args_n_counter = 1;
        REQUIRE(arg_0 == "42");
        REQUIRE(arg_1 == 42);
    }
};

TEST_CASE("behavior") {
    SECTION("free function arg 0") {
        actor_zeta::pmr::memory_resource* resource = actor_zeta::pmr::get_default_resource();
        actor_zeta::behavior_t function = actor_zeta::make_behavior(resource,  &free_function_args_0);
        auto msg = actor_zeta::make_message(resource,actor_zeta::address_t::empty_address(), size_args::args_0);
        function(msg.get());
        REQUIRE(free_function_args_0_counter == 1);
    }

    SECTION("free function arg 1") {
        actor_zeta::pmr::memory_resource* resource = actor_zeta::pmr::get_default_resource();
        actor_zeta::behavior_t function = actor_zeta::make_behavior(resource,  &free_function_args_1);
        auto msg = actor_zeta::make_message(resource,actor_zeta::address_t::empty_address(), size_args::args_1, 42);
        function(msg.get());
        REQUIRE(free_function_args_1_counter == 1);
    }

    SECTION("free function arg 1 ptr") {
        actor_zeta::pmr::memory_resource* resource = actor_zeta::pmr::get_default_resource();
        actor_zeta::behavior_t function = actor_zeta::make_behavior(resource, &free_function_args_ptr);
        foo_t foo{42, 420, 4200};
        auto msg = actor_zeta::make_message(resource,actor_zeta::address_t::empty_address(), size_args::args_1, &foo);
        function(msg.get());
        REQUIRE(free_function_args_1_ptr_counter == 1);
    }

    SECTION("free function args n") {
        actor_zeta::pmr::memory_resource* resource = actor_zeta::pmr::get_default_resource();
        actor_zeta::behavior_t function = actor_zeta::make_behavior(resource,  &free_function_args_n);
        auto msg = actor_zeta::make_message(resource,actor_zeta::address_t::empty_address(), size_args::args_n, std::string("42"), 42);
        function(msg.get());
        REQUIRE(free_function_args_n_counter == 1);
    }

    SECTION("v2 free function arg 0") {
        actor_zeta::pmr::memory_resource* resource = actor_zeta::pmr::get_default_resource();
        actor_zeta::behavior_t function = actor_zeta::make_behavior(resource,  free_function_args_0);
        auto msg = actor_zeta::make_message(resource,actor_zeta::address_t::empty_address(), size_args::args_0);
        function(msg.get());
        REQUIRE(free_function_args_0_counter == 1);
    }

    SECTION("v2 free function arg 1") {
        actor_zeta::pmr::memory_resource* resource = actor_zeta::pmr::get_default_resource();
        actor_zeta::behavior_t function = actor_zeta::make_behavior(resource,  free_function_args_1);
        auto msg = actor_zeta::make_message(resource,actor_zeta::address_t::empty_address(), size_args::args_1, 42);
        function(msg.get());
        REQUIRE(free_function_args_1_counter == 1);
    }

    SECTION("v2 free function arg 1 ptr") {
        actor_zeta::pmr::memory_resource* resource = actor_zeta::pmr::get_default_resource();
        actor_zeta::behavior_t function = actor_zeta::make_behavior(resource,  free_function_args_ptr);
        foo_t foo{42, 420, 4200};
        auto msg = actor_zeta::make_message(resource,actor_zeta::address_t::empty_address(), size_args::args_1, &foo);
        function(msg.get());
        REQUIRE(free_function_args_1_ptr_counter == 1);
    }

    SECTION("v2 free function args n") {
        actor_zeta::pmr::memory_resource* resource = actor_zeta::pmr::get_default_resource();
        actor_zeta::behavior_t function= actor_zeta::make_behavior(resource,  free_function_args_n);
        auto msg = actor_zeta::make_message(resource,actor_zeta::address_t::empty_address(), size_args::args_n, std::string("42"), 42);
        function(msg.get());
        REQUIRE(free_function_args_n_counter == 1);
    }

    SECTION("lambda arg 0") {
        actor_zeta::pmr::memory_resource* resource = actor_zeta::pmr::get_default_resource();
        int args_checker = 0;
        actor_zeta::behavior_t free_function = actor_zeta::make_behavior(resource,resource,  [&args_checker]() {
            args_checker = 1;
        });
        auto msg = actor_zeta::make_message(resource,actor_zeta::address_t::empty_address(), size_args::args_0);
        free_function(msg.get());
        REQUIRE(args_checker == 1);
    }

    SECTION("lambda arg 1") {
        actor_zeta::pmr::memory_resource* resource = actor_zeta::pmr::get_default_resource();
        int args_checker = 0;
        actor_zeta::behavior_t free_function = actor_zeta::make_behavior(resource,resource,  [&args_checker]() {
            args_checker = 1;
        });
        auto msg = actor_zeta::make_message(resource,actor_zeta::address_t::empty_address(), size_args::args_1, 42);
        free_function(msg.get());
        REQUIRE(args_checker == 1);
    }


    SECTION("lambda arg ptr 1") {
        actor_zeta::pmr::memory_resource* resource = actor_zeta::pmr::get_default_resource();
        int args_checker = 0;
        actor_zeta::behavior_t standalone_function = actor_zeta::make_behavior(resource, [&args_checker]() {
            args_checker = 1;
        });
        foo_t foo{42, 420, 4200};
        auto msg = actor_zeta::make_message(resource,actor_zeta::address_t::empty_address(), size_args::args_1, &foo);
        standalone_function(msg.get());
        REQUIRE(args_checker == 1);
    }

    SECTION("lambda args n") {
        actor_zeta::pmr::memory_resource* resource = actor_zeta::pmr::get_default_resource();
        int args_checker = 0;
        actor_zeta::behavior_t free_function = actor_zeta::make_behavior(resource, [&args_checker]() {
            args_checker = 1;
        });
        auto msg = actor_zeta::make_message(resource,actor_zeta::address_t::empty_address(), size_args::args_n, std::string("42"), 42);
        free_function(msg.get());
        REQUIRE(args_checker == 1);
    }

    SECTION("class method arg 0") {
        actor_zeta::pmr::memory_resource* resource = actor_zeta::pmr::get_default_resource();
        class_method class_method;
        actor_zeta::behavior_t function = actor_zeta::make_behavior(resource, &class_method, &class_method::class_method_args_0);
        auto msg = actor_zeta::make_message(resource,actor_zeta::address_t::empty_address(), size_args::args_0);
        function(msg.get());
        REQUIRE(class_method.args_0_counter == 1);
    }

    SECTION("class method arg 1") {
        actor_zeta::pmr::memory_resource* resource = actor_zeta::pmr::get_default_resource();
        class_method class_method;
        actor_zeta::behavior_t function = actor_zeta::make_behavior(resource, &class_method, &class_method::class_method_args_1);
        auto msg = actor_zeta::make_message(resource,actor_zeta::address_t::empty_address(), size_args::args_1, 42);
        function(msg.get());
        REQUIRE(class_method.args_1_counter == 1);
    }

    SECTION("class method ptr arg 1") {
        actor_zeta::pmr::memory_resource* resource = actor_zeta::pmr::get_default_resource();
        class_method class_method;
        actor_zeta::behavior_t function = actor_zeta::make_behavior(resource, &class_method, &class_method::class_method_args_ptr);
        foo_t foo{42, 420, 4200};
        auto msg = actor_zeta::make_message(resource,actor_zeta::address_t::empty_address(), size_args::args_1, &foo);
        function(msg.get());
        REQUIRE(class_method.args_1_ptr_counter == 1);
    }

    SECTION("class method args n") {
        actor_zeta::pmr::memory_resource* resource = actor_zeta::pmr::get_default_resource();
        class_method class_method;
        actor_zeta::behavior_t function = actor_zeta::make_behavior(resource, &class_method, &class_method::class_method_args_n);
        auto msg = actor_zeta::make_message(resource,actor_zeta::address_t::empty_address(), size_args::args_n, std::string("42"), 42);
        function(msg.get());
        REQUIRE(class_method.args_n_counter == 1);
    }
}
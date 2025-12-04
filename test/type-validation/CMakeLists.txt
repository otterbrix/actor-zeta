project(tests_type_validation)

find_package(Catch2 REQUIRED)

add_executable(
        tests_type_validation
        main.cpp
)

target_link_libraries(
        tests_type_validation
        PRIVATE
        actor-zeta
        Catch2::Catch2
)

add_test(
        NAME tests_type_validation
        COMMAND tests_type_validation
)
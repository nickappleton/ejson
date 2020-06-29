add_executable(ejson_tests ejson_tests.c json_simple_load.c json_simple_load.h)
target_link_libraries(ejson_tests ejson)
add_test(ejson_tests ejson_tests)

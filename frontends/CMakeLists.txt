add_executable(ejson_expand ejson_expand.c)
target_link_libraries(ejson_expand ejson)
install(TARGETS ejson_expand RUNTIME DESTINATION "bin$<$<NOT:$<CONFIG:Release>>:/$<CONFIG>>")

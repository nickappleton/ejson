cmake_minimum_required(VERSION 3.0 FATAL_ERROR)

if (SUPERBUILD)
  include(ExternalProject)

  if (IS_ABSOLUTE CMAKE_INSTALL_PREFIX)
    set(ABSOLUTE_INSTALL_PREFIX_PATH "${CMAKE_INSTALL_PREFIX}")
  else()
    set(ABSOLUTE_INSTALL_PREFIX_PATH "${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_PREFIX}")
  endif()

  if (NOT CMAKE_CONFIGURATION_TYPES)
    set(EJSON_BUILD_CONFIG "-DCMAKE_BUILD_TYPE=$<CONFIG>")
  else()
    # TODO Verify that this is not necessary
    #set(EJSON_BUILD_CONFIG "--config $<CONFIG>")
  endif()

  if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../cop" AND IS_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/../cop")
    ExternalProject_Add(
      cop
      UPDATE_COMMAND ""
      PATCH_COMMAND ""
      SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../cop"
      CMAKE_ARGS "-DCMAKE_INSTALL_PREFIX:PATH=${ABSOLUTE_INSTALL_PREFIX_PATH};${EJSON_BUILD_CONFIG}"
      BUILD_ALWAYS 1
      TEST_COMMAND ""
    )
  else()
    message(FATAL_ERROR "no cop")
  endif()

  ExternalProject_Add(
    ejson
    DEPENDS cop
    UPDATE_COMMAND ""
    PATCH_COMMAND ""
    SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}"
    CMAKE_ARGS "-DCMAKE_INSTALL_PREFIX:PATH=${ABSOLUTE_INSTALL_PREFIX_PATH};${EJSON_BUILD_CONFIG}"
    BUILD_ALWAYS 1
    TEST_COMMAND ""
  )

  return()
endif()

if (${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_CURRENT_LIST_DIR})
  if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "Setting CMAKE_BUILD_TYPE type to 'Release' as none was specified.")
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release")
  endif()
endif()

project(ejson VERSION 0.1.0 LANGUAGES C)

set(EJSON_PUBLIC_INCLUDES ejson.h ejson_iface.h json_iface_utils.h)

add_library(ejson STATIC
  src/ejson.c
  src/json_iface_utils.c
  src/parse_helpers.h
  ${EJSON_PUBLIC_INCLUDES})
set_property(TARGET ejson APPEND PROPERTY PUBLIC_HEADER ${EJSON_PUBLIC_INCLUDES})
set_property(TARGET ejson PROPERTY ARCHIVE_OUTPUT_DIRECTORY "$<$<NOT:$<CONFIG:Release>>:$<CONFIG>>")

if (x${CMAKE_C_COMPILER_ID} STREQUAL "xMSVC")
  set_property(TARGET ejson APPEND_STRING PROPERTY COMPILE_FLAGS " /W3")
elseif (x${CMAKE_C_COMPILER_ID} STREQUAL "xAppleClang" OR x${CMAKE_C_COMPILER_ID} STREQUAL "xGNU")
  set_property(TARGET ejson APPEND_STRING PROPERTY COMPILE_FLAGS " -Wall")
endif()

target_include_directories(ejson PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>")

find_package(cop CONFIG REQUIRED)
target_link_libraries(ejson cop)

add_subdirectory(frontends)

enable_testing()
add_subdirectory(tests)

include(CMakePackageConfigHelpers)

export(TARGETS ejson FILE "${PROJECT_BINARY_DIR}/cmake/ejson/ejsonTargets.cmake")

configure_package_config_file(cmake_support/ejsonConfig.cmake.in ${CMAKE_BINARY_DIR}/cmake/ejson/ejsonConfig.cmake
  INSTALL_DESTINATION "lib/cmake/ejson"
  NO_CHECK_REQUIRED_COMPONENTS_MACRO)
write_basic_package_version_file(${CMAKE_BINARY_DIR}/cmake/ejson/ejsonConfigVersion.cmake
  COMPATIBILITY SameMajorVersion)

install(TARGETS ejson
  EXPORT ejson_target
  RUNTIME DESTINATION "bin$<$<NOT:$<CONFIG:Release>>:/$<CONFIG>>"
  ARCHIVE DESTINATION "lib$<$<NOT:$<CONFIG:Release>>:/$<CONFIG>>"
  LIBRARY DESTINATION "lib$<$<NOT:$<CONFIG:Release>>:/$<CONFIG>>"
  PUBLIC_HEADER DESTINATION include/ejson
  INCLUDES DESTINATION include)
install(EXPORT ejson_target DESTINATION lib/cmake/ejson FILE "ejsonTargets.cmake")
install(FILES "${CMAKE_BINARY_DIR}/cmake/ejson/ejsonConfig.cmake"
              "${CMAKE_BINARY_DIR}/cmake/ejson/ejsonConfigVersion.cmake"
        DESTINATION "lib/cmake/ejson")
install(FILES LICENSE DESTINATION share/doc/ejson)
install(FILES README.md DESTINATION share/doc/ejson)

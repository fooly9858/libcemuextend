cmake_minimum_required(VERSION 3.20)

project(libcemuextend VERSION 2.0.0 LANGUAGES CXX)

option(CEMUEXTEND_BUILD_TESTS "Build libcemuextend native tests" ${PROJECT_IS_TOP_LEVEL})

add_library(cemuextend_wire INTERFACE)
add_library(cemuextend::wire ALIAS cemuextend_wire)
target_include_directories(cemuextend_wire INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(cemuextend_wire INTERFACE cxx_std_20)

add_library(cemuextend_guest STATIC src/guest2.cpp src/cemod_stubs.cpp)
add_library(cemuextend::guest ALIAS cemuextend_guest)
target_link_libraries(cemuextend_guest PUBLIC cemuextend_wire)
target_compile_features(cemuextend_guest PUBLIC cxx_std_20)

if(CEMUEXTEND_BUILD_TESTS)
    enable_testing()
    add_executable(cemuextend_wire_tests tests/wire_tests.cpp)
    target_link_libraries(cemuextend_wire_tests PRIVATE cemuextend_wire)
    add_test(NAME cemuextend_wire_tests COMMAND cemuextend_wire_tests)

    add_executable(cemuextend_guest_tests tests/guest2_tests.cpp)
    target_link_libraries(cemuextend_guest_tests PRIVATE cemuextend_guest)
    add_test(NAME cemuextend_guest_tests COMMAND cemuextend_guest_tests)
endif()

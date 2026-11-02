cmake_minimum_required(VERSION 3.20)
project(better_planner CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8 /W4)
endif()

add_library(bp_core STATIC
    src/core/store.cpp
    src/core/layout.cpp
    src/core/queries.cpp
)
target_include_directories(bp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(bp_api STATIC
    src/api/types.cpp
    src/api/registry.cpp
    src/api/json_io.cpp
    src/api/http_server.cpp
)
target_link_libraries(bp_api PUBLIC bp_core PRIVATE ws2_32)

add_executable(better_planner WIN32 src/ui/app.cpp)
target_link_libraries(better_planner PRIVATE bp_core bp_api d2d1 user32 gdi32)
target_compile_definitions(better_planner PRIVATE UNICODE _UNICODE NOMINMAX)

enable_testing()
add_executable(test_layout tests/test_layout.cpp)
target_link_libraries(test_layout PRIVATE bp_core bp_api)
add_test(NAME core_and_api COMMAND test_layout)

add_executable(test_api_http tests/test_api_http.cpp)
target_link_libraries(test_api_http PRIVATE bp_api ws2_32)
add_test(NAME api_http COMMAND test_api_http)

cmake_minimum_required(VERSION 3.16)
project(tcp-lb LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-O3 -march=native -DNDEBUG -g3)

add_library(tcp_lb_core OBJECT
    src/reactor/poller.cpp
    src/reactor/channel.cpp
    src/reactor/event_loop.cpp
    src/acceptor/acceptor.cpp
    src/proxy/tcp_connection.cpp
    src/common/ring_buffer.cpp
)

target_include_directories(tcp_lb_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(tcp_lb_core PRIVATE
    -Wall -Wextra -Wpedantic
)

find_package(Threads REQUIRED)

add_executable(echo_server src/echo_main.cpp)
target_link_libraries(echo_server PRIVATE tcp_lb_core Threads::Threads dl)

add_executable(tcp_proxy
    src/balancer/backend_pool.cpp
    src/balancer/balancer.cpp
    src/config/config.cpp
    src/health/health_checker.cpp
    src/proxy_main.cpp
    src/proxy/proxy_session.cpp
)
target_link_libraries(tcp_proxy PRIVATE tcp_lb_core Threads::Threads dl)

add_executable(tcpbench tests/tcpbench.c)

set_target_properties(echo_server tcp_proxy PROPERTIES LINK_FLAGS "-rdynamic")

set_target_properties(echo_server tcp_proxy tcpbench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
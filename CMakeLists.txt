# is an epoll reactor, so this targets any x86-64/Linux server. Build on the
# server directly, or via the provided Dockerfile from a Mac.

cmake_minimum_required(VERSION 3.16)
# C is enabled alongside C++ for the vendored picohttpparser (a C source).
project(castleplusplus LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# Single binary named `castle`. As milestones land (http/, tls/, supervisor/...)
add_executable(castle
  src/main.cpp
  src/net/socket.cpp
  src/net/event_loop.cpp
  src/net/listener.cpp
  src/net/echo_connection.cpp
  src/control/control_server.cpp
  src/config/service_config.cpp
  src/supervisor/supervisor.cpp
  src/http/router.cpp
  src/http/http_connection.cpp
  src/http/backend_connection.cpp
  src/net/transport.cpp
  src/tls/tls_context.cpp
  src/tls/tls_transport.cpp
  src/gateway/rate_limiter.cpp
  third_party/picohttpparser/picohttpparser.c
)

# `src` for our headers; project root so `third_party/...` includes resolve.
target_include_directories(castle PRIVATE src ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)  # M4: TLS termination
target_link_libraries(castle PRIVATE Threads::Threads OpenSSL::SSL OpenSSL::Crypto)

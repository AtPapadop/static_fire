cmake_minimum_required(VERSION 3.16)
project(ws_server C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# Place runtime binaries under the repository's ./bin directory.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")

find_package(Threads REQUIRED)
find_package(simple_ws REQUIRED)

add_executable(ws_server
  src/main.c
  src/app/config.c
  src/app/context.c
  src/can/can_bus.c
  src/control/command.c
  src/control/firing_sequence.c
  src/control/state.c
  src/control/wiggle.c
  src/util/heartbeat.c
  src/util/logger.c
  src/ws/ws_bridge.c
)

target_include_directories(ws_server PRIVATE include)

if(TARGET simple_ws::simple_ws_shared)
  target_link_libraries(ws_server PRIVATE simple_ws::simple_ws_shared)
else()
  target_link_libraries(ws_server PRIVATE simple_ws::simple_ws)
endif()
target_link_libraries(ws_server PRIVATE Threads::Threads)

target_compile_options(ws_server PRIVATE
  -Wall
  -Wextra
  -Wpedantic
)

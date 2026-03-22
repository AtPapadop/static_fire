cmake_minimum_required(VERSION 3.16)
project(ws_server C)

include(GNUInstallDirs)

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

# Keep runtime lookup for simple_ws after installation under /usr/local/bin.
set_target_properties(ws_server PROPERTIES
  INSTALL_RPATH "/usr/local/lib"
  INSTALL_RPATH_USE_LINK_PATH TRUE
)
option(WS_SERVER_INSTALL_SYSTEM_FILES "Install ws_server config and systemd unit under /etc" OFF)

install(TARGETS ws_server
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(WS_SERVER_INSTALL_SYSTEM_FILES)
  install(FILES config/ws_server.service.example
    DESTINATION /etc/systemd/system
    RENAME ws_server.service
  )

  # Match package-manager behavior: only create a default config when missing.
  install(CODE [[
    set(_destdir "$ENV{DESTDIR}")
    set(_cfg_dir "${_destdir}/etc/ws_server")
    set(_cfg_file "${_cfg_dir}/ws_server.conf")
    if(NOT EXISTS "${_cfg_file}")
      file(MAKE_DIRECTORY "${_cfg_dir}")
      file(INSTALL
        DESTINATION "/etc/ws_server"
        TYPE FILE
        FILES "${CMAKE_SOURCE_DIR}/config/ws_server.conf.example"
        RENAME "ws_server.conf"
      )
    endif()
  ]])
endif()

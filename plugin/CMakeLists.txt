# CMakeLists.txt — host build for the qnx-mqtt-gateway (optional convenience).
# The plain Makefiles (Makefile.host / ../test/run_tests.sh) are the
# canonical host build; this file exists for IDEs and for builders who
# prefer CMake. It builds exactly the same sources with the host stub IPC.
#
#   cmake -S . -B build/host-cmake && cmake --build build/host-cmake
#   ./build/host-cmake/test_gateway        # unit tests
#
# The verified Mosquitto headers in third-party/mosquitto/ are exposed under
# their real names (mosquitto_plugin.h etc.) via symlinks in the build
# dir, mirroring Makefile.host. Requires CMake 3.14+ (file(CREATE_LINK)).

cmake_minimum_required(VERSION 3.14)
project(qnx_mqtt_gateway C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC")
  add_compile_options(-Wall -Wextra -Werror)
endif()

set(REFDIR ${CMAKE_CURRENT_SOURCE_DIR}/../third-party/mosquitto)
set(INCDIR ${CMAKE_CURRENT_BINARY_DIR}/host-include)

file(MAKE_DIRECTORY ${INCDIR})
file(CREATE_LINK ${REFDIR}/upstream-mosquitto_plugin.h
     ${INCDIR}/mosquitto_plugin.h SYMBOLIC)
file(CREATE_LINK ${REFDIR}/upstream-mosquitto_broker.h
     ${INCDIR}/mosquitto_broker.h SYMBOLIC)
file(CREATE_LINK ${REFDIR}/upstream-mosquitto.h
     ${INCDIR}/mosquitto.h SYMBOLIC)

find_package(Threads REQUIRED)

# Broker plugin (host stub IPC; broker symbols resolve at load time).
add_library(qnx_bridge SHARED
  qnx_bridge.c
  qnx_ipc_host.c
  ../common/qnx_msg.c)
target_include_directories(qnx_bridge PRIVATE
  ${INCDIR} ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../common)
target_link_libraries(qnx_bridge PRIVATE Threads::Threads)

# Unit tests (broker functions stubbed in test_gateway.c).
add_executable(test_gateway
  ../test/test_gateway.c
  qnx_bridge.c
  qnx_ipc_host.c
  ../common/qnx_msg.c)
target_include_directories(test_gateway PRIVATE
  ${INCDIR} ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../common)
target_link_libraries(test_gateway PRIVATE Threads::Threads)

enable_testing()
add_test(NAME gateway_unit COMMAND test_gateway)

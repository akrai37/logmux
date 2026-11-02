cmake_minimum_required(VERSION 3.20)
project(logmux VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(LOGMUX_WERROR "Treat compiler warnings as errors" OFF)
option(LOGMUX_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(LOGMUX_BUILD_TESTS "Build the tests" ON)

if(LOGMUX_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=address,undefined)
endif()

# POSIX.1-2008 everywhere. macOS hides its BSD interfaces (kqueue among
# them) when only POSIX is requested, so they are enabled explicitly there.
add_compile_definitions(_POSIX_C_SOURCE=200809L)
if(APPLE)
  add_compile_definitions(_DARWIN_C_SOURCE)
endif()

add_library(logmux_warnings INTERFACE)
target_compile_options(logmux_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wdouble-promotion
  -Wstrict-prototypes -Wmissing-prototypes -Wimplicit-fallthrough -Wformat=2 -Wcast-qual -Wvla)
if(LOGMUX_WERROR)
  target_compile_options(logmux_warnings INTERFACE -Werror)
endif()

add_library(logmux_core
  src/collector.c
  src/poller.c)
target_include_directories(logmux_core PUBLIC include)
target_link_libraries(logmux_core PRIVATE logmux_warnings)

add_executable(logmux src/main.c)
target_link_libraries(logmux PRIVATE logmux_core logmux_warnings)
target_compile_definitions(logmux PRIVATE LOGMUX_VERSION="${PROJECT_VERSION}")

# Test child that writes predictable lines in hostile ways.
add_executable(logmux-child tools/child.c)
target_include_directories(logmux-child PRIVATE tools)
target_link_libraries(logmux-child PRIVATE logmux_warnings)

if(LOGMUX_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

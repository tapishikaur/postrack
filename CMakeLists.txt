cmake_minimum_required(VERSION 3.25) # 3.25: FetchContent SYSTEM
project(postrack
  VERSION 0.1.0
  DESCRIPTION "Position and P&L service with crash recovery"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(POSTRACK_BUILD_TESTS "Build the tests" ON)

# Warnings for this project's code only.
add_library(postrack_options INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(postrack_options INTERFACE -Wall -Wextra -Wpedantic -Wconversion)
endif()

# ── Dependencies ──────────────────────────────────────────────────────────────
# Boost: only the header-only Asio part is used. MSYS2: mingw-w64-x86_64-boost; Ubuntu: libboost-dev.
find_package(Boost 1.74 CONFIG REQUIRED)
find_package(Threads REQUIRED)

# PostgreSQL client library (libpq). MSYS2: mingw-w64-x86_64-postgresql; Ubuntu: libpq-dev.
find_package(PostgreSQL REQUIRED)

include(FetchContent)
# libpqxx, the official C++ client for PostgreSQL, pinned to an exact commit.
FetchContent_Declare(libpqxx
  GIT_REPOSITORY https://github.com/jtv/libpqxx.git
  GIT_TAG        9bfaf9cb5a73ac23f9b7a9dfd7ef069a6e2124fb # 7.10.1
  SYSTEM
)
set(SKIP_BUILD_TEST ON CACHE BOOL "" FORCE)
set(BUILD_DOC OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(libpqxx)

# ── The library: accounting, protocol, log, snapshots, service, test upstreams ─
add_library(postrack_core
  src/crc32c.cpp
  src/fill_log.cpp
  src/pg_snapshot_store.cpp
  src/position.cpp
  src/protocol.cpp
  src/service.cpp
  src/sim.cpp
  src/snapshot.cpp
  src/types.cpp
)
target_include_directories(postrack_core PUBLIC include)
target_link_libraries(postrack_core PUBLIC Boost::headers Threads::Threads pqxx PRIVATE postrack_options)
if(WIN32)
  target_link_libraries(postrack_core PUBLIC ws2_32 mswsock)
  target_compile_definitions(postrack_core PUBLIC _WIN32_WINNT=0x0A00) # Windows 10+ for Asio
endif()

# ── Programs ──────────────────────────────────────────────────────────────────
foreach(tool postrack fill_simulator price_replayer postrack_export postrack_bench)
  if(tool STREQUAL "postrack")
    add_executable(${tool} src/main.cpp)
  else()
    add_executable(${tool} tools/${tool}.cpp)
  endif()
  target_link_libraries(${tool} PRIVATE postrack_core postrack_options)
endforeach()

if(POSTRACK_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

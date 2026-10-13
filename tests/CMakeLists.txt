include(FetchContent)
FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        063de7e9578f82b369302001269680b4b1553359 # v1.18.0
  SYSTEM
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK   OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
include(GoogleTest)

add_executable(postrack_tests
  fill_log_test.cpp
  position_test.cpp
  protocol_test.cpp
  service_test.cpp
  snapshot_test.cpp
  types_test.cpp
)
target_link_libraries(postrack_tests PRIVATE postrack_core postrack_options GTest::gtest_main)

# PRE_TEST: list the tests when ctest runs, not at build time (the test program needs
# libpq's DLL on PATH, which the build scripts set up).
gtest_discover_tests(postrack_tests DISCOVERY_MODE PRE_TEST)

cmake_minimum_required(VERSION 3.20)

option(OGCCDB_GEN_BUILD_TESTS "Build the tests" ON)
# With vcpkg (manifest mode), GTest comes from the `tests` feature of vcpkg.json.
if(OGCCDB_GEN_BUILD_TESTS)
  list(APPEND VCPKG_MANIFEST_FEATURES tests)
endif()

project(ogccdb-gen VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# While ogccdb-core is 0.x, a minor release may change its API: 0.2 accepts 0.2.z only.
find_package(ogccdb-core 0.3 CONFIG REQUIRED)

add_executable(ogccdb-gen src/main.cpp)
target_link_libraries(ogccdb-gen PRIVATE ogccdb::core)
target_compile_options(ogccdb-gen PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
  $<$<CXX_COMPILER_ID:MSVC>:/W4>
)
install(TARGETS ogccdb-gen RUNTIME DESTINATION bin)

if(OGCCDB_GEN_BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
  add_executable(ogccdb_gen_tests tests/test_cli.cpp)
  # GDAL is used directly to build the test rasters.
  target_link_libraries(ogccdb_gen_tests PRIVATE ogccdb::core GDAL::GDAL GTest::gtest_main)
  add_dependencies(ogccdb_gen_tests ogccdb-gen)
  target_compile_definitions(ogccdb_gen_tests PRIVATE OGCCDB_GEN_EXE="$<TARGET_FILE:ogccdb-gen>")
  target_compile_options(ogccdb_gen_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
  )
  gtest_discover_tests(ogccdb_gen_tests)
endif()

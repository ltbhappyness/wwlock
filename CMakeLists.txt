cmake_minimum_required(VERSION 3.16)
project(wwlock C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# PIC static archive so a shared library can absorb these objects.
# The shared library is the wwlock package. Dependents link the static target.
add_library(wwlock STATIC src/wwlock.c src/wwlock_futex.c)
set_target_properties(wwlock PROPERTIES POSITION_INDEPENDENT_CODE ON OUTPUT_NAME wwlock)
target_include_directories(wwlock PUBLIC include)
target_compile_options(wwlock PRIVATE -Wall -Wextra -Werror)

add_library(wwlock_shared SHARED src/wwlock.c src/wwlock_futex.c)
set_target_properties(wwlock_shared PROPERTIES POSITION_INDEPENDENT_CODE ON OUTPUT_NAME wwlock)
target_include_directories(wwlock_shared PUBLIC include)
target_compile_options(wwlock_shared PRIVATE -Wall -Wextra -Werror)

if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    return()
endif()

add_library(wwlock_posix STATIC test/park_posix.c)
target_include_directories(wwlock_posix PUBLIC test include)
target_link_libraries(wwlock_posix PUBLIC wwlock)
target_compile_options(wwlock_posix PRIVATE -Wall -Wextra -Werror)

function(wwlock_bin name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE wwlock_posix pthread)
  target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
endfunction()

wwlock_bin(test_wwlock test/test_wwlock.c)
wwlock_bin(bench_wwlock test/bench_wwlock.c)
wwlock_bin(example_read_write examples/read_write.c)
wwlock_bin(example_two_locks examples/two_locks.c)
wwlock_bin(example_app_library examples/app_library.c)

enable_testing()
add_test(NAME test_wwlock COMMAND test_wwlock)
add_test(NAME bench_wwlock COMMAND bench_wwlock)
add_test(NAME example_read_write COMMAND example_read_write)
add_test(NAME example_two_locks COMMAND example_two_locks)
add_test(NAME example_app_library COMMAND example_app_library)

cmake_minimum_required(VERSION 3.25)
project(bdk_core LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_compile_options(-Wall -Wextra -Werror -pedantic)

enable_testing()

# --- The public headers -------------------------------------------------------------------------------------

file(GLOB BDK_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include/bdk/*.h")
list(SORT BDK_HEADERS)

add_library(bdk_headers INTERFACE)
target_include_directories(bdk_headers INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Every public header is compiled on its own, included twice, as C11 and as C++17, with warnings as errors.
set(BDK_CHECK_DIR "${CMAKE_CURRENT_BINARY_DIR}/header-check")
set(BDK_CHECK_C_SOURCES)
set(BDK_CHECK_CXX_SOURCES)
foreach(header IN LISTS BDK_HEADERS)
  get_filename_component(name "${header}" NAME_WE)
  foreach(lang c cc)
    set(source "${BDK_CHECK_DIR}/${name}.${lang}")
    set(wanted
        "#include <bdk/${name}.h>\n#include <bdk/${name}.h>\nint bdk_header_check_${name}_${lang};\n")
    set(current "")
    if(EXISTS "${source}")
      file(READ "${source}" current)
    endif()
    if(NOT current STREQUAL wanted) # rewritten only when it changes, so concurrent configures do not collide
      file(WRITE "${source}" "${wanted}")
    endif()
    if(lang STREQUAL "c")
      list(APPEND BDK_CHECK_C_SOURCES "${source}")
    else()
      list(APPEND BDK_CHECK_CXX_SOURCES "${source}")
    endif()
  endforeach()
endforeach()
add_library(bdk_header_check_c OBJECT ${BDK_CHECK_C_SOURCES})
set_target_properties(bdk_header_check_c PROPERTIES LINKER_LANGUAGE C)
target_link_libraries(bdk_header_check_c PRIVATE bdk_headers)
add_library(bdk_header_check_cxx OBJECT ${BDK_CHECK_CXX_SOURCES})
target_link_libraries(bdk_header_check_cxx PRIVATE bdk_headers)
add_custom_target(bdk_header_check DEPENDS bdk_header_check_c bdk_header_check_cxx
  COMMENT "Public headers compile as C11 and C++17")

# --- The ABI hash -------------------------------------------------------------------------------------------

set(BDK_ABI_HASH_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/abi-hash.cmake")
add_test(NAME abi_hash_is_current
  COMMAND "${CMAKE_COMMAND}" -DMODE=check -P "${BDK_ABI_HASH_SCRIPT}")
add_custom_target(abi_hash_update
  COMMAND "${CMAKE_COMMAND}" -DMODE=generate -P "${BDK_ABI_HASH_SCRIPT}"
  COMMENT "Regenerating packages/core/generated from the headers")

# The check also runs as part of the build, so a header edit without a regeneration fails the build.
add_custom_target(bdk_abi_hash_check ALL
  COMMAND "${CMAKE_COMMAND}" -DMODE=check -P "${BDK_ABI_HASH_SCRIPT}"
  COMMENT "Checking the recorded ABI hash")

# --- The stub runtime ---------------------------------------------------------------------------------------

add_library(bdk_stub STATIC stub/stub_core.cc stub/stub_host.cc)
target_include_directories(bdk_stub
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/stub"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/generated")
target_link_libraries(bdk_stub PUBLIC bdk_headers)
set_target_properties(bdk_stub PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
# No exception may cross the ABI; the stub is built without them.
target_compile_options(bdk_stub PRIVATE -fno-exceptions)
add_dependencies(bdk_stub bdk_abi_hash_check)
find_package(Threads REQUIRED)
target_link_libraries(bdk_stub PUBLIC Threads::Threads)

# --- The conformance suite ----------------------------------------------------------------------------------
#
# The suite is plain C against bdk/bdk.h and tests/harness.h. To run it against another implementation, pass
#   -DBDK_RUNTIME_LIBRARY=<target or library> -DBDK_HARNESS_SOURCES=<file implementing tests/harness.h>
# The default is the stub and tests/harness_stub.cc.

set(BDK_RUNTIME_LIBRARY bdk_stub CACHE STRING "Library implementing the ABI under test")
set(BDK_HARNESS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/tests/harness_stub.cc"
    CACHE STRING "Sources implementing tests/harness.h for that library")

add_executable(bdk_conformance
  tests/test_support.c tests/tests_base.c tests/tests_runtime.c tests/tests_broker.c
  tests/tests_profile.c tests/tests_contents.c tests/tests_ui.c
  ${BDK_HARNESS_SOURCES})
target_include_directories(bdk_conformance PRIVATE tests)
target_link_libraries(bdk_conformance PRIVATE bdk_headers ${BDK_RUNTIME_LIBRARY})
if(BDK_RUNTIME_LIBRARY STREQUAL "bdk_stub")
  target_include_directories(bdk_conformance PRIVATE stub) # only harness_stub.cc uses it
endif()
add_dependencies(bdk_conformance bdk_header_check)
add_test(NAME conformance COMMAND bdk_conformance)

cmake_minimum_required(VERSION 3.25)
project(bdk_core LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_compile_options(-Wall -Wextra -Werror -pedantic)

# Optional sanitizer build (not the default): cmake -S packages/core -B packages/core/build-sanitize -DBDK_SANITIZE=ON
option(BDK_SANITIZE "Build the stub and the conformance suite with AddressSanitizer and UBSan" OFF)
if(BDK_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined
                      -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()

enable_testing()

# --- The public headers -------------------------------------------------------------------------------------

file(GLOB BDK_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include/bdk/*.h")
list(SORT BDK_HEADERS)

add_library(bdk_headers INTERFACE)
target_include_directories(bdk_headers INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Every public header is compiled on its own, included twice, as C11 and as C++23, with warnings as errors.
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
  COMMENT "Public headers compile as C11 and C++23")

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

# The runtime core (runtime/) is shared with the Chromium engine; the stub supplies its in-memory backend.
add_library(bdk_stub STATIC runtime/core.cc runtime/core_host.cc runtime/core_windows.cc runtime/ui_router.cc
  runtime/extension_policy.cc runtime/app_settings.cc runtime/broker.cc runtime/routes.cc
  runtime/broker_args.cc runtime/automation.cc runtime/contents_commands.cc runtime/local_socket_posix.cc
  runtime/providers.cc runtime/translation.cc runtime/reputation_lists.cc runtime/navigation_policy.cc
  runtime/profile_versions.cc runtime/web_apps.cc
  stub/stub_backend.cc stub/stub_services.cc stub/stub_extensions.cc stub/stub_broker.cc stub/stub_automation.cc
  stub/stub_providers.cc stub/stub_web_apps.cc)
target_include_directories(bdk_stub
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/stub"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/runtime" "${CMAKE_CURRENT_SOURCE_DIR}/generated")
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
# tests/harness_broker.h: the stub has its own; any other runtime lists its implementation in
# BDK_HARNESS_SOURCES, or gets the one that skips the tests needing it.
set(BDK_BROKER_HARNESS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/tests/harness_broker_unsupported.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/harness_automation_unsupported.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/harness_providers_unsupported.c")
if(BDK_RUNTIME_LIBRARY STREQUAL "bdk_stub")
  set(BDK_BROKER_HARNESS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/tests/harness_stub_broker.cc"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/harness_stub_automation.cc"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/harness_stub_providers.cc")
endif()

add_executable(bdk_conformance
  tests/test_support.c tests/tests_base.c tests/tests_runtime.c tests/tests_broker.c
  tests/tests_profile.c tests/tests_contents.c tests/tests_ui.c tests/tests_automation.c
  tests/tests_endpoint.c tests/tests_providers.c tests/tests_pages.c tests/tests_extensions.c
  tests/tests_navigation_policy.c tests/tests_profile_versions.c tests/tests_robustness.c
  tests/tests_web_apps.c
  ${BDK_HARNESS_SOURCES} ${BDK_BROKER_HARNESS_SOURCES})
target_include_directories(bdk_conformance PRIVATE tests)
target_link_libraries(bdk_conformance PRIVATE bdk_headers ${BDK_RUNTIME_LIBRARY})
if(BDK_RUNTIME_LIBRARY STREQUAL "bdk_stub")
  target_include_directories(bdk_conformance PRIVATE stub) # only the stub harness sources use it
endif()
add_dependencies(bdk_conformance bdk_header_check)
add_test(NAME conformance COMMAND bdk_conformance)

# The stub with its automation endpoint open, for tests of external tools in other languages
# (packages/js/automation). Stub only, POSIX.
if(BDK_RUNTIME_LIBRARY STREQUAL "bdk_stub")
  add_executable(bdk_stub_automation_server tests/automation_server.c)
  target_link_libraries(bdk_stub_automation_server PRIVATE bdk_headers bdk_stub)
endif()

# --- The conformance suite against the Chromium engine ------------------------------------------------------
#
# With -DBDK_ENGINE_FRAMEWORK=<directory holding "Chromium Framework.framework"> (a packaged runtime's
# Chromium.app/Contents/Frameworks), the same suite, plus the scenarios only a real engine can run
# (tests/tests_engine.c), builds as BDKEngineConformance.app around that framework, through the engine's test
# hooks (chromium/bdk/engine/bdk_testing.h). macOS only. Run it with
#   <build>/BDKEngineConformance.app/Contents/MacOS/BDKEngineConformance [--list] [--run <test>]

set(BDK_ENGINE_FRAMEWORK "" CACHE PATH "Directory with the engine's Chromium Framework.framework")
if(BDK_ENGINE_FRAMEWORK AND APPLE)
  enable_language(OBJCXX)
  set(BDK_ENGINE_HOOKS "${CMAKE_CURRENT_SOURCE_DIR}/../../chromium/bdk/engine")
  add_executable(bdk_engine_conformance MACOSX_BUNDLE
    tests/test_support.c tests/tests_base.c tests/tests_runtime.c tests/tests_broker.c
    tests/tests_profile.c tests/tests_contents.c tests/tests_ui.c tests/tests_automation.c
    tests/tests_endpoint.c tests/tests_providers.c tests/tests_pages.c tests/tests_extensions.c tests/tests_engine.c
    tests/tests_profile_versions.c tests/tests_robustness.c
    tests/tests_web_apps.c
    tests/tests_engine_ui.c tests/tests_engine_prompts.c tests/tests_engine_automation.c
    tests/tests_engine_providers.c tests/tests_engine_privacy.c tests/tests_engine_pages.c
    tests/tests_engine_extensions.c tests/tests_engine_ext_ui.c tests/tests_engine_ext_panels.c
    tests/tests_engine_ext_probe.c tests/tests_engine_webauthn.c tests/tests_engine_dialogs.c
    tests/tests_engine_arc.c
    tests/engine_ext_support.c
    tests/harness_engine.mm tests/harness_engine_ext.mm
    tests/harness_broker_unsupported.c tests/harness_automation_unsupported.c tests/harness_providers_unsupported.c)
  target_compile_definitions(bdk_engine_conformance PRIVATE BDK_HARNESS_OWNS_MAIN BDK_HARNESS_ENGINE_TESTS)
  # The automation scripts and DevTools front ends of tests_engine_automation.c, where `bdk build` puts
  # them in an application, with the digests the resolved configuration records.
  set(BDK_FIXTURES "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  foreach(script acceptance wait fail debug spaces busy)
    set(file "${BDK_FIXTURES}/automation/${script}.js")
    file(SHA256 "${file}" digest)
    string(TOUPPER "${script}" upper)
    target_compile_definitions(bdk_engine_conformance PRIVATE
      "BDK_TEST_${upper}_DIGEST=\"sha256:${digest}\"")
    target_sources(bdk_engine_conformance PRIVATE "${file}")
    set_source_files_properties("${file}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources/BDK/Automation)
  endforeach()
  set(BDK_OVERLAY "${BDK_FIXTURES}/devtools/Overlay/overlay.css" "${BDK_FIXTURES}/devtools/Overlay/overlay.js")
  target_sources(bdk_engine_conformance PRIVATE ${BDK_OVERLAY} "${BDK_FIXTURES}/devtools/Frontend/index.html")
  set_source_files_properties(${BDK_OVERLAY} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/BDK/DevTools/Overlay)
  set_source_files_properties("${BDK_FIXTURES}/devtools/Frontend/index.html"
    PROPERTIES MACOSX_PACKAGE_LOCATION Resources/BDK/DevTools/Frontend)
  target_include_directories(bdk_engine_conformance PRIVATE tests "${BDK_ENGINE_HOOKS}")
  # The unpacked extensions the extension scenarios load (tests/extensions/fixtures).
  target_compile_definitions(bdk_engine_conformance PRIVATE
    BDK_EXTENSION_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/../../tests/extensions/fixtures")
  target_link_libraries(bdk_engine_conformance PRIVATE bdk_headers)
  target_compile_options(bdk_engine_conformance PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
  target_link_options(bdk_engine_conformance PRIVATE
    "SHELL:-F \"${BDK_ENGINE_FRAMEWORK}\"" "SHELL:-framework \"Chromium Framework\""
    "SHELL:-framework AppKit")
  set_target_properties(bdk_engine_conformance PROPERTIES
    OUTPUT_NAME BDKEngineConformance
    MACOSX_BUNDLE_GUI_IDENTIFIER dev.bdk.conformance
    MACOSX_BUNDLE_BUNDLE_NAME BDKEngineConformance)
  # The framework goes inside the bundle, where Chromium looks for it and its helpers (an APFS clone), again
  # whenever the framework is rebuilt.
  set(BDK_ENGINE_APP "${CMAKE_CURRENT_BINARY_DIR}/BDKEngineConformance.app")
  set(BDK_ENGINE_BINARY
      "${BDK_ENGINE_FRAMEWORK}/Chromium Framework.framework/Versions/Current/Chromium Framework")
  # A packaged runtime carries normalised timestamps, so a newly selected one is no newer than the stamp. The
  # copy therefore also depends on where BDK_ENGINE_FRAMEWORK resolves (.bdk/runtime links to the runtime's own
  # directory); file(CONFIGURE) rewrites this file only when that changes.
  file(REAL_PATH "${BDK_ENGINE_FRAMEWORK}" BDK_ENGINE_FRAMEWORK_REAL)
  set(BDK_ENGINE_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/engine-framework.source")
  file(CONFIGURE OUTPUT "${BDK_ENGINE_SOURCE}" CONTENT "${BDK_ENGINE_FRAMEWORK_REAL}\n")
  add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/engine-framework.stamp"
    DEPENDS "${BDK_ENGINE_BINARY}" "${BDK_ENGINE_SOURCE}"
    COMMAND rm -rf "${BDK_ENGINE_APP}/Contents/Frameworks"
    COMMAND mkdir -p "${BDK_ENGINE_APP}/Contents/Frameworks"
    COMMAND cp -cR "${BDK_ENGINE_FRAMEWORK}/Chromium Framework.framework" "${BDK_ENGINE_APP}/Contents/Frameworks/"
    COMMAND chmod -R u+w "${BDK_ENGINE_APP}/Contents/Frameworks"
    COMMAND touch "${CMAKE_CURRENT_BINARY_DIR}/engine-framework.stamp"
    COMMENT "Embedding the engine framework")
  add_custom_target(bdk_engine_framework DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/engine-framework.stamp")
  add_dependencies(bdk_engine_conformance bdk_engine_framework)
  add_dependencies(bdk_engine_conformance bdk_header_check)
endif()

cmake_minimum_required(VERSION 3.22)

# Force vcpkg to build statically-linked libraries (must be set BEFORE project())
set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "Vcpkg target triplet" FORCE)

# Use the project-local RmlUi overlay so font rasterization is deterministic.
# Preserve any caller-provided overlays while ensuring our patched RmlUi port
# takes precedence. This must be configured before project() loads vcpkg.
set(_OMNISTATS_VCPKG_OVERLAY "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg-overlay-ports")
set(_OMNISTATS_VCPKG_OVERLAYS "${_OMNISTATS_VCPKG_OVERLAY}")
if(VCPKG_OVERLAY_PORTS)
    list(APPEND _OMNISTATS_VCPKG_OVERLAYS ${VCPKG_OVERLAY_PORTS})
endif()
list(REMOVE_DUPLICATES _OMNISTATS_VCPKG_OVERLAYS)
set(VCPKG_OVERLAY_PORTS "${_OMNISTATS_VCPKG_OVERLAYS}" CACHE STRING "Vcpkg overlay ports" FORCE)
unset(_OMNISTATS_VCPKG_OVERLAY)
unset(_OMNISTATS_VCPKG_OVERLAYS)

# Test-only ports are manifest features, so release-only configures do not install them.
if(NOT DEFINED BUILD_TESTING OR BUILD_TESTING)
    list(APPEND VCPKG_MANIFEST_FEATURES "tests")
endif()
if(OMNISTATS_BUILD_BENCHMARKS)
    list(APPEND VCPKG_MANIFEST_FEATURES "benchmarks")
endif()

# Read version from vcpkg.json (Single Source of Truth)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg.json" VCPKG_JSON_CONTENT)
string(JSON OMNISTATS_VERSION GET "${VCPKG_JSON_CONTENT}" "version")

project(OmniStats VERSION ${OMNISTATS_VERSION} LANGUAGES CXX)

# Generate versioned Windows resource scripts in the build tree.
string(REPLACE "." "," OMNISTATS_VERSION_COMMA ${OMNISTATS_VERSION})
set(OMNISTATS_VERSION_COMMA "${OMNISTATS_VERSION_COMMA},0")
file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources" OMNISTATS_RESOURCE_DIR)
set(OMNISTATS_GENERATED_RESOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated-resources")
file(MAKE_DIRECTORY "${OMNISTATS_GENERATED_RESOURCE_DIR}")
set(OMNISTATS_APP_RESOURCE_FILE "${OMNISTATS_GENERATED_RESOURCE_DIR}/OmniStats.rc")
set(OMNISTATS_UPDATER_RESOURCE_FILE "${OMNISTATS_GENERATED_RESOURCE_DIR}/OmniStatsUpdater.rc")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/resources/omnistats.rc.in" "${OMNISTATS_APP_RESOURCE_FILE}" @ONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/resources/updater.rc.in" "${OMNISTATS_UPDATER_RESOURCE_FILE}" @ONLY)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Define Windows target version (Windows 10) to resolve Asio/Winsock compiler warnings
add_compile_definitions(NOMINMAX _WIN32_WINNT=0x0A00)

# Force MSVC to use the static C/C++ runtime library (/MT instead of /MD)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
    add_compile_options(/MP /W4)
endif()
option(OMNISTATS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors in OmniStats, OmniStatsUpdater and OmniStatsTests" ON)

# Find the packages defined in vcpkg.json
find_package(nlohmann_json CONFIG REQUIRED)
find_package(RmlUi CONFIG REQUIRED)
find_package(CURL CONFIG REQUIRED)
find_package(SDL2 CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(unofficial-sodium CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(asio CONFIG REQUIRED)

option(BUILD_TESTING "Build the testing tree" ON)
option(OMNISTATS_BUILD_BENCHMARKS "Build the Google Benchmark executable" OFF)
option(OMNISTATS_BUILD_FUZZER "Build the fuzzing executable" OFF)
option(ENABLE_SANITIZERS "Enable Address and Undefined sanitizers" OFF)
option(ENABLE_TSAN "Enable Thread sanitizer" OFF)
option(ENABLE_COVERAGE "Enable code coverage instrumentation" OFF)
option(OMNISTATS_BUILD_UPDATER "Build OmniStatsUpdater.exe" ON)
option(OMNISTATS_BUILD_INSTALLER "Deprecated compatibility option; the WiX MSI is the only supported installer" OFF)
option(OMNISTATS_ENABLE_LOW_LEVEL_HOOK "Enable the legacy low-level keyboard hook" OFF)

if(OMNISTATS_BUILD_INSTALLER)
    message(FATAL_ERROR "The legacy bootstrap installer was retired. Build the supported WiX MSI from installer/wix.")
endif()

if(ENABLE_SANITIZERS)
    if(MSVC)
        add_compile_options(/fsanitize=address)
        add_compile_definitions(_DISABLE_VECTOR_ANNOTATION=1 _DISABLE_STRING_ANNOTATION=1)
        add_link_options(/fsanitize=address)
    else()
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
elseif(ENABLE_TSAN)
    if(NOT MSVC)
        add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
        add_link_options(-fsanitize=thread)
    endif()
elseif(ENABLE_COVERAGE)
    if(NOT MSVC)
        add_compile_options(--coverage -O0 -g)
        add_link_options(--coverage)
    endif()
endif()

if(BUILD_TESTING)
    enable_testing()
    find_package(GTest CONFIG REQUIRED)
    if(OMNISTATS_BUILD_BENCHMARKS)
        find_package(benchmark CONFIG REQUIRED)
    endif()
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/DiscordSocialSdk.cmake")

# -----------------------------------------------------------------------------
# Source Discovery (per-directory GLOB_RECURSE with CONFIGURE_DEPENDS)
# -----------------------------------------------------------------------------
file(GLOB_RECURSE UTILS_SOURCES CONFIGURE_DEPENDS
    "src/utils/*.cpp" "src/utils/*.h" "src/utils/*.hpp"
)
file(GLOB_RECURSE PLATFORM_SOURCES CONFIGURE_DEPENDS
    "src/platform/*.cpp" "src/platform/*.h" "src/platform/*.hpp"
)
file(GLOB_RECURSE DATABASE_SOURCES CONFIGURE_DEPENDS
    "src/database/*.cpp" "src/database/*.h" "src/database/*.hpp"
)
file(GLOB_RECURSE NETWORK_SOURCES CONFIGURE_DEPENDS
    "src/network/*.cpp" "src/network/*.h" "src/network/*.hpp"
)
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS
    "src/core/*.cpp" "src/core/*.h" "src/core/*.hpp"
)
file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS
    "src/app/*.cpp" "src/app/*.h" "src/app/*.hpp"
)
if(APP_SOURCES)
    list(APPEND CORE_SOURCES ${APP_SOURCES})
endif()
file(GLOB_RECURSE UI_SOURCES CONFIGURE_DEPENDS
    "src/ui/*.cpp" "src/ui/*.h" "src/ui/*.hpp"
)

# Updater sources: minimal source set kept separate
file(GLOB UPDATER_ALL_SOURCES CONFIGURE_DEPENDS
    "src/updater/*.cpp" "src/updater/*.h" "src/updater/*.hpp"
)
set(UPDATER_ENTRY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/updater/main.cpp")
set(UPDATER_ONLY_SOURCES ${UPDATER_ALL_SOURCES})
list(REMOVE_ITEM UPDATER_ONLY_SOURCES "${UPDATER_ENTRY_SOURCE}")
set(UPDATER_SOURCES
    "${UPDATER_ENTRY_SOURCE}"
    ${UPDATER_ONLY_SOURCES}
)

# -----------------------------------------------------------------------------
# Precompiled Headers & Layer Library Helpers
# -----------------------------------------------------------------------------
function(omnistats_configure_layer_target target)
    target_include_directories(${target} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_definitions(${target} PRIVATE
        OMNISTATS_ENABLE_LOW_LEVEL_HOOK=$<BOOL:${OMNISTATS_ENABLE_LOW_LEVEL_HOOK}>
        OMNISTATS_VERSION="${OMNISTATS_VERSION}"
    )
    if(MSVC)
        target_compile_options(${target} PRIVATE
            $<$<BOOL:${OMNISTATS_WARNINGS_AS_ERRORS}>:/WX>
            $<$<CONFIG:Release>:/O2>
            $<$<CONFIG:Release>:/GF>
            $<$<CONFIG:Release>:/Gy>
            $<$<CONFIG:Release>:/Zi>
        )
    endif()
endfunction()

function(omnistats_enable_shared_pch target)
    target_precompile_headers(${target} PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/src/pch/OmniStatsPch.hpp>"
    )
endfunction()

function(omnistats_enable_rmlui_pch target)
    target_precompile_headers(${target} PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/src/ui/rml/RmlUiPch.hpp>"
    )
endfunction()

# Builds a static library for a layer, plus an optional test variant if the layer
# contains OMNISTATS_TEST_ENVIRONMENT hooks. Other slices are removing these hooks;
# once removed, remove HAS_TEST_VARIANT to cut over cleanly.
function(omnistats_add_layer_library name)
    cmake_parse_arguments(ARG "HAS_TEST_VARIANT" "" "SOURCES;LIBRARIES" ${ARGN})

    # Clean production library (links into OmniStats.exe, no test hooks)
    add_library(${name} STATIC ${ARG_SOURCES})
    omnistats_configure_layer_target(${name})
    if(ARG_LIBRARIES)
        target_link_libraries(${name} PUBLIC ${ARG_LIBRARIES})
    endif()

    if(name STREQUAL "omnistats_ui")
        omnistats_enable_rmlui_pch(${name})
        set(ui_non_rml ${ARG_SOURCES})
        list(FILTER ui_non_rml INCLUDE REGEX ".*\\.cpp$")
        list(FILTER ui_non_rml EXCLUDE REGEX "/src/ui/rml/.*")
        set_source_files_properties(${ui_non_rml} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
        target_compile_definitions(${name} PRIVATE
            $<$<CONFIG:Debug>:OMNISTATS_RML_DEV_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources/rml">
        )
    else()
        omnistats_enable_shared_pch(${name})
    endif()

    # Test variant for targets with OMNISTATS_TEST_ENVIRONMENT hooks
    if(BUILD_TESTING AND ARG_HAS_TEST_VARIANT)
        set(test_target "${name}_test")
        add_library(${test_target} STATIC ${ARG_SOURCES})
        omnistats_configure_layer_target(${test_target})
        target_compile_definitions(${test_target} PUBLIC OMNISTATS_TEST_ENVIRONMENT=1)
        if(ARG_LIBRARIES)
            target_link_libraries(${test_target} PUBLIC ${ARG_LIBRARIES})
        endif()

        if(name STREQUAL "omnistats_ui")
            omnistats_enable_rmlui_pch(${test_target})
            set(ui_non_rml ${ARG_SOURCES})
            list(FILTER ui_non_rml INCLUDE REGEX ".*\\.cpp$")
            list(FILTER ui_non_rml EXCLUDE REGEX "/src/ui/rml/.*")
            set_source_files_properties(${ui_non_rml} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
            target_compile_definitions(${test_target} PRIVATE
                $<$<CONFIG:Debug>:OMNISTATS_RML_DEV_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources/rml">
            )
        else()
            omnistats_enable_shared_pch(${test_target})
        endif()
    endif()
endfunction()

# -----------------------------------------------------------------------------
# Private Service Module Integration
# -----------------------------------------------------------------------------
option(OMNISTATS_USE_PRIVATE_SERVICES "Use private OmniStats service module if present" ON)
if(OMNISTATS_USE_PRIVATE_SERVICES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/private/CMakeLists.txt")
    set(OMNISTATS_PUBLIC_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_subdirectory(private)
    message(STATUS "OmniNet backend: private service module")
else()
    add_library(omnistats_omninet STATIC "${CMAKE_CURRENT_SOURCE_DIR}/omninet-stub/OmniNetStub.cpp")
    target_include_directories(omnistats_omninet PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
    message(STATUS "OmniNet backend: stub (tracker.gg only, no sign-in/telemetry)")
endif()

# -----------------------------------------------------------------------------
# Static Layer Libraries
# -----------------------------------------------------------------------------
omnistats_add_layer_library(omnistats_utils
    HAS_TEST_VARIANT
    SOURCES ${UTILS_SOURCES}
    LIBRARIES
        nlohmann_json::nlohmann_json
        unofficial::sqlite3::sqlite3
        ZLIB::ZLIB
        crypt32.lib
        shell32.lib
        ole32.lib
)

omnistats_add_layer_library(omnistats_platform
    SOURCES ${PLATFORM_SOURCES}
    LIBRARIES
        SDL2::SDL2-static
        Imm32.lib
        Setupapi.lib
        winmm.lib
        version.lib
)

omnistats_add_layer_library(omnistats_database
    SOURCES ${DATABASE_SOURCES}
    LIBRARIES
        unofficial::sqlite3::sqlite3
        nlohmann_json::nlohmann_json
)

omnistats_add_layer_library(omnistats_network
    HAS_TEST_VARIANT
    SOURCES ${NETWORK_SOURCES}
    LIBRARIES
        omnistats_omninet
        CURL::libcurl
        unofficial-sodium::sodium
        nlohmann_json::nlohmann_json
        asio::asio
        DiscordSocialSdk
        crypt32.lib
        Ws2_32.lib
        Wldap32.lib
        Normaliz.lib
        bcrypt.lib
        wintrust.lib
)

omnistats_add_layer_library(omnistats_core
    HAS_TEST_VARIANT
    SOURCES ${CORE_SOURCES}
    LIBRARIES
        nlohmann_json::nlohmann_json
        DiscordSocialSdk
        crypt32.lib
        shell32.lib
        ole32.lib
        uuid.lib
)

omnistats_add_layer_library(omnistats_ui
    SOURCES ${UI_SOURCES}
    LIBRARIES
        RmlUi::RmlUi
        SDL2::SDL2-static
        d3d11.lib
        d3dcompiler.lib
        dxgi.lib
        gdiplus.lib
        windowscodecs.lib
        comdlg32.lib
        Imm32.lib
        Setupapi.lib
        winmm.lib
        version.lib
        ole32.lib
        uuid.lib
        shell32.lib
)

if(BUILD_TESTING)
    add_library(omnistats_updater_test STATIC ${UPDATER_ONLY_SOURCES})
    omnistats_configure_layer_target(omnistats_updater_test)
    omnistats_enable_shared_pch(omnistats_updater_test)
    target_compile_definitions(omnistats_updater_test PUBLIC OMNISTATS_TEST_ENVIRONMENT=1)
    target_link_libraries(omnistats_updater_test PUBLIC
        CURL::libcurl
        version.lib
        crypt32.lib
        wintrust.lib
        Ws2_32.lib
        Wldap32.lib
        Normaliz.lib
        bcrypt.lib
        ZLIB::ZLIB
        ole32.lib
        uuid.lib
    )
endif()

# -----------------------------------------------------------------------------
# Main Application Executable (OmniStats.exe)
# -----------------------------------------------------------------------------
set(RESOURCE_FILE "${OMNISTATS_APP_RESOURCE_FILE}")

add_executable(${PROJECT_NAME}
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
    ${RESOURCE_FILE}
)
omnistats_copy_discord_sdk(${PROJECT_NAME})

target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(${PROJECT_NAME} PRIVATE
    OMNISTATS_ENABLE_LOW_LEVEL_HOOK=$<BOOL:${OMNISTATS_ENABLE_LOW_LEVEL_HOOK}>
    OMNISTATS_VERSION="${OMNISTATS_VERSION}"
    $<$<CONFIG:Debug>:OMNISTATS_RML_DEV_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources/rml">
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    omnistats_ui
    omnistats_platform
    omnistats_network
    omnistats_database
    omnistats_core
    omnistats_utils
    dbghelp.lib
)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ThirdPartyNotices.cmake")
set(OMNISTATS_NOTICES_FILE "${CMAKE_CURRENT_BINARY_DIR}/THIRD_PARTY_NOTICES.txt")
omnistats_generate_third_party_notices("${OMNISTATS_NOTICES_FILE}")
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OMNISTATS_NOTICES_FILE}" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/THIRD_PARTY_NOTICES.txt"
)

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        $<$<BOOL:${OMNISTATS_WARNINGS_AS_ERRORS}>:/WX>
        $<$<CONFIG:Release>:/O2>
        $<$<CONFIG:Release>:/GF>
        $<$<CONFIG:Release>:/Gy>
        $<$<CONFIG:Release>:/Zi>
    )

    target_link_options(${PROJECT_NAME} PRIVATE
        "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/resources/app.manifest"
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
        $<$<CONFIG:Release>:/DEBUG>
        $<$<CONFIG:Release>:/SUBSYSTEM:WINDOWS>
        $<$<CONFIG:Release>:/ENTRY:mainCRTStartup>
    )
endif()

# -----------------------------------------------------------------------------
# Updater Target (OmniStatsUpdater.exe)
# -----------------------------------------------------------------------------
if(OMNISTATS_BUILD_UPDATER)
    add_executable(OmniStatsUpdater
        ${UPDATER_SOURCES}
        "${OMNISTATS_UPDATER_RESOURCE_FILE}"
    )
    target_include_directories(OmniStatsUpdater PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_definitions(OmniStatsUpdater PRIVATE
        OMNISTATS_VERSION="${OMNISTATS_VERSION}"
    )
    target_link_libraries(OmniStatsUpdater PRIVATE
        CURL::libcurl
        unofficial-sodium::sodium
        version.lib
        crypt32.lib
        wintrust.lib
        Ws2_32.lib
        Wldap32.lib
        Normaliz.lib
        bcrypt.lib
        ZLIB::ZLIB
        ole32.lib
        uuid.lib
    )
    if(MSVC)
        target_compile_options(OmniStatsUpdater PRIVATE
            $<$<BOOL:${OMNISTATS_WARNINGS_AS_ERRORS}>:/WX>
            $<$<CONFIG:Release>:/O2>
            $<$<CONFIG:Release>:/GF>
            $<$<CONFIG:Release>:/Gy>
            $<$<CONFIG:Release>:/Zi>
        )
        target_link_options(OmniStatsUpdater PRIVATE
            "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/resources/app.manifest"
            $<$<CONFIG:Release>:/OPT:REF>
            $<$<CONFIG:Release>:/OPT:ICF>
            $<$<CONFIG:Release>:/DEBUG>
            $<$<CONFIG:Release>:/SUBSYSTEM:WINDOWS>
            $<$<CONFIG:Release>:/ENTRY:mainCRTStartup>
        )
    endif()
endif()

add_executable(OmniStatsReleaseSigner tools/ReleaseSigner.cpp)
target_include_directories(OmniStatsReleaseSigner PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(OmniStatsReleaseSigner PRIVATE unofficial-sodium::sodium)

# -----------------------------------------------------------------------------
# Test Target (OmniStatsTests.exe)
# -----------------------------------------------------------------------------
if(BUILD_TESTING)
    file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS "tests/*.cpp")
    file(GLOB_RECURSE FUZZ_SOURCES CONFIGURE_DEPENDS "tests/fuzz/*.cpp")
    if(FUZZ_SOURCES)
        list(REMOVE_ITEM TEST_SOURCES ${FUZZ_SOURCES})
    endif()

    add_executable(OmniStatsTests ${TEST_SOURCES})
    omnistats_copy_discord_sdk(OmniStatsTests)
    target_include_directories(OmniStatsTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    omnistats_enable_shared_pch(OmniStatsTests)
    target_compile_definitions(OmniStatsTests PRIVATE
        OMNISTATS_TEST_ENVIRONMENT=1
        OMNISTATS_ENABLE_LOW_LEVEL_HOOK=$<BOOL:${OMNISTATS_ENABLE_LOW_LEVEL_HOOK}>
        OMNISTATS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        OMNISTATS_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
        OMNISTATS_VERSION="${OMNISTATS_VERSION}"
    )

    target_link_libraries(OmniStatsTests PRIVATE
        GTest::gtest_main
        GTest::gmock
        omnistats_ui
        omnistats_platform
        omnistats_network_test
        omnistats_database
        omnistats_core_test
        omnistats_utils_test
        omnistats_updater_test
        dbghelp.lib
    )

    if(MSVC)
        target_compile_options(OmniStatsTests PRIVATE
            $<$<BOOL:${OMNISTATS_WARNINGS_AS_ERRORS}>:/WX>
            $<$<CONFIG:Release>:/O2>
            $<$<CONFIG:Release>:/GF>
            $<$<CONFIG:Release>:/Gy>
            $<$<CONFIG:Release>:/Zi>
        )
        target_link_options(OmniStatsTests PRIVATE
            "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/resources/app.manifest"
            $<$<CONFIG:Release>:/OPT:REF>
            $<$<CONFIG:Release>:/OPT:ICF>
            $<$<CONFIG:Release>:/DEBUG>
        )
    endif()

    add_test(NAME OmniStatsTests COMMAND OmniStatsTests)

    # -------------------------------------------------------------------------
    # Google Benchmark Target
    # -------------------------------------------------------------------------
    file(GLOB_RECURSE BENCH_SOURCES CONFIGURE_DEPENDS "benchmarks/*.cpp")

    if(BENCH_SOURCES AND OMNISTATS_BUILD_BENCHMARKS)
        add_executable(OmniStatsBenchmarks ${BENCH_SOURCES})
        omnistats_copy_discord_sdk(OmniStatsBenchmarks)
        target_include_directories(OmniStatsBenchmarks PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        omnistats_enable_shared_pch(OmniStatsBenchmarks)
        target_compile_definitions(OmniStatsBenchmarks PRIVATE
            OMNISTATS_TEST_ENVIRONMENT=1
            OMNISTATS_ENABLE_LOW_LEVEL_HOOK=$<BOOL:${OMNISTATS_ENABLE_LOW_LEVEL_HOOK}>
            OMNISTATS_VERSION="${OMNISTATS_VERSION}"
        )
        target_link_libraries(OmniStatsBenchmarks PRIVATE
            benchmark::benchmark_main
            omnistats_ui
            omnistats_platform
            omnistats_network_test
            omnistats_database
            omnistats_core_test
            omnistats_utils_test
            dbghelp.lib
        )
        if(MSVC)
            target_compile_options(OmniStatsBenchmarks PRIVATE
                $<$<CONFIG:Release>:/O2>
                $<$<CONFIG:Release>:/GF>
                $<$<CONFIG:Release>:/Gy>
                $<$<CONFIG:Release>:/Zi>
            )
            target_link_options(OmniStatsBenchmarks PRIVATE
                $<$<CONFIG:Release>:/OPT:REF>
                $<$<CONFIG:Release>:/OPT:ICF>
                $<$<CONFIG:Release>:/DEBUG>
            )
        endif()
    endif()

    # -------------------------------------------------------------------------
    # Fuzzer Target
    # -------------------------------------------------------------------------
    if(FUZZ_SOURCES AND OMNISTATS_BUILD_FUZZER)
        set_source_files_properties(${FUZZ_SOURCES} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
        add_executable(OmniStatsFuzzer ${FUZZ_SOURCES})
        omnistats_copy_discord_sdk(OmniStatsFuzzer)
        target_include_directories(OmniStatsFuzzer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_compile_definitions(OmniStatsFuzzer PRIVATE
            OMNISTATS_TEST_ENVIRONMENT=1
            OMNISTATS_ENABLE_LOW_LEVEL_HOOK=$<BOOL:${OMNISTATS_ENABLE_LOW_LEVEL_HOOK}>
            OMNISTATS_VERSION="${OMNISTATS_VERSION}"
        )
        target_link_libraries(OmniStatsFuzzer PRIVATE
            omnistats_ui
            omnistats_platform
            omnistats_network_test
            omnistats_database
            omnistats_core_test
            omnistats_utils_test
            dbghelp.lib
        )
        if(MSVC)
            # Only clang-cl implements libFuzzer on Windows. Plain cl.exe ignores
            # the flag and links an executable without a fuzzing entry point,
            # which then hangs instead of fuzzing.
            if(NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC" OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
                message(FATAL_ERROR "OMNISTATS_BUILD_FUZZER requires clang-cl on Windows: configure with -T ClangCL or -DCMAKE_CXX_COMPILER=clang-cl.")
            endif()
            target_compile_options(OmniStatsFuzzer PRIVATE /fsanitize=fuzzer,address)
            target_link_options(OmniStatsFuzzer PRIVATE /fsanitize=fuzzer,address)
        endif()
    endif()
endif()

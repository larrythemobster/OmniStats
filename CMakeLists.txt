cmake_minimum_required(VERSION 3.22)

# Force vcpkg to build statically-linked libraries (must be set BEFORE project())
set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "Vcpkg target triplet" FORCE)

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
add_compile_definitions(_WIN32_WINNT=0x0A00)

# Force MSVC to use the static C/C++ runtime library (/MT instead of /MD)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
    add_compile_options(/MP)
endif()

# Find the packages defined in vcpkg.json
find_package(nlohmann_json CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(implot CONFIG REQUIRED)
find_package(CURL CONFIG REQUIRED)
find_package(SDL2 CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
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
# discord-rpc doesn't provide a CMake config, find both debug and release libs
find_library(DISCORD_RPC_LIB_RELEASE discord-rpc
    PATHS "${CMAKE_CURRENT_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/lib"
    NO_DEFAULT_PATH
)
find_library(DISCORD_RPC_LIB_DEBUG discord-rpc
    PATHS "${CMAKE_CURRENT_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/debug/lib"
    NO_DEFAULT_PATH
)

file(GLOB_RECURSE ALL_SOURCE_FILES CONFIGURE_DEPENDS "src/*.cpp" "src/*.h" "src/*.hpp")
list(REMOVE_DUPLICATES ALL_SOURCE_FILES)

set(MAIN_ENTRY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
set(UPDATER_ENTRY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/updater/main.cpp")
set(UPDATER_ONLY_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/network/UpdaterCommon.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/network/UpdaterPaths.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/network/UpdaterDownload.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/network/UpdaterProcess.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/network/ShortcutUtils.cpp"
)
set(UPDATER_RUNTIME_SOURCES
    ${UPDATER_ONLY_SOURCES}
)

set(SHARED_NON_UPDATER_SOURCES ${ALL_SOURCE_FILES})
list(REMOVE_ITEM SHARED_NON_UPDATER_SOURCES
    "${MAIN_ENTRY_SOURCE}"
    "${UPDATER_ENTRY_SOURCE}"
    ${UPDATER_ONLY_SOURCES}
)

set(MAIN_APP_SOURCES
    "${MAIN_ENTRY_SOURCE}"
    ${SHARED_NON_UPDATER_SOURCES}
)

set(UPDATER_SOURCES
    "${UPDATER_ENTRY_SOURCE}"
    ${UPDATER_RUNTIME_SOURCES}
)

set(TEST_SUPPORT_SOURCES
    ${SHARED_NON_UPDATER_SOURCES}
    ${UPDATER_ONLY_SOURCES}
)

# Embed application resources through the configured build-tree script.
set(RESOURCE_FILE "${OMNISTATS_APP_RESOURCE_FILE}")

add_executable(${PROJECT_NAME} ${MAIN_APP_SOURCES} ${RESOURCE_FILE})

target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(${PROJECT_NAME} PRIVATE
    OMNISTATS_ENABLE_LOW_LEVEL_HOOK=$<BOOL:${OMNISTATS_ENABLE_LOW_LEVEL_HOOK}>
    OMNISTATS_VERSION="${OMNISTATS_VERSION}"
)

# Application dependencies
target_link_libraries(${PROJECT_NAME} PRIVATE
    nlohmann_json::nlohmann_json
    imgui::imgui
    implot::implot
    CURL::libcurl
    SDL2::SDL2-static
    unofficial::sqlite3::sqlite3
    ZLIB::ZLIB
    asio::asio
    $<IF:$<CONFIG:Debug>,${DISCORD_RPC_LIB_DEBUG},${DISCORD_RPC_LIB_RELEASE}>
    d3d11.lib
    d3dcompiler.lib
    dxgi.lib
    winmm.lib
    version.lib
    Imm32.lib
    Setupapi.lib
    crypt32.lib
    Ws2_32.lib
    Wldap32.lib
    Normaliz.lib
    gdiplus.lib
    dbghelp.lib
    bcrypt.lib
)

if(MSVC)
    # Target-specific compiler options for MSVC
    target_compile_options(${PROJECT_NAME} PRIVATE
        $<$<CONFIG:Release>:/O2>          # Optimize for speed
        $<$<CONFIG:Release>:/GF>          # Eliminate Duplicate Strings
        $<$<CONFIG:Release>:/Gy>          # Function-Level Linking
        $<$<CONFIG:Release>:/Zi>          # Generate debug information (PDB)
    )

    # Target-specific linker options for MSVC
    target_link_options(${PROJECT_NAME} PRIVATE
        $<$<CONFIG:Release>:/OPT:REF>     # Remove Unreferenced Functions/Data (Dead code elimination)
        $<$<CONFIG:Release>:/OPT:ICF>     # Merge Identical COMDATs (Identical template folding)
        $<$<CONFIG:Release>:/DEBUG>       # Generate PDB file
    )
endif()

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
        version.lib
        crypt32.lib
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
            $<$<CONFIG:Release>:/O2>
            $<$<CONFIG:Release>:/GF>
            $<$<CONFIG:Release>:/Gy>
            $<$<CONFIG:Release>:/Zi>
        )
        target_link_options(OmniStatsUpdater PRIVATE
            $<$<CONFIG:Release>:/OPT:REF>
            $<$<CONFIG:Release>:/OPT:ICF>
            $<$<CONFIG:Release>:/DEBUG>
        )
    endif()
endif()

if(BUILD_TESTING)
    file(GLOB_RECURSE TEST_SOURCES "tests/*.cpp")
    file(GLOB_RECURSE FUZZ_SOURCES "tests/fuzz/*.cpp")
    if(FUZZ_SOURCES)
        list(REMOVE_ITEM TEST_SOURCES ${FUZZ_SOURCES})
    endif()

    add_executable(OmniStatsTests ${TEST_SOURCES} ${TEST_SUPPORT_SOURCES})
    target_include_directories(OmniStatsTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
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
        nlohmann_json::nlohmann_json
        imgui::imgui
        implot::implot
        CURL::libcurl
        SDL2::SDL2-static
        unofficial::sqlite3::sqlite3
        ZLIB::ZLIB
        asio::asio
        $<IF:$<CONFIG:Debug>,${DISCORD_RPC_LIB_DEBUG},${DISCORD_RPC_LIB_RELEASE}>
        d3d11.lib
        d3dcompiler.lib
        dxgi.lib
        winmm.lib
        version.lib
        Imm32.lib
        Setupapi.lib
        crypt32.lib
            Ws2_32.lib
        Wldap32.lib
        Normaliz.lib
        gdiplus.lib
        bcrypt.lib
        ole32.lib
        uuid.lib
    )

    if(MSVC)
        target_compile_options(OmniStatsTests PRIVATE
            $<$<CONFIG:Release>:/O2>
            $<$<CONFIG:Release>:/GF>
            $<$<CONFIG:Release>:/Gy>
            $<$<CONFIG:Release>:/Zi>
        )
        target_link_options(OmniStatsTests PRIVATE
            $<$<CONFIG:Release>:/OPT:REF>
            $<$<CONFIG:Release>:/OPT:ICF>
            $<$<CONFIG:Release>:/DEBUG>
        )
    endif()

    add_test(NAME OmniStatsTests COMMAND OmniStatsTests)

    # -------------------------------------------------------------
    # Google Benchmark Target
    # -------------------------------------------------------------
    file(GLOB_RECURSE BENCH_SOURCES "benchmarks/*.cpp")
    if(BENCH_SOURCES AND OMNISTATS_BUILD_BENCHMARKS)
        add_executable(OmniStatsBenchmarks ${BENCH_SOURCES} ${SHARED_NON_UPDATER_SOURCES})
        target_include_directories(OmniStatsBenchmarks PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_compile_definitions(OmniStatsBenchmarks PRIVATE
            OMNISTATS_TEST_ENVIRONMENT=1
            OMNISTATS_ENABLE_LOW_LEVEL_HOOK=$<BOOL:${OMNISTATS_ENABLE_LOW_LEVEL_HOOK}>
            OMNISTATS_VERSION="${OMNISTATS_VERSION}"
        )
        target_link_libraries(OmniStatsBenchmarks PRIVATE
            benchmark::benchmark
            nlohmann_json::nlohmann_json
            imgui::imgui
            implot::implot
            CURL::libcurl
            SDL2::SDL2-static
            unofficial::sqlite3::sqlite3
            ZLIB::ZLIB
            asio::asio
            $<IF:$<CONFIG:Debug>,${DISCORD_RPC_LIB_DEBUG},${DISCORD_RPC_LIB_RELEASE}>
            d3d11.lib
            d3dcompiler.lib
            dxgi.lib
            winmm.lib
            version.lib
            Imm32.lib
            Setupapi.lib
            crypt32.lib
                    Ws2_32.lib
            Wldap32.lib
            Normaliz.lib
            gdiplus.lib
            bcrypt.lib
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

    # -------------------------------------------------------------
    # Fuzzer Target
    # -------------------------------------------------------------
    if(FUZZ_SOURCES AND OMNISTATS_BUILD_FUZZER)
        add_executable(OmniStatsFuzzer ${FUZZ_SOURCES} ${SHARED_NON_UPDATER_SOURCES})
        target_include_directories(OmniStatsFuzzer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_compile_definitions(OmniStatsFuzzer PRIVATE
            OMNISTATS_TEST_ENVIRONMENT=1
            OMNISTATS_ENABLE_LOW_LEVEL_HOOK=$<BOOL:${OMNISTATS_ENABLE_LOW_LEVEL_HOOK}>
            OMNISTATS_VERSION="${OMNISTATS_VERSION}"
        )
        target_link_libraries(OmniStatsFuzzer PRIVATE
            nlohmann_json::nlohmann_json
            imgui::imgui
            implot::implot
            CURL::libcurl
            SDL2::SDL2-static
            unofficial::sqlite3::sqlite3
            ZLIB::ZLIB
            asio::asio
            $<IF:$<CONFIG:Debug>,${DISCORD_RPC_LIB_DEBUG},${DISCORD_RPC_LIB_RELEASE}>
            d3d11.lib
            d3dcompiler.lib
            dxgi.lib
            winmm.lib
            version.lib
            Imm32.lib
            Setupapi.lib
            crypt32.lib
                    Ws2_32.lib
            Wldap32.lib
            Normaliz.lib
            gdiplus.lib
            bcrypt.lib
        )
        if(MSVC)
            target_compile_options(OmniStatsFuzzer PRIVATE /fsanitize=fuzzer)
            target_link_options(OmniStatsFuzzer PRIVATE /fsanitize=fuzzer)
        endif()
    endif()
endif()

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO mikke89/RmlUi
    REF ${VERSION}
    SHA512 b66d2a3621ca2690d6e826661203fc3a4babe16243ed01b2f884d149f5b7ee598241f1f743262c7a8fbf8ae98641d5c5526fc63d8a6283638b6dc021d23e103e
    HEAD_REF master
)

# OmniStats renders compact grayscale text. Keep FreeType's antialiasing, but
# use light hinting so small rounded glyphs are not aggressively grid-fitted
# along the horizontal axis. The source archive is SHA-pinned above, so these
# replacements intentionally fail if the upstream source no longer matches.
vcpkg_replace_string(
    "${SOURCE_PATH}/Source/Core/FontEngineDefault/FreeTypeInterface.cpp"
    "FT_Error error = FT_Load_Glyph(ft_face, index, FT_LOAD_COLOR);"
    "constexpr FT_Int32 glyph_load_flags = FT_LOAD_COLOR | FT_LOAD_TARGET_LIGHT;\n\tFT_Error error = FT_Load_Glyph(ft_face, index, glyph_load_flags);"
)
vcpkg_replace_string(
    "${SOURCE_PATH}/Source/Core/FontEngineDefault/FreeTypeInterface.cpp"
    "if (index != 0 && FT_Load_Glyph(ft_face, index, 0) == 0)"
    "if (index != 0 && FT_Load_Glyph(ft_face, index, FT_LOAD_TARGET_LIGHT) == 0)"
)

# Keep vcpkg package discovery deterministic. RmlUi ships its own container
# headers in the source tree, so this overlay does not need separate itlib or
# robin-hood packages.
vcpkg_replace_string(
    "${SOURCE_PATH}/CMake/RmlUiConfig.cmake.in"
    [=[list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/Modules")

]=]
    ""
)
vcpkg_replace_string(
    "${SOURCE_PATH}/CMakeLists.txt"
    [=[install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/CMake/Modules"
	DESTINATION "${RMLUI_INSTALL_TARGETS_DIR}"
)

]=]
    ""
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        lua             RMLUI_LUA_BINDINGS
        svg             RMLUI_SVG_PLUGIN
        lottie          RMLUI_LOTTIE_PLUGIN
)

if("freetype" IN_LIST FEATURES)
    set(RMLUI_FONT_ENGINE "freetype")
else()
    set(RMLUI_FONT_ENGINE "none")
endif()

vcpkg_cmake_configure(
    SOURCE_PATH ${SOURCE_PATH}
    OPTIONS
        ${FEATURE_OPTIONS}
        "-DRMLUI_FONT_ENGINE=${RMLUI_FONT_ENGINE}"
        "-DRMLUI_COMPILER_OPTIONS=OFF"
        "-DRMLUI_INSTALL_RUNTIME_DEPENDENCIES=OFF"
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/RmlUi)
vcpkg_copy_pdbs()
file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
)

if(VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/RmlUi/Core/Header.h"
        "#if !defined RMLUI_STATIC_LIB"
        "#if 0"
    )
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/RmlUi/Debugger/Header.h"
        "#if !defined RMLUI_STATIC_LIB"
        "#if 0"
    )
    if("lua" IN_LIST FEATURES)
        vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/RmlUi/Lua/Header.h"
            "#if !defined RMLUI_STATIC_LIB"
            "#if 0"
        )
    endif()
endif()

configure_file("${CMAKE_CURRENT_LIST_DIR}/usage" "${CURRENT_PACKAGES_DIR}/share/${PORT}/usage" COPYONLY)
vcpkg_install_copyright(
    FILE_LIST
    "${SOURCE_PATH}/LICENSE.txt"
    "${SOURCE_PATH}/Include/RmlUi/Core/Containers/LICENSE.txt"
    "${SOURCE_PATH}/Source/Debugger/LICENSE.txt"
)

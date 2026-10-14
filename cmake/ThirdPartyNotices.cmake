# Builds THIRD_PARTY_NOTICES.txt from the licenses of everything shipped in the installer:
# statically linked vcpkg ports, bundled fonts and the libcurl-impersonate runtime DLLs.
function(omnistats_generate_third_party_notices output)
    set(ports asio brotli bzip2 curl discord-rpc freetype libpng libsodium nlohmann-json rapidjson rmlui sdl2 sqlite3 zlib)
    set(share_dir "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share")

    set(text "OmniStats third-party notices\n\nOmniStats includes the following third-party software. Each component is provided under the license reproduced below.\n")

    foreach(port IN LISTS ports)
        set(license_file "${share_dir}/${port}/copyright")
        if(NOT EXISTS "${license_file}")
            message(FATAL_ERROR "Missing license for shipped vcpkg port '${port}': ${license_file}")
        endif()
        file(READ "${license_file}" license)
        string(APPEND text "\n================================================================================\n${port} (statically linked)\n================================================================================\n\n${license}\n")
    endforeach()

    file(GLOB font_licenses "${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts/*-LICENSE.txt")
    list(SORT font_licenses)
    foreach(license_file IN LISTS font_licenses)
        get_filename_component(name "${license_file}" NAME)
        string(REPLACE "-LICENSE.txt" "" name "${name}")
        file(READ "${license_file}" license)
        string(APPEND text "\n================================================================================\n${name} font (bundled)\n================================================================================\n\n${license}\n")
    endforeach()

    set(runtime_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party/licenses/libcurl-impersonate")
    string(APPEND text "\n================================================================================\nlibcurl-impersonate.dll and zlib.dll (runtime DLLs)\n================================================================================\n\nlibcurl-impersonate statically includes curl, BoringSSL, nghttp2, nghttp3, ngtcp2, brotli, zstd and zlib. The curl, brotli and zlib licenses appear above.\n")
    foreach(component curl-impersonate boringssl nghttp2 nghttp3 ngtcp2 zstd)
        set(license_file "${runtime_dir}/${component}.txt")
        if(NOT EXISTS "${license_file}")
            message(FATAL_ERROR "Missing runtime license: ${license_file}")
        endif()
        file(READ "${license_file}" license)
        string(APPEND text "\n--- ${component} ---\n\n${license}\n")
    endforeach()

    file(WRITE "${output}" "${text}")
endfunction()

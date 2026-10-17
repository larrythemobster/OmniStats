set(OMNISTATS_DISCORD_SDK_VERSION "1.10.19337")

# Check for local vendored directory under Client/third_party/discord-social-sdk first
set(_discord_local_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party/discord-social-sdk/discord_social_sdk")

if(EXISTS "${_discord_local_dir}/include/discordpp.h" AND EXISTS "${_discord_local_dir}/bin/release/discord_partner_sdk.dll")
    set(OMNISTATS_DISCORD_SDK_ROOT "${_discord_local_dir}")
else()
    # Fallback to archive path if specified or downloaded
    set(OMNISTATS_DISCORD_SDK_SHA256 "d784097504685953849cc2842561d8a8013326fe0b8df65a1de63ddefde62045")
    set(_discord_archive_default "$ENV{OMNISTATS_DISCORD_SDK_ARCHIVE}")
    if(NOT _discord_archive_default AND DEFINED ENV{USERPROFILE})
        set(_discord_archive_default "$ENV{USERPROFILE}/Downloads/DiscordSocialSdk-${OMNISTATS_DISCORD_SDK_VERSION}.zip")
    endif()
    set(OMNISTATS_DISCORD_SDK_ARCHIVE "${_discord_archive_default}" CACHE FILEPATH "Path to the official Discord Social SDK ${OMNISTATS_DISCORD_SDK_VERSION} archive")
    if(NOT EXISTS "${OMNISTATS_DISCORD_SDK_ARCHIVE}")
        message(FATAL_ERROR "Discord Social SDK not found in third_party/discord-social-sdk and archive not found. Set OMNISTATS_DISCORD_SDK_ARCHIVE to its path.")
    endif()
    file(SHA256 "${OMNISTATS_DISCORD_SDK_ARCHIVE}" _discord_archive_sha256)
    if(NOT _discord_archive_sha256 STREQUAL OMNISTATS_DISCORD_SDK_SHA256)
        message(FATAL_ERROR "Discord Social SDK archive SHA-256 mismatch; expected ${OMNISTATS_DISCORD_SDK_SHA256}.")
    endif()
    set(_discord_extract_dir "${CMAKE_CURRENT_BINARY_DIR}/_deps/discord-social-sdk-${OMNISTATS_DISCORD_SDK_VERSION}")
    file(ARCHIVE_EXTRACT INPUT "${OMNISTATS_DISCORD_SDK_ARCHIVE}"
        DESTINATION "${_discord_extract_dir}"
        PATTERNS
            "discord_social_sdk/include/discordpp.h"
            "discord_social_sdk/include/cdiscord.h"
            "discord_social_sdk/bin/release/discord_partner_sdk.dll"
            "discord_social_sdk/lib/release/discord_partner_sdk.lib"
            "discord_social_sdk/License-Notices.txt"
    )
    set(OMNISTATS_DISCORD_SDK_ROOT "${_discord_extract_dir}/discord_social_sdk")
endif()

set(OMNISTATS_DISCORD_SDK_DLL "${OMNISTATS_DISCORD_SDK_ROOT}/bin/release/discord_partner_sdk.dll")
add_library(DiscordSocialSdk SHARED IMPORTED GLOBAL)
set_target_properties(DiscordSocialSdk PROPERTIES
    IMPORTED_IMPLIB "${OMNISTATS_DISCORD_SDK_ROOT}/lib/release/discord_partner_sdk.lib"
    IMPORTED_LOCATION "${OMNISTATS_DISCORD_SDK_DLL}"
    INTERFACE_INCLUDE_DIRECTORIES "${OMNISTATS_DISCORD_SDK_ROOT}/include"
)

function(omnistats_copy_discord_sdk target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${OMNISTATS_DISCORD_SDK_DLL}" "$<TARGET_FILE_DIR:${target}>/discord_partner_sdk.dll"
    )
endfunction()

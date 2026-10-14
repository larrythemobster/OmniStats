#pragma once

#include <string_view>

// Pins for the official curl-impersonate v1.5.6 Windows build. The DLL hashes are
// of the files inside the pinned archive; the MSI ships those exact files.
namespace CurlRuntime {
    inline constexpr std::string_view kArchiveUrl =
        "https://github.com/lexiforest/curl-impersonate/releases/download/v1.5.6/libcurl-impersonate-v1.5.6.x86_64-win32.tar.gz";
    inline constexpr std::string_view kArchiveSha256 = "fe8ce2488d5467fda6061b8b130b5834bc30cdfff40712692e8c5685dbbda6c7";
    inline constexpr std::string_view kLibcurlDllName = "libcurl-impersonate.dll";
    inline constexpr std::string_view kLibcurlDllSha256 = "97f1e2988e2edd296d902a4bdc4f12d61b3b565ff2a320fff72d65075afb9d33";
    inline constexpr std::string_view kZlibDllName = "zlib.dll";
    inline constexpr std::string_view kZlibDllSha256 = "be040a8719af7ecc809df37b2a7f1b8997dba038f8ca774ee58e7384b37b58da";
} // namespace CurlRuntime

#include "network/http/CurlImpersonate.hpp"
#include "network/http/CurlRuntime.hpp"
#include "utils/FileHash.hpp"
#include "utils/Storage.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <memory>
#include <system_error>

namespace {
    struct FileHandleCloser {
        void operator()(HANDLE handle) const noexcept {
            if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
                CloseHandle(handle);
            }
        }
    };
    using UniqueFileHandle = std::unique_ptr<void, FileHandleCloser>;

    // The returned handle omits FILE_SHARE_WRITE and FILE_SHARE_DELETE, so the verified file
    // cannot be overwritten or renamed until the handle is released after LoadLibrary.
    UniqueFileHandle OpenVerifiedFile(const std::string& path, std::string_view expectedSha256) {
        HANDLE raw = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (raw == INVALID_HANDLE_VALUE) {
            return {};
        }
        UniqueFileHandle handle(raw);
        std::string actual = CalculateSHA256OfHandle(handle.get());
        std::transform(actual.begin(), actual.end(), actual.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (actual != expectedSha256) {
            return {};
        }
        return handle;
    }

    std::string AbsolutePath(const std::string& path) {
        std::error_code ec;
        const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
        return ec ? path : absolute.string();
    }

    HMODULE LoadFromOwnDirectory(const std::string& absolutePath) {
        return LoadLibraryExA(absolutePath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    }
} // namespace

CurlImpersonate::CurlImpersonate() = default;

CurlImpersonate::~CurlImpersonate() {
    if (m_hDll) {
        FreeLibrary(m_hDll);
        m_hDll = nullptr;
    }
    if (m_hZlib) {
        FreeLibrary(m_hZlib);
        m_hZlib = nullptr;
    }
}

CurlImpersonate::CurlImpersonate(CurlImpersonate&& other) noexcept
    : easy_init(other.easy_init),
      easy_cleanup(other.easy_cleanup),
      easy_setopt(other.easy_setopt),
      easy_perform(other.easy_perform),
      easy_getinfo(other.easy_getinfo),
      easy_escape(other.easy_escape),
      free_ptr(other.free_ptr),
      slist_append(other.slist_append),
      slist_free_all(other.slist_free_all),
      easy_impersonate(other.easy_impersonate),
      m_hDll(other.m_hDll),
      m_hZlib(other.m_hZlib),
      m_loaded(other.m_loaded),
      m_attempted(other.m_attempted) {
    other.m_hDll = nullptr;
    other.m_hZlib = nullptr;
    other.m_loaded = false;
}

CurlImpersonate& CurlImpersonate::operator=(CurlImpersonate&& other) noexcept {
    if (this == &other) return *this;
    if (m_hDll) FreeLibrary(m_hDll);
    if (m_hZlib) FreeLibrary(m_hZlib);
    easy_init = other.easy_init;
    easy_cleanup = other.easy_cleanup;
    easy_setopt = other.easy_setopt;
    easy_perform = other.easy_perform;
    easy_getinfo = other.easy_getinfo;
    easy_escape = other.easy_escape;
    free_ptr = other.free_ptr;
    slist_append = other.slist_append;
    slist_free_all = other.slist_free_all;
    easy_impersonate = other.easy_impersonate;
    m_hDll = other.m_hDll;
    m_hZlib = other.m_hZlib;
    m_loaded = other.m_loaded;
    m_attempted = other.m_attempted;
    other.m_hDll = nullptr;
    other.m_hZlib = nullptr;
    other.m_loaded = false;
    return *this;
}

std::string CurlImpersonate::GetDLLPath() {
    return AbsolutePath(Storage::GetDataDirectory() + std::string(CurlRuntime::kLibcurlDllName));
}

std::string CurlImpersonate::GetZlibPath() {
    return AbsolutePath(Storage::GetDataDirectory() + std::string(CurlRuntime::kZlibDllName));
}

bool CurlImpersonate::EnsureAvailable() {
    if (m_loaded) return true;
    if (m_attempted) return false;
    m_attempted = true;
    return LoadDLL();
}

bool CurlImpersonate::LoadDLL() {
    const std::string dllPath = GetDLLPath();
    const std::string zlibPath = GetZlibPath();

    UniqueFileHandle zlibLock = OpenVerifiedFile(zlibPath, CurlRuntime::kZlibDllSha256);
    UniqueFileHandle dllLock = OpenVerifiedFile(dllPath, CurlRuntime::kLibcurlDllSha256);
    if (!zlibLock || !dllLock) {
        std::cout << "[CurlImpersonate] Required network runtime files are missing or invalid. Please repair or reinstall OmniStats.\n";
        return false;
    }

    m_hZlib = LoadFromOwnDirectory(zlibPath);
    if (!m_hZlib) {
        std::cout << "[CurlImpersonate] Failed to load zlib: " << GetLastError() << "\n";
        return false;
    }
    m_hDll = LoadFromOwnDirectory(dllPath);
    if (!m_hDll) {
        std::cout << "[CurlImpersonate] Failed to load DLL: " << GetLastError() << "\n";
        return false;
    }

    easy_init = reinterpret_cast<pfn_curl_easy_init>(GetProcAddress(m_hDll, "curl_easy_init"));
    easy_cleanup = reinterpret_cast<pfn_curl_easy_cleanup>(GetProcAddress(m_hDll, "curl_easy_cleanup"));
    easy_setopt = reinterpret_cast<pfn_curl_easy_setopt>(GetProcAddress(m_hDll, "curl_easy_setopt"));
    easy_perform = reinterpret_cast<pfn_curl_easy_perform>(GetProcAddress(m_hDll, "curl_easy_perform"));
    easy_getinfo = reinterpret_cast<pfn_curl_easy_getinfo>(GetProcAddress(m_hDll, "curl_easy_getinfo"));
    easy_escape = reinterpret_cast<pfn_curl_easy_escape>(GetProcAddress(m_hDll, "curl_easy_escape"));
    free_ptr = reinterpret_cast<pfn_curl_free>(GetProcAddress(m_hDll, "curl_free"));
    slist_append = reinterpret_cast<pfn_curl_slist_append>(GetProcAddress(m_hDll, "curl_slist_append"));
    slist_free_all = reinterpret_cast<pfn_curl_slist_free_all>(GetProcAddress(m_hDll, "curl_slist_free_all"));
    easy_impersonate = reinterpret_cast<pfn_curl_easy_impersonate>(GetProcAddress(m_hDll, "curl_easy_impersonate"));

    if (!easy_init || !easy_cleanup || !easy_setopt || !easy_perform || !easy_getinfo || !slist_append || !slist_free_all || !easy_impersonate) {
        std::cout << "[CurlImpersonate] Failed to load required functions!\n";
        FreeLibrary(m_hDll);
        m_hDll = nullptr;
        return false;
    }

    m_loaded = true;
    std::cout << "[CurlImpersonate] DLL loaded successfully.\n";
    return true;
}

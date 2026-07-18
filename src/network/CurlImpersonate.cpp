#include "CurlImpersonate.hpp"
#include "core/Storage.hpp"
#include <curl/curl.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <algorithm>

#include <windows.h>
#include "core/FileHash.hpp"

static const char* EXPECTED_DLL_HASH = "97f1e2988e2edd296d902a4bdc4f12d61b3b565ff2a320fff72d65075afb9d33";

CurlImpersonate& CurlImpersonate::Instance() {
    static CurlImpersonate inst;
    return inst;
}

std::string CurlImpersonate::GetDLLPath() {
    return Storage::GetDataDirectory() + "libcurl-impersonate.dll";
}

std::string CurlImpersonate::GetZlibPath() {
    return Storage::GetDataDirectory() + "zlib.dll";
}

// EnsureAvailable checks for libcurl-impersonate.dll and zlib.dll in the AppData directory.
// If they are missing or invalid, it returns false and logs a repair warning.

bool CurlImpersonate::EnsureAvailable() {
    if (m_loaded) return true;
    if (m_attempted) return false;
    m_attempted = true;

    std::string dllPath = GetDLLPath();
    std::string zlibPath = GetZlibPath();

    bool dllExists = (GetFileAttributesA(dllPath.c_str()) != INVALID_FILE_ATTRIBUTES);
    bool zlibExists = (GetFileAttributesA(zlibPath.c_str()) != INVALID_FILE_ATTRIBUTES);

    if (!dllExists || !zlibExists) {
        std::cout << "[CurlImpersonate] Required network runtime files are missing or invalid. Please repair or reinstall OmniStats.\n";
        return false;
    }

    // Validate hash if expected hash is available
    std::string actualHash = CalculateSHA256(dllPath);
    std::transform(actualHash.begin(), actualHash.end(), actualHash.begin(), ::tolower);
    if (actualHash != EXPECTED_DLL_HASH) {
        std::cout << "[CurlImpersonate] Required network runtime files are missing or invalid. Please repair or reinstall OmniStats. (DLL hash mismatch)\n";
        return false;
    }

    return LoadDLL();
}

bool CurlImpersonate::LoadDLL() {
    std::string dllPath = GetDLLPath();
    std::string zlibPath = GetZlibPath();

    // Load zlib first if present (dependency)
    if (GetFileAttributesA(zlibPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        m_hZlib = LoadLibraryA(zlibPath.c_str());
    }

    m_hDll = LoadLibraryA(dllPath.c_str());
    if (!m_hDll) {
        std::cout << "[CurlImpersonate] Failed to load DLL: " << GetLastError() << "\n";
        return false;
    }

    // Load function pointers
    easy_init = (pfn_curl_easy_init)GetProcAddress(m_hDll, "curl_easy_init");
    easy_cleanup = (pfn_curl_easy_cleanup)GetProcAddress(m_hDll, "curl_easy_cleanup");
    easy_setopt = (pfn_curl_easy_setopt)GetProcAddress(m_hDll, "curl_easy_setopt");
    easy_perform = (pfn_curl_easy_perform)GetProcAddress(m_hDll, "curl_easy_perform");
    easy_getinfo = (pfn_curl_easy_getinfo)GetProcAddress(m_hDll, "curl_easy_getinfo");
    easy_escape = (pfn_curl_easy_escape)GetProcAddress(m_hDll, "curl_easy_escape");
    free_ptr = (pfn_curl_free)GetProcAddress(m_hDll, "curl_free");
    slist_append = (pfn_curl_slist_append)GetProcAddress(m_hDll, "curl_slist_append");
    slist_free_all = (pfn_curl_slist_free_all)GetProcAddress(m_hDll, "curl_slist_free_all");
    easy_impersonate = (pfn_curl_easy_impersonate)GetProcAddress(m_hDll, "curl_easy_impersonate");

    if (!easy_init || !easy_perform || !easy_setopt || !easy_impersonate) {
        std::cout << "[CurlImpersonate] Failed to load required functions!\n";
        FreeLibrary(m_hDll);
        m_hDll = nullptr;
        return false;
    }

    m_loaded = true;
    std::cout << "[CurlImpersonate] DLL loaded successfully.\n";
    return true;
}

bool CurlImpersonate::DownloadAndExtract() {
    // Runtime repair is handled by the installer/updater path
    return false;
}

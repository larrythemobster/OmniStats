#pragma once
#include <string>
#include <windows.h>

// We use void* and int for the function pointers to avoid
// clashing with the real curl.h types (which are enums).
// These are only used for LoadLibrary/GetProcAddress dynamic dispatch.
struct ci_curl_slist;

// Standard curl function signatures (using opaque types to avoid redefinition)
typedef void* (*pfn_curl_easy_init)();
typedef void (*pfn_curl_easy_cleanup)(void*);
typedef int (*pfn_curl_easy_setopt)(void*, int, ...);
typedef int (*pfn_curl_easy_perform)(void*);
typedef int (*pfn_curl_easy_getinfo)(void*, int, ...);
typedef char* (*pfn_curl_easy_escape)(void*, const char*, int);
typedef void (*pfn_curl_free)(void*);
typedef void* (*pfn_curl_slist_append)(void*, const char*);
typedef void (*pfn_curl_slist_free_all)(void*);
// curl-impersonate specific
typedef int (*pfn_curl_easy_impersonate)(void*, const char*, int);

// Curl option constants (from curl.h)
#define CI_CURLOPT_URL 10002
#define CI_CURLOPT_WRITEFUNCTION 20011
#define CI_CURLOPT_WRITEDATA 10001
#define CI_CURLOPT_HTTPHEADER 10023
#define CI_CURLOPT_TIMEOUT 13
#define CI_CURLOPT_SSL_VERIFYPEER 64
#define CI_CURLOPT_SSL_VERIFYHOST 81
#define CI_CURLOPT_SSL_OPTIONS 216
#define CI_CURLSSLOPT_NATIVE_CA (1 << 4)
#define CI_CURLOPT_FOLLOWLOCATION 52
#define CI_CURLOPT_NOPROGRESS 43
#define CI_CURLOPT_XFERINFOFUNCTION 20219
#define CI_CURLOPT_XFERINFODATA 10057
#define CI_CURLINFO_RESPONSE_CODE 0x200002

class CurlImpersonate {
  public:
    static CurlImpersonate& Instance();

    // Returns true if the DLL is loaded and ready
    bool IsReady() const {
        return m_loaded;
    }

    // Verifies that the required runtime DLLs are present and compatible.
    [[nodiscard]] bool EnsureAvailable();

    // Function pointers
    pfn_curl_easy_init easy_init = nullptr;
    pfn_curl_easy_cleanup easy_cleanup = nullptr;
    pfn_curl_easy_setopt easy_setopt = nullptr;
    pfn_curl_easy_perform easy_perform = nullptr;
    pfn_curl_easy_getinfo easy_getinfo = nullptr;
    pfn_curl_easy_escape easy_escape = nullptr;
    pfn_curl_free free_ptr = nullptr;
    pfn_curl_slist_append slist_append = nullptr;
    pfn_curl_slist_free_all slist_free_all = nullptr;
    pfn_curl_easy_impersonate easy_impersonate = nullptr;

  private:
    CurlImpersonate() = default;
    bool LoadDLL();
    bool DownloadAndExtract();
    std::string GetDLLPath();
    std::string GetZlibPath();

    HMODULE m_hDll = nullptr;
    HMODULE m_hZlib = nullptr;
    bool m_loaded = false;
    bool m_attempted = false;
};

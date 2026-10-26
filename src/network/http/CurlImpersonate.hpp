#pragma once

#include <curl/curl.h>
#include <string>
#include <windows.h>

// Option and info constants verified against curl-impersonate DLL expectations
static_assert(CURLOPT_URL == 10002);
static_assert(CURLOPT_WRITEFUNCTION == 20011);
static_assert(CURLOPT_WRITEDATA == 10001);
static_assert(CURLOPT_HTTPHEADER == 10023);
static_assert(CURLOPT_POST == 47);
static_assert(CURLOPT_POSTFIELDS == 10015);
static_assert(CURLOPT_POSTFIELDSIZE == 60);
static_assert(CURLOPT_TIMEOUT == 13);
static_assert(CURLOPT_SSL_VERIFYPEER == 64);
static_assert(CURLOPT_SSL_VERIFYHOST == 81);
static_assert(CURLOPT_SSL_OPTIONS == 216);
static_assert(CURLSSLOPT_NATIVE_CA == (1 << 4));
static_assert(CURLOPT_FOLLOWLOCATION == 52);
static_assert(CURLOPT_NOPROGRESS == 43);
static_assert(CURLOPT_XFERINFOFUNCTION == 20219);
static_assert(CURLOPT_XFERINFODATA == 10057);
static_assert(CURLINFO_RESPONSE_CODE == 0x200002);
static_assert(CURLOPT_HEADERFUNCTION == 20079);
static_assert(CURLOPT_HEADERDATA == 10029);
static_assert(CURLOPT_ACCEPT_ENCODING == 10102);

// Standard curl function signatures using real curl.h types
using pfn_curl_easy_init = CURL* (*)();
using pfn_curl_easy_cleanup = void (*)(CURL*);
using pfn_curl_easy_setopt = CURLcode (*)(CURL*, CURLoption, ...);
using pfn_curl_easy_perform = CURLcode (*)(CURL*);
using pfn_curl_easy_getinfo = CURLcode (*)(CURL*, CURLINFO, ...);
using pfn_curl_easy_escape = char* (*)(CURL*, const char*, int);
using pfn_curl_free = void (*)(void*);
using pfn_curl_slist_append = struct curl_slist* (*)(struct curl_slist*, const char*);
using pfn_curl_slist_free_all = void (*)(struct curl_slist*);
// curl-impersonate specific
using pfn_curl_easy_impersonate = int (*)(CURL*, const char*, int);

class CurlImpersonate {
  public:
    static CurlImpersonate& Instance();

    // Returns true if the DLL is loaded and ready
    bool IsReady() const {
        return m_loaded;
    }

    // Verifies that the required runtime DLLs are present and compatible.
    [[nodiscard]] bool EnsureAvailable();

    // Function pointers bound with real types
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

    void SetReadyForTests(bool ready) {
        m_loaded = ready;
    }

  private:
    CurlImpersonate() = default;
    bool LoadDLL();
    std::string GetDLLPath();
    std::string GetZlibPath();

    HMODULE m_hDll = nullptr;
    HMODULE m_hZlib = nullptr;
    bool m_loaded = false;
    bool m_attempted = false;
};

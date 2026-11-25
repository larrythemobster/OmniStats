#include "network/http/CurlHttpClient.hpp"
#include "network/http/CurlRequest.hpp"

namespace Http {
    bool CurlHttpClient::IsAvailable() const {
        return true;
    }

    HttpResponse CurlHttpClient::Request(const HttpRequest& request) {
        const Detail::CurlOperations ops{curl_easy_init, curl_easy_cleanup, curl_easy_setopt,
                                         curl_easy_perform, curl_easy_getinfo,
                                         curl_slist_append, curl_slist_free_all, nullptr, false,
                                         curl_mime_init, curl_mime_addpart, curl_mime_name,
                                         curl_mime_filedata, curl_mime_free, curl_easy_strerror};
        return Detail::ExecuteCurl(request, ops);
    }
}

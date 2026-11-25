#include "network/http/CurlImpersonateHttpClient.hpp"
#include "network/http/CurlRequest.hpp"

namespace Http {
    CurlImpersonateHttpClient::CurlImpersonateHttpClient(std::shared_ptr<CurlImpersonate> backend)
        : m_backend(backend ? std::move(backend) : std::make_shared<CurlImpersonate>()) {}

    bool CurlImpersonateHttpClient::IsAvailable() const {
        return m_backend && (m_backend->IsReady() || m_backend->EnsureAvailable());
    }

    HttpResponse CurlImpersonateHttpClient::Request(const HttpRequest& request) {
        if (!IsAvailable()) {
            HttpResponse response;
            response.transportError = -1;
            response.errorMessage = "curl-impersonate runtime not available";
            return response;
        }
        const Detail::CurlOperations ops{m_backend->easy_init, m_backend->easy_cleanup, m_backend->easy_setopt,
                                         m_backend->easy_perform, m_backend->easy_getinfo,
                                         m_backend->slist_append, m_backend->slist_free_all,
                                         m_backend->easy_impersonate, true};
        return Detail::ExecuteCurl(request, ops);
    }
}

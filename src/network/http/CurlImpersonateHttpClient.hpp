#pragma once

#include "network/http/IHttpClient.hpp"
#include <memory>

class CurlImpersonate;

namespace Http {

    class CurlImpersonateHttpClient : public IHttpClient {
      public:
        explicit CurlImpersonateHttpClient(std::shared_ptr<CurlImpersonate> backend = nullptr);
        ~CurlImpersonateHttpClient() override = default;

        bool IsAvailable() const override;
        HttpResponse Request(const HttpRequest& request) override;

        std::shared_ptr<CurlImpersonate> GetBackend() const {
            return m_backend;
        }

      private:
        std::shared_ptr<CurlImpersonate> m_backend;
    };
} // namespace Http

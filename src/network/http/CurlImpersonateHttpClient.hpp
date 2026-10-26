#pragma once

#include "network/http/IHttpClient.hpp"

class CurlImpersonate;

namespace Http {

    class CurlImpersonateHttpClient : public IHttpClient {
      public:
        explicit CurlImpersonateHttpClient(CurlImpersonate* backend = nullptr);
        ~CurlImpersonateHttpClient() override = default;

        bool IsAvailable() const override;
        HttpResponse Request(const HttpRequest& request) override;

      private:
        CurlImpersonate* m_backend = nullptr;
    };

} // namespace Http

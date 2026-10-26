#pragma once

#include "network/http/IHttpClient.hpp"

namespace Http {

    class CurlHttpClient : public IHttpClient {
      public:
        CurlHttpClient() = default;
        ~CurlHttpClient() override = default;

        bool IsAvailable() const override;
        HttpResponse Request(const HttpRequest& request) override;
    };

} // namespace Http

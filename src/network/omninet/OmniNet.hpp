#pragma once

#include <functional>
#include <map>
#include <string>

// Transport to OmniStats services. Official builds link the private implementation;
// source builds link a stub where Available() is false and Send() always fails.
namespace OmniNet {

    enum class Op {
        AuthStart,
        AuthToken,
        AuthExchangeKey,
        AuthRefresh,
        AuthLogout,
        Ranks,
        TournamentSchedule,
        ClientConfig,
        Telemetry,
        Crash,
    };

    struct Request {
        Op op = Op::ClientConfig;
        // JSON body for every op except Crash, where it is the raw minidump bytes.
        std::string payload;
        std::string accessToken;
        std::string devicePublicId;
        // TournamentSchedule: "region".
        std::map<std::string, std::string> query;
        // Crash: "client_uuid", "version".
        std::map<std::string, std::string> fields;
        long timeoutSeconds = 10;
        // Polled during the transfer; returning true aborts it.
        std::function<bool()> shouldAbort;
    };

    struct Response {
        // Nonzero when no usable server response arrived (network failure, abort, undecodable reply).
        int transportError = 0;
        long status = 0;
        std::string body;
        int retryAfterSeconds = 0;
        // The service no longer accepts this build; callers must not treat it as an auth failure.
        bool updateRequired = false;
    };

    bool Available();
    Response Send(const Request& request);
    // Sticky for the process lifetime once any response reported updateRequired.
    bool UpdateRequired();

    using Transport = std::function<Response(const Request&)>;
    // Tests only: routes Send() through transport and makes Available() true; empty restores the linked backend.
    void SetTransportForTesting(Transport transport);
    void ResetUpdateRequiredForTesting();

} // namespace OmniNet

// Implemented by exactly one linked target: the private module or the public stub.
namespace OmniNetBackend {

    bool Available();
    OmniNet::Response Send(const OmniNet::Request& request);

} // namespace OmniNetBackend

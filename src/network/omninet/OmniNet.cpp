#include "network/omninet/OmniNet.hpp"

#include <atomic>
#include <mutex>

namespace OmniNet {

    namespace {
        std::mutex g_transportMutex;
        Transport g_testTransport;
        std::atomic<bool> g_updateRequired{false};
    } // namespace

    bool Available() {
        std::lock_guard<std::mutex> lock(g_transportMutex);
        if (g_testTransport) {
            return true;
        }
#ifdef OMNISTATS_TEST_ENVIRONMENT
        return false;
#else
        return OmniNetBackend::Available();
#endif
    }

    Response Send(const Request& request) {
        Transport transport;
        {
            std::lock_guard<std::mutex> lock(g_transportMutex);
            transport = g_testTransport;
        }

        Response res;
        if (transport) {
            res = transport(request);
        } else {
#ifdef OMNISTATS_TEST_ENVIRONMENT
            // Test builds never reach the live service; unmocked calls fail like the stub backend.
            res.transportError = -1;
#else
            res = OmniNetBackend::Send(request);
#endif
        }

        if (res.updateRequired || res.status == 426) {
            res.updateRequired = true;
            g_updateRequired.store(true, std::memory_order_relaxed);
        }

        return res;
    }

    bool UpdateRequired() {
        return g_updateRequired.load(std::memory_order_relaxed);
    }

    void SetTransportForTesting(Transport transport) {
        std::lock_guard<std::mutex> lock(g_transportMutex);
        g_testTransport = std::move(transport);
    }

    void ResetUpdateRequiredForTesting() {
        g_updateRequired.store(false, std::memory_order_relaxed);
    }

} // namespace OmniNet

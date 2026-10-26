#include "network/omninet/OmniNet.hpp"

#include <atomic>
#include <mutex>

namespace OmniNet {

    namespace {
        std::mutex g_transportMutex;
        Transport g_testTransport;
        Transport g_defaultTransport;
        std::atomic<bool> g_updateRequired{false};
    } // namespace

    bool Available() {
        std::lock_guard<std::mutex> lock(g_transportMutex);
        if (g_testTransport) {
            return true;
        }
        if (g_defaultTransport) {
            return false;
        }
        return OmniNetBackend::Available();
    }

    Response Send(const Request& request) {
        Transport transport;
        {
            std::lock_guard<std::mutex> lock(g_transportMutex);
            transport = g_testTransport;
            if (!transport) {
                transport = g_defaultTransport;
            }
        }

        Response res;
        if (transport) {
            res = transport(request);
        } else {
            res = OmniNetBackend::Send(request);
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

    void SetDefaultTransportForTesting(Transport transport) {
        std::lock_guard<std::mutex> lock(g_transportMutex);
        g_defaultTransport = std::move(transport);
    }

    void ResetUpdateRequiredForTesting() {
        g_updateRequired.store(false, std::memory_order_relaxed);
    }

} // namespace OmniNet

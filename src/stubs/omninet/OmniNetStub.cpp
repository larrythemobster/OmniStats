#include "network/omninet/OmniNet.hpp"

namespace OmniNetBackend {

    bool Available() {
        return false;
    }

    OmniNet::Response Send(const OmniNet::Request& /*request*/) {
        OmniNet::Response res;
        res.transportError = -1;
        return res;
    }

} // namespace OmniNetBackend

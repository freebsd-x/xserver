#include "xserver/priv_broker.h"

// Test stub for the service_roundtrip harness: there is no xsessiond daemon in
// the test, so every privileged op is "unreachable" and consumers fall back to
// their unprivileged path. The real client lives in xsessiond_client.cc.
namespace fbx::xserver {

int runPrivilegedOp(std::string_view /*verb*/, std::string_view /*arg*/) noexcept {
    return kBrokerUnreachable;
}

}  // namespace fbx::xserver

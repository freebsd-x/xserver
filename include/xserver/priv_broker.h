#pragma once

#include <string_view>

namespace fbx::xserver {

// Returned by runPrivilegedOp when the xsessiond broker can't be reached (not
// running / socket absent) — distinct from a negative errno the daemon returns
// on a real failure. Callers fall back to a best-effort unprivileged path.
inline constexpr int kBrokerUnreachable = -1000;

// Ask the root xsessiond daemon to perform an allowlisted privileged op (see
// priv_ops.h). verb/arg must pass privops::validateOp — the daemon re-validates
// and refuses anything else. Returns 0 on success, a negative errno on a real
// failure, or kBrokerUnreachable if the daemon isn't running. Definition in
// xsessiond_client.cc (unprivileged compositor side).
int runPrivilegedOp(std::string_view verb, std::string_view arg) noexcept;

}  // namespace fbx::xserver

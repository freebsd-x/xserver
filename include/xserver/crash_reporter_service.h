#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>

namespace fbx::xserver {

// com.freebsdx.CrashReporter — serves the crash spool written by the crash
// handler (~/.freebsdx/crashes/<proc>-<pid>.crash) to the shell's Crash Reports
// panel. Stateless: each request re-scans the spool directory.
//   ListReports  (uint32_t page)        → CrashReportPage (3 per page)
//   DeleteReport (CrashDeleteRequest)   → removes one .crash file (no reply)
//   ClearAll                            → removes all .crash files (no reply)
class CrashReporterService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver

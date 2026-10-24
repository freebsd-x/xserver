#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>
#include <string>

namespace fbx::xserver {

// com.freebsdx.DiskUsage — a du-style per-directory size scanner.
//
// Scan(path) walks the tree in the background and, when done, broadcasts
// ScanComplete. GetResults(page) returns the immediate children of the scan root
// with their recursive sizes, sorted largest-first, paged 3 at a time.
class DiskUsageService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;

    // Run a scan synchronously (test seam; the handler runs this on a thread).
    static void scanNow(const std::string& path) noexcept;
    [[nodiscard]] static uint32_t resultCount() noexcept;
};

}  // namespace fbx::xserver

#pragma once

#include "shared/protocol.h"
#include <memory>
#include <string>
#include <vector>

namespace fbx::xserver {

class IpcSocket;

// PrintService — CUPS printing portal.
//
// Wraps lpstat / lp / cancel via popen(). No libcups dependency.
// All file access is validated against $HOME and /tmp (portal sandbox rule).
//
class PrintService {
public:
    static void ProcessMessage(std::shared_ptr<IpcSocket> client,
                               const shared::XBusMessage& msg) noexcept;

private:
    static void handleListPrinters(std::shared_ptr<IpcSocket> client,
                                   const shared::XBusMessage& msg) noexcept;
    static void handlePrintFile(std::shared_ptr<IpcSocket> client,
                                const shared::XBusMessage& msg) noexcept;
    static void handleGetJobStatus(std::shared_ptr<IpcSocket> client,
                                   const shared::XBusMessage& msg) noexcept;
    static void handleCancelJob(std::shared_ptr<IpcSocket> client,
                                const shared::XBusMessage& msg) noexcept;
    static void handleShowDialog(std::shared_ptr<IpcSocket> client,
                                 const shared::XBusMessage& msg) noexcept;

    // Send a PrintResponse back to the client.
    static void sendResponse(std::shared_ptr<IpcSocket> client,
                             const shared::XBusMessage& req,
                             bool success, uint32_t job_id,
                             const std::string& message) noexcept;

    // Run a command and return its stdout output.
    static std::string runCmd(const std::string& cmd) noexcept;

    // Parse printer list from `lpstat -p -l` output.
    struct ParsedPrinter {
        std::string name;
        std::string description;
        std::string location;
        bool        is_default{false};
        uint32_t    state{0};  // 0=idle, 1=processing, 2=stopped
    };
    static std::vector<ParsedPrinter> parsePrinters() noexcept;

    // Get the system default printer name (empty if none).
    static std::string defaultPrinter() noexcept;

    // Validate file path is under $HOME or /tmp.
    static bool pathAllowed(const std::string& path) noexcept;

    // Submit a print job; returns job_id (0 on failure) and sets message.
    static uint32_t submitJob(const shared::PrintRequest& req,
                              std::string& message) noexcept;
};

} // namespace fbx::xserver

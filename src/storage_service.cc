#include "xserver/storage_service.h"
#include "xserver/ipc_socket.h"
#include "xserver/async_shell.h"
#include "xserver/geom_parse.h"
#include <iostream>
#include <cstring>
#include <string>
#include <span>

namespace fbx::xserver {

bool StorageService::ProcessMessage(std::shared_ptr<IpcSocket> client, const shared::XBusMessage& msg) noexcept {
    if (std::strcmp(msg.service_name_, "com.freebsdx.Storage") != 0) return false;

    std::cout << "[StorageService] Intercepted Storage service request (opcode: " << msg.opcode_ << ")\n";

    shared::XBusMessage response = msg;

    if (msg.opcode_ == 0x01) { // GetDisks
        // The real disks, from GEOM (readable without root).
        std::string disks = geom::diskSummary(spawnCapture("geom disk list 2>/dev/null"));
        std::memset(response.payload_, 0, sizeof(response.payload_));
        std::strncpy(reinterpret_cast<char*>(response.payload_), disks.c_str(), sizeof(response.payload_) - 1);
    } else if (msg.opcode_ == 0x02) { // InstallToDisk
        std::string target_disk(reinterpret_cast<const char*>(msg.payload_),
                                strnlen(reinterpret_cast<const char*>(msg.payload_), sizeof(msg.payload_)));
        // A desktop session never repartitions disks: installing runs as root
        // from the installer. This used to print a dry run and report
        // success, which would tell an installer UI a system was installed.
        std::cerr << "[StorageService] InstallToDisk(" << target_disk
                  << ") refused: installing needs the root installer, not the desktop session\n";
        uint32_t failure = 0;
        std::memset(response.payload_, 0, sizeof(response.payload_));
        std::memcpy(response.payload_, &failure, sizeof(failure));
    } else {
        std::cerr << "[StorageService] -> Unknown opcode requested.\n";
    }

    // Send Response back to client
    std::span<const uint8_t> resp_span(reinterpret_cast<const uint8_t*>(&response), sizeof(response));
    (void)client->sendMessage(resp_span);

    return true;
}

} // namespace fbx::xserver

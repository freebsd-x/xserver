#include "xserver/storage_service.h"
#include "xserver/ipc_socket.h"
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
        std::cout << "[StorageService] -> Fulfilling GetDisks request.\n";
        // Mock disk list: comma-separated list of disk names
        std::string disks = "nvd0 (1TB NVMe),ada0 (500GB SSD)";
        std::strncpy(reinterpret_cast<char*>(response.payload_), disks.c_str(), sizeof(response.payload_) - 1);
        response.payload_[sizeof(response.payload_) - 1] = '\0';
    } else if (msg.opcode_ == 0x02) { // InstallToDisk
        std::string target_disk(reinterpret_cast<const char*>(msg.payload_));
        std::cout << "[StorageService] -> Fulfilling InstallToDisk request for target: " << target_disk << "\n";
        
        // Mock installation logic for safety during development
        std::cout << "   [DRY RUN] Executing: gpart destroy -F " << target_disk << "\n";
        std::cout << "   [DRY RUN] Executing: gpart create -s gpt " << target_disk << "\n";
        std::cout << "   [DRY RUN] Executing: gpart add -t efi -s 260M " << target_disk << "\n";
        std::cout << "   [DRY RUN] Executing: gpart add -t freebsd-zfs " << target_disk << "\n";
        std::cout << "   [DRY RUN] Executing: zpool create zroot /dev/" << target_disk << "p2\n";
        std::cout << "   [DRY RUN] Copying rootfs to /mnt...\n";
        std::cout << "   [DRY RUN] Configuring boot environment...\n";
        
        // Return success code
        uint32_t success = 1;
        std::memcpy(response.payload_, &success, sizeof(success));
    } else {
        std::cerr << "[StorageService] -> Unknown opcode requested.\n";
    }

    // Send Response back to client
    std::span<const uint8_t> resp_span(reinterpret_cast<const uint8_t*>(&response), sizeof(response));
    (void)client->sendMessage(resp_span);

    return true;
}

} // namespace fbx::xserver

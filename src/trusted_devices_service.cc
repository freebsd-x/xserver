#include "xserver/trusted_devices_service.h"
#include "xserver/ipc_socket.h"
#include <cstring>
#include <iostream>

namespace fbx::xserver { void BroadcastXBusMessage(const shared::XBusMessage& msg); }

namespace fbx::xserver {

std::mutex                                  TrustedDevicesService::mutex_;
std::vector<TrustedDevicesService::Device>  TrustedDevicesService::devices_;
bool                                        TrustedDevicesService::loaded_ = false;

void TrustedDevicesService::load() noexcept {
    if (loaded_) return;
    loaded_ = true;
    // Seed a plausible set of paired devices. A real implementation would read
    // the pairing/keychain database.
    devices_ = {
        {"Junghyun's iPhone", 1},
        {"Living Room TV",     0},
        {"Work Laptop",        1},
        {"AirPods Pro",        1},
        {"Kitchen HomePod",    0},
    };
}

void TrustedDevicesService::broadcastChanged() noexcept {
    shared::XBusMessage bcast{};
    bcast.magic_ = shared::XBUS_MAGIC;
    std::strncpy(bcast.service_name_, "com.freebsdx.TrustedDevices",
                 sizeof(bcast.service_name_) - 1);
    bcast.opcode_ = static_cast<uint32_t>(shared::TrustedDeviceOpcode::DevicesChanged);
    // No payload — the shell re-fetches the list.
    BroadcastXBusMessage(bcast);
}

bool TrustedDevicesService::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                           const shared::XBusMessage& msg) noexcept {
    if (std::strcmp(msg.service_name_, "com.freebsdx.TrustedDevices") != 0) return false;

    using Op = shared::TrustedDeviceOpcode;
    std::lock_guard<std::mutex> lk(mutex_);
    load();

    shared::XBusMessage response = msg;

    switch (static_cast<Op>(msg.opcode_)) {

    case Op::ListDevices: {
        uint32_t page = 0;
        std::memcpy(&page, msg.payload_, sizeof(page));
        const uint32_t total = static_cast<uint32_t>(devices_.size());

        shared::TrustedDevicePage pg{};
        pg.page_  = page;
        pg.total_ = total;
        if (page < total) {
            pg.valid_    = 1;
            pg.has_more_ = (page + 1 < total) ? 1u : 0u;
            std::strncpy(pg.device_name_, devices_[page].name.c_str(),
                         sizeof(pg.device_name_) - 1);
            pg.trusted_  = devices_[page].trusted;
        } else {
            pg.valid_    = 0;
            pg.has_more_ = 0;
        }
        std::memset(response.payload_, 0, sizeof(response.payload_));
        std::memcpy(response.payload_, &pg, sizeof(pg));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    case Op::SetTrust: {
        shared::TrustedDeviceRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.device_name_[sizeof(req.device_name_) - 1] = '\0';
        bool found = false;
        for (auto& d : devices_) {
            if (d.name == req.device_name_) {
                d.trusted = req.trusted_ ? 1u : 0u;
                found = true;
                break;
            }
        }
        std::cout << "[TrustedDevices] SetTrust '" << req.device_name_ << "' -> "
                  << (req.trusted_ ? "trusted" : "blocked")
                  << (found ? "" : " (not found)") << "\n";
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        if (found) broadcastChanged();
        break;
    }

    default:
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }

    return true;
}

} // namespace fbx::xserver

#include "xserver/usb_service.h"
#include "xserver/usb_list.h"
#include "xserver/async_shell.h"
#include "shared/logger.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace fbx::xserver {

namespace {

// Full enumeration: `usbconfig list` for the device set, then one
// dump_device_desc per device for vid/pid/class/manufacturer. posix_spawn, not
// popen (popen deadlocks in this threaded compositor — see async_shell.h).
std::vector<usb::UsbInfo> enumerate() {
    auto devs = usb::parseList(spawnCapture("usbconfig list 2>/dev/null"));
    for (auto& u : devs) {
        char cmd[96];
        std::snprintf(cmd, sizeof(cmd),
                      "usbconfig -d ugen%u.%u dump_device_desc 2>/dev/null", u.bus, u.addr);
        usb::parseDeviceDesc(spawnCapture(cmd), u);
    }
    return devs;
}

uint32_t devId(const usb::UsbInfo& u) noexcept { return (u.bus << 8) | u.addr; }

// UsbDevice.type_ vocabulary ("storage"/"hid"/"audio"/"hub"/"other").
const char* typeFor(const std::string& cls) noexcept {
    if (cls == "MSC") return "storage";
    if (cls == "HID") return "hid";
    if (cls == "AUD") return "audio";
    if (cls == "HUB") return "hub";
    return "other";
}

void reply(std::shared_ptr<IpcSocket>& client, const shared::XBusMessage& req,
           const char* service, uint32_t opcode, const void* page, size_t len) {
    shared::XBusMessage r{};
    r.transaction_id_ = req.transaction_id_;
    std::strncpy(r.service_name_, service, sizeof(r.service_name_) - 1);
    r.opcode_ = opcode;
    std::memcpy(r.payload_, page, len);
    client->sendMessage({reinterpret_cast<const uint8_t*>(&r), sizeof(r)});
}

}  // namespace

// ─── com.freebsdx.Usb (one device/page) ─────────────────────────────────────────
void UsbService::handleMessage(const shared::XBusMessage& msg,
                               std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::UsbOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListDevices: {
        uint32_t page = 0; std::memcpy(&page, msg.payload_, sizeof(page));
        auto devs = enumerate();

        shared::UsbDevicePage pg{};
        pg.page_  = page;
        pg.total_ = static_cast<uint32_t>(devs.size());
        if (page < devs.size()) {
            const usb::UsbInfo& u = devs[page];
            std::strncpy(pg.device_.name_, u.product.c_str(), sizeof(pg.device_.name_) - 1);
            std::strncpy(pg.device_.type_, typeFor(u.cls),    sizeof(pg.device_.type_) - 1);
            pg.device_.capacity_mb_ = 0;        // da-device correlation = follow-up
            pg.device_.addr_        = devId(u);
            pg.count_ = 1;
        }
        pg.has_more_ = (page + 1 < devs.size()) ? 1u : 0u;
        reply(client, msg, "com.freebsdx.Usb", static_cast<uint32_t>(O::ListDevices), &pg, sizeof(pg));
        break;
    }
    case O::Eject:
        LOG_INFO("UsbService: Eject requested (needs root/umount — not performed)");
        break;
    default: break;
    }
}

// ─── com.freebsdx.UsbDevices (three entries/page) ────────────────────────────────
void UsbDevicesService::handleMessage(const shared::XBusMessage& msg,
                                      std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::UsbDevOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListDevices: {
        uint32_t page = 0; std::memcpy(&page, msg.payload_, sizeof(page));
        auto devs = enumerate();

        shared::UsbDevPage pg{};
        pg.page_  = page;
        pg.total_ = static_cast<uint32_t>(devs.size());
        uint32_t start = page * 3, n = 0;
        for (; n < 3 && start + n < devs.size(); ++n) {
            const usb::UsbInfo& u = devs[start + n];
            shared::UsbDevEntry& e = pg.entries_[n];
            std::strncpy(e.name_,  u.product.c_str(), sizeof(e.name_) - 1);
            std::strncpy(e.class_, u.cls.c_str(),     sizeof(e.class_) - 1);
            e.vid_  = u.vid;
            e.pid_  = u.pid;
            e.port_ = devId(u);
        }
        pg.count_    = n;
        pg.has_more_ = (start + n < devs.size()) ? 1u : 0u;
        reply(client, msg, "com.freebsdx.UsbDevices", static_cast<uint32_t>(O::ListDevices), &pg, sizeof(pg));
        break;
    }
    case O::Rescan:
        // No persistent state to refresh — each ListDevices re-enumerates. Ack.
        reply(client, msg, "com.freebsdx.UsbDevices", static_cast<uint32_t>(O::Rescan), "", 1);
        break;
    default: break;
    }
}

// ─── com.freebsdx.UsbDevice (one detail page/device) ─────────────────────────────
void UsbDeviceService::handleMessage(const shared::XBusMessage& msg,
                                     std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::UsbDeviceOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListDevices: {
        uint32_t page = 0; std::memcpy(&page, msg.payload_, sizeof(page));
        auto devs = enumerate();

        shared::UsbDeviceDetailPage pg{};
        pg.page_  = page;
        pg.total_ = static_cast<uint32_t>(devs.size());
        if (page < devs.size()) {
            const usb::UsbInfo& u = devs[page];
            pg.valid_      = 1;
            pg.device_id_  = devId(u);
            pg.vendor_id_  = u.vid;
            pg.product_id_ = u.pid;
            pg.class_      = u.device_class;
            pg.ejectable_  = (u.cls == "MSC") ? 1u : 0u;
            std::strncpy(pg.product_,      u.product.c_str(),      sizeof(pg.product_) - 1);
            std::strncpy(pg.manufacturer_, u.manufacturer.c_str(), sizeof(pg.manufacturer_) - 1);
        }
        pg.has_more_ = (page + 1 < devs.size()) ? 1u : 0u;
        reply(client, msg, "com.freebsdx.UsbDevice", static_cast<uint32_t>(O::ListDevices), &pg, sizeof(pg));
        break;
    }
    case O::Eject:
        LOG_INFO("UsbDeviceService: Eject requested (needs root/umount — not performed)");
        break;
    default: break;
    }
}

}  // namespace fbx::xserver

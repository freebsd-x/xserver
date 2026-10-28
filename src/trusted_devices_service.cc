#include "xserver/trusted_devices_service.h"
#include "xserver/ipc_socket.h"
#include "xserver/user_files.h"
#include "xserver/xbus_reply.h"
#include "shared/logger.h"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace fbx::xserver {

namespace {

using namespace svc;

// /etc/bluetooth/hosts: "<bdaddr> <name> [aliases…]", '#' comments.
std::vector<std::string> knownDevices() {
    std::vector<std::string> names;
    std::istringstream in(userfiles::readFile("/etc/bluetooth/hosts").value_or(""));
    std::string line;
    while (std::getline(in, line)) {
        if (auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        std::istringstream f(line);
        std::string addr, name;
        if (f >> addr >> name && std::count(addr.begin(), addr.end(), ':') == 5) names.push_back(name);
    }
    return names;
}

}  // namespace

bool TrustedDevicesService::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                           const shared::XBusMessage& msg) noexcept {
    if (std::strcmp(msg.service_name_, "com.freebsdx.TrustedDevices") != 0) return false;
    using Op = shared::TrustedDeviceOpcode;
    constexpr const char* kName = "com.freebsdx.TrustedDevices";
    switch (static_cast<Op>(msg.opcode_)) {
    case Op::ListDevices: {
        auto devs = knownDevices();
        uint32_t page = readPage(msg);
        shared::TrustedDevicePage pg{};
        pg.page_  = page;
        pg.total_ = static_cast<uint32_t>(devs.size());
        if (page < devs.size()) {
            pg.valid_    = 1;
            pg.has_more_ = page + 1 < devs.size() ? 1u : 0u;
            std::strncpy(pg.device_name_, devs[page].c_str(), sizeof(pg.device_name_) - 1);
            pg.trusted_  = 0;   // pairing state is in root-only hcsecd.conf
        }
        reply(msg, client, kName, msg.opcode_, &pg);
        break;
    }
    case Op::SetTrust:
        LOG_WARN("TrustedDevicesService: pairing lives in /etc/bluetooth/hcsecd.conf (root only)");
        ack(msg, client, kName);
        break;
    default: break;
    }
    return true;
}

} // namespace fbx::xserver

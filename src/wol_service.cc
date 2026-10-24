#include "xserver/wol_service.h"
#include "xserver/net_wol.h"
#include "shared/logger.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace fbx::xserver {

namespace {
std::string targetsPath() {
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "/tmp") + "/.freebsdx-wol";
}

std::vector<std::pair<std::string, std::string>> loadTargets() {
    std::vector<std::pair<std::string, std::string>> out;
    std::ifstream f(targetsPath());
    std::string line;
    while (std::getline(f, line)) {
        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        out.emplace_back(line.substr(0, tab), line.substr(tab + 1));
    }
    return out;
}

void saveTargets(const std::vector<std::pair<std::string, std::string>>& t) {
    std::ofstream f(targetsPath());
    for (const auto& [name, mac] : t) f << name << '\t' << mac << '\n';
}

// Broadcast the 102-byte magic packet to 255.255.255.255:9. Returns true on send.
bool sendMagic(const uint8_t mac[6]) {
    auto pkt = wol::buildMagicPacket(mac);
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(9);
    addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    ssize_t n = ::sendto(fd, pkt.data(), pkt.size(), 0,
                         reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::close(fd);
    return n == static_cast<ssize_t>(pkt.size());
}

void ack(IpcSocket* client, uint32_t tid, shared::WakeOnLanOpcode op) {
    shared::XBusMessage m{};
    m.transaction_id_ = tid;
    std::strncpy(m.service_name_, "com.freebsdx.WakeOnLan", sizeof(m.service_name_) - 1);
    m.opcode_ = static_cast<uint32_t>(op);
    client->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
}
}  // namespace

void WakeOnLanService::handleMessage(const shared::XBusMessage& msg,
                                     std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::WakeOnLanOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::Send: {
        shared::WakeOnLanRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.mac_[sizeof(req.mac_) - 1] = '\0';
        uint8_t mac[6];
        if (wol::parseMac(req.mac_, mac)) {
            if (sendMagic(mac)) LOG_INFO("WakeOnLanService: sent magic packet to {}", req.mac_);
            else                LOG_WARN("WakeOnLanService: send failed for {}", req.mac_);
        } else {
            LOG_WARN("WakeOnLanService: invalid MAC '{}'", req.mac_);
        }
        ack(client.get(), msg.transaction_id_, O::Send);
        break;
    }
    case O::ListTargets: {
        uint32_t page = 0;
        std::memcpy(&page, msg.payload_, sizeof(page));
        auto tg = loadTargets();

        shared::WakeOnLanPage pg{};
        pg.page_  = page;
        pg.total_ = static_cast<uint32_t>(tg.size());
        if (page < tg.size()) {
            std::strncpy(pg.name_, tg[page].first.c_str(),  sizeof(pg.name_) - 1);
            std::strncpy(pg.mac_,  tg[page].second.c_str(), sizeof(pg.mac_)  - 1);
            pg.valid_ = 1;
        }
        pg.has_more_ = (page + 1 < tg.size()) ? 1u : 0u;

        shared::XBusMessage reply{};
        reply.transaction_id_ = msg.transaction_id_;
        std::strncpy(reply.service_name_, "com.freebsdx.WakeOnLan",
                     sizeof(reply.service_name_) - 1);
        reply.opcode_ = static_cast<uint32_t>(O::ListTargets);
        std::memcpy(reply.payload_, &pg, sizeof(pg));
        client->sendMessage({reinterpret_cast<const uint8_t*>(&reply), sizeof(reply)});
        break;
    }
    case O::AddTarget: {
        shared::WakeOnLanTarget req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.name_[sizeof(req.name_) - 1] = '\0';
        req.mac_[sizeof(req.mac_) - 1]   = '\0';
        uint8_t mac[6];
        if (req.name_[0] && wol::parseMac(req.mac_, mac)) {
            auto tg = loadTargets();
            // Replace an existing target with the same name, else append.
            bool found = false;
            for (auto& t : tg) if (t.first == req.name_) { t.second = req.mac_; found = true; break; }
            if (!found) tg.emplace_back(req.name_, req.mac_);
            saveTargets(tg);
            LOG_INFO("WakeOnLanService: saved target '{}' → {}", req.name_, req.mac_);
        } else {
            LOG_WARN("WakeOnLanService: rejected target '{}' (bad name/MAC)", req.name_);
        }
        ack(client.get(), msg.transaction_id_, O::AddTarget);
        break;
    }
    case O::RemoveTarget: {
        shared::WakeOnLanRemoveRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.name_[sizeof(req.name_) - 1] = '\0';
        auto tg = loadTargets();
        std::vector<std::pair<std::string, std::string>> kept;
        for (auto& t : tg) if (t.first != req.name_) kept.push_back(std::move(t));
        saveTargets(kept);
        ack(client.get(), msg.transaction_id_, O::RemoveTarget);
        break;
    }
    default: break;
    }
}

}  // namespace fbx::xserver

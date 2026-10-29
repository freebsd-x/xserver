#include "xserver/pkgupdates_service.h"
#include "xserver/pkg_parse.h"
#include "xserver/async_shell.h"
#include "shared/logger.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace fbx::xserver {

namespace {
// posix_spawn, not popen (see async_shell.h — popen deadlocks the forked child on a
// libc stdio lock in the multithreaded compositor).
std::string runPkgUpgrade() {
    // Can take a while (may consult the repo catalogue) and handlers run on
    // the compositor thread: answer from the last result, refresh in the
    // background (capped at 30 s).
    return g_asyncShell.get("pkg upgrade -n 2>/dev/null", 30);
}
}  // namespace

void PkgUpdatesService::handleMessage(const shared::XBusMessage& msg,
                                      std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::PkgOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListUpdates: {
        uint32_t page = 0;
        std::memcpy(&page, msg.payload_, sizeof(page));
        auto ups = pkg::parseUpgrades(runPkgUpgrade());

        shared::PkgUpdatePage pg{};
        pg.page_  = page;
        pg.total_ = static_cast<uint32_t>(ups.size());
        uint32_t start = page * 2, n = 0;
        for (; n < 2 && start + n < ups.size(); ++n) {
            const pkg::Upgrade& u = ups[start + n];
            shared::PkgUpdateEntry& e = pg.entries_[n];
            std::strncpy(e.name_,    u.name.c_str(),    sizeof(e.name_) - 1);
            std::strncpy(e.old_ver_, u.old_ver.c_str(), sizeof(e.old_ver_) - 1);
            std::strncpy(e.new_ver_, u.new_ver.c_str(), sizeof(e.new_ver_) - 1);
        }
        pg.count_    = n;
        pg.has_more_ = (start + n < ups.size()) ? 1u : 0u;

        shared::XBusMessage reply{};
        reply.transaction_id_ = msg.transaction_id_;
        std::strncpy(reply.service_name_, "com.freebsdx.PkgUpdates",
                     sizeof(reply.service_name_) - 1);
        reply.opcode_ = static_cast<uint32_t>(O::ListUpdates);
        std::memcpy(reply.payload_, &pg, sizeof(pg));
        client->sendMessage({reinterpret_cast<const uint8_t*>(&reply), sizeof(reply)});
        break;
    }
    case O::InstallPackage:
    case O::InstallAll:
        LOG_INFO("PkgUpdatesService: install requested (needs root/network)");
        break;
    case O::CheckNow:
        LOG_INFO("PkgUpdatesService: CheckNow requested (needs network)");
        break;
    default: break;
    }
}

}  // namespace fbx::xserver

#include "xserver/kmod_service.h"
#include "xserver/kmod_list.h"
#include "xserver/async_shell.h"
#include "xserver/priv_broker.h"
#include "xserver/priv_ops.h"
#include "shared/logger.h"

#include <cstring>
#include <string>
#include <vector>

namespace fbx::xserver {

void BroadcastXBusMessage(const shared::XBusMessage&);

void KmodService::handleMessage(const shared::XBusMessage& msg,
                                std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::KmodOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListModules: {
        uint32_t page = 0; std::memcpy(&page, msg.payload_, sizeof(page));
        // posix_spawn, not popen (popen deadlocks the threaded compositor).
        auto mods = kmod::parseKldstat(spawnCapture("kldstat 2>/dev/null"));

        shared::KmodPage pg{};
        pg.page_  = page;
        pg.total_ = static_cast<uint32_t>(mods.size());
        uint32_t start = page * 3, n = 0;
        for (; n < 3 && start + n < mods.size(); ++n) {
            const kmod::KmodInfo& m = mods[start + n];
            shared::KmodEntry& e = pg.entries_[n];
            std::strncpy(e.name_, m.name.c_str(), sizeof(e.name_) - 1);
            e.size_kb_ = m.size_kb;
            e.refs_    = m.refs;
            e.flags_   = 0;                 // kldstat doesn't expose kld flags
        }
        pg.count_    = n;
        pg.has_more_ = (start + n < mods.size()) ? 1u : 0u;

        shared::XBusMessage reply{};
        reply.transaction_id_ = msg.transaction_id_;
        std::strncpy(reply.service_name_, "com.freebsdx.KernelModules",
                     sizeof(reply.service_name_) - 1);
        reply.opcode_ = static_cast<uint32_t>(O::ListModules);
        std::memcpy(reply.payload_, &pg, sizeof(pg));
        client->sendMessage({reinterpret_cast<const uint8_t*>(&reply), sizeof(reply)});
        break;
    }
    case O::UnloadModule: {
        shared::KmodUnloadRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.name_[sizeof(req.name_) - 1] = '\0';
        // Unload via the privilege broker (kldunload needs root). On success,
        // broadcast ModulesChanged (no payload — the shell re-polls ListModules).
        int r = runPrivilegedOp(privops::kKmodUnload, req.name_);
        if (r == 0) {
            LOG_INFO("KmodService: unloaded '{}' via broker", req.name_);
            shared::XBusMessage b{};
            std::strncpy(b.service_name_, "com.freebsdx.KernelModules", sizeof(b.service_name_) - 1);
            b.opcode_ = static_cast<uint32_t>(shared::KmodOpcode::ModulesChanged);
            BroadcastXBusMessage(b);
        } else if (r == kBrokerUnreachable) {
            LOG_INFO("KmodService: UnloadModule '{}' — broker unavailable (needs root kldunload)", req.name_);
        } else {
            LOG_WARN("KmodService: kldunload '{}' failed ({})", req.name_, r);
        }
        break;
    }
    default: break;
    }
}

}  // namespace fbx::xserver

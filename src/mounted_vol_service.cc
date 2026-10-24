#include "xserver/mounted_vol_service.h"
#include "xserver/sys_inspect.h"

#include <cstring>
#include <vector>

#include <sys/param.h>
#include <sys/mount.h>
#include <sys/ucred.h>

namespace fbx::xserver {

namespace {
std::vector<shared::VolumeEntry> enumerate() {
    std::vector<shared::VolumeEntry> out;
    struct statfs* mnt = nullptr;
    int n = getmntinfo(&mnt, MNT_NOWAIT);   // kernel-owned buffer; do not free
    for (int i = 0; i < n; ++i) {
        const struct statfs& s = mnt[i];
        shared::VolumeEntry e{};
        uint64_t bsize = s.f_bsize ? s.f_bsize : 512;
        e.total_kb_ = static_cast<uint64_t>(s.f_blocks) * bsize / 1024;
        e.used_kb_  = static_cast<uint64_t>(s.f_blocks - s.f_bfree) * bsize / 1024;
        std::strncpy(e.mount_,  s.f_mntonname,   sizeof(e.mount_) - 1);
        std::strncpy(e.fstype_, s.f_fstypename,  sizeof(e.fstype_) - 1);
        e.readonly_ = (s.f_flags & MNT_RDONLY) ? 1u : 0u;
        out.push_back(e);
    }
    return out;
}
}  // namespace

void MountedVolService::handleMessage(const shared::XBusMessage& msg,
                                      std::shared_ptr<IpcSocket> client) noexcept {
    if (static_cast<shared::MountedVolOpcode>(msg.opcode_) != shared::MountedVolOpcode::ListVolumes)
        return;

    uint32_t page = 0;
    std::memcpy(&page, msg.payload_, sizeof(page));
    shared::VolumePage pg = sysinspect::buildVolumePage(enumerate(), page);

    shared::XBusMessage reply{};
    reply.transaction_id_ = msg.transaction_id_;
    std::strncpy(reply.service_name_, "com.freebsdx.MountedVolumes",
                 sizeof(reply.service_name_) - 1);
    reply.opcode_ = static_cast<uint32_t>(shared::MountedVolOpcode::ListVolumes);
    std::memcpy(reply.payload_, &pg, sizeof(pg));
    client->sendMessage({reinterpret_cast<const uint8_t*>(&reply), sizeof(reply)});
}

}  // namespace fbx::xserver

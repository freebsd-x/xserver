#include "xserver/sysinfo_service.h"
#include "xserver/sys_inspect.h"

#include <cstring>
#include <ctime>
#include <string>

#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

namespace fbx::xserver {

namespace {
std::string sysctlStr(const char* name) {
    size_t len = 0;
    if (sysctlbyname(name, nullptr, &len, nullptr, 0) != 0 || len == 0) return "";
    std::string buf(len, '\0');
    if (sysctlbyname(name, buf.data(), &len, nullptr, 0) != 0) return "";
    if (!buf.empty() && buf.back() == '\0') buf.pop_back();
    return buf;
}
template <typename T>
T sysctlNum(const char* name, T fallback) {
    T v{};
    size_t len = sizeof(v);
    if (sysctlbyname(name, &v, &len, nullptr, 0) != 0) return fallback;
    return v;
}
uint32_t uptimeSecs() {
    struct timeval bt{};
    size_t len = sizeof(bt);
    if (sysctlbyname("kern.boottime", &bt, &len, nullptr, 0) != 0 || bt.tv_sec == 0) return 0;
    time_t now = time(nullptr);
    return (now > bt.tv_sec) ? static_cast<uint32_t>(now - bt.tv_sec) : 0;
}
}  // namespace

void SysInfoService::handleMessage(const shared::XBusMessage& msg,
                                   std::shared_ptr<IpcSocket> client) noexcept {
    if (static_cast<shared::SysInfoOpcode>(msg.opcode_) != shared::SysInfoOpcode::GetInfo)
        return;

    shared::SysInfoState st{};
    char host[256] = {};
    if (gethostname(host, sizeof(host) - 1) == 0)
        std::strncpy(st.hostname_, host, sizeof(st.hostname_) - 1);

    std::string osv = sysinspect::osVersion(sysctlStr("kern.ostype"),
                                            sysctlStr("kern.osrelease"));
    std::strncpy(st.os_version_, osv.c_str(), sizeof(st.os_version_) - 1);
    std::strncpy(st.cpu_model_, sysctlStr("hw.model").c_str(), sizeof(st.cpu_model_) - 1);
    std::strncpy(st.gpu_model_, "N/A", sizeof(st.gpu_model_) - 1);
    std::strncpy(st.serial_,    "N/A", sizeof(st.serial_) - 1);

    st.cpu_cores_    = sysctlNum<int>("hw.ncpu", 0);
    uint64_t physmem = sysctlNum<uint64_t>("hw.physmem", 0);
    st.ram_total_mb_ = static_cast<uint32_t>(physmem / (1024 * 1024));
    st.uptime_secs_  = uptimeSecs();

    shared::XBusMessage reply{};
    reply.transaction_id_ = msg.transaction_id_;
    std::strncpy(reply.service_name_, "com.freebsdx.SysInfo",
                 sizeof(reply.service_name_) - 1);
    reply.opcode_ = static_cast<uint32_t>(shared::SysInfoOpcode::GetInfo);
    std::memcpy(reply.payload_, &st, sizeof(st));
    client->sendMessage({reinterpret_cast<const uint8_t*>(&reply), sizeof(reply)});
}

}  // namespace fbx::xserver

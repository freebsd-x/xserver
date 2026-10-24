#include "xserver/mem_pressure_service.h"

#include <cstring>

#if __has_include(<sys/sysctl.h>)
#  include <sys/sysctl.h>
#  include <sys/types.h>
#  define HAVE_SYSCTL 1
#endif

namespace fbx::xserver {

namespace {
#ifdef HAVE_SYSCTL
uint64_t sysctlU64(const char* name) {
    uint64_t v = 0; std::size_t sz = sizeof(v);
    if (sysctlbyname(name, &v, &sz, nullptr, 0) == 0 && sz == sizeof(v)) return v;
    return 0;
}
uint32_t sysctlU32(const char* name) {
    uint32_t v = 0; std::size_t sz = sizeof(v);
    if (sysctlbyname(name, &v, &sz, nullptr, 0) == 0 && sz == sizeof(v)) return v;
    return 0;
}
#endif
}  // namespace

shared::MemPressureState MemPressureService::readState() noexcept {
    shared::MemPressureState st{};
#ifdef HAVE_SYSCTL
    uint64_t physmem  = sysctlU64("hw.physmem");
    uint32_t pagesize = sysctlU32("hw.pagesize");
    if (pagesize == 0) pagesize = 4096;

    // "Available" = free + inactive + cache pages (reclaimable without swapping).
    uint64_t free_pages = static_cast<uint64_t>(sysctlU32("vm.stats.vm.v_free_count")) +
                          sysctlU32("vm.stats.vm.v_inactive_count") +
                          sysctlU32("vm.stats.vm.v_cache_count");

    st.total_kb_ = physmem / 1024;
    st.free_kb_  = (free_pages * pagesize) / 1024;
    if (st.free_kb_ > st.total_kb_) st.free_kb_ = st.total_kb_;

    // Swap is not cheaply available via a single sysctl without kvm; report 0/0.
    st.swap_total_kb_   = 0;
    st.swap_used_kb_    = 0;
    st.page_faults_sec_ = 0;   // needs a rate sample; left at 0

    // Pressure from the free fraction.
    uint32_t free_pct = st.total_kb_ ? static_cast<uint32_t>(st.free_kb_ * 100 / st.total_kb_) : 100;
    st.pressure_level_ = (free_pct >= 20) ? 0u : (free_pct >= 10) ? 1u : 2u;
#endif
    return st;
}

void MemPressureService::handleMessage(const shared::XBusMessage& msg,
                                       std::shared_ptr<IpcSocket> client) noexcept {
    if (static_cast<shared::MemPressureOpcode>(msg.opcode_) != shared::MemPressureOpcode::GetPressure)
        return;
    if (!client) return;
    shared::MemPressureState st = readState();
    shared::XBusMessage m{};
    m.transaction_id_ = msg.transaction_id_;
    std::strncpy(m.service_name_, "com.freebsdx.MemPressure", sizeof(m.service_name_) - 1);
    m.opcode_ = static_cast<uint32_t>(shared::MemPressureOpcode::GetPressure);
    std::memcpy(m.payload_, &st, sizeof(st));
    client->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
}

}  // namespace fbx::xserver

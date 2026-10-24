#include "xserver/auth_throttle.h"

#include <algorithm>

namespace fbx::xserver {

AuthThrottle::AuthThrottle(uint32_t max_failures, uint32_t lockout_seconds,
                           uint32_t base_delay_s, uint32_t max_delay_s) noexcept
    : max_failures_(max_failures),
      lockout_seconds_(lockout_seconds),
      base_delay_s_(base_delay_s),
      max_delay_s_(max_delay_s) {}

uint32_t AuthThrottle::delayFor(uint32_t failures) const noexcept {
    if (failures == 0) return 0;
    // base_delay << (failures - 1), guarding against shift overflow, then cap.
    uint32_t shift = failures - 1;
    if (shift >= 31) return max_delay_s_;
    uint64_t d = static_cast<uint64_t>(base_delay_s_) << shift;
    return static_cast<uint32_t>(std::min<uint64_t>(d, max_delay_s_));
}

AuthThrottle::Decision AuthThrottle::check(const std::string& user,
                                           time_point now) noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = entries_.find(user);
    if (it == entries_.end() || it->second.failures == 0) return {};  // allowed

    Entry& e = it->second;
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - e.last).count();
    const int64_t el = elapsed < 0 ? 0 : elapsed;  // clock never goes backwards for us

    if (e.failures >= max_failures_) {
        if (el >= static_cast<int64_t>(lockout_seconds_)) {
            entries_.erase(it);            // lockout served → fresh start
            return {};
        }
        return { false, true, static_cast<uint32_t>(lockout_seconds_ - el) };
    }

    const uint32_t need = delayFor(e.failures);
    if (el >= static_cast<int64_t>(need)) return {};   // backed off long enough
    return { false, false, static_cast<uint32_t>(need - el) };
}

void AuthThrottle::recordFailure(const std::string& user, time_point now) noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    Entry& e = entries_[user];
    if (e.failures < max_failures_) ++e.failures;  // clamp at ceiling
    e.last = now;
}

void AuthThrottle::recordSuccess(const std::string& user) noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    entries_.erase(user);
}

AuthThrottle& AuthThrottle::global() noexcept {
    static AuthThrottle instance;
    return instance;
}

}  // namespace fbx::xserver

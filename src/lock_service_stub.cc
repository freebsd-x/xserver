#include "xserver/lock_service.h"

// Test stub for the service_roundtrip harness: the real LockService spawns the
// lock screen and links PAM. ScreenLock only needs the idle timeout and a
// lock() it can count, so keep both in memory here.
namespace fbx::xserver {

std::atomic<uint32_t> LockService::timeout_seconds_{10 * 60};

uint32_t LockService::timeoutSeconds() noexcept { return timeout_seconds_.load(); }
void LockService::setTimeoutSeconds(uint32_t seconds) noexcept { timeout_seconds_.store(seconds); }

std::atomic<int> g_stub_lock_calls{0};
void LockService::lock() noexcept { ++g_stub_lock_calls; }

}  // namespace fbx::xserver

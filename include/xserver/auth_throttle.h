#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace fbx::xserver {

// Brute-force protection for password authentication (login greeter + lock
// screen). Non-blocking by design: instead of sleeping the compositor thread,
// it rejects attempts that arrive too soon after a failure and locks an account
// out after too many failures. Escalating back-off before lockout:
//   after 1 failure  → must wait base_delay
//   after n failures → must wait base_delay << (n-1)  (capped)
//   after max_failures → locked out for lockout_seconds, then the counter resets
// A successful auth clears the account's record. Time is passed in (steady clock)
// so the decision logic is pure and unit-testable.
class AuthThrottle {
public:
    using clock      = std::chrono::steady_clock;
    using time_point = clock::time_point;

    struct Decision {
        bool     allowed        = true;   // may attempt authentication now
        bool     locked_out     = false;  // hit the failure ceiling (vs. still backing off)
        uint32_t retry_after_s  = 0;      // seconds until the next attempt is allowed
    };

    explicit AuthThrottle(uint32_t max_failures   = 5,
                          uint32_t lockout_seconds = 60,
                          uint32_t base_delay_s    = 1,
                          uint32_t max_delay_s     = 30) noexcept;

    // Check before authenticating. If a lockout window has fully elapsed the
    // record is reset and the attempt is allowed.
    Decision check(const std::string& user, time_point now) noexcept;

    void recordFailure(const std::string& user, time_point now) noexcept;
    void recordSuccess(const std::string& user) noexcept;

    // Process-wide instance shared by the login and lock auth paths, so an
    // attacker can't sidestep a lockout by switching surfaces.
    static AuthThrottle& global() noexcept;

private:
    uint32_t delayFor(uint32_t failures) const noexcept;

    struct Entry { uint32_t failures = 0; time_point last{}; };

    const uint32_t max_failures_;
    const uint32_t lockout_seconds_;
    const uint32_t base_delay_s_;
    const uint32_t max_delay_s_;

    std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
};

}  // namespace fbx::xserver

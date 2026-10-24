// Unit tests for AuthThrottle — brute-force back-off / lockout logic.
// Time is injected, so these are pure and instantaneous (no real sleeping).

#include "xserver/auth_throttle.h"
#include <gtest/gtest.h>

using fbx::xserver::AuthThrottle;
using Clock = AuthThrottle::clock;

namespace {
// Small config for fast reasoning: lock after 3 failures for 10s;
// pre-lockout back-off 1s, 2s (base=1, cap=4).
AuthThrottle make() { return AuthThrottle(/*max*/3, /*lockout*/10, /*base*/1, /*cap*/4); }
Clock::time_point at(Clock::time_point t0, int secs) { return t0 + std::chrono::seconds(secs); }
}  // namespace

TEST(AuthThrottleTest, FreshUserAllowed) {
    auto th = make();
    auto now = Clock::now();
    auto d = th.check("alice", now);
    EXPECT_TRUE(d.allowed);
    EXPECT_FALSE(d.locked_out);
}

TEST(AuthThrottleTest, BacksOffAfterFailure) {
    auto th = make();
    auto t0 = Clock::now();
    th.recordFailure("alice", t0);
    // Immediately after: must wait ~1s.
    auto d0 = th.check("alice", t0);
    EXPECT_FALSE(d0.allowed);
    EXPECT_FALSE(d0.locked_out);
    EXPECT_EQ(d0.retry_after_s, 1u);
    // After the back-off elapses: allowed again.
    EXPECT_TRUE(th.check("alice", at(t0, 1)).allowed);
}

TEST(AuthThrottleTest, BackoffEscalates) {
    auto th = make();
    auto t0 = Clock::now();
    th.recordFailure("bob", t0);                 // 1st failure → 1s
    th.recordFailure("bob", at(t0, 1));          // 2nd failure → 2s
    auto d = th.check("bob", at(t0, 1));
    EXPECT_FALSE(d.allowed);
    EXPECT_EQ(d.retry_after_s, 2u);
    EXPECT_TRUE(th.check("bob", at(t0, 3)).allowed);
}

TEST(AuthThrottleTest, LocksOutAfterMaxFailures) {
    auto th = make();
    auto t0 = Clock::now();
    th.recordFailure("eve", t0);
    th.recordFailure("eve", at(t0, 2));
    th.recordFailure("eve", at(t0, 5));          // 3rd == max → lockout
    auto d = th.check("eve", at(t0, 5));
    EXPECT_FALSE(d.allowed);
    EXPECT_TRUE(d.locked_out);
    EXPECT_EQ(d.retry_after_s, 10u);
    // Still locked partway through.
    EXPECT_FALSE(th.check("eve", at(t0, 9)).allowed);
    // Lockout served → fresh start.
    EXPECT_TRUE(th.check("eve", at(t0, 15)).allowed);
}

TEST(AuthThrottleTest, SuccessResetsCounter) {
    auto th = make();
    auto t0 = Clock::now();
    th.recordFailure("carol", t0);
    th.recordFailure("carol", at(t0, 2));
    th.recordSuccess("carol");
    // Cleared: allowed immediately, and a subsequent failure starts back at 1s.
    EXPECT_TRUE(th.check("carol", at(t0, 2)).allowed);
    th.recordFailure("carol", at(t0, 2));
    EXPECT_EQ(th.check("carol", at(t0, 2)).retry_after_s, 1u);
}

TEST(AuthThrottleTest, PerUserIsolation) {
    auto th = make();
    auto t0 = Clock::now();
    th.recordFailure("mallory", t0);
    th.recordFailure("mallory", at(t0, 1));
    th.recordFailure("mallory", at(t0, 2));      // mallory locked out
    EXPECT_TRUE(th.check("mallory", at(t0, 2)).locked_out);
    EXPECT_TRUE(th.check("trent", at(t0, 2)).allowed);  // unrelated user unaffected
}

#include "xserver/idle_policy.h"
#include <gtest/gtest.h>

using namespace fbx::xserver::idle;

namespace {
Decision at(uint32_t idle, Settings s = {600, 600, 30}) {
    Now n;
    n.idle_secs = idle;
    return decide(s, n);
}
Decision locked(uint32_t idle, uint32_t since_lock, Settings s = {600, 600, 30}) {
    Now n;
    n.idle_secs = idle;
    n.locked = true;
    n.locked_secs = since_lock;
    return decide(s, n);
}
}  // namespace

TEST(Idle, DimsThenLocksThenGoesDark) {
    EXPECT_EQ(at(584).phase, Phase::Active);
    EXPECT_EQ(at(585).phase, Phase::Dim);
    EXPECT_FALSE(at(599).lock);
    EXPECT_TRUE(at(600).lock);
    EXPECT_EQ(at(600).phase, Phase::Active);          // the lock screen is seen
    EXPECT_EQ(locked(629, 29).phase, Phase::Active);
    EXPECT_EQ(locked(630, 30).phase, Phase::Off);
}

TEST(Idle, DarkBeforeLockingWhenThatsSooner) {
    const Settings s{600, 120, 30};
    EXPECT_EQ(at(105, s).phase, Phase::Dim);
    EXPECT_EQ(at(120, s).phase, Phase::Off);
    EXPECT_FALSE(at(599, s).lock);
    EXPECT_TRUE(at(600, s).lock);                     // and locks all the same
    EXPECT_EQ(at(600, s).phase, Phase::Off);
    EXPECT_EQ(locked(601, 1, s).phase, Phase::Off);   // without lighting up for it
}

TEST(Idle, NeverMeansNever) {
    EXPECT_FALSE(at(1000000, {0, 600, 30}).lock);
    EXPECT_EQ(at(1000000, {0, 600, 30}).phase, Phase::Off);
    EXPECT_TRUE(at(1000000, {600, 0, 30}).lock);
    EXPECT_EQ(locked(1000000, 1000000, {600, 0, 30}).phase, Phase::Active);
    EXPECT_EQ(at(1000000, {0, 0, 30}).phase, Phase::Active);
    EXPECT_EQ(at(10, {30, 600, 30}).phase, Phase::Active);   // too short to warn of
}

TEST(Idle, AHoldStopsItUntilLocked) {
    Now n;
    n.idle_secs = 10000;
    n.held = true;
    EXPECT_FALSE(decide({600, 600, 30}, n).lock);
    EXPECT_EQ(decide({600, 600, 30}, n).phase, Phase::Active);
    n.locked = true;
    n.locked_secs = 40;
    EXPECT_EQ(decide({600, 600, 30}, n).phase, Phase::Off);
}

TEST(Idle, TheGreeterIsNeverLocked) {
    Now n;
    n.idle_secs = 10000;
    n.at_greeter = true;
    EXPECT_FALSE(decide({600, 600, 30}, n).lock);
    EXPECT_EQ(decide({600, 600, 30}, n).phase, Phase::Off);
}

TEST(Idle, LockedByHandGoesDarkAfterAWhileAndInputBringsItBack) {
    EXPECT_EQ(locked(2, 2).phase, Phase::Active);
    EXPECT_EQ(locked(30, 30).phase, Phase::Off);
    EXPECT_EQ(locked(0, 300).phase, Phase::Active);
}

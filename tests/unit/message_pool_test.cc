// Tests for XBusMessagePool — lock-free slab allocator.
// Uses a local pool instance (not the global singleton) so tests are isolated.

#include "xserver/xbus_message_pool.h"
#include <gtest/gtest.h>
#include <atomic>
#include <span>
#include <thread>
#include <vector>

namespace fbx::xserver::testing {

class MessagePoolTest : public ::testing::Test {
protected:
    XBusMessagePool pool_; // fresh pool per test, not the global singleton
};

// ── Basic acquire / release ───────────────────────────────────────────────────

TEST_F(MessagePoolTest, AcquireReturnsNonNull) {
    auto* slot = pool_.acquire();
    ASSERT_NE(slot, nullptr);
    pool_.release(slot);
}

TEST_F(MessagePoolTest, AcquireCountIncremented) {
    auto before = pool_.acquireCount();
    auto* slot = pool_.acquire();
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(pool_.acquireCount(), before + 1);
    pool_.release(slot);
}

TEST_F(MessagePoolTest, ReleaseAllowsReacquire) {
    auto* slot = pool_.acquire();
    ASSERT_NE(slot, nullptr);
    pool_.release(slot);

    // After release the slot re-enters the free-list; acquire should succeed
    auto* slot2 = pool_.acquire();
    EXPECT_NE(slot2, nullptr);
    pool_.release(slot2);
}

TEST_F(MessagePoolTest, AllSlotsAreDistinctPointers) {
    std::vector<XBusMessagePool::Slot*> slots;
    slots.reserve(XBusMessagePool::kSlabCount);

    for (uint32_t i = 0; i < XBusMessagePool::kSlabCount; ++i) {
        auto* s = pool_.acquire();
        ASSERT_NE(s, nullptr) << "Ran out of slots at index " << i;
        slots.push_back(s);
    }

    // Every pointer must be unique
    for (size_t i = 0; i < slots.size(); ++i)
        for (size_t j = i + 1; j < slots.size(); ++j)
            EXPECT_NE(slots[i], slots[j]) << "Duplicate at " << i << " and " << j;

    for (auto* s : slots) pool_.release(s);
}

// ── Pool exhaustion ───────────────────────────────────────────────────────────

TEST_F(MessagePoolTest, ExhaustedPoolReturnsNullptr) {
    std::vector<XBusMessagePool::Slot*> slots;
    for (uint32_t i = 0; i < XBusMessagePool::kSlabCount; ++i)
        slots.push_back(pool_.acquire());

    // Pool exhausted: acquire() returns nullptr
    auto* extra = pool_.acquire();
    EXPECT_EQ(extra, nullptr);

    for (auto* s : slots) pool_.release(s);
}

TEST_F(MessagePoolTest, AcquireOwnedFallbackOnExhaustion) {
    // Drain the pool entirely
    std::vector<XBusMessagePool::Slot*> slots;
    for (uint32_t i = 0; i < XBusMessagePool::kSlabCount; ++i)
        slots.push_back(pool_.acquire());

    // acquireOwned() must never return null — uses thread_local fallback
    auto before_fallback = pool_.fallbackCount();
    auto owned = pool_.acquireOwned();
    EXPECT_NE(owned.get(), nullptr);
    EXPECT_EQ(pool_.fallbackCount(), before_fallback + 1);

    for (auto* s : slots) pool_.release(s);
}

// ── RAII PooledSlot ───────────────────────────────────────────────────────────

TEST_F(MessagePoolTest, PooledSlotReleasesOnDestruction) {
    auto before = pool_.acquireCount();
    {
        auto owned = pool_.acquireOwned();
        ASSERT_NE(owned.get(), nullptr);
        // Slot is held here
    }
    // Destructor fired — slot should be back in the free-list.
    // Verify by draining the full pool: if the slot wasn't returned, we'd get
    // kSlabCount-1 successful acquires before nullptr.
    std::vector<XBusMessagePool::Slot*> slots;
    for (uint32_t i = 0; i < XBusMessagePool::kSlabCount; ++i) {
        auto* s = pool_.acquire();
        if (!s) break;
        slots.push_back(s);
    }
    EXPECT_EQ(slots.size(), XBusMessagePool::kSlabCount); // all slots accounted for
    for (auto* s : slots) pool_.release(s);
    (void)before;
}

TEST_F(MessagePoolTest, FallbackSlotDeleterIsNoop) {
    // A PooledSlot wrapping the thread_local fallback has pool_==nullptr.
    // Destroying it must not crash or corrupt the pool.
    std::vector<XBusMessagePool::Slot*> slots;
    for (uint32_t i = 0; i < XBusMessagePool::kSlabCount; ++i)
        slots.push_back(pool_.acquire());

    {
        auto owned = pool_.acquireOwned(); // fallback slot
        ASSERT_NE(owned.get(), nullptr);
        // Goes out of scope here — no-op release
    }

    // Pool integrity check: release all real slots, pool should be full again
    for (auto* s : slots) pool_.release(s);
    auto* check = pool_.acquire();
    EXPECT_NE(check, nullptr);
    pool_.release(check);
}

// ── Slot content ─────────────────────────────────────────────────────────────

TEST_F(MessagePoolTest, SlotSpanReflectsByteCount) {
    auto owned = pool_.acquireOwned();
    ASSERT_NE(owned.get(), nullptr);

    owned->byte_count_ = static_cast<ssize_t>(sizeof(shared::XBusMessage));
    auto sp = owned->span();
    EXPECT_EQ(sp.size(), sizeof(shared::XBusMessage));
    EXPECT_EQ(sp.data(), reinterpret_cast<const uint8_t*>(owned->buf_));
}

TEST_F(MessagePoolTest, SlotBufSizeCoversXBusMessage) {
    // Slot::buf_ must be large enough to hold the largest protocol struct.
    EXPECT_GE(sizeof(XBusMessagePool::Slot::buf_), sizeof(shared::XBusMessage));
}

TEST_F(MessagePoolTest, SlotAsTypedView) {
    auto owned = pool_.acquireOwned();
    ASSERT_NE(owned.get(), nullptr);

    // Write via typed view, read back via span
    auto* msg = owned->as<shared::XBusMessage>();
    msg->magic_ = shared::XBUS_MAGIC;
    owned->byte_count_ = sizeof(shared::XBusMessage);

    auto sp = owned->span();
    uint32_t magic_readback = 0;
    std::memcpy(&magic_readback, sp.data(), sizeof(uint32_t));
    EXPECT_EQ(magic_readback, shared::XBUS_MAGIC);
}

// ── Concurrent stress test ────────────────────────────────────────────────────

TEST_F(MessagePoolTest, ConcurrentAcquireReleaseNoErrors) {
    constexpr int kThreads = 8;
    constexpr int kIter    = 500;
    std::atomic<int> errors{0};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < kIter; ++i) {
                auto owned = pool_.acquireOwned();
                if (!owned) { errors.fetch_add(1, std::memory_order_relaxed); continue; }
                // Touch the buffer to catch data races under TSan
                owned->byte_count_ = 1;
                owned->buf_[0]     = static_cast<char>(i & 0xFF);
            }
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_EQ(errors.load(), 0);
}

TEST_F(MessagePoolTest, ConcurrentPoolNotCorrupted) {
    // After concurrent use, the pool should still hand out valid slots.
    constexpr int kThreads = 4;
    constexpr int kIter    = 200;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < kIter; ++i) {
                auto* s = pool_.acquire();
                if (s) pool_.release(s);
            }
        });
    }
    for (auto& th : threads) th.join();

    // Pool must still be functional
    auto* slot = pool_.acquire();
    EXPECT_NE(slot, nullptr);
    pool_.release(slot);
}

} // namespace fbx::xserver::testing

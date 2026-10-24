#include "xserver/xbus_message_pool.h"
#include <iostream>

namespace fbx::xserver {

// ─── Construction ─────────────────────────────────────────────────────────────

XBusMessagePool::XBusMessagePool() noexcept {
    // Build the initial free-list: slot 0 → 1 → 2 → … → (N-1) → kNilIdx.
    for (uint32_t i = 0; i < kSlabCount - 1; ++i) next_[i] = i + 1;
    next_[kSlabCount - 1] = kNilIdx;

    // Initial head: tag=0, index=0.
    free_head_.store(0, std::memory_order_relaxed);
}

XBusMessagePool& XBusMessagePool::global() noexcept {
    static XBusMessagePool instance;
    return instance;
}

// ─── Lock-free acquire ────────────────────────────────────────────────────────

XBusMessagePool::Slot* XBusMessagePool::acquire() noexcept {
    uint64_t head = free_head_.load(std::memory_order_acquire);
    for (;;) {
        uint32_t idx = static_cast<uint32_t>(head & 0xFFFF'FFFFu);
        if (idx == kNilIdx) return nullptr; // pool exhausted

        // Next tag (prevent ABA).
        uint32_t tag = static_cast<uint32_t>(head >> 32) + 1u;
        uint64_t next = (static_cast<uint64_t>(tag) << 32) |
                        static_cast<uint64_t>(next_[idx]);

        if (free_head_.compare_exchange_weak(head, next,
                std::memory_order_release,
                std::memory_order_acquire)) {
            acquire_count_.fetch_add(1, std::memory_order_relaxed);
            return &slots_[idx];
        }
        // head was reloaded by CAS on failure — retry.
    }
}

// ─── Lock-free release ────────────────────────────────────────────────────────

void XBusMessagePool::release(Slot* slot) noexcept {
    if (!slot) return;
    uint32_t idx = static_cast<uint32_t>(slot - slots_.data());

    uint64_t head = free_head_.load(std::memory_order_acquire);
    for (;;) {
        uint32_t old_idx = static_cast<uint32_t>(head & 0xFFFF'FFFFu);
        next_[idx] = old_idx;

        uint32_t tag = static_cast<uint32_t>(head >> 32) + 1u;
        uint64_t new_head = (static_cast<uint64_t>(tag) << 32) |
                            static_cast<uint64_t>(idx);

        if (free_head_.compare_exchange_weak(head, new_head,
                std::memory_order_release,
                std::memory_order_acquire)) return;
    }
}

// ─── RAII acquire with fallback ───────────────────────────────────────────────

XBusMessagePool::PooledSlot XBusMessagePool::acquireOwned() noexcept {
    Slot* s = acquire();
    if (s) {
        return PooledSlot(s, SlotDeleter{this});
    }

    // Fallback: thread-local buffer; deleter is a no-op (pool_ = nullptr).
    // This path is only hit when all 512 slots are simultaneously in flight,
    // which would require 512 concurrent unacknowledged clients. In normal
    // operation this never happens.
    fallback_count_.fetch_add(1, std::memory_order_relaxed);
    thread_local Slot tl_slot;
    std::cerr << "[XBusMessagePool] WARNING: pool exhausted — using thread_local fallback\n";
    return PooledSlot(&tl_slot, SlotDeleter{nullptr});
}

} // namespace fbx::xserver

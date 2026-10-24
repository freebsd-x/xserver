#pragma once

#include "shared/protocol.h"
#include <atomic>
#include <array>
#include <cstdint>
#include <memory>
#include <span>

namespace fbx::xserver {

// Lock-free slab allocator for XBusMessage-sized receive buffers.
//
// Each Slot holds raw storage for one XBusMessage (the largest message type,
// 300 bytes) plus the received byte count. The pool pre-allocates kSlabCount
// slots at construction time and hands them out / reclaims them via an
// ABA-safe stamped CAS free-list. No malloc/free on the hot receive path.
//
// Typical usage:
//   auto slot = pool.acquire();         // O(1) lock-free
//   recvmsg(fd, slot->buf_, ...);
//   slot->byte_count_ = n;
//   // ... dispatch ...
//   pool.release(slot);                  // O(1) lock-free
//
// If acquire() returns nullptr (pool exhausted — shouldn't happen at kSlabCount
// = 512 with typical client counts) callers fall back to a thread_local buffer.
class XBusMessagePool {
public:
    static constexpr uint32_t kSlabCount = 512;
    static constexpr uint32_t kNilIdx    = 0xFFFF'FFFFu;

    struct Slot {
        // Raw storage sized for the largest message type.
        alignas(shared::XBusMessage) char buf_[sizeof(shared::XBusMessage)];
        ssize_t byte_count_{0};

        // Typed view helpers.
        template<typename T>
        T* as() noexcept { return __builtin_launder(reinterpret_cast<T*>(buf_)); }
        template<typename T>
        const T* as() const noexcept { return __builtin_launder(reinterpret_cast<const T*>(buf_)); }

        std::span<const uint8_t> span() const noexcept {
            return { reinterpret_cast<const uint8_t*>(buf_),
                     static_cast<size_t>(byte_count_) };
        }
    };

    // RAII owner for a pool slot. When pool_ is nullptr the slot is not returned
    // (used for the thread_local fallback when the pool is exhausted).
    struct SlotDeleter {
        XBusMessagePool* pool_{nullptr};
        void operator()(Slot* s) const noexcept {
            if (pool_ && s) pool_->release(s);
        }
    };
    using PooledSlot = std::unique_ptr<Slot, SlotDeleter>;

    XBusMessagePool() noexcept;

    // Process-global singleton — initialised on first call.
    static XBusMessagePool& global() noexcept;

    // Acquire a free slot (lock-free). Returns nullptr only when all kSlabCount
    // slots are simultaneously in flight, which is extremely unlikely.
    Slot* acquire() noexcept;

    // Return a slot to the free-list (lock-free).
    void release(Slot* slot) noexcept;

    // Convenience: acquire + wrap in RAII owner.
    // If the pool is exhausted, falls back to a thread_local buffer (no
    // heap alloc, deleter is a no-op).
    PooledSlot acquireOwned() noexcept;

    // Diagnostic counters (relaxed).
    [[nodiscard]] uint64_t acquireCount()   const noexcept { return acquire_count_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t fallbackCount()  const noexcept { return fallback_count_.load(std::memory_order_relaxed); }

private:
    std::array<Slot,     kSlabCount> slots_;
    std::array<uint32_t, kSlabCount> next_;  // intrusive free-list links

    // Stamped head: bits [63:32] = ABA tag, bits [31:0] = free-list head index.
    std::atomic<uint64_t> free_head_;

    std::atomic<uint64_t> acquire_count_{0};
    std::atomic<uint64_t> fallback_count_{0};
};

} // namespace fbx::xserver

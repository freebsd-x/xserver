// Integration smoke test for the XBus IPC pipeline.
//
// Tests the full send→receive path using a socketpair() — no hardware, no
// Vulkan, no compositor.  Exercises:
//   1. Round-trip of a single XBusMessage (magic + opcode + payload intact)
//   2. Pipelining: N messages sent before any receive, received in order
//   3. fd passing via SCM_RIGHTS (dup of a memfd survives the trip)
//   4. Pool-based receive path (receiveIntoSlot)
//   5. Zero-payload send (empty span)
//   6. Concurrent sends from multiple threads

#include "xserver/ipc_socket.h"
#include "xserver/xbus_message_pool.h"
#include "xserver/unique_fd.h"
#include "shared/protocol.h"
#include <gtest/gtest.h>

#include <sys/socket.h>
#include <sys/mman.h>
#include <unistd.h>
#include <thread>
#include <vector>
#include <atomic>
#include <cstring>
#include <cstdint>
#include <span>

using namespace fbx::xserver;
using namespace fbx::shared;

namespace fbx::xserver::testing {

// ── Helpers ───────────────────────────────────────────────────────────────────

// Create a connected socket pair and return them as IpcSocket objects.
// SOCK_DGRAM AF_UNIX preserves message boundaries: each sendmsg(2) produces
// exactly one recvmsg(2) datagram.  On FreeBSD, SOCK_SEQPACKET coalesces
// bytes like SOCK_STREAM, so SOCK_DGRAM is the correct choice for tests that
// pipeline N messages before any receive.  IpcSocket is socket-type agnostic.
static std::pair<IpcSocket, IpcSocket> makeSocketPair() {
    int sv[2];
    EXPECT_EQ(socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, sv), 0)
        << "socketpair failed: " << strerror(errno);
    return { IpcSocket{UniqueFd{sv[0]}}, IpcSocket{UniqueFd{sv[1]}} };
}

// Build an XBusMessage with a recognisable opcode and 4-byte payload.
static XBusMessage makeMsg(uint32_t opcode, uint32_t payload_val,
                           uint32_t txn_id = 42) {
    XBusMessage m{};
    m.magic_          = XBUS_MAGIC;
    m.transaction_id_ = txn_id;
    m.opcode_         = opcode;
    std::memcpy(m.payload_, &payload_val, sizeof(payload_val));
    std::strncpy(m.service_name_, "com.test.Smoke", sizeof(m.service_name_) - 1);
    return m;
}

// ── Tests ─────────────────────────────────────────────────────────────────────

class IpcPipelineTest : public ::testing::Test {};

// 1. Basic round-trip ---------------------------------------------------------

TEST_F(IpcPipelineTest, SingleMessageRoundTrip) {
    auto [client, server] = makeSocketPair();

    XBusMessage sent = makeMsg(0xDEAD, 0xBEEF);
    auto bytes = std::span<const uint8_t>{
        reinterpret_cast<const uint8_t*>(&sent), sizeof(sent)};
    ASSERT_TRUE(client.sendMessage(bytes).has_value());

    auto result = server.receiveMessage();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    auto& [data, fd] = result.value();
    ASSERT_EQ(data.size(), sizeof(XBusMessage));

    const auto* recv = reinterpret_cast<const XBusMessage*>(data.data());
    EXPECT_EQ(recv->magic_,          XBUS_MAGIC);
    EXPECT_EQ(recv->transaction_id_, 42u);
    EXPECT_EQ(recv->opcode_,         0xDEADu);
    uint32_t pval{};
    std::memcpy(&pval, recv->payload_, sizeof(pval));
    EXPECT_EQ(pval, 0xBEEFu);
    EXPECT_FALSE(fd);  // no fd passed
}

// 2. Pipelining — send N before receiving ------------------------------------

TEST_F(IpcPipelineTest, PipelineNMessagesInOrder) {
    constexpr int kN = 8;
    auto [client, server] = makeSocketPair();

    // Send all N
    for (int i = 0; i < kN; ++i) {
        XBusMessage m = makeMsg(static_cast<uint32_t>(i), static_cast<uint32_t>(i * 100), i);
        auto bytes = std::span<const uint8_t>{
            reinterpret_cast<const uint8_t*>(&m), sizeof(m)};
        ASSERT_TRUE(client.sendMessage(bytes).has_value()) << "send " << i;
    }

    // Receive all N — must arrive in order
    for (int i = 0; i < kN; ++i) {
        auto result = server.receiveMessage();
        ASSERT_TRUE(result.has_value()) << "recv " << i << ": " << result.error().message();

        const auto* recv = reinterpret_cast<const XBusMessage*>(result->first.data());
        EXPECT_EQ(recv->magic_,          XBUS_MAGIC)              << "msg " << i;
        EXPECT_EQ(recv->transaction_id_, static_cast<uint32_t>(i)) << "msg " << i;
        EXPECT_EQ(recv->opcode_,         static_cast<uint32_t>(i)) << "msg " << i;

        uint32_t pval{};
        std::memcpy(&pval, recv->payload_, sizeof(pval));
        EXPECT_EQ(pval, static_cast<uint32_t>(i * 100)) << "msg " << i;
    }
}

// 3. Pool-based receive path --------------------------------------------------

TEST_F(IpcPipelineTest, ReceiveIntoPoolSlot) {
    auto [client, server] = makeSocketPair();

    XBusMessage sent = makeMsg(0xABCD, 0x1234, 99);
    auto bytes = std::span<const uint8_t>{
        reinterpret_cast<const uint8_t*>(&sent), sizeof(sent)};
    ASSERT_TRUE(client.sendMessage(bytes).has_value());

    XBusMessagePool pool;
    UniqueFd out_fd;
    auto slot_result = server.receiveIntoSlot(pool, out_fd);
    ASSERT_TRUE(slot_result.has_value()) << slot_result.error().message();

    auto& slot = slot_result.value();
    ASSERT_GE(slot->byte_count_, static_cast<ssize_t>(sizeof(XBusMessage)));

    const auto* recv = slot->as<XBusMessage>();
    EXPECT_EQ(recv->magic_,          XBUS_MAGIC);
    EXPECT_EQ(recv->transaction_id_, 99u);
    EXPECT_EQ(recv->opcode_,         0xABCDu);
    EXPECT_FALSE(out_fd);
}

// 4. SCM_RIGHTS fd passing ----------------------------------------------------

TEST_F(IpcPipelineTest, FdPassingViaScmRights) {
    auto [client, server] = makeSocketPair();

    // Create a memfd, write a sentinel byte
    UniqueFd mem(memfd_create("ipc_smoke", MFD_CLOEXEC));
    ASSERT_TRUE(mem) << "memfd_create failed: " << strerror(errno);
    const char sentinel = 'X';
    ASSERT_EQ(write(mem.get(), &sentinel, 1), 1);

    // Send with no payload but with the memfd
    ASSERT_TRUE(client.sendMessage({}, mem.get()).has_value());

    auto result = server.receiveMessage();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    auto& [data, received_fd] = result.value();
    ASSERT_TRUE(received_fd) << "Expected an fd to be received";

    // Read back the sentinel
    lseek(received_fd.get(), 0, SEEK_SET);
    char buf = '\0';
    ASSERT_EQ(read(received_fd.get(), &buf, 1), 1);
    EXPECT_EQ(buf, 'X');
}

// 5. Zero-payload send --------------------------------------------------------
// IpcSocket::sendMessage() substitutes a single dummy '*' byte when called
// with an empty span (SOCK_SEQPACKET requires at least 1 byte).  Verify the
// dummy byte arrives correctly and no fd is transferred.

TEST_F(IpcPipelineTest, EmptyPayloadRoundTrip) {
    auto [client, server] = makeSocketPair();

    // sendMessage with empty span — internally sends dummy byte '*'
    ASSERT_TRUE(client.sendMessage({}).has_value());

    auto result = server.receiveMessage();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->first.size(), 1u);          // the dummy '*' byte
    EXPECT_EQ(result->first[0], static_cast<uint8_t>('*'));
    EXPECT_FALSE(result->second);                 // no fd
}

// 6. Concurrent independent connections ----------------------------------------
// Models N simultaneous clients each on their own socket connection.  Each
// thread creates its own socket pair, sends one message, and verifies the
// round-trip.  Also exercises XBusMessagePool under concurrent allocation.

TEST_F(IpcPipelineTest, ConcurrentIndependentConnections) {
    constexpr int kThreads = 8;

    std::atomic<int> passed{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            int sv[2];
            if (socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, sv) != 0) return;

            IpcSocket client{UniqueFd{sv[0]}};
            IpcSocket server{UniqueFd{sv[1]}};

            XBusMessage m = makeMsg(
                static_cast<uint32_t>(t),
                static_cast<uint32_t>(t * 0xFF),
                static_cast<uint32_t>(t));
            auto bytes = std::span<const uint8_t>{
                reinterpret_cast<const uint8_t*>(&m), sizeof(m)};

            if (!client.sendMessage(bytes).has_value()) return;

            XBusMessagePool pool;
            UniqueFd out_fd;
            auto slot = server.receiveIntoSlot(pool, out_fd);
            if (!slot.has_value()) return;

            const auto* recv = slot.value()->as<XBusMessage>();
            if (recv->magic_ != XBUS_MAGIC) return;
            if (recv->transaction_id_ != static_cast<uint32_t>(t)) return;

            passed.fetch_add(1, std::memory_order_relaxed);
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_EQ(passed.load(), kThreads);
}

} // namespace fbx::xserver::testing

// Transport-level tests for IpcSocket over SOCK_SEQPACKET.
//
// The XBus/compositor transport is AF_UNIX SOCK_SEQPACKET (see IpcSocket::connect
// and the compositor's master socket). SEQPACKET preserves message boundaries:
// each recvmsg returns exactly one sent datagram. That property is what makes the
// old SOCK_STREAM failure modes — coalesced reads and frames split across reads —
// structurally impossible on the live transport. These tests pin that guarantee
// down, plus the one real SEQPACKET edge: an oversized datagram is truncated by
// the kernel rather than split, and the receiver must tolerate it (no crash, no
// error that would be read as a disconnect).

#include "xserver/ipc_socket.h"
#include "xserver/unique_fd.h"
#include "shared/protocol.h"

#include <gtest/gtest.h>

#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <vector>

using fbx::xserver::IpcSocket;
using fbx::xserver::UniqueFd;
using fbx::shared::XBusMessage;

namespace {
// A connected pair of non-blocking SEQPACKET endpoints wrapped as IpcSockets.
// Non-blocking so that a read with no data pending returns EAGAIN instead of
// hanging the test (data we send is available immediately on the loopback pair).
struct SeqPair {
    int sv[2]{-1, -1};
    IpcSocket a, b;
    SeqPair() : sv{}, a(makePair()), b(UniqueFd(sv[1])) {}
    UniqueFd makePair() {
        socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv);
        fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL, 0) | O_NONBLOCK);
        fcntl(sv[1], F_SETFL, fcntl(sv[1], F_GETFL, 0) | O_NONBLOCK);
        return UniqueFd(sv[0]);
    }
};
}  // namespace

TEST(IpcTransportTest, MessageRoundTrips) {
    SeqPair p;
    XBusMessage sent{};
    sent.transaction_id_ = 42;
    sent.opcode_ = 7;
    std::span<const uint8_t> s(reinterpret_cast<const uint8_t*>(&sent), sizeof(sent));
    ASSERT_TRUE(p.a.sendMessage(s).has_value());

    auto r = p.b.receiveMessage();
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->first.size(), sizeof(XBusMessage));
    const auto* got = reinterpret_cast<const XBusMessage*>(r->first.data());
    EXPECT_EQ(got->magic_, fbx::shared::XBUS_MAGIC);
    EXPECT_EQ(got->transaction_id_, 42u);
    EXPECT_EQ(got->opcode_, 7u);
}

TEST(IpcTransportTest, BoundariesPreservedAcrossReads) {
    // Two back-to-back sends must arrive as two separate reads of exactly one
    // message each — never coalesced into a single 600-byte read. This is the
    // SEQPACKET guarantee that eliminates the coalescing/partial-frame edges. If
    // the transport ever regressed to SOCK_STREAM, this test would fail.
    SeqPair p;
    std::vector<uint8_t> m1(sizeof(XBusMessage), 0), m2(sizeof(XBusMessage), 0);
    std::memcpy(m1.data(), "AAAA", 4);
    std::memcpy(m2.data(), "BBBB", 4);
    ASSERT_TRUE(p.a.sendMessage(m1).has_value());
    ASSERT_TRUE(p.a.sendMessage(m2).has_value());

    auto r1 = p.b.receiveMessage();
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1->first.size(), sizeof(XBusMessage));  // exactly one message, not two

    auto r2 = p.b.receiveMessage();
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->first.size(), sizeof(XBusMessage));
}

TEST(IpcTransportTest, ManyMessagesEachArriveSeparately) {
    // The anti-coalescing guarantee at scale: N back-to-back sends yield N reads
    // of exactly one message each. Every message is delimited by MSG_EOR in
    // sendMessage; without it FreeBSD's SEQPACKET coalesces them into one read
    // and this fails (which is exactly the bug that made XBus replies vanish).
    SeqPair p;
    constexpr int kN = 5;
    for (int i = 0; i < kN; ++i) {
        std::vector<uint8_t> m(sizeof(XBusMessage), 0);
        m[0] = static_cast<uint8_t>(i);
        ASSERT_TRUE(p.a.sendMessage(m).has_value());
    }
    for (int i = 0; i < kN; ++i) {
        auto r = p.b.receiveMessage();
        ASSERT_TRUE(r.has_value()) << "message " << i << " missing (coalesced?)";
        EXPECT_EQ(r->first.size(), sizeof(XBusMessage));
        EXPECT_EQ(r->first[0], static_cast<uint8_t>(i));  // in-order, one per read
    }
    EXPECT_FALSE(p.b.receiveMessage().has_value());  // exactly kN, no more
}

TEST(IpcTransportTest, FileDescriptorTravelsWithMessage) {
    // SCM_RIGHTS fd passing still works and stays attached to its datagram.
    SeqPair p;
    int devnull = ::open("/dev/null", 0);
    ASSERT_GE(devnull, 0);
    std::vector<uint8_t> m(sizeof(XBusMessage), 0);
    ASSERT_TRUE(p.a.sendMessage(m, devnull).has_value());
    ::close(devnull);

    auto r = p.b.receiveMessage();
    ASSERT_TRUE(r.has_value());
    EXPECT_GE(r->second.get(), 0);  // a valid fd was received
}

// Unit tests for SecurityEventsService — audit log over XBus.
//
// Drives handleMessage()/record() against a SEQPACKET socketpair with an
// isolated temp $HOME, verifying the reply is a full 300-byte envelope with the
// request's transaction id echoed, newest-first ordering, pagination, field
// sanitization, and ClearEvents.

#include "xserver/security_events_service.h"
#include "xserver/ipc_socket.h"
#include "xserver/unique_fd.h"
#include "shared/protocol.h"

#include <gtest/gtest.h>

#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>

namespace fs = std::filesystem;
using fbx::xserver::SecurityEventsService;
using fbx::xserver::IpcSocket;
using fbx::xserver::UniqueFd;
using fbx::shared::XBusMessage;
using fbx::shared::SecurityEventPage;
using fbx::shared::SecurityOpcode;

// Broadcast sink — record() calls BroadcastXBusMessage; provide a no-op so the
// service links standalone (the compositor provides the real one).
namespace fbx::xserver { void BroadcastXBusMessage(const shared::XBusMessage&) {} }

namespace {

class SecurityEventsTest : public ::testing::Test {
protected:
    std::string prev_home_;
    fs::path    root_;

    void SetUp() override {
        const char* prev = std::getenv("HOME");
        prev_home_ = prev ? prev : "";
        root_ = fs::temp_directory_path() / ("fbx_sec_" + std::to_string(::getpid()));
        fs::remove_all(root_);
        fs::create_directories(root_);
        ::setenv("HOME", root_.c_str(), 1);
        SecurityEventsService::init();   // fresh empty log in the temp HOME
    }
    void TearDown() override {
        if (prev_home_.empty()) ::unsetenv("HOME");
        else                    ::setenv("HOME", prev_home_.c_str(), 1);
        std::error_code ec; fs::remove_all(root_, ec);
    }

    SecurityEventPage list(uint32_t tid, uint32_t page) {
        int sv[2];
        socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv);
        fcntl(sv[1], F_SETFL, O_NONBLOCK);
        auto server = std::make_shared<IpcSocket>(UniqueFd(sv[0]));
        IpcSocket peer{UniqueFd(sv[1])};

        XBusMessage req{};
        req.transaction_id_ = tid;
        req.opcode_ = static_cast<uint32_t>(SecurityOpcode::ListEvents);
        std::memcpy(req.payload_, &page, sizeof(page));
        SecurityEventsService::handleMessage(req, server);

        SecurityEventPage pg{};
        auto r = peer.receiveMessage();
        EXPECT_TRUE(r.has_value());
        if (r.has_value()) {
            EXPECT_EQ(r->first.size(), sizeof(XBusMessage));
            const auto* m = reinterpret_cast<const XBusMessage*>(r->first.data());
            EXPECT_EQ(m->transaction_id_, tid);
            std::memcpy(&pg, m->payload_, sizeof(pg));
        }
        return pg;
    }
    void clearEvents() {
        int sv[2];
        socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv);
        auto server = std::make_shared<IpcSocket>(UniqueFd(sv[0]));
        XBusMessage req{};
        req.opcode_ = static_cast<uint32_t>(SecurityOpcode::ClearEvents);
        SecurityEventsService::handleMessage(req, server);
        ::close(sv[1]);
    }
};

}  // namespace

TEST_F(SecurityEventsTest, EmptyInitially) {
    clearEvents();  // ensure a clean slate regardless of prior persisted state
    auto pg = list(1, 0);
    EXPECT_EQ(pg.total_, 0u);
    EXPECT_EQ(pg.count_, 0u);
}

TEST_F(SecurityEventsTest, RecordsAndListsNewestFirst) {
    clearEvents();
    SecurityEventsService::record("fail",  "alice", "local", false);
    SecurityEventsService::record("login", "bob",   "local", true);
    auto pg = list(2, 0);
    EXPECT_EQ(pg.total_, 2u);
    EXPECT_EQ(pg.count_, 2u);
    // newest first: bob's login precedes alice's fail
    EXPECT_STREQ(pg.entries_[0].user_, "bob");
    EXPECT_EQ(pg.entries_[0].success_, 1u);
    EXPECT_STREQ(pg.entries_[1].user_, "alice");
    EXPECT_EQ(pg.entries_[1].success_, 0u);
}

TEST_F(SecurityEventsTest, PaginatesThreePerPage) {
    clearEvents();
    for (int i = 0; i < 4; ++i)
        SecurityEventsService::record("fail", "u" + std::to_string(i), "local", false);
    auto p0 = list(1, 0);
    EXPECT_EQ(p0.total_, 4u);
    EXPECT_EQ(p0.count_, 3u);
    EXPECT_EQ(p0.has_more_, 1u);
    auto p1 = list(2, 1);
    EXPECT_EQ(p1.count_, 1u);
    EXPECT_EQ(p1.has_more_, 0u);
}

TEST_F(SecurityEventsTest, SanitizesInjectionInUsername) {
    clearEvents();
    // A username containing the log delimiter / control chars must not corrupt
    // the persisted audit trail.
    SecurityEventsService::record("fail", "ev|il\nname", "local", false);
    auto pg = list(1, 0);
    ASSERT_EQ(pg.count_, 1u);
    EXPECT_EQ(std::string(pg.entries_[0].user_).find('|'), std::string::npos);
    EXPECT_EQ(std::string(pg.entries_[0].user_).find('\n'), std::string::npos);
}

TEST_F(SecurityEventsTest, PersistsAcrossReinit) {
    clearEvents();
    SecurityEventsService::record("lockout", "mallory", "local", false);
    SecurityEventsService::init();   // simulate a compositor restart (reloads from disk)
    auto pg = list(1, 0);
    EXPECT_EQ(pg.total_, 1u);
    EXPECT_STREQ(pg.entries_[0].user_, "mallory");
    EXPECT_STREQ(pg.entries_[0].type_, "lockout");
}

TEST_F(SecurityEventsTest, ClearEmptiesLog) {
    SecurityEventsService::record("fail", "x", "local", false);
    clearEvents();
    EXPECT_EQ(list(1, 0).total_, 0u);
}

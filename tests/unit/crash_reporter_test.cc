// Unit tests for CrashReporterService — serves the crash spool over XBus.
//
// Drives handleMessage() against a SEQPACKET socketpair with an isolated temp
// $HOME/.freebsdx/crashes, and reads back the reply. Verifies the reply is a
// full 300-byte envelope with the request's transaction id echoed (so the
// client's Call callback matches), paging over the CrashReportPage, and that
// DeleteReport/ClearAll remove spool files.

#include "xserver/crash_reporter_service.h"
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
#include <fstream>
#include <memory>

namespace fs = std::filesystem;
using fbx::xserver::CrashReporterService;
using fbx::xserver::IpcSocket;
using fbx::xserver::UniqueFd;
using fbx::shared::XBusMessage;
using fbx::shared::CrashReportPage;
using fbx::shared::CrashOpcode;

namespace {

class CrashReporterTest : public ::testing::Test {
protected:
    fs::path    dir_;
    std::string prev_home_;

    void SetUp() override {
        const char* prev = std::getenv("HOME");
        prev_home_ = prev ? prev : "";
        auto root = fs::temp_directory_path() /
                    ("fbx_crashsvc_" + std::to_string(::getpid()));
        fs::remove_all(root);
        dir_ = root / ".freebsdx" / "crashes";
        fs::create_directories(dir_);
        ::setenv("HOME", root.c_str(), 1);
    }
    void TearDown() override {
        if (prev_home_.empty()) ::unsetenv("HOME");
        else                    ::setenv("HOME", prev_home_.c_str(), 1);
        std::error_code ec;
        fs::remove_all(dir_.parent_path().parent_path(), ec);
    }

    void writeCrash(const std::string& proc, int pid, const std::string& sig) {
        std::ofstream(dir_ / (proc + "-" + std::to_string(pid) + ".crash"))
            << "\n=== FreeBSD-X crash: " << sig << " @ 0x0 ===\n"
            << "0x1 <someFrame+0x1> at /bin/" << proc << "\n";
    }

    // Send one request into the service and read the reply page back.
    CrashReportPage roundtrip(CrashOpcode op, uint32_t tid,
                              const void* payload, size_t plen,
                              bool* got_reply = nullptr) {
        int sv[2];
        socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv);
        fcntl(sv[1], F_SETFL, O_NONBLOCK);
        auto server = std::make_shared<IpcSocket>(UniqueFd(sv[0]));
        IpcSocket peer{UniqueFd(sv[1])};

        XBusMessage req{};
        req.transaction_id_ = tid;
        req.opcode_ = static_cast<uint32_t>(op);
        if (payload && plen) std::memcpy(req.payload_, payload, plen);
        CrashReporterService::handleMessage(req, server);

        CrashReportPage pg{};
        auto r = peer.receiveMessage();
        if (got_reply) *got_reply = r.has_value();
        if (r.has_value()) {
            EXPECT_EQ(r->first.size(), sizeof(XBusMessage));  // full envelope
            const auto* m = reinterpret_cast<const XBusMessage*>(r->first.data());
            EXPECT_EQ(m->transaction_id_, tid);               // tid echoed
            std::memcpy(&pg, m->payload_, sizeof(pg));
        }
        return pg;
    }
};

}  // namespace

TEST_F(CrashReporterTest, EmptySpoolReportsZero) {
    bool got = false;
    uint32_t page = 0;
    auto pg = roundtrip(CrashOpcode::ListReports, 7, &page, sizeof(page), &got);
    ASSERT_TRUE(got);
    EXPECT_EQ(pg.total_, 0u);
    EXPECT_EQ(pg.count_, 0u);
}

TEST_F(CrashReporterTest, ListsCrashesWithMetadata) {
    writeCrash("firefox", 100, "SIGSEGV");
    writeCrash("mailer",  200, "SIGABRT");
    uint32_t page = 0;
    auto pg = roundtrip(CrashOpcode::ListReports, 11, &page, sizeof(page));
    EXPECT_EQ(pg.total_, 2u);
    EXPECT_EQ(pg.count_, 2u);
    EXPECT_EQ(pg.has_more_, 0u);
    // Entries carry app + signal; both crashes present.
    std::string apps = std::string(pg.entries_[0].app_) + "," + pg.entries_[1].app_;
    EXPECT_NE(apps.find("firefox"), std::string::npos);
    EXPECT_NE(apps.find("mailer"),  std::string::npos);
    EXPECT_NE(pg.entries_[0].report_id_, 0u);
}

TEST_F(CrashReporterTest, PaginatesThreePerPage) {
    for (int i = 0; i < 4; ++i) writeCrash("app", i, "SIGSEGV");
    uint32_t page0 = 0, page1 = 1;
    auto p0 = roundtrip(CrashOpcode::ListReports, 1, &page0, sizeof(page0));
    EXPECT_EQ(p0.total_, 4u);
    EXPECT_EQ(p0.count_, 3u);
    EXPECT_EQ(p0.has_more_, 1u);
    auto p1 = roundtrip(CrashOpcode::ListReports, 2, &page1, sizeof(page1));
    EXPECT_EQ(p1.count_, 1u);
    EXPECT_EQ(p1.has_more_, 0u);
}

TEST_F(CrashReporterTest, DeleteReportRemovesOne) {
    writeCrash("firefox", 100, "SIGSEGV");
    writeCrash("mailer",  200, "SIGABRT");
    uint32_t page = 0;
    auto pg = roundtrip(CrashOpcode::ListReports, 1, &page, sizeof(page));
    ASSERT_EQ(pg.total_, 2u);

    fbx::shared::CrashDeleteRequest del{};
    del.report_id_ = pg.entries_[0].report_id_;
    roundtrip(CrashOpcode::DeleteReport, 2, &del, sizeof(del));  // no reply expected

    auto after = roundtrip(CrashOpcode::ListReports, 3, &page, sizeof(page));
    EXPECT_EQ(after.total_, 1u);
}

TEST_F(CrashReporterTest, ClearAllEmptiesSpool) {
    writeCrash("a", 1, "SIGSEGV");
    writeCrash("b", 2, "SIGBUS");
    roundtrip(CrashOpcode::ClearAll, 1, nullptr, 0);
    uint32_t page = 0;
    auto pg = roundtrip(CrashOpcode::ListReports, 2, &page, sizeof(page));
    EXPECT_EQ(pg.total_, 0u);
}

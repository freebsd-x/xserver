#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "xserver/fetch_broker.h"
#include "xserver/desktop_services.h"
#include "xserver/fetch_policy.h"
#include "xserver/ipc_socket.h"
#include "xserver/peer_identity.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <future>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <unordered_map>

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/capsicum.h>
#include <sys/socket.h>

namespace fbx::xserver {
namespace {

constexpr const char* kName = "com.freebsdx.Fetch";
constexpr size_t kMaxPending = 256;   // relayed requests awaiting xfetch

struct Pending {
    std::weak_ptr<IpcSocket> client;  // weak: the app may be gone (and its fd reused) by reply time
    uint32_t                 txn;     // the app's own transaction id
    // Set instead of client for the compositor's own requests (fetchBlocking).
    std::shared_ptr<std::promise<std::pair<shared::FetchReply, UniqueFd>>> internal;
};

std::mutex                            g_mu;
std::weak_ptr<IpcSocket>              g_provider;
std::unordered_map<uint32_t, Pending> g_pending;   // keyed by the id we gave xfetch
uint32_t                              g_next_id = 1;
std::thread::id                       g_loop_thread;   // where replies are handled (set on registration)

void replyTo(const std::shared_ptr<IpcSocket>& c, uint32_t txn, const shared::FetchReply& r, int fd = -1) {
    shared::XBusMessage m{};
    m.magic_ = shared::XBUS_MAGIC;
    m.transaction_id_ = txn;
    std::strncpy(m.service_name_, kName, sizeof(m.service_name_) - 1);
    m.opcode_ = static_cast<uint32_t>(shared::FetchOpcode::Get);
    std::memcpy(m.payload_, &r, sizeof(r));
    (void)c->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)}, fd);
}

void fail(const std::shared_ptr<IpcSocket>& c, uint32_t txn, shared::FetchStatus st) {
    shared::FetchReply r{};
    r.status_ = static_cast<int32_t>(st);
    replyTo(c, txn, r);
}

// Answer everything still waiting on a provider that's gone or was replaced.
void failAllPendingLocked() {
    for (auto& [id, p] : g_pending) {
        if (p.internal) {
            shared::FetchReply r{};
            r.status_ = static_cast<int32_t>(shared::FetchStatus::Unavailable);
            p.internal->set_value({r, UniqueFd{}});
        } else if (auto c = p.client.lock()) {
            fail(c, p.txn, shared::FetchStatus::Unavailable);
        }
    }
    g_pending.clear();
}

// ── Direct connections ──────────────────────────────────────────────────────

std::atomic<int> g_connecting{0};
constexpr int kMaxConnecting = 8;
constexpr int kConnectTimeoutMs = 15000;

void replyConnect(const std::shared_ptr<IpcSocket>& c, uint32_t txn, shared::FetchStatus st,
                  const std::string& error = {}, int fd = -1) {
    shared::NetConnectReply r{};
    r.status_ = static_cast<int32_t>(st);
    std::strncpy(r.error_, error.c_str(), sizeof(r.error_) - 1);
    shared::XBusMessage m{};
    m.magic_ = shared::XBUS_MAGIC;
    m.transaction_id_ = txn;
    std::strncpy(m.service_name_, kName, sizeof(m.service_name_) - 1);
    m.opcode_ = static_cast<uint32_t>(shared::FetchOpcode::Connect);
    std::memcpy(m.payload_, &r, sizeof(r));
    (void)c->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)}, fd);
}

// Tests talk to a server on an unprivileged port. Headless sessions only, so it
// can't widen what a real desktop allows.
bool g_test_ports_allowed = false;

bool testPortAllowed(uint16_t port) {
    if (!g_test_ports_allowed) return false;
    const char* ports = std::getenv("FBX_TEST_CONNECT_PORTS");
    if (!ports) return false;
    const std::string list = std::string(",") + ports + ",";
    return list.find("," + std::to_string(port) + ",") != std::string::npos;
}

UniqueFd connectTo(const std::string& host, uint16_t port, std::string& error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (int rc = ::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found); rc != 0) {
        error = std::string("can't find ") + host + ": " + ::gai_strerror(rc);
        return {};
    }
    UniqueFd result;
    for (addrinfo* a = found; a && !result; a = a->ai_next) {
        UniqueFd s(::socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, a->ai_protocol));
        if (!s) { error = std::strerror(errno); continue; }
        int rc = ::connect(s.get(), a->ai_addr, a->ai_addrlen);
        if (rc != 0 && errno == EINPROGRESS) {
            pollfd p{s.get(), POLLOUT, 0};
            rc = ::poll(&p, 1, kConnectTimeoutMs);
            if (rc == 0) { error = "the connection timed out"; continue; }
            int so = 0;
            socklen_t len = sizeof(so);
            (void)::getsockopt(s.get(), SOL_SOCKET, SO_ERROR, &so, &len);
            errno = so;
            rc = (rc > 0 && so == 0) ? 0 : -1;
        }
        if (rc != 0) { error = std::strerror(errno); continue; }
        result = std::move(s);
    }
    ::freeaddrinfo(found);
    if (!result) return {};
    // A connection, not a way to make others: it can't connect, bind or listen.
    cap_rights_t rights;
    cap_rights_init(&rights, CAP_READ, CAP_WRITE, CAP_EVENT, CAP_FCNTL, CAP_FSTAT, CAP_GETPEERNAME,
                    CAP_GETSOCKNAME, CAP_GETSOCKOPT, CAP_SETSOCKOPT, CAP_SHUTDOWN);
    (void)cap_rights_limit(result.get(), &rights);
    return result;
}

void handleConnect(const std::shared_ptr<IpcSocket>& client, const shared::XBusMessage& msg) {
    shared::NetConnectRequest req{};
    std::memcpy(&req, msg.payload_, sizeof(req));
    req.host_[sizeof(req.host_) - 1] = '\0';
    const std::string host = req.host_;
    const uint16_t port = req.port_;
    const std::string app = verifiedAppId(client->fd());
    if (app.empty() || (!fetch_policy::mayConnect(app, port) && !(app == "com.freebsdx.mail" && testPortAllowed(port)))) {
        std::cerr << "[Fetch] DENIED: " << (app.empty() ? "unknown app" : app) << " connecting to " << host << ":" << port << "\n";
        replyConnect(client, msg.transaction_id_, shared::FetchStatus::Denied, "that port isn't allowed");
        return;
    }
    if (host.empty()) {
        replyConnect(client, msg.transaction_id_, shared::FetchStatus::BadRequest, "no server name");
        return;
    }
    if (g_connecting.fetch_add(1) >= kMaxConnecting) {
        g_connecting.fetch_sub(1);
        replyConnect(client, msg.transaction_id_, shared::FetchStatus::Unavailable, "too many connections at once");
        return;
    }
    // Name lookup and connecting can take seconds: not on the compositor's thread.
    std::thread([weak = std::weak_ptr<IpcSocket>(client), txn = msg.transaction_id_, host, port] {
        std::string error;
        UniqueFd s = connectTo(host, port, error);
        if (auto c = weak.lock()) {
            if (s) replyConnect(c, txn, shared::FetchStatus::Ok, {}, s.get());
            else replyConnect(c, txn, shared::FetchStatus::NetworkError, error);
        }
        g_connecting.fetch_sub(1);
    }).detach();
}

uint32_t nextIdLocked() {
    uint32_t id = g_next_id++;
    return id ? id : g_next_id++;
}

}  // namespace

void FetchBroker::allowTestPorts() noexcept { g_test_ports_allowed = true; }

namespace {

// A long address from a client's descriptor: an ordinary memory object of a sane size,
// not a pipe that would leave the compositor waiting.
bool readLongUrl(int fd, std::string& out) {
    struct stat st{};
    if (fd < 0 || ::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        static_cast<size_t>(st.st_size) >= fetch_policy::kWebMaxUrl)
        return false;
    out.resize(static_cast<size_t>(st.st_size));
    const ssize_t got = ::pread(fd, out.data(), out.size(), 0);
    return got == st.st_size;
}

UniqueFd textFd(const std::string& text) {
    UniqueFd fd(shm_open(SHM_ANON, O_RDWR | O_CLOEXEC, 0600));
    if (!fd || ::ftruncate(fd.get(), static_cast<off_t>(text.size())) != 0) return {};
    void* map = ::mmap(nullptr, text.size(), PROT_WRITE, MAP_SHARED, fd.get(), 0);
    if (map == MAP_FAILED) return {};
    std::memcpy(map, text.data(), text.size());
    ::munmap(map, text.size());
    return fd;
}

}  // namespace

void FetchBroker::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                 const shared::XBusMessage& msg, UniqueFd& fd) noexcept {
    using O = shared::FetchOpcode;
    if (static_cast<O>(msg.opcode_) == O::Connect) {
        handleConnect(client, msg);
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    switch (static_cast<O>(msg.opcode_)) {
    case O::ProviderRegister:
        // Only the real xfetch binary may serve fetches (by executable, like
        // the shell's trust check) — anything else could read every app's
        // requests and answer them with whatever it liked.
        if (!peer::isOneOf(client->fd(), {"xfetch"})) {
            std::cerr << "[Fetch] DENIED: provider registration from " << peer::executable(client->fd()) << "\n";
            return;
        }
        failAllPendingLocked();
        g_provider = client;
        g_loop_thread = std::this_thread::get_id();
        std::cout << "[Fetch] xfetch registered\n";
        return;

    case O::ProviderReply: {
        if (client != g_provider.lock()) return;   // only the registered provider answers
        auto it = g_pending.find(msg.transaction_id_);
        if (it == g_pending.end()) return;
        Pending p = it->second;
        g_pending.erase(it);
        shared::FetchReply r{};
        std::memcpy(&r, msg.payload_, sizeof(r));
        if (p.internal) {   // a compositor service waiting in fetchBlocking reads the body itself
            p.internal->set_value({r, std::move(fd)});
            return;
        }
        auto c = p.client.lock();
        if (!c) return;
        replyTo(c, p.txn, r, fd ? fd.get() : -1);
        return;
    }

    case O::Get: {
        shared::FetchRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.url_[sizeof(req.url_) - 1] = '\0';
        const bool web = (req.flags_ & shared::kFetchWeb) != 0;
        // A long address comes in a descriptor. It's read once and passed on as our own
        // copy: the client could change its copy after we'd checked it.
        std::string long_url;
        UniqueFd url_copy;
        if (web && (req.flags_ & shared::kFetchUrlInFd)) {
            if (!readLongUrl(fd.get(), long_url) || !(url_copy = textFd(long_url))) {
                fail(client, msg.transaction_id_, shared::FetchStatus::BadRequest);
                return;
            }
        }
        const std::string url = long_url.empty() ? std::string(req.url_) : long_url;
        // Browsing is for our web engine alone, the viewer or the Browser app built on
        // it, known by what they run.
        const std::string app = web && peer::isOneOf(client->fd(), {"xwebview", "browser"}) ? fetch_policy::kWebApp : verifiedAppId(client->fd());
        const bool permitted = web ? app == fetch_policy::kWebApp && fetch_policy::webUrlAllowed(url)
                                   : fetch_policy::allowed(app, req.url_);
        // The compositor's identity can't be claimed by an app's manifest.
        if (app == fetch_policy::kCompositor || !permitted) {
            std::cerr << "[Fetch] DENIED: " << (app.empty() ? "unknown app" : app) << " → " << url.substr(0, 200) << "\n";
            fail(client, msg.transaction_id_, shared::FetchStatus::Denied);
            return;
        }
        auto provider = g_provider.lock();
        if (!provider) { fail(client, msg.transaction_id_, shared::FetchStatus::Unavailable); return; }
        if (g_pending.size() >= kMaxPending) { fail(client, msg.transaction_id_, shared::FetchStatus::Unavailable); return; }

        uint32_t id = nextIdLocked();
        shared::XBusMessage fwd = msg;
        fwd.transaction_id_ = id;
        fwd.opcode_ = static_cast<uint32_t>(O::Get);
        std::memcpy(fwd.payload_, &req, sizeof(req));
        if (!provider->sendMessage({reinterpret_cast<const uint8_t*>(&fwd), sizeof(fwd)}, url_copy ? url_copy.get() : -1)) {
            fail(client, msg.transaction_id_, shared::FetchStatus::Unavailable);
            return;
        }
        g_pending[id] = {client, msg.transaction_id_};
        return;
    }
    }
    fail(client, msg.transaction_id_, shared::FetchStatus::BadRequest);
}

FetchBroker::Result FetchBroker::fetchBlocking(const std::string& url, std::chrono::seconds timeout) noexcept {
    Result out;
    if (!fetch_policy::allowed(fetch_policy::kCompositor, url)) {
        std::cerr << "[Fetch] DENIED: compositor → " << url << " (not in fetch_policy.h)\n";
        out.status = shared::FetchStatus::Denied;
        return out;
    }
    auto promise = std::make_shared<std::promise<std::pair<shared::FetchReply, UniqueFd>>>();
    auto future = promise->get_future();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (std::this_thread::get_id() == g_loop_thread) {
            std::cerr << "[Fetch] fetchBlocking called on the main loop (would deadlock): " << url << "\n";
            return out;
        }
        auto provider = g_provider.lock();
        if (!provider || g_pending.size() >= kMaxPending) return out;
        shared::XBusMessage m{};
        m.magic_ = shared::XBUS_MAGIC;
        m.transaction_id_ = nextIdLocked();
        std::strncpy(m.service_name_, kName, sizeof(m.service_name_) - 1);
        m.opcode_ = static_cast<uint32_t>(shared::FetchOpcode::Get);
        shared::FetchRequest req{};
        if (url.size() >= sizeof(req.url_)) { out.status = shared::FetchStatus::BadRequest; return out; }
        std::strncpy(req.url_, url.c_str(), sizeof(req.url_) - 1);
        std::memcpy(m.payload_, &req, sizeof(req));
        if (!provider->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)})) return out;
        g_pending[m.transaction_id_] = {{}, 0, promise};
    }
    if (future.wait_for(timeout) != std::future_status::ready) return out;   // (its late reply is dropped)
    auto [reply, fd] = future.get();
    out.status = static_cast<shared::FetchStatus>(reply.status_);
    out.http_status = reply.http_status_;
    if (out.ok()) {
        if (!fd) { out.status = shared::FetchStatus::NetworkError; return out; }
        out.body.resize(reply.size_);
        size_t got = 0;
        while (got < out.body.size()) {
            ssize_t r = ::pread(fd.get(), out.body.data() + got, out.body.size() - got, static_cast<off_t>(got));
            if (r <= 0) break;
            got += static_cast<size_t>(r);
        }
        out.body.resize(got);
    }
    return out;
}

bool FetchBroker::fetchToFile(const std::string& url, const std::string& path,
                              std::chrono::seconds timeout) noexcept {
    Result r = fetchBlocking(url, timeout);
    if (!r.ok()) {
        std::cerr << "[Fetch] " << url << " failed (status " << static_cast<int>(r.status)
                  << ", HTTP " << r.http_status << ")\n";
        return false;
    }
    // mkstemp: a new file of our own (O_EXCL, never a pre-planted symlink —
    // package downloads land in the shared /tmp), renamed into place.
    std::string tmp = path + ".XXXXXX";
    const int fd = ::mkstemp(tmp.data());
    if (fd < 0) return false;
    size_t off = 0;
    while (off < r.body.size()) {
        ssize_t w = ::write(fd, r.body.data() + off, r.body.size() - off);
        if (w <= 0) break;
        off += static_cast<size_t>(w);
    }
    const bool written = off == r.body.size() && ::close(fd) == 0;
    if (!written || std::rename(tmp.c_str(), path.c_str()) != 0) {
        if (!written) ::close(fd);
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

}  // namespace fbx::xserver

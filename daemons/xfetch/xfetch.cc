// xfetch — does the network fetches sandboxed apps can't (com.freebsdx.Fetch).
//
// The compositor checks each app's request against its host allowlist
// (xserver/fetch_policy.h) and relays it here; xfetch fetches it (re-checking
// the host is one the policy knows, so it is never an open proxy), keeps a
// disk cache for cacheable hosts (map tiles), and answers with the body in a
// read-only anonymous shared-memory fd. It only moves bytes: decoding (PNG,
// JSON) stays in the sandboxed app.
#include "shared/logger.h"
#include "shared/protocol.h"
#include "xserver/crash_handler.h"
#include "xserver/fetch_policy.h"
#include "xserver/ipc_socket.h"

#include <curl/curl.h>
#include <fcntl.h>
#include <sys/capsicum.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <semaphore>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace fbx;

namespace {

constexpr size_t kMaxBody      = 8u << 20;       // 8 MiB
constexpr long   kTimeoutSec   = 15;
constexpr time_t kCacheMaxAge  = 7 * 24 * 3600;  // tiles: a week (OSM tile usage policy)
constexpr const char* kUserAgent = "FreeBSD-X/1.0 (xfetch; +https://github.com/freebsd-x)";

std::shared_ptr<xserver::IpcSocket> g_sock;
std::mutex                          g_send_mu;
std::counting_semaphore<6>          g_slots{6};   // concurrent transfers
fs::path                            g_cache_dir;

// Cache file name: FNV-1a 64 of the URL, in hex.
std::string cacheName(const std::string& url) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : url) { h ^= c; h *= 1099511628211ull; }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

bool readCache(const std::string& url, std::string& body) {
    if (g_cache_dir.empty()) return false;
    fs::path p = g_cache_dir / cacheName(url);
    struct stat st{};
    if (::stat(p.c_str(), &st) != 0 || st.st_size <= 0 || static_cast<size_t>(st.st_size) > kMaxBody) return false;
    if (std::time(nullptr) - st.st_mtime > kCacheMaxAge) return false;
    std::ifstream f(p, std::ios::binary);
    body.assign(std::istreambuf_iterator<char>(f), {});
    return static_cast<bool>(f) || f.eof();
}

void writeCache(const std::string& url, const std::string& body) {
    if (g_cache_dir.empty()) return;
    fs::path p = g_cache_dir / cacheName(url);
    fs::path tmp = p; tmp += ".tmp" + std::to_string(::getpid()) + "-" +
                             std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(body.data(), static_cast<std::streamsize>(body.size()));
        if (!f) { std::error_code ec; fs::remove(tmp, ec); return; }
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);   // atomic: a reader never sees half a tile
    if (ec) fs::remove(tmp, ec);
}

size_t onData(char* p, size_t sz, size_t n, void* ud) {
    auto* body = static_cast<std::string*>(ud);
    if (body->size() + sz * n > kMaxBody) return 0;   // aborts the transfer
    body->append(p, sz * n);
    return sz * n;
}

shared::FetchStatus httpGet(const std::string& url, std::string& body, long& code) {
    CURL* c = curl_easy_init();
    if (!c) return shared::FetchStatus::NetworkError;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, onData);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    // No redirects: a redirect could lead off the allowlisted host.
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_TIMEOUT, kTimeoutSec);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(c, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(kMaxBody));
    CURLcode rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    if (rc == CURLE_WRITE_ERROR || rc == CURLE_FILESIZE_EXCEEDED) return shared::FetchStatus::TooLarge;
    if (rc != CURLE_OK) return shared::FetchStatus::NetworkError;
    if (code < 200 || code >= 300) return shared::FetchStatus::HttpError;
    return shared::FetchStatus::Ok;
}

// The body in a sealed-for-reading anonymous shm object.
int bodyFd(const std::string& body) {
    int fd = shm_open(SHM_ANON, O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    if (::ftruncate(fd, static_cast<off_t>(body.size())) != 0) { ::close(fd); return -1; }
    if (!body.empty()) {   // shm objects are filled by mapping them, not write(2)
        void* p = ::mmap(nullptr, body.size(), PROT_WRITE, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) { ::close(fd); return -1; }
        std::memcpy(p, body.data(), body.size());
        ::munmap(p, body.size());
    }
    cap_rights_t rights;
    cap_rights_init(&rights, CAP_READ, CAP_SEEK, CAP_FSTAT, CAP_MMAP_R);
    (void)cap_rights_limit(fd, &rights);   // the app can read it, never write it back
    return fd;
}

void reply(uint32_t txn, const shared::FetchReply& r, int fd) {
    shared::XBusMessage m{};
    m.magic_ = shared::XBUS_MAGIC;
    m.transaction_id_ = txn;
    std::strncpy(m.service_name_, "com.freebsdx.Fetch", sizeof(m.service_name_) - 1);
    m.opcode_ = static_cast<uint32_t>(shared::FetchOpcode::ProviderReply);
    std::memcpy(m.payload_, &r, sizeof(r));
    std::lock_guard<std::mutex> lk(g_send_mu);
    (void)g_sock->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)}, fd);
}

void serve(uint32_t txn, std::string url) {
    g_slots.acquire();
    shared::FetchReply r{};
    bool cacheable = false;
    std::string body;
    if (!xserver::fetch_policy::knownHost(url, &cacheable)) {
        r.status_ = static_cast<int32_t>(shared::FetchStatus::Denied);
    } else if (cacheable && readCache(url, body)) {
        r.status_ = static_cast<int32_t>(shared::FetchStatus::Ok);
        r.http_status_ = 200;
        r.from_cache_ = 1;
    } else {
        long code = 0;
        auto st = httpGet(url, body, code);
        r.status_ = static_cast<int32_t>(st);
        r.http_status_ = static_cast<uint32_t>(code);
        if (st == shared::FetchStatus::Ok && cacheable) writeCache(url, body);
    }
    int fd = -1;
    if (r.status_ == static_cast<int32_t>(shared::FetchStatus::Ok)) {
        fd = bodyFd(body);
        if (fd < 0) r.status_ = static_cast<int32_t>(shared::FetchStatus::NetworkError);
        else r.size_ = body.size();
    }
    if (r.status_ != static_cast<int32_t>(shared::FetchStatus::Ok))
        LOG_INFO("{} → status {} (HTTP {})", url, r.status_, r.http_status_);
    reply(txn, r, fd);
    if (fd >= 0) ::close(fd);
    g_slots.release();
}

}  // namespace

int main() {
    shared::Logger::init("xfetch");
    xserver::installCrashHandler("xfetch");
    std::signal(SIGPIPE, SIG_IGN);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    if (const char* home = std::getenv("HOME"); home && *home) {
        std::error_code ec;
        fs::path dir = fs::path(home) / ".cache" / "freebsdx" / "fetch";
        fs::create_directories(dir, ec);
        if (!ec) g_cache_dir = dir;
    }

    const char* path = std::getenv("XBUS_SOCKET");
    g_sock = std::make_shared<xserver::IpcSocket>(std::string(path && *path ? path : "/tmp/freebsdx.sock"));
    bool connected = false;
    for (int i = 0; i < 50 && !(connected = g_sock->connect()); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!connected) { LOG_ERROR("can't reach the compositor"); return 1; }

    shared::XBusMessage reg{};
    reg.magic_ = shared::XBUS_MAGIC;
    std::strncpy(reg.service_name_, "com.freebsdx.Fetch", sizeof(reg.service_name_) - 1);
    reg.opcode_ = static_cast<uint32_t>(shared::FetchOpcode::ProviderRegister);
    {
        std::lock_guard<std::mutex> lk(g_send_mu);
        (void)g_sock->sendMessage({reinterpret_cast<const uint8_t*>(&reg), sizeof(reg)});
    }
    LOG_INFO("serving com.freebsdx.Fetch (cache: {})", g_cache_dir.empty() ? "none" : g_cache_dir.string());

    // Every connection also gets the compositor's broadcasts; only relayed Gets matter.
    while (true) {
        auto res = g_sock->receiveMessage();
        if (!res) break;   // the compositor went away: so do we
        const auto& data = res->first;
        if (data.size() < sizeof(shared::XBusMessage)) continue;
        shared::XBusMessage m{};
        std::memcpy(&m, data.data(), sizeof(m));
        if (m.magic_ != shared::XBUS_MAGIC || std::strcmp(m.service_name_, "com.freebsdx.Fetch") != 0 ||
            m.opcode_ != static_cast<uint32_t>(shared::FetchOpcode::Get))
            continue;
        shared::FetchRequest req{};
        std::memcpy(&req, m.payload_, sizeof(req));
        req.url_[sizeof(req.url_) - 1] = '\0';
        std::thread(serve, m.transaction_id_, std::string(req.url_)).detach();
    }
    LOG_INFO("compositor closed the connection; exiting");
    std::_Exit(0);   // detached transfers may be mid-request: skip atexit teardown
}

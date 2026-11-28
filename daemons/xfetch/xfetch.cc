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

#include <algorithm>
#include <vector>
#include <curl/curl.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <map>
#include <fcntl.h>
#include <sys/capsicum.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <deque>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace fbx;

namespace {

constexpr const char* kWebUserAgent = "Mozilla/5.0 (FreeBSD; x86_64) FreeBSD-X-xweb/0.1";
constexpr size_t kMaxCached    = 8u << 20;       // cache entries are tiles; anything larger isn't cached
constexpr long   kTimeoutSec   = 15;
constexpr time_t kCacheMaxAge  = 7 * 24 * 3600;  // tiles: a week (OSM tile usage policy)
constexpr const char* kUserAgent = "FreeBSD-X/1.0 (xfetch; +https://github.com/freebsd-x)";

std::shared_ptr<xserver::IpcSocket> g_sock;
std::mutex                          g_send_mu;
fs::path                            g_cache_dir;

// Requests queue here for the network thread, which runs every transfer in
// ONE curl multi handle: requests to a host share its connection (HTTP/2
// streams, multiplexed), so a screenful of map tiles costs one TLS handshake
// and arrives about as fast as the server sends them. (One easy handle per
// request paid a handshake per tile; six pooled ones still took ~2x longer.)
struct Job {
    uint32_t txn;
    std::string url;
    bool web;
    std::chrono::steady_clock::time_point queued;
    bool cookies = false;        // send and keep them (web only)
    bool post = false;           // a form's POST (web only)
    std::string content_type, body;
    bool script = false;         // a script's request (web only): method, headers, body
    std::string method;
    std::vector<std::string> headers;   // "Name: value", checked by the compositor
    // A request with cookies is the site's whose process asked (the compositor saw to
    // that): its redirects are followed here, one at a time, and a step to any other
    // site goes without them. curl's own following would send that site's cookies too,
    // and the page asking would read what it answered.
    std::string cookie_site;
    int hops = 0;
};
const bool g_trace = std::getenv("FBX_FETCH_TRACE") != nullptr;   // per-request timing to the log
std::mutex              g_jobs_mu;
std::deque<Job>         g_jobs;
std::atomic<CURLM*>     g_multi{nullptr};   // set once the network thread is up

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
    if (::stat(p.c_str(), &st) != 0 || st.st_size <= 0 || static_cast<size_t>(st.st_size) > kMaxCached) return false;
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

// A body being received, and the most its rule allows (fetch_policy max_bytes).
struct Sink { std::string data; size_t limit = 0; };

size_t onData(char* p, size_t sz, size_t n, void* ud) {
    auto* sink = static_cast<Sink*>(ud);
    if (sink->data.size() + sz * n > sink->limit) return 0;   // aborts the transfer
    sink->data.append(p, sz * n);
    return sz * n;
}

// Options for one transfer (its body accumulates in *sink).
void setOptions(CURL* c, const std::string& url, Sink* sink) {
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, onData);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, sink);
    // No redirects: a redirect could lead off the allowlisted host.
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_TIMEOUT, kTimeoutSec);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(c, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(sink->limit));
    curl_easy_setopt(c, CURLOPT_PIPEWAIT, 1L);   // wait to multiplex rather than open another connection
}

// Browsing: the extra rules for a kFetchWeb transfer.
// Refuses, as it connects, anything on this machine or the local network, so neither a
// link nor a redirect can reach them. (Tests serve their pages locally and say so.)
curl_socket_t openPublicSocket(void*, curlsocktype, struct curl_sockaddr* addr) {
    static const bool allow_local = std::getenv("FBX_TEST_WEB_LOCAL") != nullptr;
    if (!allow_local && xserver::fetch_policy::isPrivateAddress(&addr->addr)) return CURL_SOCKET_BAD;
    return ::socket(addr->family, addr->socktype, addr->protocol);
}

// ── Missing intermediates ───────────────────────────────────────────────────────
// A server that sends its certificate without the one that signed it (kakaobank.com):
// browsers fetch that one from where the certificate says it's published (its Authority
// Information Access "CA issuers" URL) and verify again. It only helps build the chain:
// the chain must still end at a root we trust.
constexpr size_t kMaxIssuerBytes = 64 * 1024;
constexpr int kMaxIssuerHops = 3;

size_t onIssuerData(char* p, size_t sz, size_t n, void* ud) {
    auto* out = static_cast<std::string*>(ud);
    if (out->size() + sz * n > kMaxIssuerBytes) return 0;
    out->append(p, sz * n);
    return sz * n;
}

// The certificate published at `url` (DER, or PEM), fetched once and remembered.
X509* issuerAt(const std::string& url) {
    static std::mutex mu;
    static std::map<std::string, std::string> fetched;   // url -> its bytes ("" if none)
    std::string bytes;
    {
        std::lock_guard<std::mutex> lk(mu);
        if (const auto it = fetched.find(url); it != fetched.end()) bytes = it->second;
        else bytes = "\x01";   // not yet
    }
    if (bytes == "\x01") {
        bytes.clear();
        if (CURL* c = curl_easy_init()) {
            curl_easy_setopt(c, CURLOPT_URL, url.c_str());
            curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
            curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(c, CURLOPT_MAXREDIRS, 3L);
            curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
            curl_easy_setopt(c, CURLOPT_OPENSOCKETFUNCTION, openPublicSocket);
            curl_easy_setopt(c, CURLOPT_TIMEOUT, 5L);
            curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(c, CURLOPT_USERAGENT, kUserAgent);
            curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, onIssuerData);
            curl_easy_setopt(c, CURLOPT_WRITEDATA, &bytes);
            long status = 0;
            if (curl_easy_perform(c) != CURLE_OK || (curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status), status != 200)) bytes.clear();
            curl_easy_cleanup(c);
        }
        std::lock_guard<std::mutex> lk(mu);
        fetched[url] = bytes;
    }
    if (bytes.empty()) return nullptr;
    const auto* p = reinterpret_cast<const unsigned char*>(bytes.data());
    if (X509* x = d2i_X509(nullptr, &p, static_cast<long>(bytes.size()))) return x;
    BIO* bio = BIO_new_mem_buf(bytes.data(), static_cast<int>(bytes.size()));
    X509* x = bio ? PEM_read_bio_X509(bio, nullptr, nullptr, nullptr) : nullptr;
    if (bio) BIO_free(bio);
    return x;
}

// Where a certificate says the one that signed it is published (http or https only).
std::string caIssuersUrl(X509* cert) {
    std::string url;
    auto* aia = static_cast<AUTHORITY_INFO_ACCESS*>(X509_get_ext_d2i(cert, NID_info_access, nullptr, nullptr));
    if (!aia) return url;
    for (int i = 0; i < sk_ACCESS_DESCRIPTION_num(aia) && url.empty(); ++i) {
        const ACCESS_DESCRIPTION* ad = sk_ACCESS_DESCRIPTION_value(aia, i);
        if (OBJ_obj2nid(ad->method) != NID_ad_ca_issuers || ad->location->type != GEN_URI) continue;
        const ASN1_IA5STRING* s = ad->location->d.uniformResourceIdentifier;
        std::string u(reinterpret_cast<const char*>(s->data), static_cast<size_t>(s->length));
        if (u.rfind("http://", 0) == 0 || u.rfind("https://", 0) == 0) url = std::move(u);
    }
    AUTHORITY_INFO_ACCESS_free(aia);
    return url;
}

int verifyFetchingIssuers(X509_STORE_CTX* ctx, void*) {
    if (X509_verify_cert(ctx) == 1) return 1;
    const auto missing = [](int e) { return e == X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY || e == X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT; };
    if (!missing(X509_STORE_CTX_get_error(ctx))) return 0;
    X509* leaf = X509_STORE_CTX_get0_cert(ctx);
    STACK_OF(X509)* extra = sk_X509_new_null();
    if (!extra) return 0;
    if (const STACK_OF(X509)* sent = X509_STORE_CTX_get0_untrusted(ctx))
        for (int i = 0; i < sk_X509_num(sent); ++i) {
            X509_up_ref(sk_X509_value(sent, i));
            sk_X509_push(extra, sk_X509_value(sent, i));
        }
    // The one whose signer is missing (a reference of our own, as each try's context goes).
    X509* lacking = X509_STORE_CTX_get_current_cert(ctx);
    if (!lacking) lacking = leaf;
    X509_up_ref(lacking);
    int ok = 0;
    for (int hop = 0; hop < kMaxIssuerHops && lacking && !ok; ++hop) {
        const std::string url = caIssuersUrl(lacking);
        X509_free(lacking);
        lacking = nullptr;
        X509* issuer = url.empty() ? nullptr : issuerAt(url);
        if (!issuer) break;
        sk_X509_push(extra, issuer);
        X509_STORE_CTX* again = X509_STORE_CTX_new();
        if (!again) break;
        if (X509_STORE_CTX_init(again, X509_STORE_CTX_get0_store(ctx), leaf, extra) == 1) {
            X509_VERIFY_PARAM_set1(X509_STORE_CTX_get0_param(again), X509_STORE_CTX_get0_param(ctx));   // the host name too
            ok = X509_verify_cert(again) == 1;
            const int e = X509_STORE_CTX_get_error(again);
            if (!ok) X509_STORE_CTX_set_error(ctx, e);
            if (!ok && missing(e) && (lacking = X509_STORE_CTX_get_current_cert(again))) X509_up_ref(lacking);
        }
        X509_STORE_CTX_free(again);
    }
    if (lacking) X509_free(lacking);
    sk_X509_pop_free(extra, X509_free);
    if (ok) X509_STORE_CTX_set_error(ctx, X509_V_OK);
    return ok;
}

CURLcode onSslContext(CURL*, void* ssl_ctx, void*) {
    SSL_CTX_set_cert_verify_callback(static_cast<SSL_CTX*>(ssl_ctx), verifyFetchingIssuers, nullptr);
    return CURLE_OK;
}

void setWebOptions(CURL* c) {
    curl_easy_setopt(c, CURLOPT_SSL_CTX_FUNCTION, onSslContext);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");   // whatever curl can decompress
    curl_easy_setopt(c, CURLOPT_OPENSOCKETFUNCTION, openPublicSocket);
    curl_easy_setopt(c, CURLOPT_USERAGENT, kWebUserAgent);
    // A page's many pictures share one HTTP/2 connection, each waiting its turn: a stalled
    // transfer gives up, not one still coming (velog's 30 large thumbnails ran past a
    // 15 s total and failed with their status already 200).
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, kTimeoutSec);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 256L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, kTimeoutSec);
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

// Answer one request: the body (if ok) goes in a read-only shm fd.
void finish(const Job& job, shared::FetchReply r, const std::string& body,
            std::chrono::steady_clock::time_point started) {
    int fd = -1;
    if (r.status_ == static_cast<int32_t>(shared::FetchStatus::Ok)) {
        fd = bodyFd(body);
        if (fd < 0) r.status_ = static_cast<int32_t>(shared::FetchStatus::NetworkError);
        else r.size_ = body.size();
    }
    if (r.status_ != static_cast<int32_t>(shared::FetchStatus::Ok))
        LOG_INFO("{} → status {} (HTTP {})", job.url, r.status_, r.http_status_);
    reply(job.txn, r, fd);
    if (fd >= 0) ::close(fd);
    if (g_trace) {
        using ms = std::chrono::milliseconds;
        const auto end = std::chrono::steady_clock::now();
        LOG_INFO("trace {} waited {} ms, took {} ms{}", job.url,
                 std::chrono::duration_cast<ms>(started - job.queued).count(),
                 std::chrono::duration_cast<ms>(end - started).count(), r.from_cache_ ? " (cache)" : "");
    }
}

struct Transfer {
    Job         job;
    Sink        body;
    bool        cacheable = false;
    std::chrono::steady_clock::time_point started;
    curl_slist* headers = nullptr;   // a POST's content type, a script's headers
    std::string response_headers;    // a script's request: the last response's, as "name: value\r\n"
};

// A response header line, for a script's request. A redirect's headers are dropped
// when the next status line comes; Set-Cookie never reaches a script.
size_t onHeader(char* p, size_t sz, size_t n, void* ud) {
    auto* out = static_cast<std::string*>(ud);
    std::string line(p, sz * n);
    if (line.rfind("HTTP/", 0) == 0) {
        out->clear();
        return sz * n;
    }
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    const size_t colon = line.find(':');
    if (colon == std::string::npos || colon == 0) return sz * n;
    std::string name = line.substr(0, colon);
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (name == "set-cookie" || name == "set-cookie2") return sz * n;
    std::string value = line.substr(colon + 1);
    while (!value.empty() && value.front() == ' ') value.erase(value.begin());
    if (out->size() < 256 * 1024) *out += name + ": " + value + "\r\n";
    return sz * n;
}

// Cookies: curl's own engine, one jar shared by the transfers that may use it, kept in
// a file only the user can read. Session cookies are written too, so a sign-in lasts
// until the site ends it. Everything here runs on the network thread.
fs::path g_cookie_file;
CURLSH* g_cookies = nullptr;
CURL* g_cookie_jar = nullptr;   // a handle that never transfers: it loads and writes the file
bool g_cookies_dirty = false;
std::chrono::steady_clock::time_point g_cookies_written;

void openCookies() {
    if (g_cookie_file.empty()) return;
    g_cookies = curl_share_init();
    curl_share_setopt(g_cookies, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
    g_cookie_jar = curl_easy_init();
    if (!g_cookies || !g_cookie_jar) return;
    curl_easy_setopt(g_cookie_jar, CURLOPT_SHARE, g_cookies);
    curl_easy_setopt(g_cookie_jar, CURLOPT_COOKIEFILE, g_cookie_file.c_str());
    curl_easy_setopt(g_cookie_jar, CURLOPT_COOKIEJAR, g_cookie_file.c_str());
    curl_easy_setopt(g_cookie_jar, CURLOPT_COOKIELIST, "RELOAD");
}

void writeCookies(bool now = false) {
    const auto t = std::chrono::steady_clock::now();
    if (!g_cookie_jar || !g_cookies_dirty || (!now && t - g_cookies_written < std::chrono::seconds(3))) return;
    curl_easy_setopt(g_cookie_jar, CURLOPT_COOKIELIST, "FLUSH");
    std::error_code ec;
    fs::permissions(g_cookie_file, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
    g_cookies_dirty = false;
    g_cookies_written = t;
}

constexpr int kMaxRedirects = 8;

// What a redirect makes of a request, as browsers do it: a 303, and a 301 or 302 after
// a POST, become a GET without the body (or what said what the body was).
void redirected(Job& job, long code) {
    const bool to_get = code == 303 || ((code == 301 || code == 302) && (job.post || job.method == "POST"));
    if (!to_get) return;
    if (job.post) {
        job.post = false;
        job.content_type.clear();
        job.body.clear();
    }
    if (job.script && job.method != "HEAD" && job.method != "GET") {
        job.method = "GET";
        job.body.clear();
        std::erase_if(job.headers, [](const std::string& h) {
            std::string name = h.substr(0, h.find(':'));
            for (char& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return name == "content-type" || name == "content-length" || name == "content-encoding" || name == "content-language";
        });
    }
}

// The network thread: takes queued requests, answers policy refusals and cache
// hits at once, and runs everything else concurrently in one multi handle.
void networkLoop() {
    openCookies();
    CURLM* multi = curl_multi_init();
    curl_multi_setopt(multi, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
    curl_multi_setopt(multi, CURLMOPT_MAX_HOST_CONNECTIONS, 2L);   // be a polite tile client
    curl_multi_setopt(multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, 6L);
    g_multi = multi;
    while (true) {
        std::deque<Job> jobs;
        {
            std::lock_guard<std::mutex> lk(g_jobs_mu);
            jobs.swap(g_jobs);
        }
        for (Job& job : jobs) {
            const auto now = std::chrono::steady_clock::now();
            shared::FetchReply r{};
            bool cacheable = false;
            size_t limit = 0;
            std::string body;
            if (job.web) {
                limit = xserver::fetch_policy::kWebMaxBytes;   // the compositor checked who's asking
            }
            if (!job.web && !xserver::fetch_policy::knownHost(job.url, &cacheable, &limit)) {
                r.status_ = static_cast<int32_t>(shared::FetchStatus::Denied);
                finish(job, r, body, now);
            } else if (cacheable && readCache(job.url, body)) {
                r.status_ = static_cast<int32_t>(shared::FetchStatus::Ok);
                r.http_status_ = 200;
                r.from_cache_ = 1;
                finish(job, r, body, now);
            } else if (CURL* c = curl_easy_init()) {
                auto* t = new Transfer{std::move(job), {{}, limit}, cacheable, now};
                setOptions(c, t->job.url, &t->body);
                if (t->job.web) setWebOptions(c);
                if (!t->job.cookie_site.empty()) curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
                if (t->job.web && t->job.cookies && g_cookies) {
                    curl_easy_setopt(c, CURLOPT_SHARE, g_cookies);
                    curl_easy_setopt(c, CURLOPT_COOKIEFILE, "");   // the engine on, the jar shared
                }
                if (t->job.web && t->job.script) {
                    // The method, the page's headers, a body if there's one.
                    if (t->job.method == "HEAD") curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
                    else if (t->job.method != "GET") curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, t->job.method.c_str());
                    if (!t->job.body.empty() || t->job.method == "POST" || t->job.method == "PUT" || t->job.method == "PATCH") {
                        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(t->job.body.size()));
                        curl_easy_setopt(c, CURLOPT_COPYPOSTFIELDS, t->job.body.c_str());
                        if (t->job.method != "POST") curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, t->job.method.c_str());
                    }
                    for (const std::string& h : t->job.headers) t->headers = curl_slist_append(t->headers, h.c_str());
                    // curl's own defaults a page didn't ask for go (an empty value removes one).
                    t->headers = curl_slist_append(t->headers, "Expect:");
                    curl_easy_setopt(c, CURLOPT_HTTPHEADER, t->headers);
                    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, onHeader);
                    curl_easy_setopt(c, CURLOPT_HEADERDATA, &t->response_headers);
                } else if (t->job.web && t->job.post) {
                    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(t->job.body.size()));
                    curl_easy_setopt(c, CURLOPT_COPYPOSTFIELDS, t->job.body.c_str());
                    t->headers = curl_slist_append(nullptr, ("Content-Type: " + t->job.content_type).c_str());
                    curl_easy_setopt(c, CURLOPT_HTTPHEADER, t->headers);
                }
                curl_easy_setopt(c, CURLOPT_PRIVATE, t);
                curl_multi_add_handle(multi, c);
            } else {
                r.status_ = static_cast<int32_t>(shared::FetchStatus::NetworkError);
                finish(job, r, body, now);
            }
        }

        int running = 0;
        curl_multi_perform(multi, &running);
        int left = 0;
        while (CURLMsg* msg = curl_multi_info_read(multi, &left)) {
            if (msg->msg != CURLMSG_DONE) continue;
            CURL* c = msg->easy_handle;
            Transfer* t = nullptr;
            curl_easy_getinfo(c, CURLINFO_PRIVATE, &t);
            long code = 0;
            curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
            const CURLcode rc = msg->data.result;
            shared::FetchReply r{};
            r.http_status_ = static_cast<uint32_t>(code);
            if (rc == CURLE_OK && !t->job.cookie_site.empty() && (code == 301 || code == 302 || code == 303 || code == 307 || code == 308)) {
                char* location = nullptr;
                curl_easy_getinfo(c, CURLINFO_REDIRECT_URL, &location);
                if (location && xserver::fetch_policy::webUrlAllowed(location)) {
                    if (t->job.cookies) g_cookies_dirty = true;   // a step's Set-Cookie
                    if (t->job.hops >= kMaxRedirects) {
                        r.status_ = static_cast<int32_t>(shared::FetchStatus::NetworkError);
                        finish(t->job, r, {}, t->started);
                    } else {
                        Job next = std::move(t->job);
                        next.url = location;
                        ++next.hops;
                        next.cookies = xserver::fetch_policy::webSite(next.url) == next.cookie_site;
                        redirected(next, code);
                        std::lock_guard<std::mutex> lk(g_jobs_mu);
                        g_jobs.push_front(std::move(next));
                    }
                    curl_multi_remove_handle(multi, c);
                    curl_easy_cleanup(c);
                    if (t->headers) curl_slist_free_all(t->headers);
                    delete t;
                    continue;
                }
            }
            if (rc == CURLE_WRITE_ERROR || rc == CURLE_FILESIZE_EXCEEDED)
                r.status_ = static_cast<int32_t>(shared::FetchStatus::TooLarge);
            else if (rc != CURLE_OK)
                r.status_ = static_cast<int32_t>(shared::FetchStatus::NetworkError);
            else if (t->job.web)
                r.status_ = static_cast<int32_t>(shared::FetchStatus::Ok);   // a 404 page is still a page
            else if (code < 200 || code >= 300)
                r.status_ = static_cast<int32_t>(shared::FetchStatus::HttpError);
            else
                r.status_ = static_cast<int32_t>(shared::FetchStatus::Ok);
            if (t->job.web && r.status_ == static_cast<int32_t>(shared::FetchStatus::Ok)) {
                // Where it ended up, and what it says it is: relative links need the one,
                // and deciding how to show it the other.
                char* final_url = nullptr;
                char* content_type = nullptr;
                curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &final_url);
                curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &content_type);
                std::string head = std::string(final_url ? final_url : t->job.url.c_str()) + "\n" + (content_type ? content_type : "") + "\n";
                if (t->job.script) head += std::to_string(t->response_headers.size()) + "\n" + t->response_headers;
                t->body.data.insert(0, head);
            }
            if (r.status_ == static_cast<int32_t>(shared::FetchStatus::Ok) && t->cacheable &&
                t->body.data.size() <= kMaxCached)
                writeCache(t->job.url, t->body.data);
            finish(t->job, r, t->body.data, t->started);
            curl_multi_remove_handle(multi, c);
            if (t->job.cookies) g_cookies_dirty = true;
            curl_easy_cleanup(c);
            if (t->headers) curl_slist_free_all(t->headers);
            delete t;
        }
        // Sleep until a socket is ready, a timer is due, or main() queues a
        // request (curl_multi_wakeup).
        writeCookies();
        curl_multi_poll(multi, nullptr, 0, 1000, nullptr);
    }
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
        const fs::path data = fs::path(home) / ".local" / "share" / "freebsdx";
        fs::create_directories(data, ec);
        if (!ec) g_cookie_file = data / "cookies.txt";
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
    std::thread(networkLoop).detach();
    LOG_INFO("serving com.freebsdx.Fetch (cache: {})", g_cache_dir.empty() ? "none" : g_cache_dir.string());

    // Every connection also gets the compositor's broadcasts; only relayed Gets matter.
    // The compositor caps relayed requests (kMaxPending), which bounds this queue.
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
        std::string url(req.url_);
        Job job{m.transaction_id_, {}, (req.flags_ & shared::kFetchWeb) != 0, std::chrono::steady_clock::now()};
        job.cookies = job.web && (req.flags_ & shared::kFetchCookies);
        if ((req.flags_ & shared::kFetchScript) && job.web) {
            // The compositor's own checked copy: "<method>\n<url>\n<headers>\n\n<body>".
            struct stat st{};
            const int fd = res->second.get();
            if (fd < 0 || ::fstat(fd, &st) != 0 || st.st_size <= 0 || static_cast<size_t>(st.st_size) > xserver::fetch_policy::kWebMaxScriptRequest)
                continue;
            std::string request(static_cast<size_t>(st.st_size), '\0');
            if (::pread(fd, request.data(), request.size(), 0) != st.st_size) continue;
            const size_t m_end = request.find('\n');
            const size_t u_end = m_end == std::string::npos ? std::string::npos : request.find('\n', m_end + 1);
            if (u_end == std::string::npos) continue;
            job.script = true;
            job.method = request.substr(0, m_end);
            url = request.substr(m_end + 1, u_end - m_end - 1);
            size_t at = u_end + 1;
            while (at < request.size()) {
                const size_t eol = request.find('\n', at);
                if (eol == std::string::npos) break;
                if (eol == at) { at = eol + 1; break; }
                job.headers.push_back(request.substr(at, eol - at));
                at = eol + 1;
            }
            job.body = request.substr(std::min(at, request.size()));
        } else if ((req.flags_ & shared::kFetchPost) && job.web) {
            // The compositor's own copy, already checked: "<url>\n<content type>\n<body>".
            struct stat st{};
            const int fd = res->second.get();
            if (fd < 0 || ::fstat(fd, &st) != 0 || st.st_size <= 0 || static_cast<size_t>(st.st_size) >= xserver::fetch_policy::kWebMaxPost)
                continue;
            std::string request(static_cast<size_t>(st.st_size), '\0');
            if (::pread(fd, request.data(), request.size(), 0) != st.st_size) continue;
            const size_t first = request.find('\n');
            const size_t second = first == std::string::npos ? std::string::npos : request.find('\n', first + 1);
            if (second == std::string::npos) continue;
            url = request.substr(0, first);
            job.post = true;
            job.content_type = request.substr(first + 1, second - first - 1);
            job.body = request.substr(second + 1);
        } else if (req.flags_ & shared::kFetchUrlInFd) {
            // The compositor's own copy of a long address, already checked.
            struct stat st{};
            const int url_fd = res->second.get();
            if (url_fd < 0 || ::fstat(url_fd, &st) != 0 || st.st_size <= 0 ||
                static_cast<size_t>(st.st_size) >= xserver::fetch_policy::kWebMaxUrl)
                continue;
            url.resize(static_cast<size_t>(st.st_size));
            if (::pread(url_fd, url.data(), url.size(), 0) != st.st_size) continue;
        }
        {
            std::lock_guard<std::mutex> lk(g_jobs_mu);
            job.url = std::move(url);
            if (job.cookies) job.cookie_site = xserver::fetch_policy::webSite(job.url);
            g_jobs.push_back(std::move(job));
        }
        if (CURLM* multi = g_multi.load()) curl_multi_wakeup(multi);
    }
    LOG_INFO("compositor closed the connection; exiting");
    std::_Exit(0);   // detached transfers may be mid-request: skip atexit teardown
}

#include "xserver/keychain_service.h"
#include "shared/logger.h"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace fbx::xserver {

void BroadcastXBusMessage(const shared::XBusMessage&);

// ─── Static member definitions ────────────────────────────────────────────────
std::mutex        KeychainService::mutex_;
std::vector<KeychainService::Item> KeychainService::items_;
bool              KeychainService::unlocked_{false};
uint8_t           KeychainService::session_key_[32]{};
std::string       KeychainService::keychain_path_;

// Per-keychain salt (generated once, persisted in the file header).
static uint8_t g_salt[16]{};
static bool    g_salt_ready = false;

// ─── Hex helpers ──────────────────────────────────────────────────────────────
static std::string toHex(const uint8_t* data, size_t len) {
    std::ostringstream ss;
    for (size_t i = 0; i < len; ++i)
        ss << std::hex << std::setw(2) << std::setfill('0')
           << static_cast<int>(data[i]);
    return ss.str();
}

static std::vector<uint8_t> fromHex(const std::string& s) {
    std::vector<uint8_t> out;
    out.reserve(s.size() / 2);
    for (size_t i = 0; i + 1 < s.size(); i += 2)
        out.push_back(static_cast<uint8_t>(std::stoul(s.substr(i, 2), nullptr, 16)));
    return out;
}

// ─── Init ─────────────────────────────────────────────────────────────────────
void KeychainService::init() noexcept {
    const char* home = std::getenv("HOME");
    keychain_path_ = home ? std::string(home) + "/.freebsdx-keychain"
                          : "/tmp/.freebsdx-keychain";
    LOG_INFO("KeychainService: ready (com.freebsdx.Keychain) path={}", keychain_path_);
}

// ─── Disk I/O ─────────────────────────────────────────────────────────────────
// File format:
//   # FreeBSD-X Keychain v1
//   SALT:<32 hex bytes — 16-byte random salt for PBKDF2>
//   <service>\t<account>\t<32-hex IV>\t<AES-256-CBC ciphertext hex>
//   ...
//
// Call with mutex_ held.
bool KeychainService::loadFromDisk() noexcept {
    std::ifstream f(keychain_path_);
    if (!f.is_open()) {
        // First use: generate a new salt.
        if (!g_salt_ready) {
            RAND_bytes(g_salt, sizeof(g_salt));
            g_salt_ready = true;
        }
        items_.clear();
        return true;
    }

    std::string line;
    std::getline(f, line); // skip "# FreeBSD-X Keychain v1"

    if (std::getline(f, line) && line.rfind("SALT:", 0) == 0) {
        auto salt_bytes = fromHex(line.substr(5));
        if (salt_bytes.size() == sizeof(g_salt)) {
            std::memcpy(g_salt, salt_bytes.data(), sizeof(g_salt));
            g_salt_ready = true;
        }
    }

    items_.clear();
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto t1 = line.find('\t');
        if (t1 == std::string::npos) continue;
        auto t2 = line.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        auto t3 = line.find('\t', t2 + 1);
        if (t3 == std::string::npos) continue;

        Item item;
        item.service    = line.substr(0, t1);
        item.account    = line.substr(t1 + 1, t2 - t1 - 1);
        item.iv_hex     = line.substr(t2 + 1, t3 - t2 - 1);
        item.cipher_hex = line.substr(t3 + 1);
        items_.push_back(std::move(item));
    }
    return true;
}

// Call with mutex_ held.
bool KeychainService::saveToDisk() noexcept {
    std::ofstream f(keychain_path_, std::ios::trunc);
    if (!f.is_open()) return false;
    f << "# FreeBSD-X Keychain v1\n";
    f << "SALT:" << toHex(g_salt, sizeof(g_salt)) << "\n";
    for (const auto& item : items_)
        f << item.service << '\t' << item.account << '\t'
          << item.iv_hex  << '\t' << item.cipher_hex << '\n';
    return true;
}

// ─── Crypto ───────────────────────────────────────────────────────────────────
// Call with mutex_ held and session_key_ set.
std::string KeychainService::encrypt(const std::string& plaintext) noexcept {
    uint8_t iv[16];
    RAND_bytes(iv, sizeof(iv));

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return {};

    EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr, session_key_, iv);

    std::vector<uint8_t> buf(plaintext.size() + EVP_MAX_BLOCK_LENGTH);
    int len1 = 0, len2 = 0;
    EVP_EncryptUpdate(ctx, buf.data(), &len1,
                      reinterpret_cast<const uint8_t*>(plaintext.data()),
                      static_cast<int>(plaintext.size()));
    EVP_EncryptFinal_ex(ctx, buf.data() + len1, &len2);
    EVP_CIPHER_CTX_free(ctx);

    return toHex(iv, sizeof(iv)) + ':' + toHex(buf.data(), len1 + len2);
}

// Call with mutex_ held and session_key_ set.
std::string KeychainService::decrypt(const std::string& iv_hex,
                                      const std::string& cipher_hex) noexcept {
    auto iv_bytes     = fromHex(iv_hex);
    auto cipher_bytes = fromHex(cipher_hex);
    if (iv_bytes.size() != 16 || cipher_bytes.empty()) return {};

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return {};

    EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr,
                       session_key_, iv_bytes.data());

    std::vector<uint8_t> buf(cipher_bytes.size() + EVP_MAX_BLOCK_LENGTH);
    int len1 = 0, len2 = 0;
    EVP_DecryptUpdate(ctx, buf.data(), &len1,
                      cipher_bytes.data(),
                      static_cast<int>(cipher_bytes.size()));
    if (!EVP_DecryptFinal_ex(ctx, buf.data() + len1, &len2)) {
        EVP_CIPHER_CTX_free(ctx);
        return {}; // wrong key or corrupted data
    }
    EVP_CIPHER_CTX_free(ctx);

    return std::string(reinterpret_cast<char*>(buf.data()), len1 + len2);
}

// ─── IPC reply helpers ────────────────────────────────────────────────────────
void KeychainService::sendStatus(IpcSocket* client, uint32_t tid,
                                  bool success,
                                  uint32_t item_count) noexcept {
    shared::KeychainStatusResponse resp{};
    resp.success_    = success ? 1u : 0u;
    resp.item_count_ = item_count;

    shared::XBusMessage reply{};
    reply.transaction_id_ = tid;
    std::strncpy(reply.service_name_, "com.freebsdx.Keychain",
                 sizeof(reply.service_name_) - 1);
    std::memcpy(reply.payload_, &resp, sizeof(resp));
    client->sendMessage({reinterpret_cast<const uint8_t*>(&reply), sizeof(reply)});
}

void KeychainService::sendItem(IpcSocket* client, uint32_t tid, bool found,
                                const std::string& service,
                                const std::string& account,
                                const std::string& secret) noexcept {
    shared::KeychainItemResponse resp{};
    resp.found_ = found ? 1u : 0u;
    std::strncpy(resp.service_, service.c_str(), sizeof(resp.service_) - 1);
    std::strncpy(resp.account_, account.c_str(), sizeof(resp.account_) - 1);
    std::strncpy(resp.secret_,  secret.c_str(),  sizeof(resp.secret_)  - 1);

    shared::XBusMessage reply{};
    reply.transaction_id_ = tid;
    std::strncpy(reply.service_name_, "com.freebsdx.Keychain",
                 sizeof(reply.service_name_) - 1);
    std::memcpy(reply.payload_, &resp, sizeof(resp));
    client->sendMessage({reinterpret_cast<const uint8_t*>(&reply), sizeof(reply)});
}

void KeychainService::broadcastChanged() noexcept {
    shared::XBusMessage bcast{};
    std::strncpy(bcast.service_name_, "com.freebsdx.Keychain",
                 sizeof(bcast.service_name_) - 1);
    bcast.opcode_ = static_cast<uint32_t>(shared::KeychainOpcode::Changed);
    BroadcastXBusMessage(bcast);
}

// ─── Message handler ──────────────────────────────────────────────────────────
void KeychainService::handleMessage(const shared::XBusMessage& msg,
                                     std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::KeychainOpcode;
    const auto op = static_cast<O>(msg.opcode_);

    switch (op) {

    case O::Unlock: {
        shared::KeychainUnlockRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.master_password_[sizeof(req.master_password_) - 1] = '\0';

        std::lock_guard<std::mutex> lk(mutex_);

        // Load file to obtain (or generate) the salt before deriving the key.
        if (!g_salt_ready) loadFromDisk();

        // Derive 256-bit session key via PKCS5-PBKDF2-HMAC-SHA256.
        if (!PKCS5_PBKDF2_HMAC(req.master_password_,
                                static_cast<int>(std::strlen(req.master_password_)),
                                g_salt, sizeof(g_salt),
                                100000, EVP_sha256(),
                                32, session_key_)) {
            sendStatus(client.get(), msg.transaction_id_, false);
            break;
        }

        loadFromDisk(); // (re)load items now that we have the session key
        unlocked_ = true;
        LOG_INFO("KeychainService: unlocked ({} items)", items_.size());
        sendStatus(client.get(), msg.transaction_id_, true, static_cast<uint32_t>(items_.size()));
        break;
    }

    case O::Lock: {
        std::lock_guard<std::mutex> lk(mutex_);
        unlocked_ = false;
        std::memset(session_key_, 0, sizeof(session_key_));
        LOG_INFO("KeychainService: locked");
        sendStatus(client.get(), msg.transaction_id_, true);
        break;
    }

    case O::Store: {
        shared::KeychainStoreRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.service_[sizeof(req.service_) - 1] = '\0';
        req.account_[sizeof(req.account_) - 1] = '\0';
        req.secret_[sizeof(req.secret_)  - 1]  = '\0';

        std::string svc = req.service_, acc = req.account_, sec = req.secret_;

        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (!unlocked_) { sendStatus(client.get(), msg.transaction_id_, false); break; }

            std::string enc = encrypt(sec);
            auto colon = enc.find(':');
            if (colon == std::string::npos) { sendStatus(client.get(), msg.transaction_id_, false); break; }

            std::string iv_hex     = enc.substr(0, colon);
            std::string cipher_hex = enc.substr(colon + 1);

            bool found = false;
            for (auto& item : items_) {
                if (item.service == svc && item.account == acc) {
                    item.iv_hex     = std::move(iv_hex);
                    item.cipher_hex = std::move(cipher_hex);
                    found = true;
                    break;
                }
            }
            if (!found)
                items_.push_back({svc, acc, std::move(iv_hex), std::move(cipher_hex)});

            saveToDisk();
        }

        broadcastChanged();
        sendItem(client.get(), msg.transaction_id_, true, svc, acc, sec);
        LOG_INFO("KeychainService: stored {}:{}", svc, acc);
        break;
    }

    case O::Retrieve: {
        shared::KeychainLookupRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.service_[sizeof(req.service_) - 1] = '\0';
        req.account_[sizeof(req.account_) - 1] = '\0';

        std::string svc = req.service_, acc = req.account_;
        std::string secret;
        bool found = false;

        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (!unlocked_) { sendItem(client.get(), msg.transaction_id_, false, svc, acc, {}); break; }

            for (const auto& item : items_) {
                if (item.service == svc && item.account == acc) {
                    secret = decrypt(item.iv_hex, item.cipher_hex);
                    found = true;
                    break;
                }
            }
        }

        sendItem(client.get(), msg.transaction_id_, found, svc, acc, secret);
        LOG_INFO("KeychainService: retrieve {}:{} found={}", svc, acc, found);
        break;
    }

    case O::Delete: {
        shared::KeychainLookupRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.service_[sizeof(req.service_) - 1] = '\0';
        req.account_[sizeof(req.account_) - 1] = '\0';

        std::string svc = req.service_, acc = req.account_;
        bool removed = false;
        uint32_t remaining = 0;

        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (!unlocked_) { sendStatus(client.get(), msg.transaction_id_, false); break; }

            auto it = std::find_if(items_.begin(), items_.end(),
                [&](const Item& i){ return i.service == svc && i.account == acc; });
            if (it != items_.end()) {
                items_.erase(it);
                saveToDisk();
                removed = true;
            }
            remaining = static_cast<uint32_t>(items_.size());
        }

        if (removed) broadcastChanged();
        sendStatus(client.get(), msg.transaction_id_, removed, remaining);
        LOG_INFO("KeychainService: delete {}:{} removed={}", svc, acc, removed);
        break;
    }

    case O::List: {
        shared::KeychainPageRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));

        shared::KeychainListPage pg{};
        {
            std::lock_guard<std::mutex> lk(mutex_);
            pg.total_ = static_cast<uint32_t>(items_.size());
            pg.page_  = req.page_;
            if (req.page_ < static_cast<uint32_t>(items_.size())) {
                pg.valid_    = 1u;
                pg.has_more_ = (req.page_ + 1 < pg.total_) ? 1u : 0u;
                const auto& item = items_[req.page_];
                std::strncpy(pg.entry_.service_, item.service.c_str(),
                             sizeof(pg.entry_.service_) - 1);
                std::strncpy(pg.entry_.account_, item.account.c_str(),
                             sizeof(pg.entry_.account_) - 1);
            }
        }

        shared::XBusMessage reply{};
        reply.transaction_id_ = msg.transaction_id_;
        std::strncpy(reply.service_name_, "com.freebsdx.Keychain",
                     sizeof(reply.service_name_) - 1);
        std::memcpy(reply.payload_, &pg, sizeof(pg));
        client->sendMessage({reinterpret_cast<const uint8_t*>(&reply), sizeof(reply)});
        break;
    }

    default: break;
    }
}

} // namespace fbx::xserver

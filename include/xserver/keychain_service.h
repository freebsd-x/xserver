#pragma once
#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace fbx::xserver {

// com.freebsdx.Keychain
// AES-256-CBC encrypted credential store backed by ~/.freebsdx-keychain.
// PBKDF2-HMAC-SHA256 derives the session key from the master password;
// the key is held only in compositor memory and cleared on Lock.
//
// All public methods are thread-safe.
class KeychainService {
public:
    static void init() noexcept;

    // Central dispatcher entry point — routes all KeychainOpcode messages.
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;

private:
    // On-disk representation of one credential.
    struct Item {
        std::string service;
        std::string account;
        std::string iv_hex;      // 32 hex chars (16-byte AES IV)
        std::string cipher_hex;  // AES-256-CBC ciphertext, hex-encoded
    };

    // Disk I/O (call with mutex_ held).
    static bool loadFromDisk() noexcept;
    static bool saveToDisk() noexcept;

    // Crypto helpers (call with mutex_ held; session_key_ must be set).
    // encrypt() returns "iv_hex:cipher_hex"; decrypt() returns plaintext.
    static std::string encrypt(const std::string& plaintext) noexcept;
    static std::string decrypt(const std::string& iv_hex,
                                const std::string& cipher_hex) noexcept;

    // IPC reply helpers — do NOT hold mutex_ when calling sendItem/sendStatus
    // (they call IpcSocket::sendMessage which may take its own lock).
    static void sendStatus(IpcSocket* client, uint32_t tid,
                           bool success,
                           uint32_t item_count = 0) noexcept;
    static void sendItem(IpcSocket* client, uint32_t tid,
                         bool found,
                         const std::string& service,
                         const std::string& account,
                         const std::string& secret) noexcept;
    static void broadcastChanged() noexcept;

    static std::mutex        mutex_;
    static std::vector<Item> items_;
    static bool              unlocked_;
    static uint8_t           session_key_[32]; // zeroed on Lock
    static std::string       keychain_path_;   // ~/.freebsdx-keychain
};

} // namespace fbx::xserver

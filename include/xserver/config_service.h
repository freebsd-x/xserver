#pragma once

#include "shared/protocol.h"
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace fbx::xserver {

class IpcSocket;

class ConfigService {
public:
    static bool ProcessMessage(std::shared_ptr<IpcSocket> client, const shared::XBusMessage& msg) noexcept;
    
    // Internal access for other brokers
    static std::string get(const std::string& key, const std::string& default_val = "") noexcept;
    static void set(const std::string& key, const std::string& value) noexcept;

private:
    static void load() noexcept;
    static void save() noexcept;

    // Guards settings_/loaded_ and the file: services read and write
    // settings from worker threads too (backups, slow shell-only services).
    // Recursive because set() → load()/save() under the same lock.
    static inline std::recursive_mutex mutex_;
    static inline std::unordered_map<std::string, std::string> settings_;
    static inline bool loaded_ = false;
};

} // namespace fbx::xserver

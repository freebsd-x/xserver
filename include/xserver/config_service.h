#pragma once

#include "shared/protocol.h"
#include <memory>
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

    static inline std::unordered_map<std::string, std::string> settings_;
    static inline bool loaded_ = false;
};

} // namespace fbx::xserver

#include "xserver/config_service.h"
#include "xserver/ipc_socket.h"
#include "shared/logger.h"
#include <fstream>
#include <filesystem>
#include <iostream>
#include <cstring>
#include <cstdlib>

namespace fbx::xserver {

// Returns and caches the config file path under $HOME/.config/freebsd-x/config.
// Falls back to /tmp/freebsd-x-config if HOME is unset.
static const std::string& configPath() noexcept {
    static const std::string path = [] {
        const char* home = getenv("HOME");
        std::string dir = home ? (std::string(home) + "/.config/freebsd-x") : "/tmp";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec); // no-op if already exists
        return dir + "/config";
    }();
    return path;
}

void ConfigService::load() noexcept {
    std::lock_guard lk(mutex_);
    if (loaded_) return;

    const auto& path = configPath();
    if (std::filesystem::exists(path)) {
        std::ifstream file(path);
        std::string line;
        while (std::getline(file, line)) {
            // Split on the first '=' only — values may contain '='
            size_t sep = line.find('=');
            if (sep != std::string::npos) {
                settings_[line.substr(0, sep)] = line.substr(sep + 1);
            }
        }
    }
    loaded_ = true;
    LOG_INFO("Configuration loaded from {}", path);
}

void ConfigService::save() noexcept {
    std::lock_guard lk(mutex_);
    const auto& path = configPath();
    // Write a temp file and rename it over the config, so a crash mid-save
    // can't leave a truncated settings file.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream file(tmp, std::ios::trunc);
        for (const auto& [key, val] : settings_) file << key << "=" << val << "\n";
        if (!file) { LOG_WARN("Configuration: can't write {}", tmp); return; }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) LOG_WARN("Configuration: can't replace {}: {}", path, ec.message());
}

std::string ConfigService::get(const std::string& key, const std::string& default_val) noexcept {
    std::lock_guard lk(mutex_);
    load();
    auto it = settings_.find(key);
    return it != settings_.end() ? it->second : default_val;
}

void ConfigService::set(const std::string& key, const std::string& value) noexcept {
    std::lock_guard lk(mutex_);
    load();
    settings_[key] = value;
    save();
}

bool ConfigService::ProcessMessage(std::shared_ptr<IpcSocket> client, const shared::XBusMessage& msg) noexcept {
    if (std::strcmp(msg.service_name_, "com.freebsdx.Configuration") != 0) return false;

    load();
    shared::XBusMessage response = msg;

    if (msg.opcode_ == 0x01) { // GetValue
        const char* key = reinterpret_cast<const char*>(msg.payload_);
        std::string val = get(key);
        std::strncpy(reinterpret_cast<char*>(response.payload_), val.c_str(), sizeof(response.payload_) - 1);
        response.payload_[sizeof(response.payload_) - 1] = '\0';
    } else if (msg.opcode_ == 0x02) { // SetValue
        const auto* kv = reinterpret_cast<const shared::ConfigValue*>(msg.payload_);
        set(kv->key_, kv->value_);
        LOG_INFO("Config set: {} = {}", kv->key_, kv->value_);
    } else {
        return false;
    }

    std::span<const uint8_t> resp_span(reinterpret_cast<const uint8_t*>(&response), sizeof(response));
    (void)client->sendMessage(resp_span);
    return true;
}

} // namespace fbx::xserver

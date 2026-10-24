#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <shared/protocol.h>
#include "xserver/ipc_socket.h"

namespace fbx {

using xserver::IpcSocket;

class WeatherService {
public:
    static void init() noexcept;
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;

private:
    static bool          fetchAndStore() noexcept;
    static void          backgroundThread() noexcept;
    static std::string   sanitizeCity(const std::string& city) noexcept;
    static const char*   conditionIcon(const std::string& desc) noexcept;

    static std::mutex                         mutex_;
    static shared::WeatherCondition           current_;
    static std::vector<shared::WeatherForecastDay> forecast_; // 3 entries
    static bool                               has_data_;
    static std::string                        city_;
    static std::string                        config_path_;
};

} // namespace fbx

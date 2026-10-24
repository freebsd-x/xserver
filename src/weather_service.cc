// weather_service.cc — com.freebsdx.Weather
//
// Fetches weather from wttr.in (free JSON API, no key) using the system
// `fetch` utility every 30 minutes.  Parses temp/condition/wind/humidity
// and the 3-day forecast from the JSON response.

#include "xserver/weather_service.h"
#include "xserver/async_shell.h"

namespace fbx::xserver { void BroadcastXBusMessage(const shared::XBusMessage& msg); }
using fbx::xserver::BroadcastXBusMessage;

#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <unistd.h>

namespace fbx {

using namespace shared;

std::mutex                          WeatherService::mutex_;
WeatherCondition                    WeatherService::current_{};
std::vector<WeatherForecastDay>     WeatherService::forecast_;
bool                                WeatherService::has_data_ = false;
std::string                         WeatherService::city_     = "auto";
std::string                         WeatherService::config_path_;

// ── Helpers ───────────────────────────────────────────────────────────────────

std::string WeatherService::sanitizeCity(const std::string& city) noexcept {
    std::string out;
    for (char c : city)
        if (std::isalnum(static_cast<unsigned char>(c)) ||
            c == ' ' || c == ',' || c == '-')
            out += c;
    return out.empty() ? "auto" : out;
}

const char* WeatherService::conditionIcon(const std::string& desc) noexcept {
    if (desc.find("Sunny")    != std::string::npos ||
        desc.find("Clear")    != std::string::npos)  return "☀";
    if (desc.find("Partly")   != std::string::npos)  return "⛅";
    if (desc.find("Overcast") != std::string::npos ||
        desc.find("Cloudy")   != std::string::npos)  return "☁";
    if (desc.find("Drizzle")  != std::string::npos)  return "🌦";
    if (desc.find("Rain")     != std::string::npos)  return "🌧";
    if (desc.find("Thunder")  != std::string::npos ||
        desc.find("Storm")    != std::string::npos)  return "⛈";
    if (desc.find("Blizzard") != std::string::npos ||
        desc.find("Snow")     != std::string::npos)  return "❄";
    if (desc.find("Sleet")    != std::string::npos ||
        desc.find("Ice")      != std::string::npos)  return "🌨";
    if (desc.find("Fog")      != std::string::npos ||
        desc.find("Mist")     != std::string::npos)  return "🌫";
    return "🌡";
}

// ── JSON field extraction ─────────────────────────────────────────────────────
// Minimal key:"value" scanner — sufficient for wttr.in's flat fields.

static std::string jsonStr(const std::string& json, const std::string& key) noexcept {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};
    pos = json.find('"', pos);
    if (pos == std::string::npos) return {};
    size_t start = pos + 1;
    size_t end   = json.find('"', start);
    if (end == std::string::npos) return {};
    return json.substr(start, end - start);
}

static int jsonInt(const std::string& json, const std::string& key) noexcept {
    auto v = jsonStr(json, key);
    if (v.empty()) return 0;
    try { return std::stoi(v); } catch (...) { return 0; }
}

// Find the Nth occurrence of "key" in the json and return the string value.
static std::string jsonStrN(const std::string& json,
                            const std::string& key, int n) noexcept {
    std::string needle = "\"" + key + "\"";
    size_t pos = 0;
    for (int i = 0; i <= n; ++i) {
        pos = json.find(needle, pos);
        if (pos == std::string::npos) return {};
        if (i < n) pos += needle.size();
    }
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};
    pos = json.find('"', pos);
    if (pos == std::string::npos) return {};
    size_t start = pos + 1;
    size_t end   = json.find('"', start);
    if (end == std::string::npos) return {};
    return json.substr(start, end - start);
}

// ── Fetch and parse ───────────────────────────────────────────────────────────

bool WeatherService::fetchAndStore() noexcept {
    std::string city;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        city = city_;
    }

    // URL-encode: space → +
    std::string encoded;
    for (char c : city) encoded += (c == ' ') ? '+' : c;

    // Use fetch (FreeBSD base); fall back to curl. posix_spawn, not popen: popen
    // forks and deadlocks the child on a libc stdio lock in this multithreaded
    // compositor (see async_shell.h).
    std::string url  = "https://wttr.in/" + encoded + "?format=j1";
    std::string json = xserver::spawnCapture("fetch -qo - '" + url + "' 2>/dev/null");
    if (json.empty())
        json = xserver::spawnCapture("curl -sf '" + url + "' 2>/dev/null");

    if (json.empty() || json.find("current_condition") == std::string::npos) {
        std::cerr << "[WeatherService] empty or invalid response\n";
        return false;
    }

    // ── Current conditions ────────────────────────────────────────────────────
    WeatherCondition cond{};

    // City name: wttr.in sometimes returns it in "nearest_area"
    std::string area = jsonStr(json, "areaName");
    if (area.empty()) area = city == "auto" ? "Unknown" : city;
    std::strncpy(cond.city_, area.c_str(), sizeof(cond.city_) - 1);

    std::string desc = jsonStr(json, "value"); // weatherDesc[0].value
    std::strncpy(cond.condition_, desc.c_str(),           sizeof(cond.condition_) - 1);
    std::strncpy(cond.icon_,      conditionIcon(desc),    sizeof(cond.icon_)      - 1);
    std::strncpy(cond.wind_dir_,  jsonStr(json, "winddir16Point").c_str(),
                 sizeof(cond.wind_dir_) - 1);

    cond.temp_c_   = jsonInt(json, "temp_C");
    cond.temp_f_   = jsonInt(json, "temp_F");
    cond.feels_c_  = jsonInt(json, "FeelsLikeC");
    cond.humidity_ = static_cast<uint32_t>(jsonInt(json, "humidity"));
    cond.wind_kph_ = static_cast<uint32_t>(jsonInt(json, "windspeedKmph"));
    cond.uv_index_ = static_cast<uint32_t>(jsonInt(json, "uvIndex"));

    // ── 3-day forecast ────────────────────────────────────────────────────────
    // "weather":[{"date":"YYYY-MM-DD","maxtempC":"28","mintempC":"18",...}, ...]
    std::vector<WeatherForecastDay> forecast;
    for (int day = 0; day < 3; ++day) {
        WeatherForecastDay fd{};
        std::string date   = jsonStrN(json, "date",      day);
        std::string maxC   = jsonStrN(json, "maxtempC",  day);
        std::string minC   = jsonStrN(json, "mintempC",  day);
        std::string maxF   = jsonStrN(json, "maxtempF",  day);
        std::string minF   = jsonStrN(json, "mintempF",  day);
        // description is inside hourly[4] or the "description" field
        std::string fdesc  = jsonStrN(json, "value",     day + 1); // skip current_condition's "value"
        if (date.empty()) break;

        std::strncpy(fd.date_,      date.c_str(),         sizeof(fd.date_)      - 1);
        std::strncpy(fd.condition_, fdesc.c_str(),        sizeof(fd.condition_) - 1);
        std::strncpy(fd.icon_,      conditionIcon(fdesc), sizeof(fd.icon_)      - 1);
        try { fd.max_c_ = maxC.empty() ? 0 : std::stoi(maxC); } catch(...) {}
        try { fd.min_c_ = minC.empty() ? 0 : std::stoi(minC); } catch(...) {}
        try { fd.max_f_ = maxF.empty() ? 0 : std::stoi(maxF); } catch(...) {}
        try { fd.min_f_ = minF.empty() ? 0 : std::stoi(minF); } catch(...) {}
        forecast.push_back(fd);
    }

    {
        std::lock_guard<std::mutex> lk(mutex_);
        current_  = cond;
        forecast_ = forecast;
        has_data_ = true;
    }

    // Broadcast to all connected clients
    XBusMessage out{};
    std::strncpy(out.service_name_, "com.freebsdx.Weather",
                 sizeof(out.service_name_) - 1);
    out.opcode_ = static_cast<uint32_t>(WeatherOpcode::WeatherChanged);
    std::memcpy(out.payload_, &cond, sizeof(cond));
    BroadcastXBusMessage(out);

    std::cout << "[WeatherService] " << area << ": "
              << cond.temp_c_ << "°C " << desc << "\n";
    return true;
}

void WeatherService::backgroundThread() noexcept {
    // First fetch after a short delay (let the server finish init)
    sleep(5);
    try { fetchAndStore(); } catch (...) {
        std::cerr << "[WeatherService] initial fetch threw\n";
    }

    while (true) {
        // Re-fetch every 30 minutes
        sleep(30 * 60);
        try { fetchAndStore(); } catch (...) {
            std::cerr << "[WeatherService] periodic fetch threw, will retry\n";
        }
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

void WeatherService::init() noexcept {
    const char* home = getenv("HOME");
    config_path_ = home ? std::string(home) + "/.freebsdx-weather" : "/tmp/.freebsdx-weather";

    // Load saved city
    std::ifstream ifs(config_path_);
    if (ifs) {
        std::getline(ifs, city_);
        city_ = sanitizeCity(city_);
    }
    if (city_.empty()) city_ = "auto";

    std::cout << "[WeatherService] init (city=" << city_ << ")\n";

    // Start background fetch thread
    std::thread([]() { WeatherService::backgroundThread(); }).detach();
}

void WeatherService::handleMessage(const XBusMessage& msg,
                                   std::shared_ptr<IpcSocket> client) noexcept {
    auto op = static_cast<WeatherOpcode>(msg.opcode_);

    if (op == WeatherOpcode::GetCurrent) {
        std::lock_guard<std::mutex> lk(mutex_);
        XBusMessage resp{};
        std::strncpy(resp.service_name_, "com.freebsdx.Weather",
                     sizeof(resp.service_name_) - 1);
        resp.opcode_ = static_cast<uint32_t>(WeatherOpcode::WeatherChanged);
        std::memcpy(resp.payload_, &current_, sizeof(current_));
        client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        return;
    }

    if (op == WeatherOpcode::GetForecast) {
        std::lock_guard<std::mutex> lk(mutex_);
        uint32_t total = static_cast<uint32_t>(forecast_.size());
        for (uint32_t i = 0; i < total; ++i) {
            WeatherForecastPage page{};
            page.page_     = i;
            page.has_more_ = (i + 1 < total) ? 1 : 0;
            page.total_    = total;
            page.count_    = 1;
            page.day_      = forecast_[i];
            XBusMessage resp{};
            std::strncpy(resp.service_name_, "com.freebsdx.Weather",
                         sizeof(resp.service_name_) - 1);
            resp.opcode_ = static_cast<uint32_t>(WeatherOpcode::GetForecast);
            std::memcpy(resp.payload_, &page, sizeof(page));
            client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        }
        return;
    }

    if (op == WeatherOpcode::SetLocation) {
        const auto* req = reinterpret_cast<const WeatherSetLocationRequest*>(msg.payload_);
        std::string new_city(req->city_,
            strnlen(req->city_, sizeof(req->city_)));
        new_city = sanitizeCity(new_city);
        if (new_city.empty()) return;

        {
            std::lock_guard<std::mutex> lk(mutex_);
            city_ = new_city;
        }

        // Persist
        std::ofstream ofs(config_path_);
        if (ofs) ofs << new_city << "\n";

        // Acknowledge
        WeatherStatusResponse resp{};
        resp.success_ = 1;
        std::strncpy(resp.message_, "Location updated", sizeof(resp.message_) - 1);
        XBusMessage out{};
        std::strncpy(out.service_name_, "com.freebsdx.Weather",
                     sizeof(out.service_name_) - 1);
        out.opcode_ = static_cast<uint32_t>(WeatherOpcode::SetLocation);
        std::memcpy(out.payload_, &resp, sizeof(resp));
        client->sendMessage({reinterpret_cast<const uint8_t*>(&out), sizeof(out)});

        // Re-fetch in background
        std::thread([]() { WeatherService::fetchAndStore(); }).detach();
        return;
    }
}

} // namespace fbx

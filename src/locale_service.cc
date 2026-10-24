#include "xserver/locale_service.h"
#include "shared/logger.h"

#include <cstdlib>
#include <cstring>
#include <fstream>

namespace fbx::xserver {

void BroadcastXBusMessage(const shared::XBusMessage&);

std::string LocaleService::locale_ = "en_US.UTF-8";

std::string LocaleService::path() noexcept {
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "/tmp") + "/.freebsdx-locale";
}

void LocaleService::init() noexcept {
    std::ifstream f(path());
    std::string line;
    if (f.is_open() && std::getline(f, line) && !line.empty()) {
        locale_ = line;
    } else if (const char* lang = std::getenv("LANG"); lang && *lang) {
        locale_ = lang;   // fall back to the environment on first run
    }
    LOG_INFO("LocaleService: locale = {}", locale_);
}

std::string LocaleService::currentLocale() noexcept { return locale_; }

// Split "ll_TT.ENC@mod" into its parts for LocaleState.
shared::LocaleState LocaleService::buildState() noexcept {
    shared::LocaleState st{};
    std::strncpy(st.locale_, locale_.c_str(), sizeof(st.locale_) - 1);

    std::string lang, terr, enc;
    size_t i = 0;
    while (i < locale_.size() && locale_[i] != '_' && locale_[i] != '.' && locale_[i] != '@')
        lang += locale_[i++];
    if (i < locale_.size() && locale_[i] == '_') {
        ++i;
        while (i < locale_.size() && locale_[i] != '.' && locale_[i] != '@')
            terr += locale_[i++];
    }
    if (i < locale_.size() && locale_[i] == '.') {
        ++i;
        while (i < locale_.size() && locale_[i] != '@')
            enc += locale_[i++];
    }
    std::strncpy(st.lang_,      lang.c_str(), sizeof(st.lang_) - 1);
    std::strncpy(st.territory_, terr.c_str(), sizeof(st.territory_) - 1);
    std::strncpy(st.encoding_,  enc.c_str(),  sizeof(st.encoding_) - 1);
    return st;
}

void LocaleService::sendState(IpcSocket* client, uint32_t tid) noexcept {
    shared::LocaleState st = buildState();
    shared::XBusMessage reply{};
    reply.transaction_id_ = tid;   // echo so the client's Call callback matches
    std::strncpy(reply.service_name_, "com.freebsdx.Locale",
                 sizeof(reply.service_name_) - 1);
    reply.opcode_ = static_cast<uint32_t>(shared::LocaleOpcode::GetLocale);
    std::memcpy(reply.payload_, &st, sizeof(st));
    client->sendMessage({reinterpret_cast<const uint8_t*>(&reply), sizeof(reply)});
}

void LocaleService::handleMessage(const shared::XBusMessage& msg,
                                  std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::LocaleOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetLocale:
        sendState(client.get(), msg.transaction_id_);
        break;
    case O::SetLocale: {
        shared::LocaleSetRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.locale_[sizeof(req.locale_) - 1] = '\0';
        if (req.locale_[0] == '\0') break;
        locale_ = req.locale_;
        if (std::ofstream f(path()); f.is_open()) f << locale_ << '\n';
        LOG_INFO("LocaleService: locale set to {}", locale_);
        // Broadcast so running apps re-load catalogs (transaction_id_ 0).
        shared::XBusMessage bcast{};
        std::strncpy(bcast.service_name_, "com.freebsdx.Locale",
                     sizeof(bcast.service_name_) - 1);
        bcast.opcode_ = static_cast<uint32_t>(O::LocaleChanged);
        shared::LocaleState st = buildState();
        std::memcpy(bcast.payload_, &st, sizeof(st));
        BroadcastXBusMessage(bcast);
        break;
    }
    default:
        break;
    }
}

}  // namespace fbx::xserver

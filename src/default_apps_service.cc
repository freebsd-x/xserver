#include "xserver/default_apps_service.h"
#include "xserver/ipc_socket.h"
#include "xserver/mime_service.h"
#include "xserver/xbus_reply.h"
#include "shared/logger.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace fbx::xserver {

namespace {

using namespace svc;

struct Category {
    const char*              name;
    std::vector<std::string> mimes;   // the first one represents the category
};

const std::vector<Category>& categories() {
    static const std::vector<Category> kAll = {
        {"Web Browser",  {"text/html"}},
        {"Text Editor",  {"text/plain", "text/markdown"}},
        {"Image Viewer", {"image/png", "image/jpeg", "image/gif", "image/webp", "image/svg+xml", "image/bmp"}},
        {"Music",        {"audio/mpeg", "audio/flac", "audio/wav", "audio/ogg"}},
        {"Video",        {"video/mp4", "video/x-matroska", "video/webm"}},
        {"PDF Viewer",   {"application/pdf"}},
        {"Archives",     {"application/zip", "application/x-tar", "application/gzip"}},
    };
    return kAll;
}

// Categories some installed app handles.
std::vector<const Category*> available() {
    std::vector<const Category*> out;
    for (const auto& c : categories())
        if (!MimeService::allHandlers(c.mimes.front()).empty()) out.push_back(&c);
    return out;
}

std::string label(const std::string& app_id) {
    std::string n = MimeService::appName(app_id);
    return n.empty() ? app_id : n;
}

}  // namespace

bool DefaultAppsService::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                        const shared::XBusMessage& msg) noexcept {
    if (std::strcmp(msg.service_name_, "com.freebsdx.DefaultApps") != 0) return false;
    using Op = shared::DefaultAppsOpcode;
    constexpr const char* kName = "com.freebsdx.DefaultApps";
    switch (static_cast<Op>(msg.opcode_)) {
    case Op::ListDefaults: {
        auto cats = available();
        uint32_t page = readPage(msg);
        shared::DefaultAppPage pg{};
        pg.page_  = page;
        pg.total_ = static_cast<uint32_t>(cats.size());
        if (page < cats.size()) {
            pg.valid_    = 1;
            pg.has_more_ = page + 1 < cats.size() ? 1u : 0u;
            std::strncpy(pg.category_, cats[page]->name, sizeof(pg.category_) - 1);
            std::string h = label(MimeService::defaultHandler(cats[page]->mimes.front()));
            std::strncpy(pg.handler_, h.c_str(), sizeof(pg.handler_) - 1);
        }
        reply(msg, client, kName, msg.opcode_, &pg);
        break;
    }
    case Op::CycleDefault: {
        shared::DefaultAppRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.category_[sizeof(req.category_) - 1] = '\0';
        bool ok = false;
        for (const Category* c : available()) {
            if (std::strcmp(c->name, req.category_) != 0) continue;
            // Stable order (by name) so cycling visits every handler.
            auto handlers = MimeService::allHandlers(c->mimes.front());
            std::sort(handlers.begin(), handlers.end(),
                      [](const std::string& a, const std::string& b) { return label(a) < label(b); });
            std::string cur = MimeService::defaultHandler(c->mimes.front());
            auto it = std::find(handlers.begin(), handlers.end(), cur);
            size_t next = it == handlers.end() ? 0 : (static_cast<size_t>(it - handlers.begin()) + 1) % handlers.size();
            for (const auto& mime : c->mimes) MimeService::setDefault(mime, handlers[next]);
            LOG_INFO("DefaultAppsService: {} → {}", c->name, label(handlers[next]));
            ok = true;
            break;
        }
        ack(msg, client, kName);
        if (ok) broadcast<char>(kName, static_cast<uint32_t>(Op::DefaultsChanged), nullptr);
        break;
    }
    default: break;
    }
    return true;
}

} // namespace fbx::xserver

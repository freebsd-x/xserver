#include "xserver/bookmark_service.h"
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>

namespace fbx::xserver {

std::mutex            BookmarkService::mutex_;
std::vector<BookmarkService::Bookmark> BookmarkService::bookmarks_;
bool                  BookmarkService::loaded_ = false;

std::string BookmarkService::bookmarksPath() noexcept {
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    return std::string(home) + "/.freebsdx-bookmarks";
}

void BookmarkService::load() noexcept {
    if (loaded_) return;
    loaded_ = true;
    std::ifstream f(bookmarksPath());
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line)) {
        auto sep = line.find('|');
        if (sep == std::string::npos) continue;
        // format: url|title
        bookmarks_.push_back({line.substr(sep + 1), line.substr(0, sep)});
    }
}

void BookmarkService::save() noexcept {
    std::ofstream f(bookmarksPath(), std::ios::trunc);
    for (const auto& b : bookmarks_)
        f << b.url << '|' << b.title << '\n';
}

bool BookmarkService::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                     const shared::XBusMessage& msg) noexcept {
    using namespace shared;
    if (std::strncmp(msg.service_name_, "com.freebsdx.Bookmarks",
                     sizeof(msg.service_name_)) != 0)
        return false;

    std::lock_guard<std::mutex> lk(mutex_);
    load();

    XBusMessage resp = msg;

    switch (static_cast<BookmarkOpcode>(msg.opcode_)) {

    case BookmarkOpcode::Add: {
        BookmarkRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        // Avoid duplicates by URL
        for (const auto& b : bookmarks_) {
            if (b.url == req.url_) {
                (void)client->sendMessage(
                    {reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
                return true;
            }
        }
        bookmarks_.push_back({req.title_[0] ? req.title_ : req.url_, req.url_});
        save();
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        break;
    }

    case BookmarkOpcode::Remove: {
        BookmarkRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        auto it = std::remove_if(bookmarks_.begin(), bookmarks_.end(),
            [&](const Bookmark& b) { return b.url == req.url_; });
        if (it != bookmarks_.end()) {
            bookmarks_.erase(it, bookmarks_.end());
            save();
        }
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        break;
    }

    case BookmarkOpcode::List: {
        uint32_t total = static_cast<uint32_t>(bookmarks_.size());
        for (uint32_t i = 0; i < total; ++i) {
            XBusMessage page_resp{};
            page_resp.magic_ = XBUS_MAGIC;
            std::strncpy(page_resp.service_name_, "com.freebsdx.Bookmarks",
                         sizeof(page_resp.service_name_) - 1);
            page_resp.opcode_ = static_cast<uint32_t>(BookmarkOpcode::BookmarkPage);
            BookmarkPage pg{};
            pg.page_  = i;
            pg.total_ = total;
            pg.valid_ = 1;
            std::strncpy(pg.title_, bookmarks_[i].title.c_str(),
                         sizeof(pg.title_) - 1);
            std::strncpy(pg.url_,   bookmarks_[i].url.c_str(),
                         sizeof(pg.url_)   - 1);
            std::memcpy(page_resp.payload_, &pg, sizeof(pg));
            (void)client->sendMessage(
                {reinterpret_cast<const uint8_t*>(&page_resp), sizeof(page_resp)});
        }
        // End sentinel
        XBusMessage end{};
        end.magic_ = XBUS_MAGIC;
        std::strncpy(end.service_name_, "com.freebsdx.Bookmarks",
                     sizeof(end.service_name_) - 1);
        end.opcode_ = static_cast<uint32_t>(BookmarkOpcode::BookmarkPage);
        BookmarkPage sentinel{};
        sentinel.page_  = total;
        sentinel.total_ = total;
        sentinel.valid_ = 0;
        std::memcpy(end.payload_, &sentinel, sizeof(sentinel));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&end), sizeof(end)});
        break;
    }

    case BookmarkOpcode::Clear:
        bookmarks_.clear();
        save();
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        break;

    default:
        break;
    }
    return true;
}

} // namespace fbx::xserver

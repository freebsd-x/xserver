#pragma once
#include "xserver/ipc_socket.h"
#include "shared/protocol.h"
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace fbx::xserver {

class BookmarkService {
public:
    static bool ProcessMessage(std::shared_ptr<IpcSocket> client,
                               const shared::XBusMessage& msg) noexcept;

private:
    struct Bookmark { std::string title; std::string url; };

    static void load() noexcept;
    static void save() noexcept;
    static std::string bookmarksPath() noexcept;

    static std::mutex            mutex_;
    static std::vector<Bookmark> bookmarks_;
    static bool                  loaded_;
};

} // namespace fbx::xserver

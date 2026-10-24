#pragma once

#include <memory>
#include <mutex>
#include <vector>
#include <string>
#include <shared/protocol.h>
#include "xserver/ipc_socket.h"

namespace fbx {

using xserver::IpcSocket;

class FontManagerService {
public:
    static void init() noexcept;
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;

private:
    static std::vector<shared::FontInfo> enumerateFonts() noexcept;
    static bool                          installFont(const std::string& src_path) noexcept;
    static bool                          removeFont(const std::string& path) noexcept;
    static std::string                   detectFormat(const std::string& path) noexcept;
    static bool                          isUserFont(const std::string& path) noexcept;
    static bool                          isValidFontPath(const std::string& path) noexcept;

    static std::mutex                    mutex_;
    static std::vector<shared::FontInfo> cache_;
    static bool                          cache_ready_;
};

} // namespace fbx

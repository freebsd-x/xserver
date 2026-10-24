#pragma once

#include "shared/protocol.h"
#include <string>
#include <memory>
#include <span>

namespace fbx::xserver {

class IpcSocket;

class ClipboardService {
public:
    static bool ProcessMessage(std::shared_ptr<IpcSocket> client, const shared::XBusMessage& msg, bool has_focus) noexcept;

private:
    static std::string clipboard_data_;
};

} // namespace fbx::xserver

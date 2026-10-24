#include "xserver/clipboard_service.h"
#include "xserver/clipboard_history_service.h"
#include "xserver/ipc_socket.h"
#include <cstring>
#include <iostream>

namespace fbx::xserver {

std::string ClipboardService::clipboard_data_ = "";

bool ClipboardService::ProcessMessage(std::shared_ptr<IpcSocket> client, const shared::XBusMessage& msg, bool has_focus) noexcept {
    if (std::strcmp(msg.service_name_, "com.freebsdx.Clipboard") != 0) return false;

    if (!has_focus) {
        std::cerr << "[ClipboardService] Security Alert: Denied clipboard access to background app.\n";
        return false;
    }

    shared::XBusMessage response = msg;

    if (msg.opcode_ == 0x01) { // SetClipboard
        clipboard_data_ = reinterpret_cast<const char*>(msg.payload_);
        clipboard_data_[255] = '\0'; // ensure null-terminated
        std::cout << "[ClipboardService] Set data: " << clipboard_data_ << std::endl;
        ClipboardHistoryService::record(clipboard_data_);
    }
    else if (msg.opcode_ == 0x02) { // GetClipboard
        std::strncpy(reinterpret_cast<char*>(response.payload_), clipboard_data_.c_str(), sizeof(response.payload_) - 1);
    } 
    else {
        return false;
    }

    std::span<const uint8_t> resp_span(reinterpret_cast<const uint8_t*>(&response), sizeof(response));
    (void)client->sendMessage(resp_span);
    return true;
}

} // namespace fbx::xserver

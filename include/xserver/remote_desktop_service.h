#pragma once
#include "xserver/ipc_socket.h"
#include "shared/protocol.h"
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fbx::xserver {

class VulkanCompositor; // forward

class RemoteDesktopService {
public:
    static void setCompositor(VulkanCompositor* comp) noexcept;

    // XBus message handler
    static bool ProcessMessage(std::shared_ptr<IpcSocket> client,
                               const shared::XBusMessage& msg) noexcept;

    // Called from the render timer in main.cc to inject remote input
    static std::vector<shared::InputMessage> drainInjectedInput() noexcept;

    // Queue a synthetic input event for the compositor to process next frame.
    // Used by the VNC server and by the headless test harness (Headless/InjectInput).
    static void pushInjectedInput(const shared::InputMessage& msg) noexcept;

    static bool isRunning() noexcept;

private:
    static void startServer() noexcept;
    static void stopServer() noexcept;
    static void acceptLoop() noexcept;
    static void clientLoop(int client_fd, uint32_t screen_w, uint32_t screen_h) noexcept;
    static bool doVncAuth(int fd, const std::string& password) noexcept;
    static std::string currentPassword() noexcept;
    static void sendFrameUpdate(int fd, uint32_t w, uint32_t h) noexcept;
    static void injectKeyEvent(uint32_t keysym, uint8_t down) noexcept;
    static void injectPointerEvent(uint8_t btn, uint16_t x, uint16_t y) noexcept;
    static void broadcastStatus() noexcept;

    static VulkanCompositor*         comp_;
    static std::atomic<bool>         running_;
    static std::atomic<uint32_t>     client_count_;
    static int                       listen_fd_;
    static std::thread               accept_thread_;
    static std::string               password_;   // guarded by password_mutex_
    static std::mutex                password_mutex_;
    static constexpr uint16_t        kPort = 5900;

    static std::mutex                       inject_mutex_;
    static std::deque<shared::InputMessage> inject_queue_;

    // Capture mutex: captureToShm must not be called concurrently
    static std::mutex capture_mutex_;
};

} // namespace fbx::xserver

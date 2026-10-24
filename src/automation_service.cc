#include "xserver/automation_service.h"
#include "xserver/xbus_dispatcher.h"
#include "xserver/ipc_socket.h"
#include "xserver/vulkan_compositor.h"
#include "xserver/window_stack.h"
#include <iostream>
#include <cstring>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

namespace fbx::xserver {

std::optional<shared::InputMessage> AutomationService::ProcessMessage(std::shared_ptr<IpcSocket> client, const shared::XBusMessage& msg, VulkanCompositor* comp, WindowStack* stack) noexcept {
    if (std::strcmp(msg.service_name_, "com.freebsdx.Automation") != 0) return std::nullopt;

    std::cout << "[AutomationService] Intercepted Automation service request (opcode: " << msg.opcode_ << ")\n";

    shared::XBusMessage response = msg;
    std::optional<shared::InputMessage> injected_input = std::nullopt;

    if (msg.opcode_ == 0x01) { // QueryTree
        std::cout << "[AutomationService] -> QueryTree request.\n";
        if (stack) {
            uint32_t count = 0;
            auto windows = stack->getWindows();
            for (size_t i = 0; i < std::min(windows.size(), (size_t)10); i++) {
                uint32_t id = windows[i].surface_id_;
                std::memcpy(response.payload_ + (i * 4), &id, 4);
                count++;
            }
            std::memcpy(response.payload_ + 252, &count, 4);
        }
    } else if (msg.opcode_ == 0x02) { // InjectAction
        std::cout << "[AutomationService] -> InjectAction request. Simulating tap event.\n";
        injected_input = shared::InputMessage{
            .type_ = shared::InputEventType::MouseButton,
            .value_a_ = 1,
            .value_b_ = 1 // Pressed
        };
    } else if (msg.opcode_ == 0x03) { // GetScreenChecksum
        std::cout << "[AutomationService] -> GetScreenChecksum request.\n";
        uint32_t checksum = 0;
        if (comp) checksum = comp->getFrameChecksum();
        std::memcpy(response.payload_, &checksum, sizeof(checksum));
    } else if (msg.opcode_ == 0x04) { // FetchPixels (Safe SHM)
        std::cout << "[AutomationService] -> FetchPixels request.\n";
        if (comp) {
            auto pixels_opt = comp->readbackPixels(1920, 1080);
            if (pixels_opt) {
                int shm_fd = shm_open(SHM_ANON, O_RDWR | O_CREAT, 0600);
                if (shm_fd >= 0) {
                    size_t size = pixels_opt->size() * sizeof(uint32_t);
                    if (ftruncate(shm_fd, size) == 0) {
                        void* ptr = mmap(NULL, size, PROT_WRITE, MAP_SHARED, shm_fd, 0);
                        if (ptr != MAP_FAILED) {
                            std::memcpy(ptr, pixels_opt->data(), size);
                            munmap(ptr, size);
                            
                            std::strncpy(reinterpret_cast<char*>(response.payload_), "PIXELS_READY", 255);
                            std::span<const uint8_t> resp_span(reinterpret_cast<const uint8_t*>(&response), sizeof(response));
                            (void)client->sendMessage(resp_span, shm_fd);
                            
                            // Keep the FD open for a short time to allow transmission
                            static int last_shm_fd = -1;
                            if (last_shm_fd != -1) close(last_shm_fd);
                            last_shm_fd = shm_fd;

                            return std::nullopt;
                        }
                    }
                    close(shm_fd);
                }
            }
        }
    } else if (msg.opcode_ == 0x05) { // InjectRawInput
        std::cout << "[AutomationService] -> InjectRawInput request.\n";
        shared::InputMessage input;
        std::memcpy(&input, msg.payload_, sizeof(input));
        injected_input = input;
    } else if (msg.opcode_ == 0x06) { // InspectSurface
        uint32_t surface_id;
        std::memcpy(&surface_id, msg.payload_, 4);
        std::cout << "[AutomationService] -> InspectSurface request (ID: " << surface_id << ").\n";
        
        if (comp && stack) {
            for (const auto& w : stack->getWindows()) {
                if (w.surface_id_ == surface_id) {
                    auto pixels = comp->inspectSurfacePixels(w.surface_image_, w.size_.width_, w.size_.height_);
                    if (pixels) {
                        std::strncpy(reinterpret_cast<char*>(response.payload_), "PIXELS_CAPTURED", 255);
                    }
                }
            }
        }
    }

    // Send Response back to client
    std::span<const uint8_t> resp_span(reinterpret_cast<const uint8_t*>(&response), sizeof(response));
    (void)client->sendMessage(resp_span);

    return injected_input;
}

} // namespace fbx::xserver

#include "xserver/display_service.h"
#include "xserver/xbus_dispatcher.h"
#include "xserver/ipc_socket.h"
#include "xserver/drm_device.h"
#include <cstring>
#include <cstdio>
#include <iostream>
#include <algorithm>

namespace fbx::xserver {

// Static storage — default single 1920×1080 output.
std::vector<OutputInfo> DisplayService::g_outputs_ = {{0.0f, 1920.0f, 1080.0f}};

void DisplayService::applyOutputScale(WindowStack* stack, size_t idx, float scale) noexcept {
    if (!stack || scale <= 0.0f) return;
    stack->setOutputScale(idx, scale);
    g_outputs_ = stack->outputLayout();   // sync logical dims
    BroadcastOutputChanged();             // notify shell + apps to re-layout
}

void DisplayService::setOutputs(const std::vector<OutputInfo>& layout) noexcept {
    g_outputs_ = layout;
}

shared::DisplayOutputList DisplayService::buildOutputList() noexcept {
    shared::DisplayOutputList list{};
    list.count_ = static_cast<uint32_t>(std::min(g_outputs_.size(), size_t{4}));
    for (uint32_t i = 0; i < list.count_; ++i) {
        auto& o     = list.outputs_[i];
        o.index_      = i;
        o.width_      = static_cast<uint32_t>(g_outputs_[i].width_);  // logical
        o.height_     = static_cast<uint32_t>(g_outputs_[i].height_); // logical
        o.refresh_hz_ = 60; // real refresh rate comes from DRM — plumbed via setOutputs later
        o.x_offset_   = g_outputs_[i].x_offset_;
        o.scale_      = g_outputs_[i].scale_;
        o.vrr_capable_ = g_outputs_[i].vrr_capable_ ? 1u : 0u;
        o.vrr_enabled_ = g_outputs_[i].vrr_enabled_ ? 1u : 0u;
        o.hdr_capable_ = g_outputs_[i].hdr_capable_ ? 1u : 0u;
        o.hdr_enabled_ = g_outputs_[i].hdr_enabled_ ? 1u : 0u;
        std::snprintf(o.name_, sizeof(o.name_), "Out-%u", i);
    }
    return list;
}

void DisplayService::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                   const shared::XBusMessage& msg,
                                   WindowStack* stack,
                                   DrmDevice* drm) noexcept {
    using namespace shared;
    XBusMessage resp = msg; // copy header so transaction_id_ / service_name_ are echoed

    switch (static_cast<DisplayOpcode>(msg.opcode_)) {

    case DisplayOpcode::QueryOutputs: {
        DisplayOutputList list = buildOutputList();
        std::memset(resp.payload_, 0, sizeof(resp.payload_));
        std::memcpy(resp.payload_, &list, sizeof(list));
        (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        std::cout << "[DisplayService] QueryOutputs → " << list.count_ << " output(s)\n";
        break;
    }

    case DisplayOpcode::GetWindowOutput: {
        uint32_t surface_id = 0;
        std::memcpy(&surface_id, msg.payload_, sizeof(surface_id));

        uint32_t oi = stack ? stack->getWindowOutput(surface_id) : 0u;

        DisplayWindowInfo wi{};
        wi.output_idx_ = oi;
        if (oi < g_outputs_.size()) {
            wi.output_width_  = static_cast<uint32_t>(g_outputs_[oi].width_);
            wi.output_height_ = static_cast<uint32_t>(g_outputs_[oi].height_);
            wi.x_offset_      = g_outputs_[oi].x_offset_;
        }
        std::memset(resp.payload_, 0, sizeof(resp.payload_));
        std::memcpy(resp.payload_, &wi, sizeof(wi));
        (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        break;
    }

    case DisplayOpcode::MoveWindowToOutput: {
        DisplayMoveRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        if (stack) {
            stack->moveWindowToOutput(req.surface_id_, req.output_idx_);
            std::cout << "[DisplayService] MoveWindowToOutput: surface=" << req.surface_id_
                      << " → output " << req.output_idx_ << "\n";
        }
        // Fire-and-forget — no response needed
        break;
    }

    case DisplayOpcode::SetOutputScale: {
        DisplayScaleRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        if (stack && req.scale_ > 0.0f) {
            applyOutputScale(stack, req.output_idx_, req.scale_);
            std::cout << "[DisplayService] SetOutputScale: output=" << req.output_idx_
                      << " scale=" << req.scale_ << "\n";
        }
        break;
    }

    case DisplayOpcode::SetVrrEnabled: {
        DisplayVrrRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        bool enable = req.enabled_ != 0;
        if (drm) {
            auto res = drm->setVrrEnabled(req.output_idx_, enable);
            if (res) {
                // Mirror state into window stack and g_outputs_ so QueryOutputs
                // and broadcasts reflect the change immediately.
                if (stack) stack->setVrrEnabled(req.output_idx_, enable);
                if (req.output_idx_ < g_outputs_.size())
                    g_outputs_[req.output_idx_].vrr_enabled_ = enable;
                BroadcastOutputChanged();
                std::cout << "[DisplayService] SetVrrEnabled: output=" << req.output_idx_
                          << " enabled=" << enable << "\n";
            } else {
                std::cerr << "[DisplayService] SetVrrEnabled failed: "
                          << res.error().message() << "\n";
            }
        } else {
            std::cerr << "[DisplayService] SetVrrEnabled: no DRM device\n";
        }
        break;
    }

    case DisplayOpcode::QueryVrrCapable: {
        uint32_t output_idx = 0;
        std::memcpy(&output_idx, msg.payload_, sizeof(output_idx));

        DisplayVrrCapability cap{};
        cap.output_idx_ = output_idx;
        if (output_idx < g_outputs_.size()) {
            cap.capable_  = g_outputs_[output_idx].vrr_capable_ ? 1u : 0u;
            cap.enabled_  = g_outputs_[output_idx].vrr_enabled_ ? 1u : 0u;
        }
        std::memset(resp.payload_, 0, sizeof(resp.payload_));
        std::memcpy(resp.payload_, &cap, sizeof(cap));
        (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        break;
    }

    case DisplayOpcode::SetHdrEnabled: {
        DisplayHdrRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        bool enable = req.enabled_ != 0;
        uint32_t nits = (req.max_luminance_ == 0) ? 1000u : req.max_luminance_;
        if (drm) {
            auto res = drm->setHdrEnabled(req.output_idx_, enable, nits);
            if (res) {
                if (stack) stack->setHdrEnabled(req.output_idx_, enable);
                if (req.output_idx_ < g_outputs_.size()) {
                    g_outputs_[req.output_idx_].hdr_enabled_ = enable;
                    g_outputs_[req.output_idx_].hdr_max_luminance_ = enable ? nits : 0u;
                }
                BroadcastOutputChanged();
                std::cout << "[DisplayService] SetHdrEnabled: output=" << req.output_idx_
                          << " enabled=" << enable << " nits=" << nits << "\n";
            } else {
                std::cerr << "[DisplayService] SetHdrEnabled failed: "
                          << res.error().message() << "\n";
            }
        } else {
            std::cerr << "[DisplayService] SetHdrEnabled: no DRM device\n";
        }
        break;
    }

    case DisplayOpcode::QueryHdrCapable: {
        uint32_t output_idx = 0;
        std::memcpy(&output_idx, msg.payload_, sizeof(output_idx));

        DisplayHdrCapability cap{};
        cap.output_idx_ = output_idx;
        if (output_idx < g_outputs_.size()) {
            cap.capable_       = g_outputs_[output_idx].hdr_capable_ ? 1u : 0u;
            cap.enabled_       = g_outputs_[output_idx].hdr_enabled_ ? 1u : 0u;
            cap.max_luminance_ = g_outputs_[output_idx].hdr_max_luminance_;
        }
        std::memset(resp.payload_, 0, sizeof(resp.payload_));
        std::memcpy(resp.payload_, &cap, sizeof(cap));
        (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        break;
    }

    default:
        std::cerr << "[DisplayService] Unknown opcode: 0x" << std::hex << msg.opcode_
                  << std::dec << "\n";
        break;
    }
}

void DisplayService::BroadcastOutputChanged() noexcept {
    shared::XBusMessage msg{};
    std::strncpy(msg.service_name_, "com.freebsdx.Display", sizeof(msg.service_name_) - 1);
    msg.opcode_ = static_cast<uint32_t>(shared::DisplayOpcode::OutputChanged);
    shared::DisplayOutputList list = buildOutputList();
    std::memcpy(msg.payload_, &list, sizeof(list));
    BroadcastXBusMessage(msg);
    std::cout << "[DisplayService] Broadcast OutputChanged (" << list.count_ << " output(s))\n";
}

} // namespace fbx::xserver

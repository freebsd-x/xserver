#include "xserver/drm_device.h"
#include "xserver/cursor_glyphs.h"
#include "xserver/seat_manager.h"
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <functional>
#include <sys/mman.h>
#include <sys/stat.h>
#include <cstring>
#include <algorithm>
#include <libdrm/drm_mode.h>

namespace fbx::xserver {

std::expected<std::unique_ptr<DrmDevice>, std::error_code> DrmDevice::create(ISeatManager& seat_manager) noexcept {
    const std::vector<std::string> nodes = {"/dev/dri/card0", "/dev/dri/card1", "/dev/drm/0"};
    
    for (const auto& path : nodes) {
        if (access(path.c_str(), F_OK) != 0) continue;

        auto result = seat_manager.openDevice(path);
        if (!result) continue;

        int fd = result->fd_.get();
        struct stat st;
        if (fstat(fd, &st) == 0 && S_ISCHR(st.st_mode)) {
            std::cout << "[xserver] Hardware Found: " << path << " (Master lock acquired)\n";
            auto device = std::make_unique<DrmDevice>(std::move(result->fd_), &seat_manager, result->device_id_);
            auto enable_res = device->enableAtomic();
            if (enable_res) return device;
        }
    }

    return std::unexpected(std::make_error_code(std::errc::no_such_device));
}

DrmDevice::DrmDevice(UniqueFd fd, ISeatManager* seat_manager, int device_id) noexcept
    : fd_(std::move(fd)), seat_manager_(seat_manager), device_id_(device_id)
{
}

DrmDevice::~DrmDevice() noexcept {
    // Release cursor dumb buffer
    if (cursor_.mapped && cursor_.mapped != MAP_FAILED) {
        munmap(cursor_.mapped, kCursorSize * cursor_.pitch);
        cursor_.mapped = nullptr;
    }
    if (cursor_.gem_handle) {
        struct drm_mode_destroy_dumb dreq{};
        dreq.handle = cursor_.gem_handle;
        drmIoctl(fd_.get(), DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        cursor_.gem_handle = 0;
    }

    // Reinstate the console scanout BEFORE dropping master — the ioctls require
    // being DRM master, and it leaves the seat usable instead of showing our now
    // freed last frame.
    restoreSavedCrtcs();

    if (seat_manager_ && device_id_ >= 0) {
        (void)drmDropMaster(fd_.get());
        int raw_fd = fd_.release();
        auto res = seat_manager_->closeDevice(device_id_);
        if (!res) {
            std::cerr << "[xserver] seatd close failed: " << res.error().message() << "\n";
            ::close(raw_fd);
        }
    }
}

void DrmDevice::restoreSavedCrtcs() noexcept {
    // Reset any colour transform we applied (NightLight warm tint / ColorFilter)
    // before handing the seat back — drmModeSetCrtc restores the scanout mode and
    // framebuffer but NOT the CRTC's CTM, so without this the text console is left
    // tinted after the compositor exits. Setting the CTM property to blob 0 selects
    // the identity/bypass transform. Non-modeset; we are still DRM master here
    // (called before drmDropMaster). No-op on CRTCs without a CTM property.
    for (const auto& crtc : crtcs_) {
        uint32_t ctm_prop = getPropertyId(crtc, "CTM");
        if (ctm_prop == 0) continue;
        if (drmModeAtomicReqPtr req = drmModeAtomicAlloc()) {
            drmModeAtomicAddProperty(req, crtc.id_, ctm_prop, 0);
            (void)drmModeAtomicCommit(fd_.get(), req, 0, nullptr);
            drmModeAtomicFree(req);
        }
    }

    for (auto& s : saved_crtcs_) {
        if (!s.crtc) continue;
        // drmModeSetCrtc reprograms the CRTC with the saved mode + framebuffer
        // (the fbcon buffer, still owned by the kernel). A zero buffer_id means
        // the CRTC was disabled before us; pass it through to disable cleanly.
        uint32_t conn = s.connector_id;
        (void)drmModeSetCrtc(fd_.get(), s.crtc_id, s.crtc->buffer_id,
                             s.crtc->x, s.crtc->y,
                             s.crtc->mode_valid ? &conn : nullptr,
                             s.crtc->mode_valid ? 1 : 0,
                             s.crtc->mode_valid ? &s.crtc->mode : nullptr);
        drmModeFreeCrtc(s.crtc);
        s.crtc = nullptr;
    }
    saved_crtcs_.clear();
}

std::expected<void, std::error_code> DrmDevice::enableAtomic() noexcept {
    if (drmSetClientCap(fd_.get(), DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0) {
        return std::unexpected(std::error_code(errno, std::generic_category()));
    }
    if (drmSetClientCap(fd_.get(), DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
        return std::unexpected(std::error_code(errno, std::generic_category()));
    }
    std::cout << "[xserver] Atomic KMS capabilities enabled successfully.\n";
    return {};
}

void DrmDevice::fetchObjectProperties(DrmObject& obj) noexcept {
    drmModeObjectProperties* props = drmModeObjectGetProperties(fd_.get(), obj.id_, obj.type_);
    if (!props) return;
    for (uint32_t i = 0; i < props->count_props; ++i) {
        drmModePropertyRes* prop_info = drmModeGetProperty(fd_.get(), props->props[i]);
        if (prop_info) {
            DrmProperty prop; prop.id_ = prop_info->prop_id; prop.value_ = props->prop_values[i];
            obj.props_[prop_info->name] = prop;
            drmModeFreeProperty(prop_info);
        }
    }
    drmModeFreeObjectProperties(props);
}

uint32_t DrmDevice::getPropertyId(const DrmObject& obj, const std::string& prop_name) const noexcept {
    auto it = obj.props_.find(prop_name);
    if (it != obj.props_.end()) return it->second.id_;
    return 0;
}

std::expected<void, std::error_code> DrmDevice::discoverResources() noexcept {
    drmModeRes* res = drmModeGetResources(fd_.get());
    if (!res) return std::unexpected(std::error_code(errno, std::generic_category()));
    for (int i = 0; i < res->count_connectors; ++i) {
        drmModeConnector* conn = drmModeGetConnector(fd_.get(), res->connectors[i]);
        if (conn) {
            if (conn->connection == DRM_MODE_CONNECTED) {
                DrmObject obj; obj.id_ = conn->connector_id; obj.type_ = DRM_MODE_OBJECT_CONNECTOR;
                fetchObjectProperties(obj); connectors_.push_back(std::move(obj));
            }
            drmModeFreeConnector(conn);
        }
    }
    for (int i = 0; i < res->count_crtcs; ++i) {
        DrmObject obj; obj.id_ = res->crtcs[i]; obj.type_ = DRM_MODE_OBJECT_CRTC;
        fetchObjectProperties(obj); crtcs_.push_back(std::move(obj));
    }
    drmModePlaneRes* plane_res = drmModeGetPlaneResources(fd_.get());
    if (plane_res) {
        for (uint32_t i = 0; i < plane_res->count_planes; ++i) {
            drmModePlane* p = drmModeGetPlane(fd_.get(), plane_res->planes[i]);
            if (p) {
                DrmObject obj; obj.id_ = p->plane_id; obj.type_ = DRM_MODE_OBJECT_PLANE;
                fetchObjectProperties(obj);
                if (obj.props_.count("type") && obj.props_["type"].value_ == DRM_PLANE_TYPE_PRIMARY) planes_.push_back(std::move(obj));
                drmModeFreePlane(p);
            }
        }
        drmModeFreePlaneResources(plane_res);
    }
    std::cout << "[xserver] Hardware Probed: " << connectors_.size() << " Monitors, " << crtcs_.size() << " CRTCs, " << planes_.size() << " Primary Planes.\n";
    buildOutputAssignments(res);
    drmModeFreeResources(res);
    return {};
}

void DrmDevice::buildOutputAssignments(drmModeRes* res) noexcept {
    outputs_.clear();

    // Track which CRTC indices are already claimed by an earlier output.
    std::vector<bool> crtc_used(crtcs_.size(), false);

    for (const auto& conn_obj : connectors_) {
        drmModeConnector* conn = drmModeGetConnector(fd_.get(), conn_obj.id_);
        if (!conn) continue;

        // Find a compatible, unclaimed CRTC via the encoder possible_crtcs bitmask.
        uint32_t chosen_crtc_id    = 0;
        uint32_t chosen_crtc_index = 0;
        bool found_crtc = false;

        for (int ei = 0; ei < conn->count_encoders && !found_crtc; ++ei) {
            drmModeEncoder* enc = drmModeGetEncoder(fd_.get(), conn->encoders[ei]);
            if (!enc) continue;

            uint32_t possible = enc->possible_crtcs;
            drmModeFreeEncoder(enc);

            for (uint32_t ci = 0; ci < static_cast<uint32_t>(res->count_crtcs); ++ci) {
                if (ci < crtcs_.size() && !crtc_used[ci] && (possible & (1u << ci))) {
                    chosen_crtc_id    = crtcs_[ci].id_;
                    chosen_crtc_index = ci;
                    crtc_used[ci]     = true;
                    found_crtc        = true;
                    break;
                }
            }
        }

        drmModeFreeConnector(conn);
        if (!found_crtc) continue; // No available CRTC for this connector

        // Find a primary plane compatible with the chosen CRTC.
        uint32_t chosen_plane_id    = 0;
        uint32_t chosen_plane_index = 0;
        bool found_plane = false;

        for (uint32_t pi = 0; pi < planes_.size(); ++pi) {
            drmModePlane* p = drmModeGetPlane(fd_.get(), planes_[pi].id_);
            if (!p) continue;
            uint32_t possible = p->possible_crtcs;
            drmModeFreePlane(p);
            if (possible & (1u << chosen_crtc_index)) {
                chosen_plane_id    = planes_[pi].id_;
                chosen_plane_index = pi;
                found_plane        = true;
                break;
            }
        }

        // Fallback: use same-index plane if nothing matched the bitmask
        if (!found_plane && chosen_crtc_index < planes_.size()) {
            chosen_plane_id    = planes_[chosen_crtc_index].id_;
            chosen_plane_index = chosen_crtc_index;
            found_plane        = true;
        }

        if (!found_plane) continue;

        Output out{};
        out.connector_id  = conn_obj.id_;
        out.crtc_id       = chosen_crtc_id;
        out.plane_id      = chosen_plane_id;
        out.crtc_index    = chosen_crtc_index;
        out.plane_index   = chosen_plane_index;
        out.mode          = getNativeResolution(conn_obj.id_);
        out.active        = false;
        outputs_.push_back(out);

        // Capture the CRTC's pre-takeover scanout state (the text console) once,
        // before we ever modeset it, so restoreSavedCrtcs() can hand the seat
        // back cleanly on shutdown. Skip on a rescan() after takeover, when
        // drmModeGetCrtc would just return our own framebuffer.
        bool already_saved = false;
        for (const auto& s : saved_crtcs_)
            if (s.crtc_id == chosen_crtc_id) { already_saved = true; break; }
        if (!already_saved) {
            if (drmModeCrtc* prev = drmModeGetCrtc(fd_.get(), chosen_crtc_id))
                saved_crtcs_.push_back({chosen_crtc_id, conn_obj.id_, prev});
        }

        std::cout << "[xserver] Output " << (outputs_.size() - 1)
                  << ": connector=" << conn_obj.id_
                  << " crtc=" << chosen_crtc_id
                  << " plane=" << chosen_plane_id
                  << " (" << out.mode.width_ << "x" << out.mode.height_ << ")\n";
    }
}

std::expected<void, std::error_code> DrmDevice::rescan() noexcept {
    connectors_.clear(); crtcs_.clear(); planes_.clear(); outputs_.clear();
    return discoverResources();
}

ConnectorInfo DrmDevice::getNativeResolution(uint32_t connector_id) noexcept {
    ConnectorInfo info{1920, 1080, 60, false};
    drmModeConnector* conn = drmModeGetConnector(fd_.get(), connector_id);
    if (!conn) return info;
    if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
        info.connected_ = true;
        info.width_ = conn->modes[0].hdisplay; info.height_ = conn->modes[0].vdisplay; info.refresh_rate_ = conn->modes[0].vrefresh;
        std::cout << "[xserver] Monitor Detected: " << info.width_ << "x" << info.height_ << "@" << info.refresh_rate_ << "Hz\n";
    }
    drmModeFreeConnector(conn);
    return info;
}

drmModeAtomicReqPtr DrmDevice::allocateAtomicReq() const noexcept { return drmModeAtomicAlloc(); }

bool DrmDevice::commitAtomic(drmModeAtomicReqPtr req, uint32_t flags, void* user_data) noexcept {
    if (!req) return false;
    int ret = drmModeAtomicCommit(fd_.get(), req, flags, user_data);
    drmModeAtomicFree(req);
    if (ret != 0) {
        std::cerr << "[xserver] Atomic commit failed (errno=" << errno << "): " << strerror(errno) << "\n";
        return false;
    }
    return true;
}

std::expected<uint32_t, std::error_code> DrmDevice::createDumbBuffer(uint32_t width, uint32_t height, uint32_t bpp, uint32_t& pitch, uint64_t& size, uint32_t& handle) noexcept {
    drm_mode_create_dumb create_req = {};
    create_req.width = width; create_req.height = height; create_req.bpp = bpp;
    if (drmIoctl(fd_.get(), DRM_IOCTL_MODE_CREATE_DUMB, &create_req) < 0) return std::unexpected(std::error_code(errno, std::generic_category()));
    pitch = create_req.pitch; size = create_req.size; handle = create_req.handle;
    return handle;
}

std::expected<uint32_t, std::error_code> DrmDevice::addFramebuffer(uint32_t width, uint32_t height, uint32_t pixel_format, uint32_t handle, uint32_t pitch) noexcept {
    uint32_t fb_id = 0;
    uint32_t handles[4] = { handle, 0, 0, 0 }, pitches[4] = { pitch, 0, 0, 0 }, offsets[4] = { 0, 0, 0, 0 };
    if (drmModeAddFB2(fd_.get(), width, height, pixel_format, handles, pitches, offsets, &fb_id, 0) != 0) return std::unexpected(std::error_code(errno, std::generic_category()));
    return fb_id;
}

std::expected<uint32_t, std::error_code> DrmDevice::importFdAndAddFb(int client_fd, uint32_t width, uint32_t height, uint32_t pixel_format, uint32_t pitch) noexcept {
    uint32_t bo_handle;
    if (drmPrimeFDToHandle(fd_.get(), client_fd, &bo_handle) != 0) return std::unexpected(std::error_code(errno, std::generic_category()));
    return addFramebuffer(width, height, pixel_format, bo_handle, pitch);
}

void DrmDevice::handleEvent() noexcept {
    drmEventContext ev{}; ev.version = 3;
    ev.page_flip_handler2 = [](int /*fd*/, unsigned int sequence, unsigned int tv_sec,
                                unsigned int tv_usec, unsigned int crtc_id, void* user_data) {
        auto* ctx = static_cast<FlipCtx*>(user_data);
        if (!ctx) return;
        if (ctx->pending_flag) *ctx->pending_flag = false;
        if (ctx->self && ctx->self->flip_callback_)
            ctx->self->flip_callback_(crtc_id, sequence, tv_sec, tv_usec);
    };
    drmHandleEvent(fd_.get(), &ev);
}

std::expected<void, std::error_code> DrmDevice::forceEnableDisplay() noexcept {
    if (outputs_.empty()) return std::unexpected(std::make_error_code(std::errc::no_such_device));

    int enabled = 0;
    for (auto& out : outputs_) {
        // Create mode blob from the connector's preferred mode.
        uint32_t mode_blob_id = 0;
        drmModeConnector* conn = drmModeGetConnector(fd_.get(), out.connector_id);
        if (conn && conn->count_modes > 0) {
            if (drmModeCreatePropertyBlob(fd_.get(), &conn->modes[0],
                                          sizeof(drmModeModeInfo), &mode_blob_id) != 0) {
                std::cerr << "[xserver] Failed to create Mode Blob for connector "
                          << out.connector_id << "\n";
            }
            drmModeFreeConnector(conn);
        }

        // Retry loop — allocate a fresh req on every attempt to avoid use-after-free.
        bool success = false;
        for (int attempt = 0; attempt < 10; ++attempt) {
            drmModeAtomicReqPtr req = allocateAtomicReq();
            if (!req) break;

            drmModeAtomicAddProperty(req, out.crtc_id,
                getPropertyId(crtcs_[out.crtc_index], "ACTIVE"), 1);
            if (mode_blob_id != 0)
                drmModeAtomicAddProperty(req, out.crtc_id,
                    getPropertyId(crtcs_[out.crtc_index], "MODE_ID"), mode_blob_id);
            // Find the DrmObject for this connector by ID
            const DrmObject* conn_obj_ptr = nullptr;
            for (const auto& c : connectors_) { if (c.id_ == out.connector_id) { conn_obj_ptr = &c; break; } }
            if (conn_obj_ptr)
                drmModeAtomicAddProperty(req, out.connector_id,
                    getPropertyId(*conn_obj_ptr, "CRTC_ID"), out.crtc_id);
            drmModeAtomicAddProperty(req, out.plane_id,
                getPropertyId(planes_[out.plane_index], "CRTC_ID"), out.crtc_id);

            // commitAtomic frees req internally — safe because we reallocate each loop
            success = commitAtomic(req, DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr);
            if (success) break;
            if (errno == EBUSY || errno == EINVAL) {
                std::cerr << "[xserver] Output " << enabled
                          << " sync backoff attempt " << (attempt + 1) << "...\n";
                usleep(250000);
            } else {
                break;
            }
        }

        if (mode_blob_id) drmModeDestroyPropertyBlob(fd_.get(), mode_blob_id);

        if (success) {
            out.active = true;
            ++enabled;
            std::cout << "[xserver] Output " << enabled - 1
                      << " (connector=" << out.connector_id << ") IGNITED.\n";
        } else {
            std::cerr << "[xserver] Failed to enable output (connector="
                      << out.connector_id << ")\n";
        }
    }

    if (enabled == 0)
        return std::unexpected(std::make_error_code(std::errc::device_or_resource_busy));

    std::cout << "[xserver] " << enabled << "/" << outputs_.size()
              << " output(s) enabled.\n";
    return {};
}

// Map a vrefresh value to the RefreshRate bit index {60→0,120→1,144→2,240→3},
// with ±1 Hz tolerance (DRM reports e.g. 59/60, 143/144); -1 if not a known step.
static int refreshBit(uint32_t hz) noexcept {
    if (hz >= 59  && hz <= 61)  return 0;
    if (hz >= 119 && hz <= 121) return 1;
    if (hz >= 143 && hz <= 145) return 2;
    if (hz >= 239 && hz <= 241) return 3;
    return -1;
}

uint32_t DrmDevice::supportedRefreshMask(size_t output_idx) noexcept {
    if (output_idx >= outputs_.size()) return 0x1;
    const auto& out = outputs_[output_idx];
    uint32_t mask = 0;
    drmModeConnector* conn = drmModeGetConnector(fd_.get(), out.connector_id);
    if (conn) {
        for (int i = 0; i < conn->count_modes; ++i) {
            // Only count modes at the current resolution.
            if (conn->modes[i].hdisplay == out.mode.width_ &&
                conn->modes[i].vdisplay == out.mode.height_) {
                int b = refreshBit(conn->modes[i].vrefresh);
                if (b >= 0) mask |= (1u << b);
            }
        }
        drmModeFreeConnector(conn);
    }
    return mask == 0 ? 0x1u : mask;   // 60 Hz floor
}

std::expected<void, std::error_code> DrmDevice::setRefreshRate(size_t output_idx, uint32_t hz) noexcept {
    if (output_idx >= outputs_.size())
        return std::unexpected(std::make_error_code(std::errc::no_such_device));
    auto& out = outputs_[output_idx];

    drmModeConnector* conn = drmModeGetConnector(fd_.get(), out.connector_id);
    if (!conn) return std::unexpected(std::make_error_code(std::errc::io_error));

    // Find a mode at the current resolution with the requested refresh (±1 Hz).
    int found = -1;
    for (int i = 0; i < conn->count_modes; ++i) {
        const auto& m = conn->modes[i];
        if (m.hdisplay == out.mode.width_ && m.vdisplay == out.mode.height_ &&
            m.vrefresh + 1 >= hz && m.vrefresh <= hz + 1) { found = i; break; }
    }
    if (found < 0) {
        drmModeFreeConnector(conn);
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }

    uint32_t blob = 0;
    if (drmModeCreatePropertyBlob(fd_.get(), &conn->modes[found],
                                  sizeof(drmModeModeInfo), &blob) != 0) {
        drmModeFreeConnector(conn);
        return std::unexpected(std::make_error_code(std::errc::io_error));
    }
    uint32_t chosen_hz = conn->modes[found].vrefresh;
    drmModeFreeConnector(conn);

    bool success = false;
    for (int attempt = 0; attempt < 5; ++attempt) {
        drmModeAtomicReqPtr req = allocateAtomicReq();
        if (!req) break;
        drmModeAtomicAddProperty(req, out.crtc_id,
            getPropertyId(crtcs_[out.crtc_index], "ACTIVE"), 1);
        drmModeAtomicAddProperty(req, out.crtc_id,
            getPropertyId(crtcs_[out.crtc_index], "MODE_ID"), blob);
        const DrmObject* conn_obj_ptr = nullptr;
        for (const auto& c : connectors_) if (c.id_ == out.connector_id) { conn_obj_ptr = &c; break; }
        if (conn_obj_ptr)
            drmModeAtomicAddProperty(req, out.connector_id,
                getPropertyId(*conn_obj_ptr, "CRTC_ID"), out.crtc_id);
        drmModeAtomicAddProperty(req, out.plane_id,
            getPropertyId(planes_[out.plane_index], "CRTC_ID"), out.crtc_id);
        success = commitAtomic(req, DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr);
        if (success) break;
        if (errno == EBUSY) usleep(100000); else break;
    }
    drmModeDestroyPropertyBlob(fd_.get(), blob);
    if (!success) return std::unexpected(std::make_error_code(std::errc::io_error));
    out.mode.refresh_rate_ = chosen_hz;
    return {};
}

std::expected<void, std::error_code> DrmDevice::setColorMatrix(size_t output_idx,
                                                               const std::array<double, 9>& m) noexcept {
    if (output_idx >= outputs_.size())
        return std::unexpected(std::make_error_code(std::errc::no_such_device));
    auto& out = outputs_[output_idx];

    uint32_t ctm_prop = getPropertyId(crtcs_[out.crtc_index], "CTM");
    if (ctm_prop == 0)   // CRTC has no colour-transform-matrix property
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));

    // DRM CTM is S31.32 sign-magnitude; our coefficients are non-negative, so the
    // sign bit stays 0 and the value is coeff × 2^32.
    struct drm_color_ctm ctm{};
    for (int i = 0; i < 9; ++i) {
        double v = m[i] < 0.0 ? 0.0 : m[i];
        ctm.matrix[i] = static_cast<uint64_t>(v * 4294967296.0);
    }
    uint32_t blob = 0;
    if (drmModeCreatePropertyBlob(fd_.get(), &ctm, sizeof(ctm), &blob) != 0)
        return std::unexpected(std::make_error_code(std::errc::io_error));

    bool success = false;
    for (int attempt = 0; attempt < 5; ++attempt) {
        drmModeAtomicReqPtr req = allocateAtomicReq();
        if (!req) break;
        drmModeAtomicAddProperty(req, out.crtc_id, ctm_prop, blob);
        success = commitAtomic(req, 0, nullptr);   // non-modeset: takes effect next vblank
        if (success) break;
        if (errno == EBUSY) usleep(50000); else break;
    }
    drmModeDestroyPropertyBlob(fd_.get(), blob);
    return success ? std::expected<void, std::error_code>{}
                   : std::unexpected(std::make_error_code(std::errc::io_error));
}

std::expected<void, std::error_code> DrmDevice::flipOutput(size_t output_idx, uint32_t fb_id, void* user_data) noexcept {
    if (output_idx >= outputs_.size()) return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    const Output& out = outputs_[output_idx];
    if (!out.active) return std::unexpected(std::make_error_code(std::errc::device_or_resource_busy));

    // Ensure the flip_contexts_ vector is large enough and populate for this output.
    if (flip_contexts_.size() <= output_idx)
        flip_contexts_.resize(output_idx + 1);
    if (!flip_contexts_[output_idx])
        flip_contexts_[output_idx] = std::make_unique<FlipCtx>();

    FlipCtx* ctx = flip_contexts_[output_idx].get();
    ctx->self         = this;
    ctx->pending_flag = static_cast<bool*>(user_data); // nullptr for secondary outputs

    const DrmObject& plane_obj = planes_[out.plane_index];
    drmModeAtomicReqPtr req = allocateAtomicReq();
    if (!req) return std::unexpected(std::make_error_code(std::errc::not_enough_memory));

    drmModeAtomicAddProperty(req, out.plane_id, getPropertyId(plane_obj, "FB_ID"),    fb_id);
    drmModeAtomicAddProperty(req, out.plane_id, getPropertyId(plane_obj, "CRTC_ID"),  out.crtc_id);
    drmModeAtomicAddProperty(req, out.plane_id, getPropertyId(plane_obj, "SRC_X"),    0);
    drmModeAtomicAddProperty(req, out.plane_id, getPropertyId(plane_obj, "SRC_Y"),    0);
    drmModeAtomicAddProperty(req, out.plane_id, getPropertyId(plane_obj, "SRC_W"),    out.mode.width_  << 16);
    drmModeAtomicAddProperty(req, out.plane_id, getPropertyId(plane_obj, "SRC_H"),    out.mode.height_ << 16);
    drmModeAtomicAddProperty(req, out.plane_id, getPropertyId(plane_obj, "CRTC_X"),   0);
    drmModeAtomicAddProperty(req, out.plane_id, getPropertyId(plane_obj, "CRTC_Y"),   0);
    drmModeAtomicAddProperty(req, out.plane_id, getPropertyId(plane_obj, "CRTC_W"),   out.mode.width_);
    drmModeAtomicAddProperty(req, out.plane_id, getPropertyId(plane_obj, "CRTC_H"),   out.mode.height_);

    if (!commitAtomic(req, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, ctx))
        return std::unexpected(std::make_error_code(std::errc::device_or_resource_busy));
    return {};
}

std::expected<void, std::error_code> DrmDevice::flip(uint32_t fb_id, void* user_data) noexcept {
    return flipOutput(0, fb_id, user_data);
}

// ─────────────────────────── Hardware Cursor ──────────────────────────────
// All cursor glyphs (including the anti-aliased arrow) live in cursor_glyphs.cc
// so they can be unit-previewed without a display; see freebsdx-cursor-preview.

std::expected<void, std::error_code> DrmDevice::initCursor(float scale) noexcept {
    // Free any previous cursor buffer before re-initialising (called on hot-plug).
    if (cursor_.mapped && cursor_.mapped != MAP_FAILED) {
        munmap(cursor_.mapped, kCursorSize * cursor_.pitch);
        cursor_.mapped = nullptr;
    }
    if (cursor_.gem_handle) {
        struct drm_mode_destroy_dumb dreq{};
        dreq.handle = cursor_.gem_handle;
        drmIoctl(fd_.get(), DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        cursor_.gem_handle = 0;
    }

    // Allocate 64×64 ARGB8888 dumb buffer (hardware cursors must be power-of-two).
    struct drm_mode_create_dumb creq{};
    creq.width = kCursorSize; creq.height = kCursorSize; creq.bpp = 32;
    if (drmIoctl(fd_.get(), DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0)
        return std::unexpected(std::error_code(errno, std::generic_category()));

    cursor_.gem_handle = creq.handle;
    cursor_.pitch      = creq.pitch;

    // Map for CPU writes.
    struct drm_mode_map_dumb mreq{};
    mreq.handle = cursor_.gem_handle;
    if (drmIoctl(fd_.get(), DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0)
        return std::unexpected(std::error_code(errno, std::generic_category()));

    cursor_.mapped = mmap(nullptr, kCursorSize * cursor_.pitch,
                          PROT_READ | PROT_WRITE, MAP_SHARED, fd_.get(), mreq.offset);
    if (cursor_.mapped == MAP_FAILED) {
        cursor_.mapped = nullptr;
        return std::unexpected(std::error_code(errno, std::generic_category()));
    }

    // Render the anti-aliased pointer arrow, scaled to the display's logical→
    // physical pixel ratio (base ~27px tall at 1×; clamp so it fits 64×64).
    scale = std::clamp(scale, 0.5f, 2.0f);

    auto* pixels = static_cast<uint32_t*>(cursor_.mapped);
    std::memset(pixels, 0, kCursorSize * cursor_.pitch);
    const uint32_t stride = cursor_.pitch / sizeof(uint32_t);

    int32_t hot_px = 0, hot_py = 0;
    DrawArrow(pixels, stride, kCursorSize, scale, hot_px, hot_py);

    cursor_.hot_x = hot_px;
    cursor_.hot_y = hot_py;

    // Upload cursor image to every active CRTC.
    for (const auto& out : outputs_) {
        if (!out.active) continue;
        if (drmModeSetCursor2(fd_.get(), out.crtc_id,
                              cursor_.gem_handle, kCursorSize, kCursorSize,
                              hot_px, hot_py) != 0) {
            std::cerr << "[drm] drmModeSetCursor2 failed on crtc "
                      << out.crtc_id << ": " << strerror(errno) << "\n";
        }
    }
    std::cout << "[drm] Hardware cursor initialized (" << kCursorSize
              << "×" << kCursorSize << " dumb buffer, scale=" << scale << ").\n";
    return {};
}

void DrmDevice::moveCursor(size_t output_idx, int32_t x, int32_t y) noexcept {
    if (output_idx >= outputs_.size() || !outputs_[output_idx].active) return;
    drmModeMoveCursor(fd_.get(), outputs_[output_idx].crtc_id, x, y);
}

void DrmDevice::hideCursor(size_t output_idx) noexcept {
    if (output_idx >= outputs_.size()) return;
    drmModeSetCursor2(fd_.get(), outputs_[output_idx].crtc_id, 0, 0, 0, 0, 0);
}

void DrmDevice::showCursor(size_t output_idx) noexcept {
    if (output_idx >= outputs_.size() || !outputs_[output_idx].active) return;
    if (!cursor_.gem_handle) return;
    drmModeSetCursor2(fd_.get(), outputs_[output_idx].crtc_id,
                      cursor_.gem_handle, kCursorSize, kCursorSize,
                      cursor_.hot_x, cursor_.hot_y);
}

// ─────────────────────────── VRR / Adaptive Sync ─────────────────────────────

bool DrmDevice::isVrrCapable(size_t output_idx) const noexcept {
    if (output_idx >= outputs_.size()) return false;
    const Output& out = outputs_[output_idx];
    if (out.crtc_index >= crtcs_.size()) return false;
    // The kernel exposes VRR_ENABLED as a CRTC property only when the
    // connector and encoder support adaptive sync.
    return crtcs_[out.crtc_index].props_.count("VRR_ENABLED") > 0;
}

std::expected<void, std::error_code> DrmDevice::setVrrEnabled(size_t output_idx, bool enable) noexcept {
    if (output_idx >= outputs_.size())
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));

    Output& out = outputs_[output_idx];
    if (!out.active)
        return std::unexpected(std::make_error_code(std::errc::device_or_resource_busy));
    if (out.crtc_index >= crtcs_.size())
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));

    uint32_t vrr_prop = getPropertyId(crtcs_[out.crtc_index], "VRR_ENABLED");
    if (vrr_prop == 0) {
        std::cerr << "[drm] VRR_ENABLED property not found on CRTC "
                  << out.crtc_id << " — output does not support VRR\n";
        return std::unexpected(std::make_error_code(std::errc::not_supported));
    }

    drmModeAtomicReqPtr req = allocateAtomicReq();
    if (!req) return std::unexpected(std::make_error_code(std::errc::not_enough_memory));

    drmModeAtomicAddProperty(req, out.crtc_id, vrr_prop, enable ? 1 : 0);

    // Non-modeset commit — no screen blank, takes effect at next vblank.
    if (!commitAtomic(req, DRM_MODE_ATOMIC_NONBLOCK, nullptr))
        return std::unexpected(std::error_code(errno, std::generic_category()));

    out.vrr_enabled = enable;
    std::cout << "[drm] VRR " << (enable ? "enabled" : "disabled")
              << " on output " << output_idx
              << " (CRTC " << out.crtc_id << ")\n";
    return {};
}

// ─────────────────────────────────── HDR Output ──────────────────────────────

bool DrmDevice::isHdrCapable(size_t output_idx) const noexcept {
    if (output_idx >= outputs_.size()) return false;
    const Output& out = outputs_[output_idx];
    // HDR_OUTPUT_METADATA is a CONNECTOR property (not CRTC like VRR_ENABLED).
    for (const auto& conn : connectors_) {
        if (conn.id_ == out.connector_id)
            return conn.props_.count("HDR_OUTPUT_METADATA") > 0;
    }
    return false;
}

std::expected<void, std::error_code>
DrmDevice::setHdrEnabled(size_t output_idx, bool enable, uint32_t max_luminance_nits) noexcept {
    if (output_idx >= outputs_.size())
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));

    Output& out = outputs_[output_idx];
    if (!out.active)
        return std::unexpected(std::make_error_code(std::errc::device_or_resource_busy));

    // Find the connector DrmObject for this output.
    DrmObject* conn_obj = nullptr;
    for (auto& conn : connectors_) {
        if (conn.id_ == out.connector_id) { conn_obj = &conn; break; }
    }
    if (!conn_obj)
        return std::unexpected(std::make_error_code(std::errc::no_such_device));

    uint32_t hdr_prop = getPropertyId(*conn_obj, "HDR_OUTPUT_METADATA");
    if (hdr_prop == 0) {
        std::cerr << "[drm] HDR_OUTPUT_METADATA property not found on connector "
                  << out.connector_id << " — output does not support HDR\n";
        return std::unexpected(std::make_error_code(std::errc::not_supported));
    }

    // Destroy any existing metadata blob before creating a new one.
    if (out.hdr_metadata_blob_id != 0) {
        drmModeDestroyPropertyBlob(fd_.get(), out.hdr_metadata_blob_id);
        out.hdr_metadata_blob_id = 0;
    }

    drmModeAtomicReqPtr req = allocateAtomicReq();
    if (!req) return std::unexpected(std::make_error_code(std::errc::not_enough_memory));

    if (enable) {
        uint32_t nits = (max_luminance_nits == 0) ? 1000u : max_luminance_nits;

        // Build HDR10 static metadata (SMPTE ST 2084 / PQ, BT.2020 primaries, D65 white).
        hdr_output_metadata meta{};
        meta.metadata_type = 1; // HDR_STATIC_METADATA_TYPE1
        auto& info = meta.hdmi_metadata_type1;
        info.eotf          = 2; // SMPTE ST 2084 (Perceptual Quantiser)
        info.metadata_type = 0; // Static Metadata Descriptor Type 1

        // BT.2020 color primaries in units of 0.00002 (1.0 = 50000).
        info.display_primaries[0] = { 35400, 14600 }; // Red
        info.display_primaries[1] = {  8500, 39850 }; // Green
        info.display_primaries[2] = {  6550,  2300 }; // Blue
        info.white_point          = { 15635, 16450 }; // D65

        // Luminance values.  max in cd/m², min in 0.0001 cd/m² units.
        info.max_display_mastering_luminance =
            static_cast<uint16_t>(std::min<uint32_t>(nits, 65535u));
        info.min_display_mastering_luminance = 50;  // 0.005 cd/m²
        info.max_cll  = static_cast<uint16_t>(std::min<uint32_t>(nits, 65535u));
        info.max_fall = static_cast<uint16_t>(std::min<uint32_t>(nits / 4, 65535u));

        uint32_t blob_id = 0;
        if (drmModeCreatePropertyBlob(fd_.get(), &meta, sizeof(meta), &blob_id) != 0) {
            drmModeAtomicFree(req);
            return std::unexpected(std::error_code(errno, std::generic_category()));
        }
        out.hdr_metadata_blob_id = blob_id;
        drmModeAtomicAddProperty(req, out.connector_id, hdr_prop, blob_id);
    } else {
        // Disable: clear the property (pass 0).
        drmModeAtomicAddProperty(req, out.connector_id, hdr_prop, 0);
    }

    // Non-modeset commit — connector property update takes effect at next vblank.
    if (!commitAtomic(req, DRM_MODE_ATOMIC_NONBLOCK, nullptr))
        return std::unexpected(std::error_code(errno, std::generic_category()));

    out.hdr_enabled = enable;
    std::cout << "[drm] HDR " << (enable ? "enabled" : "disabled")
              << " on output " << output_idx
              << " (connector " << out.connector_id << ", " << max_luminance_nits << " nits)\n";
    return {};
}

void DrmDevice::hideCursorAll() noexcept {
    for (size_t i = 0; i < outputs_.size(); ++i) hideCursor(i);
}

void DrmDevice::showCursorAll() noexcept {
    for (size_t i = 0; i < outputs_.size(); ++i) showCursor(i);
}

void DrmDevice::setCursorShape(uint32_t shape, float scale) noexcept {
    if (!cursor_.mapped || cursor_.mapped == MAP_FAILED) return;

    scale = std::clamp(scale, 0.5f, 2.0f);

    auto*    pixels = static_cast<uint32_t*>(cursor_.mapped);
    uint32_t stride = cursor_.pitch / sizeof(uint32_t);
    std::memset(pixels, 0, kCursorSize * cursor_.pitch);

    // Block glyphs draw on a 20x20 logical grid scaled up by `sc`; the arrow
    // renders anti-aliased at the raw float `scale`. All glyphs live in
    // cursor_glyphs.cc — this is purely a dispatch table.
    int sc = static_cast<int>(std::round(scale));
    if (sc < 1) sc = 1;

    int32_t hot_x = 0, hot_y = 0;

    switch (shape) {
    case 9: // text — vertical I-beam
        DrawText(pixels, stride, kCursorSize, sc, hot_x, hot_y);
        break;
    case 10: // vertical_text — horizontal I-beam
        DrawVerticalText(pixels, stride, kCursorSize, sc, hot_x, hot_y);
        break;
    case 8: // crosshair
        DrawCrosshair(pixels, stride, kCursorSize, sc, hot_x, hot_y);
        break;
    case 5: // progress (arrow + busy hourglass)
        DrawProgress(pixels, stride, kCursorSize, sc, hot_x, hot_y);
        break;
    case 6: // wait (hourglass)
        DrawWait(pixels, stride, kCursorSize, sc, hot_x, hot_y);
        break;
    case 2: // context_menu
        DrawContextMenu(pixels, stride, kCursorSize, sc, hot_x, hot_y);
        break;
    case 3: // help
        DrawHelp(pixels, stride, kCursorSize, sc, hot_x, hot_y);
        break;
    case 7: // cell (thick plus)
        DrawCell(pixels, stride, kCursorSize, sc, hot_x, hot_y);
        break;
    case 32: // all_scroll (four-way scroll)
        DrawAllScroll(pixels, stride, kCursorSize, sc, hot_x, hot_y);
        break;
    case 11: // alias (link)
        DrawAlias(pixels, stride, kCursorSize, sc, hot_x, hot_y);
        break;
    case 12: // copy
        DrawCopy(pixels, stride, kCursorSize, sc, hot_x, hot_y);
        break;
    case 13: // move (four-way fleur)
        DrawMove(pixels, stride, kCursorSize, sc, hot_x, hot_y);
        break;
    case 14: // no_drop
        DrawNoDrop(pixels, stride, kCursorSize, sc, hot_x, hot_y);
        break;
    case 15: // not_allowed
        DrawNotAllowed(pixels, stride, kCursorSize, sc, hot_x, hot_y);
        break;
    case 16: // grab (open hand)
        DrawGrab(pixels, stride, kCursorSize, sc, hot_x, hot_y);
        break;
    case 17: // grabbing (closed fist)
        DrawGrabbing(pixels, stride, kCursorSize, sc, hot_x, hot_y);
        break;
    case 33: // zoom_in (magnifier with +)
        DrawZoom(pixels, stride, kCursorSize, sc, /*zoom_in=*/true, hot_x, hot_y);
        break;
    case 34: // zoom_out (magnifier with -)
        DrawZoom(pixels, stride, kCursorSize, sc, /*zoom_in=*/false, hot_x, hot_y);
        break;
    case 20: case 24: case 28: // ne_resize, sw_resize, nesw_resize
    case 21: case 23: case 29: { // nw_resize, se_resize, nwse_resize — diagonal
        // NE↔SW for the ne/sw/nesw group, NW↔SE for the nw/se/nwse group.
        bool ne_sw = (shape == 20 || shape == 24 || shape == 28);
        DrawDiagonalResize(pixels, stride, kCursorSize, sc, ne_sw, hot_x, hot_y);
        break;
    }
    case 18: case 25: case 26: case 30: // H-resize (←→): e/w/ew/col_resize
        DrawHResize(pixels, stride, kCursorSize, sc, hot_x, hot_y);
        break;
    case 19: case 22: case 27: case 31: // V-resize (↑↓): n/s/ns/row_resize
        DrawVResize(pixels, stride, kCursorSize, sc, hot_x, hot_y);
        break;
    default: // arrow — anti-aliased, floating-point scale
        DrawArrow(pixels, stride, kCursorSize, scale, hot_x, hot_y);
        break;
    }

    cursor_.hot_x = hot_x;
    cursor_.hot_y = hot_y;

    for (const auto& out : outputs_) {
        if (!out.active) continue;
        drmModeSetCursor2(fd_.get(), out.crtc_id,
                          cursor_.gem_handle, kCursorSize, kCursorSize,
                          hot_x, hot_y);
    }
}

} // namespace fbx::xserver

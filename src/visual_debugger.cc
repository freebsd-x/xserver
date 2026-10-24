#include <iostream>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <span>
#include <cstring>

struct DumbBuffer {
    int drm_fd_{-1};
    uint32_t handle_{0}, fb_id_{0}, stride_{0};
    uint64_t size_{0};
    std::span<uint8_t> mapped_data_;

    DumbBuffer(int fd, uint32_t w, uint32_t h) : drm_fd_(fd) {
        struct drm_mode_create_dumb create_req = {};
        create_req.height = h;
        create_req.width = w;
        create_req.bpp = 32;

        if (drmIoctl(drm_fd_, DRM_IOCTL_MODE_CREATE_DUMB, &create_req) < 0) return;
        handle_ = create_req.handle; stride_ = create_req.pitch; size_ = create_req.size;
        if (drmModeAddFB(drm_fd_, w, h, 24, 32, stride_, handle_, &fb_id_) < 0) return;
        struct drm_mode_map_dumb map_req = { .handle = handle_ };
        if (drmIoctl(drm_fd_, DRM_IOCTL_MODE_MAP_DUMB, &map_req) < 0) return;
        void* map = mmap(0, size_, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd_, map_req.offset);
        if (map != MAP_FAILED) mapped_data_ = std::span<uint8_t>(static_cast<uint8_t*>(map), size_);
    }
    bool isValid() const { return mapped_data_.data() != nullptr; }
    ~DumbBuffer() {
        if (mapped_data_.data()) {
            munmap(mapped_data_.data(), size_);
        }
        // LEAK INTENTIONALLY: Do not call drmModeRmFB or DESTROY_DUMB here.
        // On some FreeBSD DRM drivers, removing the FB while the hardware is transitioning
        // states can trigger a kernel page fault. The kernel will safely clean up these 
        // resources when the process exits or the DRM FD is closed.
    }
};

int main() {
    std::cout << "[DirectProbe] Opening /dev/dri/card0 directly...\n";
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        std::cerr << "FATAL: Could not open GPU. Are you running with sudo?\n";
        return 1;
    }

    if (drmSetMaster(fd) != 0) {
        std::cerr << "WARNING: Could not acquire DRM Master. Are you running inside an X11/Wayland desktop?\n";
        std::cerr << "You MUST switch to a raw TTY (Ctrl+Alt+F2) for this to work.\n";
        close(fd);
        return 1;
    }

    drmModeRes* res = drmModeGetResources(fd);
    if (!res) {
        std::cerr << "FATAL: Kernel rejected DRM query: " << strerror(errno) << "\n";
        drmDropMaster(fd);
        close(fd);
        return 1;
    }

    uint32_t crtc_id = 0;
    drmModeConnector* conn = nullptr;
    drmModeModeInfo mode;

    for (int i = 0; i < res->count_connectors; i++) {
        conn = drmModeGetConnector(fd, res->connectors[i]);
        if (conn && conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
            mode = conn->modes[0];
            auto* encoder = drmModeGetEncoder(fd, conn->encoder_id);
            if (encoder) {
                crtc_id = encoder->crtc_id;
                drmModeFreeEncoder(encoder);
                break;
            }
        }
        if (conn) drmModeFreeConnector(conn);
    }

    if (!crtc_id) {
        std::cerr << "FATAL: No monitor connected.\n";
        drmModeFreeResources(res);
        drmDropMaster(fd);
        close(fd);
        return 1;
    }

    std::cout << "[DirectProbe] Drawing to " << mode.hdisplay << "x" << mode.vdisplay << "\n";
    
    // Save original state for restoration
    drmModeCrtc* original_crtc = drmModeGetCrtc(fd, crtc_id);

    {
        DumbBuffer buf1(fd, mode.hdisplay, mode.vdisplay);
        DumbBuffer buf2(fd, mode.hdisplay, mode.vdisplay);

        if (!buf1.isValid() || !buf2.isValid()) {
            std::cerr << "FATAL: VRAM Allocation failed.\n";
            if (original_crtc) drmModeFreeCrtc(original_crtc);
            drmModeFreeResources(res);
            drmDropMaster(fd);
            close(fd);
            return 1;
        }

        uint32_t colors[] = { 0x00FF0000, 0x0000FF00, 0x000000FF };
        int current_buffer = 0;

        for (int frame = 0; frame < 300; ++frame) {
            DumbBuffer& back = (current_buffer == 0) ? buf1 : buf2;
            uint32_t color = colors[(frame / 60) % 3];
            uint32_t* pixels = reinterpret_cast<uint32_t*>(back.mapped_data_.data());
            for (size_t i = 0; i < (back.size_ / 4); ++i) pixels[i] = color;

            if (drmModeSetCrtc(fd, crtc_id, back.fb_id_, 0, 0, &conn->connector_id, 1, &mode) != 0) {
                std::cerr << "CRITICAL: Page Flip Failed.\n";
                break;
            }
            current_buffer = 1 - current_buffer;
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        
        std::cout << "[DirectProbe] Loop complete. Restoring original CRTC state...\n";
        if (original_crtc) {
            drmModeSetCrtc(fd, original_crtc->crtc_id, original_crtc->buffer_id,
                           original_crtc->x, original_crtc->y,
                           &conn->connector_id, 1, &original_crtc->mode);
            drmModeFreeCrtc(original_crtc);
            original_crtc = nullptr;
        }
        
        // WAIT FOR VBLANK: Ensure kernel has finished the switch before we continue
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } 

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    drmDropMaster(fd);
    close(fd);
    drmModeFreeResources(res);
    std::cout << "[DirectProbe] Success. Hardware released safely.\n";
    return 0;
}

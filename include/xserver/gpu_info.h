#pragma once

#include <string>
#include <string_view>

// Pure parser for pciconf(8) output (com.freebsdx.GpuMonitor). Extracts the
// display/VGA device's human name. Utilisation / VRAM / temperature / fan are
// not exposed for an Intel integrated GPU on FreeBSD (no intel_gpu_top, iGPU
// shares system RAM + the CPU die), so the service reports those as 0/unknown.
//
//   vgapci0@pci0:0:2:0:  class=0x030000 ... device=0x5917 ...
//       vendor     = 'Intel Corporation'
//       device     = 'Kaby Lake-R GT2 [UHD Graphics 620]'
//       class      = display
namespace fbx::xserver::gpu {

// Device name of the first display-class entry in `pciconf -lv`; "" if none.
inline std::string parseGpuName(std::string_view out) {
    std::string cur_name;          // "device = '...'" seen in the current block
    size_t start = 0;
    while (true) {
        size_t nl = out.find('\n', start);
        std::string_view line = out.substr(start, (nl == std::string_view::npos ? out.size() : nl) - start);

        bool indented = !line.empty() && (line[0] == ' ' || line[0] == '\t');
        if (!indented) {
            // New device block header (e.g. "vgapci0@pci0:0:2:0: ..."): reset.
            cur_name.clear();
        } else {
            // Indented "device = 'Name'" (quoted → the human name, not the header's device=0x…).
            if (line.find("device") != std::string_view::npos) {
                size_t q1 = line.find('\'');
                size_t q2 = (q1 == std::string_view::npos) ? std::string_view::npos : line.find('\'', q1 + 1);
                if (q1 != std::string_view::npos && q2 != std::string_view::npos && q2 > q1)
                    cur_name = std::string(line.substr(q1 + 1, q2 - q1 - 1));
            }
            // "class = display" closes the match for this block.
            if (line.find("class") != std::string_view::npos &&
                line.find("display") != std::string_view::npos && !cur_name.empty())
                return cur_name;
        }
        if (nl == std::string_view::npos) break;
        start = nl + 1;
    }
    return {};
}

}  // namespace fbx::xserver::gpu

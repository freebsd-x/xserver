#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <cctype>

// Pure parser for usbconfig(8) output, shared by the three USB enumeration
// services (com.freebsdx.Usb / .UsbDevices / .UsbDevice). Dependency-free and
// unit-testable; the services run usbconfig via posix_spawn and feed the text
// here. Two inputs:
//   • `usbconfig list`              → one line per device (bus.addr + product)
//   • `usbconfig -d ugenB.A dump_device_desc` → vid/pid/class/manufacturer
namespace fbx::xserver::usb {

struct UsbInfo {
    uint32_t    bus = 0;
    uint32_t    addr = 0;         // device address within the bus
    std::string product;         // product string
    std::string manufacturer;    // manufacturer string (dump only)
    std::string cls;             // "HUB"/"HID"/"MSC"/"AUD"/"CDC"/"other"
    uint32_t    vid = 0;
    uint32_t    pid = 0;
    uint8_t     device_class = 0; // USB bDeviceClass byte
};

// Map a bDeviceClass byte (0 → derive from the product name) to a short class.
inline std::string classString(uint8_t dc, std::string_view product = {}) {
    switch (dc) {
        case 0x09: return "HUB";
        case 0x03: return "HID";
        case 0x08: return "MSC";
        case 0x01: return "AUD";
        case 0x02: case 0x0a: return "CDC";
        default: break;
    }
    std::string p;
    for (char c : product) p += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto has = [&](const char* s) { return p.find(s) != std::string::npos; };
    if (has("hub")) return "HUB";
    if (has("keyboard") || has("mouse") || has("receiver") || has("trackpad")) return "HID";
    if (has("audio") || has("headset") || has("microphone") || has("speaker")) return "AUD";
    if (has("disk") || has("storage") || has("flash")) return "MSC";
    return "other";
}

namespace detail {
inline void forEachLine(std::string_view text, auto&& fn) {
    size_t start = 0;
    while (true) {
        size_t nl = text.find('\n', start);
        fn(text.substr(start, (nl == std::string_view::npos ? text.size() : nl) - start));
        if (nl == std::string_view::npos) break;
        start = nl + 1;
    }
}
// "0x1a2b" after '=' → integer (0 if absent).
inline uint32_t hexField(std::string_view line) {
    size_t eq = line.find('=');
    size_t x  = (eq == std::string_view::npos) ? std::string_view::npos : line.find("0x", eq);
    if (x == std::string_view::npos) return 0;
    uint32_t v = 0;
    for (size_t i = x + 2; i < line.size(); ++i) {
        char c = line[i]; int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = v * 16 + static_cast<uint32_t>(d);
    }
    return v;
}
// The "<...>" string on a line (e.g. iProduct = 0x02 <USB Receiver>).
inline std::string angleString(std::string_view line) {
    size_t lt = line.find('<');
    size_t gt = (lt == std::string_view::npos) ? std::string_view::npos : line.find('>', lt);
    if (lt != std::string_view::npos && gt != std::string_view::npos && gt > lt)
        return std::string(line.substr(lt + 1, gt - lt - 1));
    return {};
}
}  // namespace detail

// Parse one `usbconfig list` line: "ugen0.2: <Product> at usbus0, ...".
// Fills bus/addr/product; returns false for non-device lines.
inline bool parseListLine(std::string_view line, uint32_t& bus, uint32_t& addr,
                          std::string& product) {
    size_t p = line.find("ugen");
    if (p == std::string_view::npos) return false;
    size_t i = p + 4;
    uint32_t b = 0; bool anyb = false;
    while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) {
        b = b * 10 + static_cast<uint32_t>(line[i] - '0'); ++i; anyb = true;
    }
    if (!anyb || i >= line.size() || line[i] != '.') return false;
    ++i;
    uint32_t a = 0; bool anya = false;
    while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) {
        a = a * 10 + static_cast<uint32_t>(line[i] - '0'); ++i; anya = true;
    }
    if (!anya) return false;
    product = detail::angleString(line.substr(i));
    bus = b; addr = a;
    return true;
}

// Parse all device lines from `usbconfig list`.
inline std::vector<UsbInfo> parseList(std::string_view out) {
    std::vector<UsbInfo> v;
    detail::forEachLine(out, [&](std::string_view line) {
        uint32_t b, a; std::string prod;
        if (parseListLine(line, b, a, prod)) {
            UsbInfo u; u.bus = b; u.addr = a; u.product = prod;
            v.push_back(std::move(u));
        }
    });
    return v;
}

// Merge `usbconfig ... dump_device_desc` fields into an existing UsbInfo and set
// its class string.
inline void parseDeviceDesc(std::string_view out, UsbInfo& u) {
    detail::forEachLine(out, [&](std::string_view line) {
        if      (line.find("idVendor")      != std::string_view::npos) u.vid = detail::hexField(line);
        else if (line.find("idProduct")     != std::string_view::npos) u.pid = detail::hexField(line);
        else if (line.find("bDeviceClass")  != std::string_view::npos) u.device_class = static_cast<uint8_t>(detail::hexField(line));
        else if (line.find("iManufacturer") != std::string_view::npos) { auto s = detail::angleString(line); if (!s.empty()) u.manufacturer = s; }
        else if (line.find("iProduct")      != std::string_view::npos) { auto s = detail::angleString(line); if (!s.empty()) u.product = s; }
    });
    u.cls = classString(u.device_class, u.product);
}

}  // namespace fbx::xserver::usb

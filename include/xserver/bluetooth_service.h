#pragma once

#include "shared/protocol.h"
#include <memory>
#include <vector>
#include <string>
#include <mutex>
#include <atomic>

namespace fbx::xserver {

class IpcSocket;

// com.freebsdx.Bluetooth — adapter management and device discovery/pairing
// using FreeBSD hccontrol(8), hcsecd(8), and bthidd(8) CLI tools.
//
// Scan runs hccontrol inquiry in a background thread; discovered devices
// are broadcast as DeviceFound messages. Known/paired devices are
// enumerated from `hccontrol read_stored_link_key` and `bthidcontrol list`.
class BluetoothService {
public:
    static void ProcessMessage(std::shared_ptr<IpcSocket> client,
                               const shared::XBusMessage& msg) noexcept;

private:
    // CLI helpers (popen-based, same pattern as PrintService)
    static std::string runCmd(const std::string& cmd) noexcept;
    static bool adapterPresent() noexcept;
    static bool adapterEnabled() noexcept;
    static void setEnabled(bool on) noexcept;

    static shared::BluetoothState getState() noexcept;
    static std::vector<shared::BluetoothDevice> listDevices() noexcept;

    // Background inquiry: spawns hccontrol inquiry, parses results,
    // broadcasts DeviceFound for each discovered device.
    static void runScan() noexcept;

    static std::atomic<bool> scanning_;   // set by the detached runScan() thread
    static std::vector<shared::BluetoothDevice> known_devices_;
    // Guards known_devices_ (mutated by runScan(), read by listDevices()).
    static std::mutex devices_mutex_;
};

} // namespace fbx::xserver

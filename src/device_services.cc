#include "xserver/device_services.h"
#include "xserver/config_service.h"
#include "xserver/xbus_reply.h"
#include "shared/logger.h"

#include <cups/cups.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace fbx::xserver {

namespace {

using namespace svc;

// ── PrintQueue ───────────────────────────────────────────────────────────────

struct PrintJob {
    std::string printer, title;
    uint32_t    id = 0;
    uint32_t    status = 0;   // 0=pending, 1=held, 2=processing, 3=stopped
};

uint32_t jobStatus(ipp_jstate_t s) {
    switch (s) {
    case IPP_JSTATE_HELD:       return 1;
    case IPP_JSTATE_PROCESSING: return 2;
    case IPP_JSTATE_STOPPED:    return 3;
    default:                    return 0;
    }
}

// Active (not completed) jobs on every queue. Talks to the local cupsd over
// its socket; when cupsd isn't running the connect fails at once → empty.
std::vector<PrintJob> activeJobs() {
    std::vector<PrintJob> out;
    cups_job_t* jobs = nullptr;
    int n = cupsGetJobs2(CUPS_HTTP_DEFAULT, &jobs, nullptr, 0, CUPS_WHICHJOBS_ACTIVE);
    for (int i = 0; i < n; ++i)
        out.push_back({jobs[i].dest ? jobs[i].dest : "", jobs[i].title ? jobs[i].title : "",
                       static_cast<uint32_t>(jobs[i].id), jobStatus(jobs[i].state)});
    if (jobs) cupsFreeJobs(n, jobs);
    return out;
}

// ── InputDevice ──────────────────────────────────────────────────────────────

constexpr const char* kOverridesKey = "input.device_overrides";

InputDeviceService::Lister g_list;
InputDeviceService::Setter g_set;

}  // namespace

// ── PrintQueueService ────────────────────────────────────────────────────────

void PrintQueueService::handleMessage(const shared::XBusMessage& msg,
                                      std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::PrintOpcode;
    constexpr const char* kName = "com.freebsdx.PrintQueue";
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListJobs: {
        auto jobs = activeJobs();
        auto pg = makePage<shared::PrintJobPage>(readPage(msg), jobs,
            [](shared::PrintJobEntry& e, const PrintJob& j) {
                std::strncpy(e.printer_, j.printer.c_str(), sizeof(e.printer_) - 1);
                std::strncpy(e.document_, j.title.c_str(), sizeof(e.document_) - 1);
                e.job_id_ = j.id;
                e.pages_  = 0;          // cups_job_t has no page count
                e.status_ = j.status;
            });
        reply(msg, client, kName, msg.opcode_, &pg);
        break;
    }
    case O::CancelJob: {
        shared::PrintCancelRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        bool ok = req.job_id_ != 0 && req.job_id_ <= 0x7fffffffu &&
                  cupsCancelJob2(CUPS_HTTP_DEFAULT, nullptr, static_cast<int>(req.job_id_), 0) == IPP_STATUS_OK;
        if (ok) LOG_INFO("PrintQueueService: cancelled job {}", req.job_id_);
        else    LOG_WARN("PrintQueueService: cancel job {} failed: {}", req.job_id_, cupsLastErrorString());
        ack(msg, client, kName);
        if (ok) broadcast<char>(kName, static_cast<uint32_t>(O::QueueChanged), nullptr);
        break;
    }
    default: break;
    }
}

// ── InputDeviceService ───────────────────────────────────────────────────────

void InputDeviceService::init(Lister list, Setter set) noexcept {
    g_list = std::move(list);
    g_set  = std::move(set);
    if (!g_set) return;
    for (const auto& [id, o] : inputdev::parseOverrides(ConfigService::get(kOverridesKey)))
        g_set(id, o.sensitivity, o.adaptive);
}

void InputDeviceService::notifyDevicesChanged() noexcept {
    broadcast<char>("com.freebsdx.InputDevice",
                    static_cast<uint32_t>(shared::InputDeviceOpcode::DeviceChanged), nullptr);
}

void InputDeviceService::handleMessage(const shared::XBusMessage& msg,
                                       std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::InputDeviceOpcode;
    constexpr const char* kName = "com.freebsdx.InputDevice";
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListDevices: {
        // One device per page (InputDevicePage has no entries_ array).
        auto devs = g_list ? g_list() : std::vector<inputdev::DeviceInfo>{};
        uint32_t page = readPage(msg);
        shared::InputDevicePage pg{};
        pg.page_  = page;
        pg.total_ = static_cast<uint32_t>(devs.size());
        if (page < devs.size()) {
            const auto& d = devs[page];
            pg.valid_         = 1;
            pg.has_more_      = page + 1 < devs.size() ? 1u : 0u;
            pg.device_id_     = d.id;
            pg.type_          = d.type;
            pg.sensitivity_   = d.sensitivity;
            pg.accel_profile_ = d.adaptive ? 1u : 0u;
            std::strncpy(pg.name_, d.name.c_str(), sizeof(pg.name_) - 1);
            std::snprintf(pg.vendor_, sizeof(pg.vendor_), "%04x:%04x", d.vendor, d.product);
        }
        reply(msg, client, kName, msg.opcode_, &pg);
        break;
    }
    case O::SetSensitivity: {
        shared::InputSensitivityRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        bool known = false;
        if (g_list)
            for (const auto& d : g_list()) known = known || d.id == req.device_id_;
        if (known && req.sensitivity_ <= 100) {
            auto ov = inputdev::parseOverrides(ConfigService::get(kOverridesKey));
            ov[req.device_id_] = {req.sensitivity_, req.accel_profile_ != 0};
            ConfigService::set(kOverridesKey, inputdev::formatOverrides(ov));
            if (g_set) g_set(req.device_id_, req.sensitivity_, req.accel_profile_ != 0);
        } else {
            LOG_WARN("InputDeviceService: ignoring speed {} for device {:#x}",
                     req.sensitivity_, req.device_id_);
        }
        // No broadcast: the shell's slider sends this on every drag step, and
        // DeviceChanged would rebuild the slider under the pointer.
        ack(msg, client, kName);
        break;
    }
    default: break;
    }
}

}  // namespace fbx::xserver

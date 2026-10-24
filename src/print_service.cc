#include "xserver/print_service.h"
#include "xserver/ipc_socket.h"
#include "xserver/shell_util.h"
#include "xserver/async_shell.h"
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace fbx::xserver {

// ─── Helpers ──────────────────────────────────────────────────────────────────

std::string PrintService::runCmd(const std::string& cmd) noexcept {
    // Async + cached: lpstat/lpc hang when the print system (CUPS/lpd) is
    // unresponsive, and this runs on the compositor's single event-loop thread —
    // a hang there freezes the whole desktop. Return cached output, refresh in
    // the background.
    return g_asyncShell.get(cmd);
}

bool PrintService::pathAllowed(const std::string& path) noexcept {
    if (path.empty()) return false;
    // Allow /tmp and anything under it
    if (path.compare(0, 5, "/tmp/") == 0 || path == "/tmp") return true;
    // Allow $HOME and all subdirectories
    const char* home = std::getenv("HOME");
    if (home) {
        size_t hlen = std::strlen(home);
        if (path.size() >= hlen &&
            path.compare(0, hlen, home) == 0 &&
            (path.size() == hlen || path[hlen] == '/')) {
            return true;
        }
    }
    return false;
}

std::string PrintService::defaultPrinter() noexcept {
    std::string out = runCmd("lpstat -d 2>/dev/null");
    // Expected: "system default destination: <name>"
    const char* prefix = "system default destination: ";
    auto pos = out.find(prefix);
    if (pos == std::string::npos) return {};
    std::string name = out.substr(pos + std::strlen(prefix));
    // Strip trailing whitespace / newline
    while (!name.empty() && (name.back() == '\n' || name.back() == '\r' || name.back() == ' '))
        name.pop_back();
    return name;
}

std::vector<PrintService::ParsedPrinter> PrintService::parsePrinters() noexcept {
    std::vector<ParsedPrinter> printers;

    std::string out = runCmd("lpstat -p -l 2>/dev/null");
    if (out.empty()) return printers;

    std::string def = defaultPrinter();

    // Parse line by line
    ParsedPrinter current;
    bool in_printer = false;

    std::istringstream ss(out);
    std::string line;
    while (std::getline(ss, line)) {
        // Lines like:
        //   "printer <name> is idle."
        //   "printer <name> is now printing <name>-N."
        //   "printer <name> disabled since ..."
        if (line.compare(0, 8, "printer ") == 0) {
            // Commit previous printer
            if (in_printer) {
                if (!def.empty() && current.name == def)
                    current.is_default = true;
                printers.push_back(current);
            }
            current = ParsedPrinter{};
            in_printer = true;

            // Extract name: token between "printer " and " is"
            std::string rest = line.substr(8); // after "printer "
            auto sp = rest.find(' ');
            if (sp != std::string::npos) {
                current.name = rest.substr(0, sp);
                std::string status_part = rest.substr(sp + 1);
                // Determine state
                if (status_part.compare(0, 8, "is idle") == 0) {
                    current.state = 0;
                } else if (status_part.compare(0, 14, "is now printing") == 0) {
                    current.state = 1;
                } else if (status_part.compare(0, 8, "disabled") == 0) {
                    current.state = 2;
                } else {
                    // "is idle" variants or unknown → default idle
                    current.state = 0;
                }
            }
        } else if (in_printer && line.compare(0, 13, "\tDescription:") == 0) {
            // "\tDescription: <text>"
            std::string val = line.substr(13);
            // Strip leading space
            auto start = val.find_first_not_of(" \t");
            if (start != std::string::npos) val = val.substr(start);
            // Strip trailing newline
            while (!val.empty() && (val.back() == '\n' || val.back() == '\r'))
                val.pop_back();
            current.description = val;
        } else if (in_printer && line.compare(0, 10, "\tLocation:") == 0) {
            // "\tLocation: <text>"
            std::string val = line.substr(10);
            auto start = val.find_first_not_of(" \t");
            if (start != std::string::npos) val = val.substr(start);
            while (!val.empty() && (val.back() == '\n' || val.back() == '\r'))
                val.pop_back();
            current.location = val;
        }
        // Other indented lines (Connection:, Interface:, etc.) are ignored
    }

    // Commit last printer
    if (in_printer) {
        if (!def.empty() && current.name == def)
            current.is_default = true;
        printers.push_back(current);
    }

    return printers;
}

uint32_t PrintService::submitJob(const shared::PrintRequest& req,
                                 std::string& message) noexcept {
    // Safely copy strings from fixed-size buffers
    char printer_buf[65]{}; std::memcpy(printer_buf, req.printer_name_, 64);
    char file_buf[129]{};   std::memcpy(file_buf,    req.file_path_,    128);
    char title_buf[49]{};   std::memcpy(title_buf,   req.title_,         48);

    std::string printer(printer_buf);
    std::string file_path(file_buf);
    std::string title(title_buf);

    // Validate file access
    if (!pathAllowed(file_path)) {
        message = "Access denied: file path not under $HOME or /tmp";
        return 0;
    }

    // If no printer specified, use system default
    if (printer.empty()) {
        printer = defaultPrinter();
        if (printer.empty()) {
            message = "No default printer configured";
            return 0;
        }
    }

    // Sanitize title (escape single quotes for shell safety)
    std::string safe_title;
    safe_title.reserve(title.size() + 4);
    for (char c : title) {
        if (c == '\'') safe_title += "'\\''";
        else           safe_title += c;
    }
    if (safe_title.empty()) safe_title = "Print Job";

    // Sanitize printer name (only allow alphanumeric, hyphen, underscore)
    for (char c : printer) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.') {
            message = "Invalid printer name";
            return 0;
        }
    }

    // Build lp command
    std::ostringstream cmd;
    cmd << "lp -d '" << printer << "'";
    cmd << " -n " << std::max(1u, req.copies_);
    cmd << " -t '" << safe_title << "'";

    // Duplex
    if (req.duplex_ == 1)
        cmd << " -o sides=two-sided-long-edge";
    else if (req.duplex_ == 2)
        cmd << " -o sides=two-sided-short-edge";

    // Color mode
    if (req.color_mode_ == 2)
        cmd << " -o ColorModel=Gray";

    // Paper size
    switch (req.paper_size_) {
        case 1: cmd << " -o media=A4";     break;
        case 2: cmd << " -o media=Legal";  break;
        case 3: cmd << " -o media=A3";     break;
        default: cmd << " -o media=Letter"; break;
    }

    // File path (single-quoted for shell safety, escaping embedded single quotes)
    std::string safe_path;
    safe_path.reserve(file_path.size() + 4);
    for (char c : file_path) {
        if (c == '\'') safe_path += "'\\''";
        else           safe_path += c;
    }
    cmd << " '" << safe_path << "' 2>&1";

    std::string cmd_str = cmd.str();
    std::cout << "[PrintService] submitJob: " << cmd_str << "\n";

    std::string output = runCmd(cmd_str);

    // Parse job id from output like: "request id is HP_LaserJet-42 (1 file(s))"
    // or: "request id is printer-name-42 (1 file(s))"
    uint32_t job_id = 0;
    auto pos = output.find("request id is ");
    if (pos != std::string::npos) {
        std::string rest = output.substr(pos + 14); // skip "request id is "
        // rest looks like: "printer-42 (1 file(s))\n"
        // Find the last '-' before the job number
        auto paren = rest.find(' ');
        std::string id_token = (paren != std::string::npos) ? rest.substr(0, paren) : rest;
        // Strip trailing whitespace/newline
        while (!id_token.empty() && (id_token.back() == '\n' || id_token.back() == '\r' || id_token.back() == ' '))
            id_token.pop_back();
        // The job id is the numeric suffix after the last '-'
        auto dash = id_token.rfind('-');
        if (dash != std::string::npos) {
            std::string num_str = id_token.substr(dash + 1);
            try {
                job_id = static_cast<uint32_t>(std::stoul(num_str));
            } catch (...) {
                job_id = 0;
            }
        }
    }

    if (job_id == 0) {
        // Command ran but we couldn't parse a job id — treat as error
        if (output.empty()) output = "lp: unknown error";
        // Strip trailing newline for cleaner message
        while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
            output.pop_back();
        message = output;
        return 0;
    }

    message = "Job " + std::to_string(job_id) + " sent to " + printer;
    return job_id;
}

// ─── Response helpers ─────────────────────────────────────────────────────────

void PrintService::sendResponse(std::shared_ptr<IpcSocket> client,
                                const shared::XBusMessage& req,
                                bool success, uint32_t job_id,
                                const std::string& message) noexcept {
    shared::XBusMessage response = req;
    shared::PrintResponse resp{};
    resp.success_ = success ? 1u : 0u;
    resp.job_id_  = job_id;
    std::strncpy(resp.message_, message.c_str(), sizeof(resp.message_) - 1);
    std::memcpy(response.payload_, &resp, sizeof(resp));
    (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
}

// ─── Opcode handlers ─────────────────────────────────────────────────────────

void PrintService::handleListPrinters(std::shared_ptr<IpcSocket> client,
                                      const shared::XBusMessage& msg) noexcept {
    // Payload is a uint32_t page number (0-based)
    uint32_t page = 0;
    if (sizeof(uint32_t) <= sizeof(msg.payload_))
        std::memcpy(&page, msg.payload_, sizeof(uint32_t));

    auto printers = parsePrinters();
    uint32_t total = static_cast<uint32_t>(printers.size());

    shared::XBusMessage response = msg;
    shared::PrinterListPage pl{};
    pl.total_ = total;
    pl.page_  = page;

    if (page < total) {
        pl.count_ = 1;
        const auto& p = printers[page];
        std::strncpy(pl.printer_.name_,        p.name.c_str(),        sizeof(pl.printer_.name_) - 1);
        std::strncpy(pl.printer_.description_, p.description.c_str(), sizeof(pl.printer_.description_) - 1);
        std::strncpy(pl.printer_.location_,    p.location.c_str(),    sizeof(pl.printer_.location_) - 1);
        pl.printer_.is_default_ = p.is_default ? 1u : 0u;
        pl.printer_.state_      = p.state;
    } else {
        pl.count_ = 0; // past end — return empty page so caller knows total
    }

    std::memcpy(response.payload_, &pl, sizeof(pl));
    (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&response), sizeof(response)});

    std::cout << "[PrintService] ListPrinters page=" << page
              << " total=" << total << "\n";
}

void PrintService::handlePrintFile(std::shared_ptr<IpcSocket> client,
                                   const shared::XBusMessage& msg) noexcept {
    shared::PrintRequest req{};
    std::memcpy(&req, msg.payload_, sizeof(req));

    std::string message;
    uint32_t job_id = submitJob(req, message);
    sendResponse(client, msg, job_id != 0, job_id, message);
}

void PrintService::handleGetJobStatus(std::shared_ptr<IpcSocket> client,
                                      const shared::XBusMessage& msg) noexcept {
    // Payload: uint32_t job_id, then char printer_name[48]
    uint32_t job_id = 0;
    char printer_name[49]{};
    std::memcpy(&job_id, msg.payload_, sizeof(uint32_t));
    std::memcpy(printer_name, msg.payload_ + sizeof(uint32_t), 48);
    printer_name[48] = '\0';

    std::string printer(printer_name);
    std::string job_id_str = std::to_string(job_id);

    // Validate printer name before shell interpolation.
    if (!printer.empty() && !isValidPrinterName(printer)) {
        shared::PrintJobStatus bad{};
        bad.job_id_ = job_id;
        bad.state_  = 3; // error
        std::strncpy(bad.message_, "Invalid printer name", sizeof(bad.message_) - 1);
        shared::XBusMessage response = msg;
        std::memcpy(response.payload_, &bad, sizeof(bad));
        (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        return;
    }

    // Build the CUPS job identifier: "printer-jobid"
    std::string cups_job;
    if (!printer.empty()) {
        cups_job = printer + "-" + job_id_str;
    } else {
        cups_job = job_id_str;
    }

    std::string cmd = "lpstat -l " + cups_job + " 2>/dev/null";
    std::string out = runCmd(cmd);

    shared::PrintJobStatus status{};
    status.job_id_ = job_id;
    std::strncpy(status.printer_name_, printer_name, sizeof(status.printer_name_) - 1);

    if (out.empty()) {
        // Job not found in queue — assume completed and gone
        status.state_   = 2; // completed
        std::strncpy(status.message_, "Job completed or not found", sizeof(status.message_) - 1);
    } else {
        // Default to pending
        status.state_ = 0;

        // Parse status line by line
        std::istringstream ss(out);
        std::string line;
        bool found_completed_at = false;
        while (std::getline(ss, line)) {
            // Extract job title from first line: "<cups_job> ... '<title>' ..."
            // The first non-indented line is the job header; grab it as message
            if (!line.empty() && line[0] != '\t' && status.message_[0] == '\0') {
                std::strncpy(status.message_, line.c_str(), sizeof(status.message_) - 1);
            }

            if (line.find("Status: Processing") != std::string::npos) {
                status.state_ = 1; // processing
            } else if (line.find("Status: Completed") != std::string::npos) {
                status.state_ = 2; // completed
            } else if (line.find("Status: Cancelled") != std::string::npos ||
                       line.find("Status: Canceled")  != std::string::npos) {
                status.state_ = 3; // cancelled
            } else if (line.find("Status: Aborted") != std::string::npos) {
                status.state_ = 4; // error
            } else if (line.find("Completed-at:") != std::string::npos) {
                found_completed_at = true;
            }

            // Pages: "Sheet Completed: N"
            if (line.find("Sheet Completed:") != std::string::npos) {
                auto p = line.find("Sheet Completed:");
                std::string num = line.substr(p + 17);
                while (!num.empty() && num[0] == ' ') num.erase(num.begin());
                try { status.pages_completed_ = static_cast<uint32_t>(std::stoul(num)); } catch (...) {}
            }
            // "job-impressions-completed: N"
            if (line.find("job-impressions-completed:") != std::string::npos) {
                auto p = line.find("job-impressions-completed:");
                std::string num = line.substr(p + 26);
                while (!num.empty() && num[0] == ' ') num.erase(num.begin());
                try { status.pages_completed_ = static_cast<uint32_t>(std::stoul(num)); } catch (...) {}
            }
        }

        if (found_completed_at && status.state_ < 2)
            status.state_ = 2;
    }

    shared::XBusMessage response = msg;
    std::memcpy(response.payload_, &status, sizeof(status));
    (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&response), sizeof(response)});

    std::cout << "[PrintService] GetJobStatus job=" << job_id
              << " state=" << status.state_ << "\n";
}

void PrintService::handleCancelJob(std::shared_ptr<IpcSocket> client,
                                   const shared::XBusMessage& msg) noexcept {
    // Payload: uint32_t job_id, then char printer_name[48]
    uint32_t job_id = 0;
    char printer_name[49]{};
    std::memcpy(&job_id, msg.payload_, sizeof(uint32_t));
    std::memcpy(printer_name, msg.payload_ + sizeof(uint32_t), 48);
    printer_name[48] = '\0';

    std::string printer(printer_name);

    // Sanitize printer name
    for (char c : printer) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.') {
            sendResponse(client, msg, false, 0, "Invalid printer name");
            return;
        }
    }

    std::string cups_job;
    if (!printer.empty()) {
        cups_job = printer + "-" + std::to_string(job_id);
    } else {
        cups_job = std::to_string(job_id);
    }

    std::string cmd = "cancel " + cups_job + " 2>&1";
    std::cout << "[PrintService] CancelJob: " << cmd << "\n";
    std::string out = runCmd(cmd);

    // Strip trailing newline
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();

    bool ok = out.empty(); // cancel exits 0 with no output on success
    std::string message = ok ? ("Cancelled job " + std::to_string(job_id)) : out;
    sendResponse(client, msg, ok, job_id, message);
}

void PrintService::handleShowDialog(std::shared_ptr<IpcSocket> client,
                                    const shared::XBusMessage& msg) noexcept {
    shared::PrintDialogRequest dreq{};
    std::memcpy(&dreq, msg.payload_, sizeof(dreq));
    dreq.file_path_[sizeof(dreq.file_path_) - 1] = '\0';
    dreq.title_[sizeof(dreq.title_) - 1] = '\0';

    // Validate file path
    std::string file_path(dreq.file_path_);
    if (!pathAllowed(file_path)) {
        sendResponse(client, msg, false, 0,
                     "Access denied: file path not under $HOME or /tmp");
        return;
    }

    // Get default printer
    std::string def = defaultPrinter();
    if (def.empty()) {
        sendResponse(client, msg, false, 0, "No default printer configured");
        return;
    }

    // Build a PrintRequest using defaults
    shared::PrintRequest req{};
    std::strncpy(req.printer_name_, def.c_str(),           sizeof(req.printer_name_) - 1);
    std::strncpy(req.file_path_,    dreq.file_path_,       sizeof(req.file_path_) - 1);
    std::strncpy(req.title_,        dreq.title_,           sizeof(req.title_) - 1);
    req.copies_     = 1;
    req.color_mode_ = 0; // auto
    req.duplex_     = 0; // one-sided
    req.paper_size_ = 0; // letter

    std::string message;
    uint32_t job_id = submitJob(req, message);
    sendResponse(client, msg, job_id != 0, job_id, message);
}

// ─── Entry point ─────────────────────────────────────────────────────────────

void PrintService::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                  const shared::XBusMessage& msg) noexcept {
    if (msg.opcode_ == static_cast<uint32_t>(shared::PrintSpoolerOpcode::ListPrinters)) {
        handleListPrinters(client, msg);
    } else if (msg.opcode_ == static_cast<uint32_t>(shared::PrintSpoolerOpcode::PrintFile)) {
        handlePrintFile(client, msg);
    } else if (msg.opcode_ == static_cast<uint32_t>(shared::PrintSpoolerOpcode::GetJobStatus)) {
        handleGetJobStatus(client, msg);
    } else if (msg.opcode_ == static_cast<uint32_t>(shared::PrintSpoolerOpcode::CancelJob)) {
        handleCancelJob(client, msg);
    } else if (msg.opcode_ == static_cast<uint32_t>(shared::PrintSpoolerOpcode::ShowDialog)) {
        handleShowDialog(client, msg);
    } else {
        std::cerr << "[PrintService] Unknown opcode: 0x" << std::hex << msg.opcode_ << std::dec << "\n";
    }
}

} // namespace fbx::xserver

#include "xserver/personal_services.h"
#include "xserver/personal_parse.h"
#include "xserver/totp.h"
#include "xserver/calendar_service.h"
#include "xserver/config_service.h"
#include "xserver/lock_service.h"
#include "xserver/user_files.h"
#include "xserver/xbus_reply.h"
#include "shared/text_replace.h"
#include "shared/logger.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <set>
#include <filesystem>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace fbx::xserver {

namespace {

using namespace svc;
using userfiles::homeDir;
using userfiles::readFile;
using userfiles::writeFileAtomic;

// ── TOTP ─────────────────────────────────────────────────────────────────────

// Secrets: only trust a regular file the user owns that nobody else can read.
std::vector<totp::Account> totpAccounts() {
    std::string path = homeDir() + "/.config/freebsdx/totp";
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0) return {};
    if (!S_ISREG(st.st_mode) || st.st_uid != ::getuid() || (st.st_mode & 077)) {
        LOG_WARN("TotpService: ignoring {} — must be a file you own with mode 0600", path);
        return {};
    }
    auto text = readFile(path);
    return text ? totp::parseFile(*text) : std::vector<totp::Account>{};
}

// ── Text replacements ────────────────────────────────────────────────────────

std::string replacementsPath() { return shared::textreplace::filePath(homeDir()); }

std::vector<shared::textreplace::Entry> readReplacements() {
    auto text = readFile(replacementsPath());
    return text ? shared::textreplace::parse(*text) : std::vector<shared::textreplace::Entry>{};
}

bool writeReplacements(const std::vector<shared::textreplace::Entry>& e) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(replacementsPath()).parent_path(), ec);
    return writeFileAtomic(replacementsPath(), shared::textreplace::serialize(e), 0644);
}

// ── Reminders ────────────────────────────────────────────────────────────────

constexpr const char* kDoneKey = "reminder.done";

std::vector<personal::Reminder> currentReminders() {
    std::vector<personal::Reminder> all;
    std::set<uint32_t> live;
    for (auto& r : CalendarService::reminders()) {
        live.insert(r.event_id);
        all.push_back({r.event_id, std::move(r.title), r.start, r.due});
    }
    // Forget "done" marks for events that no longer exist.
    auto done = personal::parseIdSet(ConfigService::get(kDoneKey));
    std::set<uint32_t> kept;
    for (uint32_t id : done) if (live.count(id)) kept.insert(id);
    if (kept != done) ConfigService::set(kDoneKey, personal::formatIdSet(kept));
    return personal::selectReminders(std::move(all), std::time(nullptr), kept);
}

// ── Screen lock ──────────────────────────────────────────────────────────────

constexpr uint32_t kDefaultLockTimeout = 10 * 60;

shared::ScreenLockPolicy screenLockPolicy() {
    shared::ScreenLockPolicy p{};
    uint32_t live = LockService::timeoutSeconds();
    p.enabled_          = live > 0 ? 1u : 0u;
    // The chosen timeout survives turning auto-lock off and on again.
    p.timeout_seconds_  = live > 0 ? live
                        : static_cast<uint32_t>(std::strtoul(
                              ConfigService::get("screenlock.timeout", std::to_string(kDefaultLockTimeout)).c_str(),
                              nullptr, 10));
    p.require_password_ = 1;
    p.lock_on_sleep_    = 1;   // PowerService locks before suspend/hibernate
    return p;
}

}  // namespace

// ── TotpService ──────────────────────────────────────────────────────────────

void TotpService::handleMessage(const shared::XBusMessage& msg,
                                std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::TotpOpcode;
    constexpr const char* kName = "com.freebsdx.Totp";
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListAccounts: {
        auto accounts = totpAccounts();
        uint32_t page = readPage(msg);
        shared::TotpAccountPage pg{};
        pg.page_  = page;
        pg.total_ = static_cast<uint32_t>(accounts.size());
        if (page < accounts.size()) {
            const auto& a = accounts[page];
            pg.valid_    = 1;
            pg.has_more_ = page + 1 < accounts.size() ? 1u : 0u;
            std::strncpy(pg.id_, totp::accountId(a).c_str(), sizeof(pg.id_) - 1);
            std::strncpy(pg.issuer_, a.issuer.c_str(), sizeof(pg.issuer_) - 1);
            std::strncpy(pg.account_, a.account.c_str(), sizeof(pg.account_) - 1);
        }
        reply(msg, client, kName, msg.opcode_, &pg);
        break;
    }
    case O::GetCode: {
        shared::TotpCodeRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.id_[sizeof(req.id_) - 1] = '\0';
        shared::TotpCodeResponse rsp{};
        std::time_t now = std::time(nullptr);
        for (const auto& a : totpAccounts()) {
            if (totp::accountId(a) != req.id_) continue;
            std::strncpy(rsp.code_, totp::code(a, now).c_str(), sizeof(rsp.code_) - 1);
            rsp.seconds_remaining_ = totp::secondsRemaining(a, now);
            break;
        }
        reply(msg, client, kName, msg.opcode_, &rsp);
        break;
    }
    default: break;
    }
}

// ── ReminderService ──────────────────────────────────────────────────────────

void ReminderService::handleMessage(const shared::XBusMessage& msg,
                                    std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::ReminderOpcode;
    constexpr const char* kName = "com.freebsdx.Reminder";
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListReminders: {
        auto rs = currentReminders();
        uint32_t page = readPage(msg);
        shared::ReminderPage pg{};
        pg.page_  = page;
        pg.total_ = static_cast<uint32_t>(rs.size());
        if (page < rs.size()) {
            const auto& r = rs[page];
            pg.valid_    = 1;
            pg.has_more_ = page + 1 < rs.size() ? 1u : 0u;
            std::strncpy(pg.id_, personal::reminderWireId(r.event_id).c_str(), sizeof(pg.id_) - 1);
            std::strncpy(pg.title_, r.title.c_str(), sizeof(pg.title_) - 1);
            std::tm tm{};
            localtime_r(&r.due, &tm);
            std::strftime(pg.due_, sizeof(pg.due_), "%Y-%m-%d %H:%M", &tm);
            pg.overdue_ = r.due <= std::time(nullptr) ? 1u : 0u;
        }
        reply(msg, client, kName, msg.opcode_, &pg);
        break;
    }
    case O::CompleteReminder: {
        shared::ReminderActionRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.id_[sizeof(req.id_) - 1] = '\0';
        uint32_t id = 0;
        bool ok = personal::parseReminderWireId(req.id_, id);
        if (ok) {
            auto done = personal::parseIdSet(ConfigService::get(kDoneKey));
            done.insert(id);
            ConfigService::set(kDoneKey, personal::formatIdSet(done));
        } else {
            LOG_WARN("ReminderService: bad reminder id '{}'", req.id_);
        }
        ack(msg, client, kName);
        if (ok) broadcast<char>(kName, static_cast<uint32_t>(O::ReminderChanged), nullptr);
        break;
    }
    default: break;
    }
}

// ── TextReplacementService ───────────────────────────────────────────────────

void TextReplacementService::handleMessage(const shared::XBusMessage& msg,
                                           std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::TextReplacementOpcode;
    constexpr const char* kName = "com.freebsdx.TextReplacement";
    switch (static_cast<O>(msg.opcode_)) {
    case O::ListShortcuts: {
        auto entries = readReplacements();
        uint32_t page = readPage(msg);
        shared::TextReplacementPage pg{};
        pg.page_  = page;
        pg.total_ = static_cast<uint32_t>(entries.size());
        if (page < entries.size()) {
            const auto& e = entries[page];
            pg.valid_    = 1;
            pg.has_more_ = page + 1 < entries.size() ? 1u : 0u;
            std::strncpy(pg.shortcut_, e.shortcut.c_str(), sizeof(pg.shortcut_) - 1);
            std::strncpy(pg.phrase_, e.phrase.c_str(), sizeof(pg.phrase_) - 1);
            pg.entry_id_ = shared::textreplace::entryId(e.shortcut);
        }
        reply(msg, client, kName, msg.opcode_, &pg);
        break;
    }
    case O::AddShortcut: {
        shared::TextReplacementAddRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.shortcut_[sizeof(req.shortcut_) - 1] = '\0';
        req.phrase_[sizeof(req.phrase_) - 1]     = '\0';
        bool ok = false;
        if (!shared::textreplace::validShortcut(req.shortcut_) || !shared::textreplace::validPhrase(req.phrase_)) {
            LOG_WARN("TextReplacementService: invalid shortcut '{}'", req.shortcut_);
        } else {
            auto entries = readReplacements();
            bool replaced = false;
            for (auto& e : entries)
                if (e.shortcut == req.shortcut_) { e.phrase = req.phrase_; replaced = true; }
            if (!replaced) entries.push_back({req.shortcut_, req.phrase_});
            ok = writeReplacements(entries);
        }
        ack(msg, client, kName);
        if (ok) broadcast<char>(kName, static_cast<uint32_t>(O::ShortcutsChanged), nullptr);
        break;
    }
    case O::DeleteShortcut: {
        shared::TextReplacementDeleteRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        auto entries = readReplacements();
        size_t before = entries.size();
        std::erase_if(entries, [&](const shared::textreplace::Entry& e) {
            return shared::textreplace::entryId(e.shortcut) == req.entry_id_;
        });
        bool ok = entries.size() != before && writeReplacements(entries);
        if (!ok) LOG_WARN("TextReplacementService: no shortcut {:#x}", req.entry_id_);
        ack(msg, client, kName);
        if (ok) broadcast<char>(kName, static_cast<uint32_t>(O::ShortcutsChanged), nullptr);
        break;
    }
    default: break;
    }
}

// ── ScreenLockService ────────────────────────────────────────────────────────

void ScreenLockService::init() noexcept {
    // Only touch LockService if a policy was ever saved; otherwise keep its
    // built-in default.
    std::string enabled = ConfigService::get("screenlock.enabled");
    if (enabled.empty()) return;
    uint32_t timeout = static_cast<uint32_t>(std::strtoul(
        ConfigService::get("screenlock.timeout", std::to_string(kDefaultLockTimeout)).c_str(), nullptr, 10));
    LockService::setTimeoutSeconds(enabled == "1" ? timeout : 0);
}

void ScreenLockService::handleMessage(const shared::XBusMessage& msg,
                                      std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::ScreenLockOpcode;
    constexpr const char* kName = "com.freebsdx.ScreenLock";
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetPolicy: {
        auto p = screenLockPolicy();
        reply(msg, client, kName, msg.opcode_, &p);
        break;
    }
    case O::SetPolicy: {
        shared::ScreenLockPolicy req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        // 0 means "never" in the contract; clamp the rest to 30 s .. 24 h.
        bool on = req.enabled_ != 0 && req.timeout_seconds_ != 0;
        uint32_t t = req.timeout_seconds_ == 0 ? kDefaultLockTimeout
                   : std::clamp<uint32_t>(req.timeout_seconds_, 30, 24 * 3600);
        ConfigService::set("screenlock.enabled", on ? "1" : "0");
        ConfigService::set("screenlock.timeout", std::to_string(t));
        LockService::setTimeoutSeconds(on ? t : 0);
        if (req.require_password_ == 0 || req.lock_on_sleep_ == 0)
            LOG_INFO("ScreenLockService: the lock screen always asks for the password and always locks before sleep");
        ack(msg, client, kName);
        auto p = screenLockPolicy();
        broadcast(kName, static_cast<uint32_t>(O::PolicyChanged), &p);
        break;
    }
    case O::LockNow:
        LockService::lock();
        ack(msg, client, kName);
        break;
    default: break;
    }
}

// ── LicenseService ───────────────────────────────────────────────────────────

void LicenseService::handleMessage(const shared::XBusMessage& msg,
                                   std::shared_ptr<IpcSocket> client) noexcept {
    if (static_cast<shared::LicenseOpcode>(msg.opcode_) != shared::LicenseOpcode::GetInfo) return;
    shared::LicenseState st{};
    if (auto text = readFile("/etc/freebsdx-license.conf")) {
        auto kv = personal::parseKeyValues(*text);
        std::strncpy(st.license_key_, personal::maskKey(kv["key"]).c_str(), sizeof(st.license_key_) - 1);
        std::strncpy(st.support_id_, kv["support_id"].c_str(), sizeof(st.support_id_) - 1);
        std::strncpy(st.tier_, kv["tier"].empty() ? "Standard" : kv["tier"].c_str(), sizeof(st.tier_) - 1);
        std::strncpy(st.expiry_, kv["expiry"].empty() ? "Never" : kv["expiry"].c_str(), sizeof(st.expiry_) - 1);
        st.seats_total_ = static_cast<uint32_t>(std::strtoul(kv["seats_total"].c_str(), nullptr, 10));
        st.seats_used_  = static_cast<uint32_t>(std::strtoul(kv["seats_used"].c_str(), nullptr, 10));
    } else {
        // No commercial license installed: the MIT-licensed community build.
        std::strncpy(st.tier_, "Community", sizeof(st.tier_) - 1);
        std::strncpy(st.expiry_, "Never", sizeof(st.expiry_) - 1);
    }
    reply(msg, client, "com.freebsdx.License", msg.opcode_, &st);
}

}  // namespace fbx::xserver

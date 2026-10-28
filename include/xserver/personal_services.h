#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>

namespace fbx::xserver {

// com.freebsdx.Totp — TOTP codes (totp.h) for the otpauth:// URIs in
// ~/.config/freebsdx/totp. The file holds secrets, so it's ignored unless it
// is a regular file owned by the user with no group/other access (like ssh).
// One account per ListAccounts page; GetCode by the account's id.
class TotpService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

// com.freebsdx.Reminder — reminders of Calendar events (reminder_min > 0):
// overdue ones from the last day and those due in the next 7 days, soonest
// first, minus the ones marked done (ConfigService "reminder.done").
class ReminderService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

// com.freebsdx.TextReplacement — ~/.config/freebsdx/text-replacements
// (shared/text_replace.h), which xkit text fields expand as you type.
class TextReplacementService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

// com.freebsdx.ScreenLock — the idle auto-lock policy, applied to LockService
// and persisted (ConfigService "screenlock.*"); init() restores it at startup.
// The lock screen always asks for the password and PowerService always locks
// before suspend/hibernate, so require_password_ and lock_on_sleep_ are always
// 1 (lid-close isn't hooked).
class ScreenLockService {
public:
    static void init() noexcept;
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

// com.freebsdx.License — /etc/freebsdx-license.conf (key=value: key,
// support_id, tier, expiry, seats_total, seats_used) with the key masked to its
// last 4 characters; without the file: the open-source "Community" tier.
class LicenseService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver

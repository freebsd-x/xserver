#pragma once

#include "shared/protocol.h"
#include "xserver/hangul.h"
#include <memory>
#include <string>

namespace fbx::xserver {

class IpcSocket;

// ImeService — compositor-side input method engine.
//
// Flow:
//   1. TextInputWidget focuses → sends com.freebsdx.IME/Activate (locale=1 for Japanese).
//   2. main.cc calls ImeService::FilterKeyPress() before forwarding every KeyPress
//      to the focused client.  If the key advances a composition it is consumed and
//      NOT forwarded; instead an ImePreedit or ImeCommit InputMessage is sent back.
//   3. TextInputElement handles ImePreedit (inline preedit display) and ImeCommit
//      (UTF-8 text insertion).
//   4. TextInputWidget loses focus → sends com.freebsdx.IME/Deactivate.
//
// Built-in engines:
//   locale 0 — passthrough (no interception)
//   locale 1 — Romaji → Hiragana (full 50-on table + digraphs + double-consonant っ)
//   locale 3 — Korean 2-beolsik (hangul.h)
//
// A field that asks for no particular locale (0) gets the system default
// method (com.freebsdx.InputMethod, ConfigService "ime.default"). The 한/영 key
// or Shift+Space toggles the focused field between Latin and that method.
//
class ImeService {
public:
    // Called from main.cc immediately before routing a KeyPress to the focused client.
    // Returns true if the key was consumed by IME (caller must NOT forward the event).
    static bool FilterKeyPress(const std::shared_ptr<IpcSocket>& focused_client,
                               const shared::InputMessage& msg) noexcept;

    // XBus handler for "com.freebsdx.IME" service messages.
    static void ProcessMessage(std::shared_ptr<IpcSocket> client,
                               const shared::XBusMessage& msg) noexcept;

    // The default method for fields that don't pick one (0 = Latin).
    [[nodiscard]] static uint32_t defaultLocale() noexcept;
    // Persist a new default and switch the focused field to it now.
    static void setDefaultLocale(uint32_t locale) noexcept;
    // What the focused field is using (the default when no field is focused).
    [[nodiscard]] static uint32_t currentLocale() noexcept;

private:
    // Build an InputMessage carrying up to 8 bytes of UTF-8 text.
    static shared::InputMessage makeTextMsg(shared::InputEventType type,
                                            const std::string& utf8) noexcept;

    // Send ImePreedit (empty string → clear the preedit display).
    static void sendPreedit(const std::string& preedit) noexcept;

    // Send ImeCommit with the given UTF-8 string.
    static void sendCommit(const std::string& utf8) noexcept;

    // Clear composition without sending any event.
    static void clearComposition() noexcept;
    // Commit whatever is being composed (romaji as typed, Hangul syllable).
    static void commitPending() noexcept;
    static bool filterHangul(const shared::InputMessage& msg) noexcept;
    static void broadcastMethodChanged() noexcept;

    // Try to match composition_ against the romaji table.
    // Returns true if the key was consumed (exact match committed, or prefix pending).
    static bool tryCommitRomaji() noexcept;

    // ── State ─────────────────────────────────────────────────────────────────
    static std::shared_ptr<IpcSocket> active_client_;
    static uint32_t                   locale_;       // 0=off, 1=ja, 3=ko
    static std::string                composition_;  // accumulated ASCII romaji
    static hangul::Composer           hangul_;
};

// com.freebsdx.InputMethod — the input methods above as a list; SetActive
// makes one the default (and switches the focused field to it).
class InputMethodService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

} // namespace fbx::xserver

#pragma once

#include <string>

// The screen reader's voice. Accessibility announcements (apps' focus changes,
// the compositor's window focus) are spoken with espeak-ng while the screen
// reader is on (A11ySettings). A new utterance interrupts the previous one,
// as screen readers do. FBX_READER_LOG=<file> writes each utterance to that
// file instead of speaking (headless tests).
namespace fbx::xserver::speech {

// Speak `text` — only while the screen reader is on, unless `even_if_off`
// (e.g. confirming "Screen reader off").
void say(const std::string& text, bool even_if_off = false) noexcept;

// The text actually spoken for `text`: control characters dropped, runs of
// whitespace collapsed, at most 300 characters. Pure; for tests.
[[nodiscard]] std::string clean(const std::string& text);

}  // namespace fbx::xserver::speech

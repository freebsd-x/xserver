#pragma once

#include "shared/protocol.h"
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

namespace fbx::xserver {

class IpcSocket;

// MimeService — file type → application handler registry.
//
// Populated at startup by scanning .xapp bundle manifests for their
// declared mime_types. Persists user overrides via ConfigService
// (key pattern: "mime.default.<mime_type>").
//
// Thread safety: all methods are called from the single-threaded event
// loop; no locking needed.
class MimeService {
public:
    // Scan .xapp bundles in apps_dirs (tried in order) and build the
    // initial registry. Call once at server startup.
    static void loadFromAppsDir(const std::string& apps_dir) noexcept;

    // Handle a com.freebsdx.MIME XBus message.
    static void ProcessMessage(std::shared_ptr<IpcSocket> client,
                               const shared::XBusMessage& msg) noexcept;

    // Derive the MIME type from a file extension (e.g. ".txt" → "text/plain").
    // Returns "application/octet-stream" for unknown extensions.
    [[nodiscard]] static std::string mimeFromExtension(std::string_view ext) noexcept;

    // Return the default handler app_id for a given MIME type, or "" if none.
    [[nodiscard]] static std::string defaultHandler(const std::string& mime_type) noexcept;

    // Return all handler app_ids for a given MIME type (default handler first).
    [[nodiscard]] static std::vector<std::string> allHandlers(const std::string& mime_type) noexcept;

    // Make app_id the default for mime_type (persisted). False if it isn't a
    // registered handler for that type.
    static bool setDefault(const std::string& mime_type, const std::string& app_id) noexcept;

    // Display name of a handler app ("" if unknown).
    [[nodiscard]] static std::string appName(const std::string& app_id) noexcept;

private:
    struct AppEntry {
        std::string app_id;
        std::string app_name;
    };

    // mime_type → ordered list of capable apps (index 0 = default unless overridden)
    static inline std::unordered_map<std::string, std::vector<AppEntry>> registry_;

    // app_id → display name (for building response payloads)
    static inline std::unordered_map<std::string, std::string> app_names_;
};

} // namespace fbx::xserver

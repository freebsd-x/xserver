#pragma once

#include "shared/protocol.h"
#include <memory>

namespace fbx::xserver {

class IpcSocket;

// com.freebsdx.DefaultApps — the default app per kind of file (Web, Text,
// Images, Music, Video, PDF, Archives), a friendly view over MimeService's
// handler registry (built from the bundles' declared mime_types; overrides
// persist as "mime.default.<type>"). Categories no installed app handles are
// left out. CycleDefault moves to the next handler (in name order) for every
// type in the category that handler supports.
class DefaultAppsService {
public:
    static bool ProcessMessage(std::shared_ptr<IpcSocket> client,
                               const shared::XBusMessage& msg) noexcept;
};

} // namespace fbx::xserver

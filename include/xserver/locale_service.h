#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>
#include <string>

namespace fbx::xserver {

// com.freebsdx.Locale — system language/locale for the session. Persists the
// chosen locale to ~/.freebsdx-locale and broadcasts LocaleChanged so running
// apps re-load their message catalogs live. (The protocol envisages writing
// /etc/profile.d/locale.sh too, but the compositor is unprivileged; system-wide
// export is a privileged-helper follow-up. Newly launched apps read the user
// file via GetLocale on startup.)
//   GetLocale     → LocaleState
//   SetLocale(req)→ persist + broadcast LocaleChanged
class LocaleService {
public:
    static void init() noexcept;
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;

    static std::string currentLocale() noexcept;

private:
    static shared::LocaleState buildState() noexcept;
    static void sendState(IpcSocket* client, uint32_t tid) noexcept;
    static std::string path() noexcept;

    static std::string locale_;   // e.g. "en_US.UTF-8"
};

}  // namespace fbx::xserver

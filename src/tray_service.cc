#include "xserver/tray_service.h"
#include "shared/protocol.h"
#include <cstring>
#include <algorithm>

namespace fbx::xserver {

// Forward-declare BroadcastXBusMessage (defined in xbus_dispatcher.cc)
void BroadcastXBusMessage(const shared::XBusMessage&);

std::mutex         TrayService::mutex_;
std::vector<TrayService::Entry> TrayService::entries_;
uint32_t           TrayService::next_id_{1};

void TrayService::init() noexcept {}

shared::TrayEntry TrayService::registerIcon(
        const shared::TrayRegisterRequest& req,
        std::shared_ptr<IpcSocket> owner) noexcept
{
    std::lock_guard<std::mutex> lk(mutex_);
    Entry e{};
    e.data.id_ = next_id_++;
    std::strncpy(e.data.app_id_,    req.app_id_,    sizeof(e.data.app_id_)-1);
    std::strncpy(e.data.icon_path_, req.icon_path_,sizeof(e.data.icon_path_)-1);
    std::strncpy(e.data.icon_text_, req.icon_text_, sizeof(e.data.icon_text_)-1);
    std::strncpy(e.data.tooltip_,   req.tooltip_,   sizeof(e.data.tooltip_)-1);
    e.data.status_ = req.status_;
    e.owner = owner;
    entries_.push_back(e);
    auto result = e.data;
    broadcastChanged();
    return result;
}

void TrayService::unregister(uint32_t id) noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
        [id](const Entry& e){ return e.data.id_ == id; }), entries_.end());
    broadcastChanged();
}

void TrayService::setIcon(const shared::TrayUpdateRequest& req) noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& e : entries_) {
        if (e.data.id_ == req.id_) {
            // value_ could be icon_path or icon_text — detect by length
            if (std::strlen(req.value_) <= 8)
                std::strncpy(e.data.icon_text_, req.value_, sizeof(e.data.icon_text_)-1);
            else
                std::strncpy(e.data.icon_path_, req.value_, sizeof(e.data.icon_path_)-1);
            break;
        }
    }
    broadcastChanged();
}

void TrayService::setTooltip(const shared::TrayUpdateRequest& req) noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& e : entries_) {
        if (e.data.id_ == req.id_) {
            std::strncpy(e.data.tooltip_, req.value_, sizeof(e.data.tooltip_)-1);
            break;
        }
    }
    broadcastChanged();
}

void TrayService::setStatus(const shared::TrayStatusRequest& req) noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& e : entries_) {
        if (e.data.id_ == req.id_) { e.data.status_ = req.status_; break; }
    }
    broadcastChanged();
}

shared::TrayEntryPage TrayService::listEntries(uint32_t page) noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    // Filter out hidden entries
    std::vector<const Entry*> visible;
    for (const auto& e : entries_)
        if (e.data.status_ != static_cast<uint32_t>(shared::TrayStatus::Hidden))
            visible.push_back(&e);

    shared::TrayEntryPage pg{};
    pg.total_    = static_cast<uint32_t>(visible.size());
    pg.page_     = page;
    pg.valid_    = 0;
    pg.has_more_ = 0;
    if (page < visible.size()) {
        pg.valid_    = 1;
        pg.entry_    = visible[page]->data;
        pg.has_more_ = (page + 1 < visible.size()) ? 1 : 0;
    }
    return pg;
}

void TrayService::dispatchClick(uint32_t id) noexcept {
    std::shared_ptr<IpcSocket> owner;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (const auto& e : entries_) {
            if (e.data.id_ == id) { owner = e.owner.lock(); break; }
        }
    }
    if (!owner) return;

    // Send Click message directly to the owning app's socket
    shared::XBusMessage msg{};
    std::strncpy(msg.service_name_, "com.freebsdx.Tray", sizeof(msg.service_name_)-1);
    msg.opcode_ = static_cast<uint32_t>(shared::TrayOpcode::Click);
    shared::TrayIdRequest cr{};
    cr.id_ = id;
    std::memcpy(msg.payload_, &cr, sizeof(cr));
    (void)owner->sendMessage(
        {reinterpret_cast<const uint8_t*>(&msg), sizeof(msg)});
}

void TrayService::broadcastChanged() noexcept {
    // Called with mutex_ held — broadcast without lock to avoid re-entrancy
    shared::XBusMessage msg{};
    std::strncpy(msg.service_name_, "com.freebsdx.Tray", sizeof(msg.service_name_)-1);
    msg.opcode_ = static_cast<uint32_t>(shared::TrayOpcode::TrayChanged);
    // No payload needed — xshell re-fetches ListEntries page 0
    BroadcastXBusMessage(msg);
}

} // namespace fbx::xserver

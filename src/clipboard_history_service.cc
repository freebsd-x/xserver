#include "xserver/clipboard_history_service.h"
#include "shared/logger.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

namespace fbx::xserver {

void BroadcastXBusMessage(const shared::XBusMessage&);

// ─── Static member definitions ────────────────────────────────────────────────
std::mutex              ClipboardHistoryService::mutex_;
std::deque<ClipboardHistoryService::Entry> ClipboardHistoryService::items_;
uint32_t                ClipboardHistoryService::next_id_{1};
std::string             ClipboardHistoryService::history_path_;

// ─── Escape helpers ───────────────────────────────────────────────────────────
// Pipe-separated file; escape '|', '\n', '\\'
static std::string pipeEsc(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        if      (c == '|')  o += "\\|";
        else if (c == '\n') o += "\\n";
        else if (c == '\\') o += "\\\\";
        else                o += c;
    }
    return o;
}

[[maybe_unused]] static std::string pipeUnesc(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            ++i;
            if      (s[i] == '|')  o += '|';
            else if (s[i] == 'n')  o += '\n';
            else if (s[i] == '\\') o += '\\';
            else { o += '\\'; o += s[i]; }
        } else { o += s[i]; }
    }
    return o;
}

// ─── Init ─────────────────────────────────────────────────────────────────────
void ClipboardHistoryService::init() noexcept {
    const char* home = std::getenv("HOME");
    history_path_ = home ? std::string(home) + "/.freebsdx-clipboard-history"
                         : "/tmp/.freebsdx-clipboard-history";
    loadFromDisk();
    LOG_INFO("ClipboardHistoryService: ready ({} entries)", items_.size());
}

// ─── Disk I/O ─────────────────────────────────────────────────────────────────
// Format: one entry per line: id|pinned|timestamp|escaped_text
bool ClipboardHistoryService::loadFromDisk() noexcept {
    std::ifstream f(history_path_);
    if (!f.is_open()) return true;
    items_.clear();
    next_id_ = 1;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        // Manual pipe split (respecting escape sequences)
        std::vector<std::string> fields;
        std::string cur;
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '\\' && i + 1 < line.size()) {
                ++i;
                if      (line[i] == '|')  cur += '|';
                else if (line[i] == 'n')  cur += '\n';
                else if (line[i] == '\\') cur += '\\';
                else { cur += '\\'; cur += line[i]; }
            } else if (line[i] == '|') {
                fields.push_back(cur); cur.clear();
            } else { cur += line[i]; }
        }
        fields.push_back(cur);
        if (fields.size() < 4) continue;
        Entry e;
        // stoul throws on a malformed/corrupt line; skip it rather than crash
        // (loadFromDisk is noexcept → an escape would terminate the compositor).
        try {
            e.id        = static_cast<uint32_t>(std::stoul(fields[0]));
            e.timestamp = static_cast<uint32_t>(std::stoul(fields[2]));
        } catch (...) { continue; }
        e.pinned    = fields[1] == "1";
        e.text      = fields[3];
        if (e.id >= next_id_) next_id_ = e.id + 1;
        items_.push_back(std::move(e));
    }
    return true;
}

bool ClipboardHistoryService::saveToDisk() noexcept {
    std::ofstream f(history_path_, std::ios::trunc);
    if (!f.is_open()) return false;
    f << "# FreeBSD-X Clipboard History v1\n";
    for (const auto& e : items_)
        f << e.id << '|'
          << (e.pinned ? "1" : "0") << '|'
          << e.timestamp << '|'
          << pipeEsc(e.text) << '\n';
    return true;
}

// ─── record() — called by ClipboardService ───────────────────────────────────
void ClipboardHistoryService::record(const std::string& text) noexcept {
    if (text.empty()) return;

    std::lock_guard<std::mutex> lk(mutex_);

    // Deduplicate: if same text is already at front, skip
    if (!items_.empty() && items_.front().text == text) return;

    // Remove any existing duplicate (non-pinned)
    items_.erase(std::remove_if(items_.begin(), items_.end(),
        [&text](const Entry& e){ return !e.pinned && e.text == text; }),
        items_.end());

    // Prepend new entry
    Entry e;
    e.id        = next_id_++;
    e.pinned    = false;
    e.timestamp = static_cast<uint32_t>(std::time(nullptr));
    e.text      = text;
    items_.push_front(std::move(e));

    // Evict oldest unpinned entries beyond kMaxHistory
    while (items_.size() > kMaxHistory) {
        // Find last unpinned entry
        for (auto it = items_.end() - 1; it >= items_.begin(); --it) {
            if (!it->pinned) { items_.erase(it); break; }
        }
        // All pinned? Just cap
        if (items_.size() > kMaxHistory) { items_.pop_back(); break; }
    }

    saveToDisk();
    // Broadcast outside lock
    shared::XBusMessage bcast{};
    std::strncpy(bcast.service_name_, "com.freebsdx.ClipboardHistory",
                 sizeof(bcast.service_name_) - 1);
    bcast.opcode_ = static_cast<uint32_t>(shared::ClipboardHistoryOpcode::HistoryChanged);
    BroadcastXBusMessage(bcast);
}

// ─── Helpers ──────────────────────────────────────────────────────────────────
void ClipboardHistoryService::broadcastChanged() noexcept {
    shared::XBusMessage bcast{};
    std::strncpy(bcast.service_name_, "com.freebsdx.ClipboardHistory",
                 sizeof(bcast.service_name_) - 1);
    bcast.opcode_ = static_cast<uint32_t>(shared::ClipboardHistoryOpcode::HistoryChanged);
    BroadcastXBusMessage(bcast);
}

void ClipboardHistoryService::sendPage(IpcSocket* client, uint32_t tid, uint32_t page) noexcept {
    shared::ClipHistoryPage pg{};
    pg.page_     = page;
    pg.total_    = static_cast<uint32_t>(items_.size());
    pg.has_more_ = (page + 1 < static_cast<uint32_t>(items_.size())) ? 1u : 0u;
    if (page < static_cast<uint32_t>(items_.size())) {
        pg.valid_ = 1u;
        const auto& e = items_[page];
        pg.entry_.id_        = e.id;
        pg.entry_.pinned_    = e.pinned ? 1u : 0u;
        pg.entry_.timestamp_ = e.timestamp;
        pg.entry_.text_len_  = static_cast<uint32_t>(e.text.size());
        std::strncpy(pg.entry_.text_, e.text.c_str(), sizeof(pg.entry_.text_) - 1);
    }
    shared::XBusMessage reply{};
    reply.transaction_id_ = tid;   // echo so the client's Call callback matches
    std::strncpy(reply.service_name_, "com.freebsdx.ClipboardHistory",
                 sizeof(reply.service_name_) - 1);
    std::memcpy(reply.payload_, &pg, sizeof(pg));
    client->sendMessage({reinterpret_cast<const uint8_t*>(&reply), sizeof(reply)});
}

void ClipboardHistoryService::sendStatus(IpcSocket* client, uint32_t tid, bool ok, uint32_t count) noexcept {
    shared::ClipHistoryStatusResponse resp{};
    resp.success_ = ok ? 1u : 0u;
    resp.count_   = count;
    shared::XBusMessage reply{};
    reply.transaction_id_ = tid;   // echo so the client's Call callback matches
    std::strncpy(reply.service_name_, "com.freebsdx.ClipboardHistory",
                 sizeof(reply.service_name_) - 1);
    std::memcpy(reply.payload_, &resp, sizeof(resp));
    client->sendMessage({reinterpret_cast<const uint8_t*>(&reply), sizeof(reply)});
}

// ─── Message handler ──────────────────────────────────────────────────────────
void ClipboardHistoryService::handleMessage(const shared::XBusMessage& msg,
                                             std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::ClipboardHistoryOpcode;
    const auto op = static_cast<O>(msg.opcode_);

    switch (op) {

    case O::GetPage: {
        shared::ClipHistoryPageRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        std::lock_guard<std::mutex> lk(mutex_);
        sendPage(client.get(), msg.transaction_id_, req.page_);
        break;
    }

    case O::Clear: {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            // Remove all unpinned entries
            items_.erase(std::remove_if(items_.begin(), items_.end(),
                [](const Entry& e){ return !e.pinned; }), items_.end());
            saveToDisk();
        }
        broadcastChanged();
        {
            std::lock_guard<std::mutex> lk(mutex_);
            sendStatus(client.get(), msg.transaction_id_, true, static_cast<uint32_t>(items_.size()));
        }
        LOG_INFO("ClipboardHistoryService: cleared");
        break;
    }

    case O::Pin:
    case O::Unpin: {
        shared::ClipHistoryIdRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        bool found = false;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            for (auto& e : items_) {
                if (e.id == req.id_) {
                    e.pinned = (op == O::Pin);
                    found = true;
                    break;
                }
            }
            if (found) saveToDisk();
        }
        if (found) broadcastChanged();
        {
            std::lock_guard<std::mutex> lk(mutex_);
            sendStatus(client.get(), msg.transaction_id_, found, static_cast<uint32_t>(items_.size()));
        }
        break;
    }

    default: break;
    }
}

} // namespace fbx::xserver

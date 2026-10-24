#include "xserver/notes_service.h"
#include "shared/logger.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

namespace fbx::xserver {

void BroadcastXBusMessage(const shared::XBusMessage&);

// ─── Static member definitions ────────────────────────────────────────────────
std::mutex           NotesService::mutex_;
std::vector<NotesService::Note> NotesService::items_;
uint32_t             NotesService::next_id_{1};
std::string          NotesService::notes_path_;

// ─── Pipe-escape helpers ──────────────────────────────────────────────────────
static std::string pipeEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if      (c == '|')  out += "\\|";
        else if (c == '\n') out += "\\n";
        else if (c == '\\') out += "\\\\";
        else                out += c;
    }
    return out;
}

[[maybe_unused]] static std::string pipeUnescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            ++i;
            if      (s[i] == '|')  out += '|';
            else if (s[i] == 'n')  out += '\n';
            else if (s[i] == '\\') out += '\\';
            else { out += '\\'; out += s[i]; }
        } else {
            out += s[i];
        }
    }
    return out;
}

static std::vector<std::string> splitPipe(const std::string& line) {
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
            fields.push_back(cur);
            cur.clear();
        } else {
            cur += line[i];
        }
    }
    fields.push_back(cur);
    return fields;
}

// ─── Init ─────────────────────────────────────────────────────────────────────
void NotesService::init() noexcept {
    const char* home = std::getenv("HOME");
    notes_path_ = home ? std::string(home) + "/.freebsdx-notes"
                       : "/tmp/.freebsdx-notes";
    loadFromDisk();
    LOG_INFO("NotesService: ready ({} notes)", items_.size());
}

// ─── Disk I/O ─────────────────────────────────────────────────────────────────
// Format: one note per line, 7 pipe-separated fields:
//   id|created|modified|pinned|color|title|body
bool NotesService::loadFromDisk() noexcept {
    std::ifstream f(notes_path_);
    if (!f.is_open()) return true;

    items_.clear();
    next_id_ = 1;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto fields = splitPipe(line);
        if (fields.size() < 7) continue;
        Note n;
        // stoul throws on a corrupt line; skip it (loadFromDisk is noexcept).
        try {
            n.id       = static_cast<uint32_t>(std::stoul(fields[0]));
            n.created  = static_cast<uint32_t>(std::stoul(fields[1]));
            n.modified = static_cast<uint32_t>(std::stoul(fields[2]));
            n.color    = static_cast<uint8_t>(std::stoul(fields[4]));
        } catch (...) { continue; }
        n.pinned   = fields[3] == "1";
        n.title    = fields[5];
        n.body     = fields[6];
        if (n.id >= next_id_) next_id_ = n.id + 1;
        items_.push_back(std::move(n));
    }
    return true;
}

bool NotesService::saveToDisk() noexcept {
    std::ofstream f(notes_path_, std::ios::trunc);
    if (!f.is_open()) return false;
    f << "# FreeBSD-X Notes v1\n";
    for (const auto& n : items_) {
        f << n.id       << '|'
          << n.created  << '|'
          << n.modified << '|'
          << (n.pinned ? '1' : '0') << '|'
          << static_cast<uint32_t>(n.color) << '|'
          << pipeEscape(n.title) << '|'
          << pipeEscape(n.body)  << '\n';
    }
    return true;
}

// ─── Sorted view ─────────────────────────────────────────────────────────────
std::vector<const NotesService::Note*> NotesService::sortedView() noexcept {
    std::vector<const Note*> v;
    v.reserve(items_.size());
    for (const auto& n : items_) v.push_back(&n);
    std::stable_sort(v.begin(), v.end(), [](const Note* a, const Note* b) {
        if (a->pinned != b->pinned) return a->pinned > b->pinned;
        return a->modified > b->modified;
    });
    return v;
}

// ─── Helpers ──────────────────────────────────────────────────────────────────
void NotesService::fillEntry(shared::NoteEntry& out, const Note& n) noexcept {
    std::memset(&out, 0, sizeof(out));
    out.id_       = n.id;
    out.created_  = n.created;
    out.modified_ = n.modified;
    out.pinned_   = n.pinned ? 1 : 0;
    out.color_    = n.color;
    std::strncpy(out.title_, n.title.c_str(), sizeof(out.title_) - 1);
    std::strncpy(out.body_,  n.body.c_str(),  sizeof(out.body_)  - 1);
}

void NotesService::sendEntry(IpcSocket* client, const Note& n) noexcept {
    shared::XBusMessage resp{};
    std::strncpy(resp.service_name_, "com.freebsdx.Notes", sizeof(resp.service_name_) - 1);
    resp.opcode_ = static_cast<uint32_t>(shared::NotesOpcode::Get);

    shared::NotesListPage page{};
    page.page_     = 0;
    page.has_more_ = 0;
    page.total_    = 1;
    page.count_    = 1;
    fillEntry(page.entry_, n);
    std::memcpy(resp.payload_, &page, sizeof(page));
    client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
}

void NotesService::sendStatus(IpcSocket* client, bool ok, uint32_t id) noexcept {
    shared::XBusMessage resp{};
    std::strncpy(resp.service_name_, "com.freebsdx.Notes", sizeof(resp.service_name_) - 1);
    resp.opcode_ = static_cast<uint32_t>(shared::NotesOpcode::Add); // reused as ACK
    shared::NotesStatusResponse sr{};
    sr.success_ = ok ? 1 : 0;
    sr.id_      = id;
    std::memcpy(resp.payload_, &sr, sizeof(sr));
    client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
}

void NotesService::sendListPage(IpcSocket* client,
                                const std::vector<const Note*>& filtered,
                                uint32_t page) noexcept {
    constexpr uint32_t kPerPage = 1;
    uint32_t offset = page * kPerPage;

    shared::XBusMessage resp{};
    std::strncpy(resp.service_name_, "com.freebsdx.Notes", sizeof(resp.service_name_) - 1);
    resp.opcode_ = static_cast<uint32_t>(shared::NotesOpcode::List);

    shared::NotesListPage lp{};
    lp.page_     = page;
    lp.total_    = static_cast<uint32_t>(filtered.size());

    if (offset < filtered.size()) {
        lp.count_    = 1;
        lp.has_more_ = (offset + kPerPage < filtered.size()) ? 1 : 0;
        fillEntry(lp.entry_, *filtered[offset]);
    } else {
        lp.count_    = 0;
        lp.has_more_ = 0;
    }
    std::memcpy(resp.payload_, &lp, sizeof(lp));
    client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
}

void NotesService::broadcastChanged() noexcept {
    shared::XBusMessage bcast{};
    std::strncpy(bcast.service_name_, "com.freebsdx.Notes", sizeof(bcast.service_name_) - 1);
    bcast.opcode_ = static_cast<uint32_t>(shared::NotesOpcode::NoteChanged);
    BroadcastXBusMessage(bcast);
}

// ─── Message handler ─────────────────────────────────────────────────────────
void NotesService::handleMessage(const shared::XBusMessage& msg,
                                 std::shared_ptr<IpcSocket> client) noexcept {
    using Op = shared::NotesOpcode;
    auto op = static_cast<Op>(msg.opcode_);
    std::lock_guard<std::mutex> lk(mutex_);

    if (op == Op::Add) {
        shared::NoteEntry req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.title_[sizeof(req.title_) - 1] = '\0';
        req.body_[sizeof(req.body_)   - 1] = '\0';

        Note n;
        n.id       = next_id_++;
        n.created  = static_cast<uint32_t>(std::time(nullptr));
        n.modified = n.created;
        n.pinned   = req.pinned_ != 0;
        n.color    = req.color_;
        n.title    = req.title_;
        n.body     = req.body_;
        uint32_t new_id = n.id;
        items_.push_back(std::move(n));
        saveToDisk();
        broadcastChanged();
        sendStatus(client.get(), true, new_id);

    } else if (op == Op::Update) {
        shared::NoteEntry req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.title_[sizeof(req.title_) - 1] = '\0';
        req.body_[sizeof(req.body_)   - 1] = '\0';

        auto it = std::find_if(items_.begin(), items_.end(),
                               [&](const Note& n){ return n.id == req.id_; });
        if (it != items_.end()) {
            it->modified = static_cast<uint32_t>(std::time(nullptr));
            it->pinned   = req.pinned_ != 0;
            it->color    = req.color_;
            it->title    = req.title_;
            it->body     = req.body_;
            saveToDisk();
            broadcastChanged();
            sendStatus(client.get(), true, it->id);
        } else {
            sendStatus(client.get(), false);
        }

    } else if (op == Op::Delete) {
        shared::NotesIdRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));

        auto it = std::find_if(items_.begin(), items_.end(),
                               [&](const Note& n){ return n.id == req.id_; });
        if (it != items_.end()) {
            items_.erase(it);
            saveToDisk();
            broadcastChanged();
            sendStatus(client.get(), true);
        } else {
            sendStatus(client.get(), false);
        }

    } else if (op == Op::Get) {
        shared::NotesIdRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));

        auto it = std::find_if(items_.begin(), items_.end(),
                               [&](const Note& n){ return n.id == req.id_; });
        if (it != items_.end()) {
            sendEntry(client.get(), *it);
        } else {
            sendStatus(client.get(), false);
        }

    } else if (op == Op::List) {
        shared::NotesPageRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));

        auto view = sortedView();
        sendListPage(client.get(), view, req.page_);

    } else if (op == Op::Search) {
        shared::NotesSearchRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.query_[sizeof(req.query_) - 1] = '\0';
        std::string q(req.query_, strnlen(req.query_, sizeof(req.query_)));
        // case-insensitive: lowercase query
        std::transform(q.begin(), q.end(), q.begin(),
                       [](unsigned char c){ return std::tolower(c); });

        auto ci_contains = [&](const std::string& s) {
            std::string ls = s;
            std::transform(ls.begin(), ls.end(), ls.begin(),
                           [](unsigned char c){ return std::tolower(c); });
            return ls.find(q) != std::string::npos;
        };

        auto view = sortedView();
        std::vector<const Note*> filtered;
        for (const Note* np : view) {
            if (ci_contains(np->title) || ci_contains(np->body))
                filtered.push_back(np);
        }
        sendListPage(client.get(), filtered, req.page_);

    } else if (op == Op::Pin || op == Op::Unpin) {
        shared::NotesIdRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));

        auto it = std::find_if(items_.begin(), items_.end(),
                               [&](const Note& n){ return n.id == req.id_; });
        if (it != items_.end()) {
            it->pinned = (op == Op::Pin);
            it->modified = static_cast<uint32_t>(std::time(nullptr));
            saveToDisk();
            broadcastChanged();
            sendStatus(client.get(), true, it->id);
        } else {
            sendStatus(client.get(), false);
        }
    }
}

} // namespace fbx::xserver

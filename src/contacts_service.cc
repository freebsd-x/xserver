#include "xserver/contacts_service.h"
#include "shared/logger.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace fbx::xserver {

void BroadcastXBusMessage(const shared::XBusMessage&);

// ─── Static member definitions ────────────────────────────────────────────────
std::mutex            ContactsService::mutex_;
std::vector<ContactsService::Contact> ContactsService::items_;
uint32_t              ContactsService::next_id_{1};
std::string           ContactsService::contacts_path_;

// ─── Pipe-escape helpers ──────────────────────────────────────────────────────
// Fields are stored pipe-separated; literal '|' and '\n' are escaped.
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
void ContactsService::init() noexcept {
    const char* home = std::getenv("HOME");
    contacts_path_ = home ? std::string(home) + "/.freebsdx-contacts"
                          : "/tmp/.freebsdx-contacts";
    loadFromDisk();
    LOG_INFO("ContactsService: ready ({} contacts)", items_.size());
}

// ─── Disk I/O ─────────────────────────────────────────────────────────────────
// Format: one contact per line, 7 pipe-separated fields:
//   id|first_name|last_name|email|phone|organization|notes
bool ContactsService::loadFromDisk() noexcept {
    std::ifstream f(contacts_path_);
    if (!f.is_open()) return true;

    items_.clear();
    next_id_ = 1;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto fields = splitPipe(line);
        if (fields.size() < 7) continue;
        Contact c;
        // stoul throws on a corrupt line; skip it (loadFromDisk is noexcept).
        try { c.id = static_cast<uint32_t>(std::stoul(fields[0])); }
        catch (...) { continue; }
        c.first_name   = fields[1];
        c.last_name    = fields[2];
        c.email        = fields[3];
        c.phone        = fields[4];
        c.organization = fields[5];
        c.notes        = fields[6];
        if (c.id >= next_id_) next_id_ = c.id + 1;
        items_.push_back(std::move(c));
    }
    return true;
}

bool ContactsService::saveToDisk() noexcept {
    std::ofstream f(contacts_path_, std::ios::trunc);
    if (!f.is_open()) return false;
    f << "# FreeBSD-X Contacts v1\n";
    for (const auto& c : items_) {
        f << c.id << '|'
          << pipeEscape(c.first_name)   << '|'
          << pipeEscape(c.last_name)    << '|'
          << pipeEscape(c.email)        << '|'
          << pipeEscape(c.phone)        << '|'
          << pipeEscape(c.organization) << '|'
          << pipeEscape(c.notes)        << '\n';
    }
    return true;
}

// ─── vCard importer ───────────────────────────────────────────────────────────
// Parses a subset of RFC 6350 / vCard 3.0:
//   FN, N, EMAIL, TEL, ORG, NOTE
// Supports CRLF and LF line endings; ignores folded lines for simplicity.
uint32_t ContactsService::importVCard(const std::string& path) noexcept {
    std::ifstream f(path);
    if (!f.is_open()) return 0;

    uint32_t added = 0;
    Contact cur{};
    bool in_vcard = false;

    auto trim = [](std::string s) -> std::string {
        size_t a = s.find_first_not_of(" \t\r");
        size_t b = s.find_last_not_of(" \t\r");
        return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
    };

    // Strip property parameters: "EMAIL;TYPE=INTERNET:value" → "value"
    auto valueAfterColon = [](const std::string& line) -> std::string {
        auto pos = line.find(':');
        return (pos == std::string::npos) ? "" : line.substr(pos + 1);
    };

    auto propName = [](const std::string& line) -> std::string {
        std::string name;
        for (char c : line) {
            if (c == ':' || c == ';') break;
            name += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        return name;
    };

    std::string line;
    while (std::getline(f, line)) {
        // Strip trailing CR
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        std::string prop = propName(line);
        std::string val  = trim(valueAfterColon(line));

        if (prop == "BEGIN" && val == "VCARD") {
            cur = Contact{};
            in_vcard = true;
            continue;
        }
        if (!in_vcard) continue;
        if (prop == "END" && val == "VCARD") {
            // Commit if we have at least a name or email
            if (!cur.first_name.empty() || !cur.last_name.empty() || !cur.email.empty()) {
                cur.id = next_id_++;
                items_.push_back(cur);
                ++added;
            }
            in_vcard = false;
            continue;
        }

        if (prop == "FN" && cur.first_name.empty() && cur.last_name.empty()) {
            // Split "First Last" → first/last
            auto sp = val.find(' ');
            if (sp == std::string::npos) {
                cur.first_name = val;
            } else {
                cur.first_name = val.substr(0, sp);
                cur.last_name  = val.substr(sp + 1);
            }
        } else if (prop == "N") {
            // N:Last;First;Middle;Prefix;Suffix
            auto semi = val.find(';');
            if (semi != std::string::npos) {
                cur.last_name  = val.substr(0, semi);
                auto s2 = val.find(';', semi + 1);
                cur.first_name = val.substr(semi + 1, s2 == std::string::npos
                                            ? std::string::npos : s2 - semi - 1);
            }
        } else if (prop == "EMAIL") {
            if (cur.email.empty()) cur.email = val;
        } else if (prop == "TEL") {
            if (cur.phone.empty()) cur.phone = val;
        } else if (prop == "ORG") {
            // ORG:Company;Department → take company
            auto semi = val.find(';');
            cur.organization = (semi == std::string::npos) ? val : val.substr(0, semi);
        } else if (prop == "NOTE") {
            cur.notes = val;
        }
    }
    return added;
}

// ─── Helpers ──────────────────────────────────────────────────────────────────
void ContactsService::fillEntry(shared::ContactEntry& out, const Contact& c) noexcept {
    out.id_ = c.id;
    std::strncpy(out.first_name_,   c.first_name.c_str(),   sizeof(out.first_name_)   - 1);
    std::strncpy(out.last_name_,    c.last_name.c_str(),    sizeof(out.last_name_)    - 1);
    std::strncpy(out.email_,        c.email.c_str(),        sizeof(out.email_)        - 1);
    std::strncpy(out.phone_,        c.phone.c_str(),        sizeof(out.phone_)        - 1);
    std::strncpy(out.organization_, c.organization.c_str(), sizeof(out.organization_) - 1);
    std::strncpy(out.notes_,        c.notes.c_str(),        sizeof(out.notes_)        - 1);
}

ContactsService::Contact ContactsService::fromEntry(const shared::ContactEntry& e) noexcept {
    Contact c;
    c.id           = e.id_;
    c.first_name   = e.first_name_;
    c.last_name    = e.last_name_;
    c.email        = e.email_;
    c.phone        = e.phone_;
    c.organization = e.organization_;
    c.notes        = e.notes_;
    return c;
}

void ContactsService::sendEntry(IpcSocket* client, uint32_t tid, const Contact& c) noexcept {
    shared::ContactEntry entry{};
    fillEntry(entry, c);
    shared::XBusMessage reply{};
    reply.transaction_id_ = tid;
    std::strncpy(reply.service_name_, "com.freebsdx.Contacts",
                 sizeof(reply.service_name_) - 1);
    std::memcpy(reply.payload_, &entry, sizeof(entry));
    client->sendMessage({reinterpret_cast<const uint8_t*>(&reply), sizeof(reply)});
}

void ContactsService::sendStatus(IpcSocket* client, uint32_t tid, bool ok, uint32_t count) noexcept {
    shared::ContactsStatusResponse resp{};
    resp.success_ = ok ? 1u : 0u;
    resp.count_   = count;
    shared::XBusMessage reply{};
    reply.transaction_id_ = tid;
    std::strncpy(reply.service_name_, "com.freebsdx.Contacts",
                 sizeof(reply.service_name_) - 1);
    std::memcpy(reply.payload_, &resp, sizeof(resp));
    client->sendMessage({reinterpret_cast<const uint8_t*>(&reply), sizeof(reply)});
}

void ContactsService::broadcastChanged() noexcept {
    shared::XBusMessage bcast{};
    std::strncpy(bcast.service_name_, "com.freebsdx.Contacts",
                 sizeof(bcast.service_name_) - 1);
    bcast.opcode_ = static_cast<uint32_t>(shared::ContactsOpcode::ContactChanged);
    BroadcastXBusMessage(bcast);
}

void ContactsService::sendListPage(IpcSocket* client, uint32_t tid,
                                    const std::vector<const Contact*>& filtered,
                                    uint32_t page) noexcept {
    shared::ContactsListPage pg{};
    pg.total_    = static_cast<uint32_t>(filtered.size());
    pg.page_     = page;
    pg.has_more_ = (page + 1 < pg.total_) ? 1u : 0u;
    if (page < static_cast<uint32_t>(filtered.size())) {
        pg.valid_ = 1u;
        fillEntry(pg.entry_, *filtered[page]);
    }
    shared::XBusMessage reply{};
    reply.transaction_id_ = tid;
    std::strncpy(reply.service_name_, "com.freebsdx.Contacts",
                 sizeof(reply.service_name_) - 1);
    std::memcpy(reply.payload_, &pg, sizeof(pg));
    client->sendMessage({reinterpret_cast<const uint8_t*>(&reply), sizeof(reply)});
}

// ─── Message handler ──────────────────────────────────────────────────────────
void ContactsService::handleMessage(const shared::XBusMessage& msg,
                                     std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::ContactsOpcode;
    const auto op = static_cast<O>(msg.opcode_);

    switch (op) {

    case O::Add: {
        shared::ContactEntry entry{};
        std::memcpy(&entry, msg.payload_, sizeof(entry));
        // Null-terminate all strings
        entry.first_name_[sizeof(entry.first_name_) - 1] = '\0';
        entry.last_name_[sizeof(entry.last_name_) - 1]   = '\0';
        entry.email_[sizeof(entry.email_) - 1]           = '\0';
        entry.phone_[sizeof(entry.phone_) - 1]           = '\0';
        entry.organization_[sizeof(entry.organization_) - 1] = '\0';
        entry.notes_[sizeof(entry.notes_) - 1]           = '\0';

        Contact c = fromEntry(entry);
        {
            std::lock_guard<std::mutex> lk(mutex_);
            c.id = next_id_++;
            items_.push_back(c);
            saveToDisk();
        }
        broadcastChanged();
        sendEntry(client.get(), msg.transaction_id_, c);
        LOG_INFO("ContactsService: added id={} {}", c.id, c.first_name + " " + c.last_name);
        break;
    }

    case O::Update: {
        shared::ContactEntry entry{};
        std::memcpy(&entry, msg.payload_, sizeof(entry));
        entry.first_name_[sizeof(entry.first_name_) - 1] = '\0';
        entry.last_name_[sizeof(entry.last_name_) - 1]   = '\0';
        entry.email_[sizeof(entry.email_) - 1]           = '\0';
        entry.phone_[sizeof(entry.phone_) - 1]           = '\0';
        entry.organization_[sizeof(entry.organization_) - 1] = '\0';
        entry.notes_[sizeof(entry.notes_) - 1]           = '\0';

        Contact updated = fromEntry(entry);
        bool found = false;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            for (auto& c : items_) {
                if (c.id == updated.id) {
                    c = updated;
                    found = true;
                    break;
                }
            }
            if (found) saveToDisk();
        }
        if (found) broadcastChanged();
        if (found) sendEntry(client.get(), msg.transaction_id_, updated);
        else       sendStatus(client.get(), msg.transaction_id_, false);
        LOG_INFO("ContactsService: update id={} found={}", updated.id, found);
        break;
    }

    case O::Delete: {
        shared::ContactsIdRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));

        bool removed = false;
        uint32_t remaining = 0;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            auto it = std::find_if(items_.begin(), items_.end(),
                [&](const Contact& c){ return c.id == req.id_; });
            if (it != items_.end()) {
                items_.erase(it);
                saveToDisk();
                removed = true;
            }
            remaining = static_cast<uint32_t>(items_.size());
        }
        if (removed) broadcastChanged();
        sendStatus(client.get(), msg.transaction_id_, removed, remaining);
        LOG_INFO("ContactsService: delete id={} removed={}", req.id_, removed);
        break;
    }

    case O::Get: {
        shared::ContactsIdRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));

        std::lock_guard<std::mutex> lk(mutex_);
        for (const auto& c : items_) {
            if (c.id == req.id_) {
                sendEntry(client.get(), msg.transaction_id_, c);
                return;
            }
        }
        sendStatus(client.get(), msg.transaction_id_, false);
        break;
    }

    case O::List: {
        shared::ContactsPageRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));

        std::lock_guard<std::mutex> lk(mutex_);
        // Sort view: alphabetical by last_name, then first_name
        std::vector<const Contact*> sorted;
        sorted.reserve(items_.size());
        for (const auto& c : items_) sorted.push_back(&c);
        std::sort(sorted.begin(), sorted.end(), [](const Contact* a, const Contact* b) {
            if (a->last_name != b->last_name) return a->last_name < b->last_name;
            return a->first_name < b->first_name;
        });
        sendListPage(client.get(), msg.transaction_id_, sorted, req.page_);
        break;
    }

    case O::Search: {
        shared::ContactsSearchRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.query_[sizeof(req.query_) - 1] = '\0';
        std::string query = req.query_;
        // Case-insensitive substring search
        auto toLower = [](std::string s) {
            for (auto& c : s) c = static_cast<char>(
                std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        std::string ql = toLower(query);

        std::lock_guard<std::mutex> lk(mutex_);
        std::vector<const Contact*> matches;
        for (const auto& c : items_) {
            if (toLower(c.first_name).find(ql)   != std::string::npos ||
                toLower(c.last_name).find(ql)    != std::string::npos ||
                toLower(c.email).find(ql)        != std::string::npos ||
                toLower(c.phone).find(ql)        != std::string::npos ||
                toLower(c.organization).find(ql) != std::string::npos) {
                matches.push_back(&c);
            }
        }
        sendListPage(client.get(), msg.transaction_id_, matches, req.page_);
        break;
    }

    case O::ImportVCard: {
        shared::ContactsImportRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.path_[sizeof(req.path_) - 1] = '\0';
        std::string path = req.path_;

        uint32_t added = 0;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            added = importVCard(path);
            if (added > 0) saveToDisk();
        }
        if (added > 0) broadcastChanged();
        sendStatus(client.get(), msg.transaction_id_, added > 0, added);
        LOG_INFO("ContactsService: importVCard {} → {} contacts", path, added);
        break;
    }

    default: break;
    }
}

} // namespace fbx::xserver

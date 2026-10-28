#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Pure parsers for the per-user services (com.freebsdx.CronJobs, SshKeys,
// Captures). Each service reads a file or `crontab -l` and feeds the text
// here; rewrites are computed here too and written back by the service.
// Dependency-free / unit-testable; no syscalls.
namespace fbx::xserver::userdata {

namespace detail {
inline std::vector<std::string_view> lines(std::string_view s) {
    std::vector<std::string_view> out;
    size_t start = 0;
    while (start < s.size()) {
        size_t nl = s.find('\n', start);
        if (nl == std::string_view::npos) nl = s.size();
        out.push_back(s.substr(start, nl - start));
        start = nl + 1;
    }
    return out;
}
inline bool isSpace(char c) { return c == ' ' || c == '\t'; }
inline std::string_view trim(std::string_view s) {
    while (!s.empty() && isSpace(s.front())) s.remove_prefix(1);
    while (!s.empty() && (isSpace(s.back()) || s.back() == '\r')) s.remove_suffix(1);
    return s;
}
// Next whitespace-delimited token from `s` (advanced past it).
inline std::string_view nextToken(std::string_view& s) {
    s = trim(s);
    size_t i = 0;
    while (i < s.size() && !isSpace(s[i])) ++i;
    std::string_view t = s.substr(0, i);
    s.remove_prefix(i);
    return t;
}
// Rejoin lines, keeping a trailing newline if there's any content.
inline std::string join(const std::vector<std::string>& ls) {
    std::string out;
    for (const auto& l : ls) out += l + "\n";
    return out;
}
inline std::vector<std::string> ownedLines(std::string_view s) {
    std::vector<std::string> out;
    for (auto l : lines(s)) out.emplace_back(l);
    return out;
}
}  // namespace detail

// ── crontab(5) ───────────────────────────────────────────────────────────────

struct CronJob {
    uint32_t    line = 0;       // 1-based line in the crontab — the job id
    std::string schedule;       // "*/5 * * * *" or "@daily"
    std::string command;
    bool        enabled = true; // false: the line is commented out
};

// A schedule field: digits/names/`*`/`/`/`,`/`-`, starting with a digit or `*`
// (so `SHELL=/bin/sh` and prose comments don't look like jobs).
inline bool cronField(std::string_view f, bool first) {
    if (f.empty()) return false;
    if (first && !(f[0] == '*' || (f[0] >= '0' && f[0] <= '9'))) return false;
    for (char c : f) {
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                  (c >= 'A' && c <= 'Z') || c == '*' || c == '/' || c == ',' || c == '-';
        if (!ok) return false;
    }
    return true;
}

// Parse one (uncommented) crontab line as a job.
inline bool parseCronLine(std::string_view line, CronJob& job) {
    std::string_view rest = detail::trim(line);
    if (rest.empty()) return false;
    if (rest[0] == '@') {
        std::string_view kw = detail::nextToken(rest);
        static constexpr std::string_view kKeywords[] = {
            "@reboot", "@yearly", "@annually", "@monthly", "@weekly", "@daily",
            "@midnight", "@hourly", "@every_minute", "@every_second"};
        bool known = false;
        for (auto k : kKeywords) known = known || kw == k;
        if (!known) return false;
        job.schedule = std::string(kw);
    } else {
        std::string sched;
        for (int i = 0; i < 5; ++i) {
            std::string_view f = detail::nextToken(rest);
            if (!cronField(f, i == 0)) return false;
            if (i) sched += ' ';
            sched += f;
        }
        job.schedule = std::move(sched);
    }
    rest = detail::trim(rest);
    if (rest.empty()) return false;
    job.command = std::string(rest);
    return true;
}

// A commented-out line that is otherwise a valid job is a disabled job.
inline std::string_view uncomment(std::string_view line) {
    line = detail::trim(line);
    if (line.empty() || line[0] != '#') return {};
    line.remove_prefix(1);
    return detail::trim(line);
}

// `crontab -l` → jobs, in file order. Comments, blanks and VAR=value lines are
// skipped.
inline std::vector<CronJob> parseCrontab(std::string_view text) {
    std::vector<CronJob> jobs;
    uint32_t n = 0;
    for (auto line : detail::lines(text)) {
        ++n;
        CronJob j;
        j.line = n;
        if (parseCronLine(line, j)) {
            jobs.push_back(std::move(j));
        } else if (auto body = uncomment(line); !body.empty() && parseCronLine(body, j)) {
            j.enabled = false;
            jobs.push_back(std::move(j));
        }
    }
    return jobs;
}

// Rewrite the crontab with job `line` enabled/disabled. False if that line
// isn't a job (stale id).
inline bool setCronEnabled(std::string_view text, uint32_t line, bool enabled, std::string& out) {
    auto ls = detail::ownedLines(text);
    if (line == 0 || line > ls.size()) return false;
    std::string& l = ls[line - 1];
    CronJob j;
    if (parseCronLine(l, j)) {
        if (!enabled) l = "#" + l;
    } else if (auto body = uncomment(l); !body.empty() && parseCronLine(body, j)) {
        if (enabled) l = std::string(body);
    } else {
        return false;
    }
    out = detail::join(ls);
    return true;
}

// cron feeds text after the first unescaped `%` to stdin; RunNow runs only the
// command part, with `\%` unescaped.
inline std::string cronRunnableCommand(std::string_view cmd) {
    std::string out;
    for (size_t i = 0; i < cmd.size(); ++i) {
        if (cmd[i] == '\\' && i + 1 < cmd.size() && cmd[i + 1] == '%') { out += '%'; ++i; continue; }
        if (cmd[i] == '%') break;
        out += cmd[i];
    }
    return out;
}

// ── authorized_keys(5) ───────────────────────────────────────────────────────

struct SshKey {
    uint32_t    line = 0;   // 1-based line — the key id
    std::string algo;       // display name, ≤ 11 chars
    std::string comment;
};

inline bool isKeyType(std::string_view t) {
    return t.starts_with("ssh-") || t.starts_with("ecdsa-") || t.starts_with("sk-");
}

// Short display names for types that overflow SshKeyEntry::algo_[12].
inline std::string shortAlgo(std::string_view t) {
    if (t.starts_with("ecdsa-sha2-nistp")) return "ecdsa-p" + std::string(t.substr(16));
    if (t.starts_with("sk-ssh-ed25519")) return "sk-ed25519";
    if (t.starts_with("sk-ecdsa")) return "sk-ecdsa";
    return std::string(t.substr(0, 11));
}

// Parse one authorized_keys line: [options] keytype base64 [comment]. Options
// may hold quoted spaces (command="…"), so they're skipped quote-aware.
inline bool parseAuthorizedKey(std::string_view line, SshKey& key) {
    std::string_view rest = detail::trim(line);
    if (rest.empty() || rest[0] == '#') return false;
    std::string_view first = rest.substr(0, rest.find_first_of(" \t"));
    if (!isKeyType(first)) {
        bool quoted = false;
        size_t i = 0;
        for (; i < rest.size(); ++i) {
            if (rest[i] == '\\' && quoted) { ++i; continue; }
            if (rest[i] == '"') quoted = !quoted;
            else if (!quoted && detail::isSpace(rest[i])) break;
        }
        rest.remove_prefix(i);
    }
    std::string_view type = detail::nextToken(rest);
    std::string_view blob = detail::nextToken(rest);
    if (!isKeyType(type) || blob.empty()) return false;
    key.algo    = shortAlgo(type);
    key.comment = std::string(detail::trim(rest));
    return true;
}

inline std::vector<SshKey> parseAuthorizedKeys(std::string_view text) {
    std::vector<SshKey> keys;
    uint32_t n = 0;
    for (auto line : detail::lines(text)) {
        ++n;
        SshKey k;
        k.line = n;
        if (parseAuthorizedKey(line, k)) keys.push_back(std::move(k));
    }
    return keys;
}

// Drop key `line`. False if that line isn't a key (stale id).
inline bool removeAuthorizedKey(std::string_view text, uint32_t line, std::string& out) {
    auto ls = detail::ownedLines(text);
    SshKey k;
    if (line == 0 || line > ls.size() || !parseAuthorizedKey(ls[line - 1], k)) return false;
    ls.erase(ls.begin() + (line - 1));
    out = detail::join(ls);
    return true;
}

// known_hosts entries: non-blank, non-comment lines.
inline uint32_t countKnownHosts(std::string_view text) {
    uint32_t n = 0;
    for (auto line : detail::lines(text)) {
        line = detail::trim(line);
        if (!line.empty() && line[0] != '#') ++n;
    }
    return n;
}

// ── Captures ─────────────────────────────────────────────────────────────────

// A capture file name from the wire: a plain, visible name — no path
// separators, no `.`/`..`/dotfiles — so it can't escape the capture folder.
inline bool safeCaptureName(std::string_view name) {
    if (name.empty() || name.size() > 63 || name[0] == '.') return false;
    for (char c : name)
        if (c == '/' || c == '\0' || c == '\n') return false;
    return true;
}

}  // namespace fbx::xserver::userdata

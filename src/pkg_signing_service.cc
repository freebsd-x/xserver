#include "xserver/pkg_signing_service.h"
#include "xserver/async_shell.h"
#include "xserver/ipc_socket.h"
#include "xserver/xbus_dispatcher.h"
#include "xserver/shell_util.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace fbx::xserver {

using namespace shared;

// ── Internal helpers ──────────────────────────────────────────────────────────

std::string PkgSigningService::trustDir() noexcept {
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.freebsdx-pkg-trust"
                : "/tmp/.freebsdx-pkg-trust";
}

std::string PkgSigningService::keysDir() noexcept {
    return trustDir() + "/keys";
}

void PkgSigningService::broadcastChanged() noexcept {
    XBusMessage msg{};
    msg.magic_ = XBUS_MAGIC;
    std::strncpy(msg.service_name_, "com.freebsdx.PackageSigning",
                 sizeof(msg.service_name_) - 1);
    msg.opcode_ = static_cast<uint32_t>(PackageSigningOpcode::SigningChanged);
    BroadcastXBusMessage(msg);
}

// Parse the first two lines of a minisign .pub file.
// Returns {fingerprint, label}.
// For GPG .asc files returns {stem, stem}.
static std::pair<std::string,std::string> parsePubKey(const fs::path& p) noexcept {
    std::ifstream f(p);
    if (!f) return {p.stem().string(), p.stem().string()};

    std::string line1, line2;
    std::getline(f, line1);
    std::getline(f, line2);

    // minisign: "untrusted comment: <label>\n<base64-key>"
    std::string label;
    const char* prefix = "untrusted comment: ";
    if (line1.size() > std::strlen(prefix) &&
        line1.compare(0, std::strlen(prefix), prefix) == 0) {
        label = line1.substr(std::strlen(prefix));
    } else {
        label = p.stem().string();
    }

    // Use first 16 alphanumeric chars of the base64 key as fingerprint
    std::string fp;
    for (char c : line2) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            fp += c;
            if (fp.size() == 16) break;
        }
    }
    if (fp.empty()) fp = p.stem().string();
    return {fp, label};
}

// ── VerifyPackage ─────────────────────────────────────────────────────────────

PkgSigningService::VerifyResult
PkgSigningService::VerifyPackage(const std::string& xpkg_path) noexcept {
    std::string minisig = xpkg_path + ".minisig";
    std::string gpgsig  = xpkg_path + ".sig";

    bool has_minisig = fs::exists(minisig);
    bool has_gpgsig  = !has_minisig && fs::exists(gpgsig);

    if (!has_minisig && !has_gpgsig) {
        return {false, "no signature sidecar (.minisig or .sig) found"};
    }

    std::string kdir = keysDir();

    if (has_minisig) {
        // Try each trusted .pub key
        try {
            if (fs::exists(kdir)) {
                for (const auto& e : fs::directory_iterator(kdir)) {
                    if (!e.is_regular_file()) continue;
                    if (e.path().extension() != ".pub") continue;

                    // All paths must be shell-quoted: a crafted package path with
                    // an embedded quote could otherwise inject a command that
                    // forces system() to return 0 — a verification bypass.
                    std::string cmd = "minisign -V -q -p " + shellQuote(e.path().string())
                                    + " -m " + shellQuote(xpkg_path)
                                    + " -x " + shellQuote(minisig)
                                    + " 2>/dev/null";
                    if (fbx::xserver::spawnStatus(cmd.c_str()) == 0) {
                        auto [fp, lbl] = parsePubKey(e.path());
                        return {true, "verified by key: " + (lbl.empty() ? fp : lbl)};
                    }
                }
            }
        } catch (...) {}
        return {false, "minisign signature present but not trusted by any imported key"};
    }

    // GPG detached signature fallback
    std::string cmd = "gpg --quiet --batch --verify "
                    + shellQuote(gpgsig) + " " + shellQuote(xpkg_path) + " 2>/dev/null";
    if (fbx::xserver::spawnStatus(cmd.c_str()) == 0) {
        return {true, "verified via GPG"};
    }
    return {false, "GPG signature verification failed or key not in keyring"};
}

// ── GetPolicy ─────────────────────────────────────────────────────────────────

PkgSigningPolicy PkgSigningService::GetPolicy() noexcept {
    PkgSigningPolicy policy{};
    policy.require_signed_ = 0;
    policy.allow_unknown_  = 1;  // default: warn but allow unsigned

    fs::path pfile = fs::path(trustDir()) / "policy";
    std::ifstream f(pfile);
    if (!f) return policy;

    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("require_signed=", 0) == 0)
            policy.require_signed_ = (line[15] == '1') ? 1u : 0u;
        else if (line.rfind("allow_unknown=", 0) == 0)
            policy.allow_unknown_ = (line[14] == '1') ? 1u : 0u;
    }
    return policy;
}

// ── ProcessMessage ─────────────────────────────────────────────────────────────

void PkgSigningService::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                       const XBusMessage& msg) noexcept {
    XBusMessage resp = msg;

    auto send_status = [&](bool ok, const std::string& text) {
        PkgSigningStatusResponse sr{};
        sr.success_ = ok ? 1u : 0u;
        std::strncpy(sr.message_, text.c_str(), sizeof(sr.message_) - 1);
        std::memset(resp.payload_, 0, sizeof(resp.payload_));
        std::memcpy(resp.payload_, &sr, sizeof(sr));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
    };

    switch (static_cast<PackageSigningOpcode>(msg.opcode_)) {

    // ── ImportKey ────────────────────────────────────────────────────────────
    case PackageSigningOpcode::ImportKey: {
        PkgKeyImportRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.path_[sizeof(req.path_) - 1] = '\0';
        std::string src(req.path_, strnlen(req.path_, sizeof(req.path_)));

        if (src.empty() || !fs::exists(src)) {
            send_status(false, "Key file not found: " + src);
            break;
        }

        std::string kdir = keysDir();
        try { fs::create_directories(kdir); }
        catch (...) { send_status(false, "Cannot create trust directory"); break; }

        // Detect key type from extension or first line
        std::string ext = fs::path(src).extension().string();
        bool is_minisign = (ext == ".pub");
        bool is_gpg = (!is_minisign && (ext == ".asc" || ext == ".gpg" || ext == ".pgp"));
        if (!is_minisign && !is_gpg) {
            std::ifstream f(src);
            std::string fl;
            std::getline(f, fl);
            is_minisign = (fl.find("untrusted comment") != std::string::npos);
            is_gpg      = (fl.find("-----BEGIN PGP") != std::string::npos);
            if (!is_minisign && !is_gpg) {
                send_status(false, "Unrecognized key format (need .pub, .asc, or .gpg)");
                break;
            }
        }

        // Unique filename: timestamp-based
        std::string ts  = std::to_string(std::time(nullptr));
        std::string dest_name = ts + (is_minisign ? ".pub" : ".asc");
        fs::path dest = fs::path(kdir) / dest_name;

        try {
            fs::copy_file(src, dest, fs::copy_options::overwrite_existing);
        } catch (const std::exception& ex) {
            send_status(false, std::string("Copy failed: ") + ex.what());
            break;
        }

        // For GPG keys, also import into the GPG keyring so --verify works
        if (is_gpg) {
            std::string cmd = "gpg --import " + shellQuote(src) + " 2>/dev/null";
            fbx::xserver::spawnStatus(cmd.c_str());
        }

        auto [fp, lbl] = is_minisign
                         ? parsePubKey(dest)
                         : std::make_pair(ts, fs::path(src).stem().string());

        std::cout << "[PkgSigning] Imported key: " << lbl << " (" << fp << ")\n";
        broadcastChanged();
        send_status(true, "Key imported: " + (lbl.empty() ? dest_name : lbl));
        break;
    }

    // ── RemoveKey ────────────────────────────────────────────────────────────
    case PackageSigningOpcode::RemoveKey: {
        PkgKeyIdRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.fingerprint_[sizeof(req.fingerprint_) - 1] = '\0';
        std::string fp(req.fingerprint_, strnlen(req.fingerprint_, sizeof(req.fingerprint_)));

        std::string kdir = keysDir();
        bool removed = false;
        try {
            if (fs::exists(kdir)) {
                for (const auto& e : fs::directory_iterator(kdir)) {
                    if (!e.is_regular_file()) continue;
                    std::string stem = e.path().stem().string();
                    auto [efp, lbl] = parsePubKey(e.path());
                    if (stem == fp || efp == fp || efp.rfind(fp, 0) == 0) {
                        fs::remove(e.path());
                        removed = true;
                        break;
                    }
                }
            }
        } catch (...) {}

        if (removed) broadcastChanged();
        send_status(removed, removed ? "Key removed." : "Key not found: " + fp);
        break;
    }

    // ── ListKeys ─────────────────────────────────────────────────────────────
    case PackageSigningOpcode::ListKeys: {
        PkgKeyPageRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));

        std::string kdir = keysDir();
        std::vector<fs::path> files;
        try {
            if (fs::exists(kdir)) {
                for (const auto& e : fs::directory_iterator(kdir)) {
                    if (e.is_regular_file()) files.push_back(e.path());
                }
            }
        } catch (...) {}
        std::sort(files.begin(), files.end());

        constexpr uint32_t kPage = 3;
        uint32_t total  = static_cast<uint32_t>(files.size());
        uint32_t offset = req.page_ * kPage;

        PkgKeyListPage lp{};
        lp.page_     = req.page_;
        lp.total_    = total;
        lp.has_more_ = (offset + kPage < total) ? 1u : 0u;

        for (uint32_t i = offset; i < total && lp.count_ < kPage; ++i) {
            auto [fp, lbl] = parsePubKey(files[i]);
            std::string ext = files[i].extension().string();
            const char* type = (ext == ".pub") ? "ms" : "gpg";

            PkgKeyEntry& ke = lp.entries_[lp.count_++];
            std::strncpy(ke.fingerprint_, fp.c_str(),  sizeof(ke.fingerprint_) - 1);
            std::strncpy(ke.name_,        lbl.c_str(), sizeof(ke.name_)        - 1);
            std::strncpy(ke.type_,        type,        sizeof(ke.type_)        - 1);
        }

        std::memset(resp.payload_, 0, sizeof(resp.payload_));
        std::memcpy(resp.payload_, &lp, sizeof(lp));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        break;
    }

    // ── SetPolicy ────────────────────────────────────────────────────────────
    case PackageSigningOpcode::SetPolicy: {
        PkgSigningPolicy policy{};
        std::memcpy(&policy, msg.payload_, sizeof(policy));

        std::string tdir = trustDir();
        try { fs::create_directories(tdir); } catch (...) {}

        std::ofstream f(fs::path(tdir) / "policy");
        if (f) {
            f << "require_signed=" << policy.require_signed_ << "\n";
            f << "allow_unknown=" << policy.allow_unknown_ << "\n";
        }
        std::cout << "[PkgSigning] Policy: require_signed=" << policy.require_signed_
                  << " allow_unknown=" << policy.allow_unknown_ << "\n";
        broadcastChanged();
        send_status(true, "Policy updated.");
        break;
    }

    // ── GetPolicy ────────────────────────────────────────────────────────────
    case PackageSigningOpcode::GetPolicy: {
        auto policy = GetPolicy();
        std::memset(resp.payload_, 0, sizeof(resp.payload_));
        std::memcpy(resp.payload_, &policy, sizeof(policy));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        break;
    }

    default:
        std::cerr << "[PkgSigning] Unknown opcode 0x"
                  << std::hex << msg.opcode_ << std::dec << "\n";
        break;
    }
}

} // namespace fbx::xserver

// The parsers the compositor runs on downloaded or third-party data —
// mini_json (app manifests, the package catalog) and adblock (the hosts
// blocklist) — on correct, hostile and randomly mutated input. Run under
// ASan+UBSan by scripts/sanitize-tests.sh, an out-of-bounds read or UB in any
// of them fails the suite. Deterministic (fixed seed) so a failure reproduces.

#include "xserver/adblock.h"
#include "xserver/mini_json.h"

#include <gtest/gtest.h>

#include <chrono>
#include <random>
#include <string>

namespace mj = fbx::xserver::mini_json;
namespace ab = fbx::xserver::adblock;

namespace {

const std::string kCatalog = R"({"updated": "2026-11-02", "packages": [
  {"identifier": "com.example.a", "name": "Alpha", "version": "1.0", "size_kb": 120,
   "download_url": "https://pkg.freebsd-x.org/a.xpkg"},
  {"identifier": "com.example.b", "name": "Beta", "size_kb": "x9"}
]})";
const std::string kManifest = R"({"name": "Maps", "identifier": "com.freebsdx.maps",
  "executable": "bin/maps", "mime_types": ["text/x-a", "text/x-b"]})";
const std::string kHosts = "# comment\n0.0.0.0 ads.example.com\n127.0.0.1 localhost\n"
                           "0.0.0.0 tracker.example.net # trailing\n::1 ip6-localhost\n";

// One random edit: flip, insert a structural character, delete, truncate or
// duplicate a span.
std::string mutate(std::string s, std::mt19937& rng) {
    static const char kStructural[] = "\"{}[]:,\\ \n\0";
    std::uniform_int_distribution<int> op(0, 4);
    auto at = [&](size_t n) { return n ? std::uniform_int_distribution<size_t>(0, n - 1)(rng) : 0; };
    switch (op(rng)) {
    case 0: if (!s.empty()) s[at(s.size())] = static_cast<char>(rng()); break;
    case 1: s.insert(s.begin() + static_cast<long>(at(s.size() + 1)), kStructural[at(sizeof(kStructural) - 1)]); break;
    case 2: if (!s.empty()) s.erase(at(s.size()), 1 + at(8)); break;
    case 3: s.resize(at(s.size() + 1)); break;
    case 4: if (!s.empty()) { size_t p = at(s.size()); s.insert(p, s.substr(p, 1 + at(32))); } break;
    }
    return s;
}

}  // namespace

TEST(MiniJson, ReadsTheCatalogAndManifest) {
    auto pkgs = mj::objectArray(kCatalog, "packages");
    ASSERT_EQ(pkgs.size(), 2u);
    EXPECT_EQ(pkgs[0]["identifier"], "com.example.a");
    EXPECT_EQ(pkgs[0]["size_kb"], "120");
    EXPECT_EQ(pkgs[0]["download_url"], "https://pkg.freebsd-x.org/a.xpkg");
    EXPECT_EQ(pkgs[1]["name"], "Beta");
    EXPECT_EQ(mj::value(kManifest, "identifier"), "com.freebsdx.maps");
    auto types = mj::array(kManifest, "mime_types");
    ASSERT_EQ(types.size(), 2u);
    EXPECT_EQ(types[1], "text/x-b");
}

TEST(MiniJson, HostileShapesDontEscapeTheirBounds) {
    for (const char* s : {"", "\"", "\"packages\"", "\"packages\":[", "\"packages\":[{", "\"packages\":[{\"a\"",
                          "\"packages\":[{\"a\":", "\"packages\":[{\"a\": 5", "\"packages\":[{\"a\":\"x}]",
                          "\"packages\":[{}{}{]", "\"k\":\"", "\"k\": [\"a", "\"k\": [\"a\"", "]{[\"k\":}"}) {
        (void)mj::objectArray(s, "packages");
        (void)mj::value(s, "k");
        (void)mj::array(s, "k");
    }
    // A value can't run past its object into the next one.
    auto pkgs = mj::objectArray(R"("packages":[{"a": 1}, {"b": "2"}])", "packages");
    ASSERT_EQ(pkgs.size(), 2u);
    EXPECT_EQ(pkgs[0]["a"], "1");
}

TEST(MiniJson, LargeCatalogParsesInLinearTime) {
    // A catalog of 200k packages (a hostile server could send far bigger):
    // scanning it used to re-search the array end per object — quadratic.
    std::string big = "{\"packages\": [";
    for (int i = 0; i < 200000; ++i)
        big += "{\"identifier\": \"com.x." + std::to_string(i) + "\", \"name\": \"n\"},";
    big += "]}";
    auto t0 = std::chrono::steady_clock::now();
    auto pkgs = mj::objectArray(big, "packages");
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    EXPECT_EQ(pkgs.size(), 200000u);
    EXPECT_LT(ms, 5000) << "catalog scan is no longer linear";
}

TEST(ParserFuzz, MiniJsonSurvivesMutatedInput) {
    std::mt19937 rng(20261102);
    for (const std::string& seed : {kCatalog, kManifest}) {
        std::string s = seed;
        for (int i = 0; i < 20000; ++i) {
            s = (i % 50 == 0) ? seed : mutate(s, rng);   // drift, re-anchored now and then
            (void)mj::objectArray(s, "packages");
            (void)mj::value(s, "identifier");
            (void)mj::array(s, "mime_types");
        }
    }
}

TEST(ParserFuzz, AdblockSurvivesMutatedInput) {
    std::mt19937 rng(20261102);
    std::string s = kHosts;
    for (int i = 0; i < 20000; ++i) {
        s = (i % 50 == 0) ? kHosts : mutate(s, rng);
        auto domains = ab::parseList(s);
        for (const auto& d : domains) EXPECT_TRUE(ab::validDomain(d)) << d;
        (void)ab::blockedCount(s);
        (void)ab::spliceHosts(s, domains);
    }
}

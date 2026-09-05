// mcpp.build.version_floor — "this needs at least that much of something".
//
// WHY THIS EXISTS
//
// Some facts about a machine bound what may be built for it, and the failure
// when they are ignored arrives late. The case this was written for: a device
// runtime must not be newer than the driver it runs against, and when it is,
// the build compiles and links cleanly and then fails at the first allocation
// with a message that names neither the toolkit nor the driver.
//
// Both numbers are knowable before anything is compiled. What mcpp must not do
// is go and ask a vendor's tool for them — `tests/unit/test_runtime_contract`
// refuses provider-specific probes in `src/`, and rightly: an engine that
// learns to run one vendor's probe learns to run four. So the numbers arrive as
// DECLARATIONS and this module compares them.
//
// WHAT THE DECLARATIONS LOOK LIKE
//
// A package that needs something states a floor, in `runtime.requirements`:
//
//     [[runtime.requirements]]
//     kind  = "version-floor"
//     value = "cuda.driver >= 12.0"
//
// A package that KNOWS a fact about this machine states it, in
// `runtime.provides`, having established it at install time:
//
//     provides = ["cuda.driver=12.4"]
//
// NO VENDOR NAME APPEARS IN THIS FILE, and that is the point rather than a
// coincidence. `cuda.driver` is data flowing through: this module reads a name,
// a relation and a version, and knows nothing about what any of them mean. A
// second backend needs no change here.
//
// WHAT A VERSION IS
//
// A dot-separated sequence of integers, compared component by component, with a
// missing component reading as zero — so `12` and `12.0` are the same version
// and `12.4` is above both. Anything that is not that shape yields no version,
// and an absent version on either side yields no claim: a check that cannot
// reach an answer must not manufacture a refusal.

export module mcpp.build.version_floor;

import std;

export namespace mcpp::build {

// A parsed `<name> >= <version>` requirement.
struct VersionFloor {
    std::string name;
    std::string version;
    bool valid() const { return !name.empty() && !version.empty(); }
};

// A parsed `<name>=<version>` fact.
struct VersionFact {
    std::string name;
    std::string version;
    bool valid() const { return !name.empty() && !version.empty(); }
};

// Read `<name> >= <version>`. Whitespace around each part is ignored; any other
// shape yields an invalid result rather than a guess.
VersionFloor parse_version_floor(std::string_view text);

// Read `<name>=<version>`. The separator is a bare `=` so the spelling matches
// the capability strings a descriptor already writes.
VersionFact parse_version_fact(std::string_view text);

// Is `have` at or above `want`? Both are dot-separated integers; a missing
// component reads as zero.
//
// Returns std::nullopt when either side is not a version, which callers report
// as "no claim" rather than as a failure.
std::optional<bool> version_at_least(std::string_view have, std::string_view want);

} // namespace mcpp::build

namespace mcpp::build {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t')) s.remove_suffix(1);
    return s;
}

// The components of a dotted version, or an empty vector when the text is not
// one. A trailing or leading dot makes it not one: `12.` is a typo, not `12`.
std::optional<std::vector<long long>> components(std::string_view v) {
    v = trim(v);
    if (v.empty()) return std::nullopt;
    std::vector<long long> out;
    long long acc = 0;
    bool digits = false;
    for (char c : v) {
        if (c == '.') {
            if (!digits) return std::nullopt;
            out.push_back(acc);
            acc = 0;
            digits = false;
            continue;
        }
        if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
        acc = acc * 10 + (c - '0');
        digits = true;
    }
    if (!digits) return std::nullopt;
    out.push_back(acc);
    return out;
}

} // namespace

VersionFloor parse_version_floor(std::string_view text) {
    VersionFloor f;
    auto at = text.find(">=");
    if (at == std::string_view::npos) return f;
    auto name = trim(text.substr(0, at));
    auto ver  = trim(text.substr(at + 2));
    if (name.empty() || ver.empty()) return f;
    if (!components(ver)) return f;
    f.name    = std::string(name);
    f.version = std::string(ver);
    return f;
}

VersionFact parse_version_fact(std::string_view text) {
    VersionFact f;
    auto at = text.find('=');
    if (at == std::string_view::npos) return f;
    // `>=` is a floor, not a fact; refusing it here keeps one spelling from
    // being read as the other when both live in string lists.
    if (at > 0 && text[at - 1] == '>') return f;
    auto name = trim(text.substr(0, at));
    auto ver  = trim(text.substr(at + 1));
    if (name.empty() || ver.empty()) return f;
    if (!components(ver)) return f;
    f.name    = std::string(name);
    f.version = std::string(ver);
    return f;
}

std::optional<bool> version_at_least(std::string_view have, std::string_view want) {
    auto h = components(have);
    auto w = components(want);
    if (!h || !w) return std::nullopt;
    const auto n = std::max(h->size(), w->size());
    for (std::size_t i = 0; i < n; ++i) {
        const long long a = i < h->size() ? (*h)[i] : 0;
        const long long b = i < w->size() ? (*w)[i] : 0;
        if (a != b) return a > b;
    }
    return true;
}

} // namespace mcpp::build

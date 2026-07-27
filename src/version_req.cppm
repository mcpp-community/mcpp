// mcpp.version_req — parse + match a SemVer-subset requirement grammar.
//
// Grammar (subset):
//   "1.2.3"        → caret-default: >=1.2.3, <2.0.0
//   "^1.2.3"       → explicit caret
//   "~1.2.3"       → tilde:           >=1.2.3, <1.3.0
//   "=1.2.3"       → exact:           ==1.2.3
//   ">=1.2, <2.0"  → comma-separated comparators (AND)
//   "*"            → any
//   ""             → any (treated as *)
//
// Versions: major.minor.patch[.revision] with all parts ≥ 0; missing parts
// default to 0 (e.g. "1.2" == "1.2.0", "1" == "1.0.0").
//
// The fourth segment exists for mcpp's own date-based scheme (YYYY.M.D.N,
// e.g. "2026.7.27.1"). Without it the parser silently truncated, making every
// release of a given day compare EQUAL — which quietly disabled the E0006
// index-floor check. Ordinary three-segment SemVer is unaffected: the fourth
// component is 0 on both sides, so ^ / ~ / = behave exactly as before.
//
// Pre-release / build metadata are NOT supported in M4 V1 — versions
// containing '-' or '+' are still parsed by stripping after first such
// char (matches semver "prerelease ignored for M4 V1" stance).

export module mcpp.version_req;

import std;

export namespace mcpp::version_req {

struct Version {
    int major = 0, minor = 0, patch = 0, revision = 0;
    // How many segments the source string actually wrote. Only str() reads
    // it — it must NOT take part in ordering, or "1.2" and "1.2.0" would
    // compare unequal.
    int components = 0;

    // Explicit, not `= default`: the defaulted <=> would compare
    // `components` too. Order is the four numbers, nothing else.
    std::strong_ordering operator<=>(const Version& o) const {
        if (auto c = major    <=> o.major;    c != 0) return c;
        if (auto c = minor    <=> o.minor;    c != 0) return c;
        if (auto c = patch    <=> o.patch;    c != 0) return c;
        return revision <=> o.revision;
    }
    bool operator==(const Version& o) const { return (*this <=> o) == 0; }

    // Three segments are the floor, so every version that parsed before this
    // field existed still renders byte-identically ("1.2" → "1.2.0"). The
    // fourth is appended only when the source actually wrote it.
    //
    // Load-bearing: pm/resolver.cppm returns str() as the RESOLVED dependency
    // version, which flows into the lock file and the xlings wire address —
    // so this has to reproduce the index's literal version key. In particular
    // a date version ending in ".0" (mcpp's formal-release convention) must
    // stay four segments; collapsing it to "2026.8.1" would address a key
    // that does not exist.
    std::string str() const {
        auto base = std::format("{}.{}.{}", major, minor, patch);
        return components >= 4 ? std::format("{}.{}", base, revision) : base;
    }
};

std::expected<Version, std::string> parse_version(std::string_view s);

enum class Op { Eq, Gt, Ge, Lt, Le, Caret, Tilde };

struct Comparator {
    Op       op;
    Version  v;
};

struct Requirement {
    bool                       any = false;
    std::vector<Comparator>    parts;       // AND-combined
};

std::expected<Requirement, std::string> parse_req(std::string_view s);

bool matches(const Requirement& r, const Version& v);

// Pick the highest version from `available` matching `req`. Returns the
// chosen version's index, or nullopt if none match.
std::optional<std::size_t>
choose(const Requirement& req, const std::vector<Version>& available);

} // namespace mcpp::version_req

namespace mcpp::version_req {

std::expected<Version, std::string> parse_version(std::string_view s) {
    // Strip prerelease/build metadata for M4 V1.
    if (auto dash = s.find_first_of("-+"); dash != std::string_view::npos) {
        s = s.substr(0, dash);
    }
    Version v;
    int* parts[4] = { &v.major, &v.minor, &v.patch, &v.revision };
    int idx = 0;
    std::size_t i = 0;
    while (idx < 4 && i <= s.size()) {
        std::size_t start = i;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
        if (start == i) {
            if (idx == 0)
                return std::unexpected(std::format("version: not a number ('{}')", s));
            break;     // missing minor/patch → 0
        }
        int n = 0;
        for (std::size_t k = start; k < i; ++k) n = n * 10 + (s[k] - '0');
        *parts[idx++] = n;
        if (i < s.size() && s[i] == '.') ++i;
        else break;
    }
    v.components = idx;
    return v;
}

namespace {

std::string_view strip_ws(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t')) s.remove_suffix(1);
    return s;
}

std::expected<Comparator, std::string> parse_comparator(std::string_view s) {
    s = strip_ws(s);
    if (s.empty()) return std::unexpected("empty comparator");

    Op op;
    std::size_t skip = 0;
    if      (s.starts_with(">=")) { op = Op::Ge;    skip = 2; }
    else if (s.starts_with("<=")) { op = Op::Le;    skip = 2; }
    else if (s.starts_with(">"))  { op = Op::Gt;    skip = 1; }
    else if (s.starts_with("<"))  { op = Op::Lt;    skip = 1; }
    else if (s.starts_with("="))  { op = Op::Eq;    skip = 1; }
    else if (s.starts_with("^"))  { op = Op::Caret; skip = 1; }
    else if (s.starts_with("~"))  { op = Op::Tilde; skip = 1; }
    else                           { op = Op::Caret; skip = 0; }   // bare = caret

    auto v = parse_version(strip_ws(s.substr(skip)));
    if (!v) return std::unexpected(v.error());
    return Comparator{op, *v};
}

} // namespace

std::expected<Requirement, std::string> parse_req(std::string_view s) {
    s = strip_ws(s);
    Requirement r;
    if (s.empty() || s == "*") { r.any = true; return r; }

    std::size_t i = 0;
    while (i < s.size()) {
        std::size_t start = i;
        while (i < s.size() && s[i] != ',') ++i;
        auto piece = s.substr(start, i - start);
        auto cmp = parse_comparator(piece);
        if (!cmp) return std::unexpected(cmp.error());
        r.parts.push_back(*cmp);
        if (i < s.size() && s[i] == ',') ++i;
    }
    return r;
}

bool matches(const Requirement& r, const Version& v) {
    if (r.any) return true;
    for (auto& c : r.parts) {
        switch (c.op) {
            case Op::Eq: if (!(v == c.v)) return false; break;
            case Op::Gt: if (!(v >  c.v)) return false; break;
            case Op::Ge: if (!(v >= c.v)) return false; break;
            case Op::Lt: if (!(v <  c.v)) return false; break;
            case Op::Le: if (!(v <= c.v)) return false; break;
            case Op::Caret: {
                // ^X.Y.Z = >=X.Y.Z, <(X+1).0.0   (leftmost-nonzero rule)
                // For simplicity here: bump major; if major==0 bump minor; if both 0 bump patch.
                // Every branch must also zero `revision`, or the upper bound
                // inherits the constraint's own fourth segment and wrongly
                // excludes releases below it (^2026.7.27.3 would cut off
                // 2027.0.0.0..2).
                Version upper = c.v;
                upper.revision = 0;
                if (c.v.major != 0)      { ++upper.major; upper.minor = 0; upper.patch = 0; }
                else if (c.v.minor != 0) { ++upper.minor; upper.patch = 0; }
                else                      { ++upper.patch; }
                if (!(v >= c.v && v < upper)) return false;
                break;
            }
            case Op::Tilde: {
                // ~X.Y.Z = >=X.Y.Z, <X.(Y+1).0
                Version upper = c.v;
                ++upper.minor;  upper.patch = 0;  upper.revision = 0;
                if (!(v >= c.v && v < upper)) return false;
                break;
            }
        }
    }
    return true;
}

std::optional<std::size_t>
choose(const Requirement& req, const std::vector<Version>& available) {
    std::optional<std::size_t> best;
    for (std::size_t i = 0; i < available.size(); ++i) {
        if (!matches(req, available[i])) continue;
        if (!best || available[i] > available[*best]) best = i;
    }
    return best;
}

} // namespace mcpp::version_req

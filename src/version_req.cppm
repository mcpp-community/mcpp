// mcpp.version_req — parse + match a SemVer requirement grammar.
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
// A VERSION IS AN ORDER, NOT AN IDENTITY (mcpp#363)
//
// What this module produces is a place in a total order. It is NOT the thing
// that addresses a package: `Version::str()` is a rendering, and a rendering
// cannot reproduce an arbitrary index key. Before #363, pm/resolver.cppm parsed
// the index's literal version keys, threw them away, and re-rendered an address
// out of the parsed numbers — so `1.92.8-docking` and `25.0.4.7.1` resolved to
// addresses (`1.92.8`, `25.0.4.7`) that do not exist. The literal key now travels
// with the order (`pm::VersionCandidate`), and `str()` is DISPLAY ONLY. Do not
// re-introduce a code path that turns a Version back into an address.
//
// Numbers: an arbitrary-length dot-separated list, all parts ≥ 0; absent parts
// compare as 0 (so "1.2" == "1.2.0" == "1.2.0.0"). Fixed at four segments until
// #363: mcpp's own scheme needs a fourth (YYYY.M.D.N), and the real index has
// five-segment keys (`jdk-corretto` publishes `25.0.4.7.1`), which used to be
// truncated to four — making `25.0.4.7.1` and a hypothetical `25.0.4.7.2`
// compare EQUAL. The list has no length limit for the same reason the fourth
// segment was added: a truncating comparison silently merges distinct releases.
//
// Pre-release (`-rc.1`) is ordered per SemVer §11: a release outranks any
// pre-release of the same numbers, and identifiers compare dot-segment by
// dot-segment with numeric < alphanumeric.
//
// Build metadata (`+7`) is deliberately NOT stored: SemVer §10 excludes it from
// precedence, so two keys differing only in metadata are EQUAL here. They are
// still different addresses — which is exactly why the literal has to be carried
// separately, and why the resolver treats such a tie as an error rather than
// picking one.

export module mcpp.version_req;

import std;

export namespace mcpp::version_req {

// One dot-separated pre-release identifier. SemVer §11.4: numeric identifiers
// always rank below alphanumeric ones; numeric compare numerically (so `rc.9` <
// `rc.10`), alphanumeric compare by ASCII.
struct PreId {
    bool          numeric = false;
    std::uint64_t num     = 0;    // when `numeric`
    std::string   text;           // when !`numeric`

    std::strong_ordering operator<=>(const PreId& o) const {
        if (numeric != o.numeric)
            return numeric ? std::strong_ordering::less : std::strong_ordering::greater;
        if (numeric) return num <=> o.num;
        return text <=> o.text;
    }
    bool operator==(const PreId& o) const { return (*this <=> o) == 0; }
};

struct Version {
    // Dot-separated numeric segments as WRITTEN. Absent segments read as 0 via
    // seg(); the stored length only affects str().
    std::vector<std::int64_t> nums;
    std::vector<PreId>        prerelease;   // empty = a release

    std::int64_t seg(std::size_t i) const { return i < nums.size() ? nums[i] : 0; }
    bool isPrerelease() const { return !prerelease.empty(); }

    // Named accessors for the first four segments. They exist because most
    // callers (and every diagnostic) think in major/minor/patch, not in list
    // indices — the list is the storage, not the vocabulary.
    std::int64_t major()    const { return seg(0); }
    std::int64_t minor()    const { return seg(1); }
    std::int64_t patch()    const { return seg(2); }
    std::int64_t revision() const { return seg(3); }
    std::size_t  components() const { return nums.size(); }

    // Explicit, not `= default`: a defaulted <=> would compare the vectors
    // element-wise and make "1.2" != "1.2.0".
    std::strong_ordering operator<=>(const Version& o) const {
        const std::size_t n = std::max(nums.size(), o.nums.size());
        for (std::size_t i = 0; i < n; ++i)
            if (auto c = seg(i) <=> o.seg(i); c != 0) return c;
        // SemVer §11.3 — a release outranks any pre-release of the same numbers.
        if (prerelease.empty() != o.prerelease.empty())
            return prerelease.empty() ? std::strong_ordering::greater
                                      : std::strong_ordering::less;
        const std::size_t m = std::min(prerelease.size(), o.prerelease.size());
        for (std::size_t i = 0; i < m; ++i)
            if (auto c = prerelease[i] <=> o.prerelease[i]; c != 0) return c;
        return prerelease.size() <=> o.prerelease.size();
    }
    bool operator==(const Version& o) const { return (*this <=> o) == 0; }

    // Do the NUMERIC parts match? The pre-release visibility rule (see
    // `matches`) is defined on the numeric tuple alone.
    bool same_numbers(const Version& o) const {
        const std::size_t n = std::max(nums.size(), o.nums.size());
        for (std::size_t i = 0; i < n; ++i)
            if (seg(i) != o.seg(i)) return false;
        return true;
    }

    // DISPLAY ONLY (see the header note). Three segments are the floor, so
    // everything that rendered before #363 renders byte-identically; a written
    // fourth (or fifth) segment is preserved, and build metadata is gone
    // because it was never parsed.
    std::string str() const;
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

// Indices of ALL versions in `available` that match `req` and tie for highest
// precedence. Normally one element. More than one means distinct entries
// compare EQUAL — only possible when they differ solely in build metadata
// (`1.0.0+a` vs `1.0.0+b`) or in insignificant trailing zeros. The caller holds
// the literal keys and is the only one that can say whether that is benign, so
// the tie is REPORTED rather than broken here.
std::vector<std::size_t>
choose_all(const Requirement& req, const std::vector<Version>& available);

// Pick the highest version from `available` matching `req`. Returns the
// chosen version's index, or nullopt if none match. On a tie, the first.
std::optional<std::size_t>
choose(const Requirement& req, const std::vector<Version>& available);

} // namespace mcpp::version_req

namespace mcpp::version_req {

std::string Version::str() const {
    std::string out;
    const std::size_t n = std::max<std::size_t>(nums.size(), 3);
    for (std::size_t i = 0; i < n; ++i) {
        if (i) out += '.';
        out += std::to_string(seg(i));
    }
    if (!prerelease.empty()) {
        out += '-';
        for (std::size_t i = 0; i < prerelease.size(); ++i) {
            if (i) out += '.';
            out += prerelease[i].numeric ? std::to_string(prerelease[i].num)
                                         : prerelease[i].text;
        }
    }
    return out;
}

namespace {

bool is_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '-';
}

// A run of digits with a leading zero is NOT treated as numeric. SemVer forbids
// leading zeros outright; erroring would reject an index key over a rule the
// index never signed up for, so it is compared as text instead. Either way
// `01` and `1` stay distinguishable, which is the property that matters.
PreId make_pre_id(std::string_view s) {
    PreId id;
    const bool allDigits = !s.empty() &&
        std::all_of(s.begin(), s.end(),
                    [](char c){ return std::isdigit(static_cast<unsigned char>(c)); });
    if (allDigits && (s.size() == 1 || s.front() != '0')) {
        std::uint64_t n = 0;
        bool overflow = false;
        for (char c : s) {
            if (n > (std::numeric_limits<std::uint64_t>::max() - 9) / 10) { overflow = true; break; }
            n = n * 10 + static_cast<std::uint64_t>(c - '0');
        }
        if (!overflow) { id.numeric = true; id.num = n; return id; }
    }
    id.text = std::string(s);
    return id;
}

} // namespace

std::expected<Version, std::string> parse_version(std::string_view s) {
    const std::string_view original = s;

    // Build metadata: everything after the first '+'. Not stored (SemVer §10
    // excludes it from precedence) but still validated, so a malformed key is
    // reported as unorderable rather than silently truncated.
    if (auto plus = s.find('+'); plus != std::string_view::npos) {
        auto meta = s.substr(plus + 1);
        if (meta.empty())
            return std::unexpected(std::format("version: empty build metadata ('{}')", original));
        for (char c : meta)
            if (!is_ident_char(c) && c != '.')
                return std::unexpected(std::format(
                    "version: invalid build metadata ('{}')", original));
        s = s.substr(0, plus);
    }

    // Pre-release: everything after the first '-'.
    std::string_view pre;
    if (auto dash = s.find('-'); dash != std::string_view::npos) {
        pre = s.substr(dash + 1);
        s   = s.substr(0, dash);
        if (pre.empty())
            return std::unexpected(std::format("version: empty pre-release ('{}')", original));
    }

    Version v;
    // Numeric core: dot-separated digit runs, nothing else. Trailing garbage is
    // an ERROR, not something to stop at: `1.2.3abc` used to parse as 1.2.3 and
    // therefore compared EQUAL to it — the same silent merge the fourth and
    // fifth segments exist to prevent. An unparseable key is not a failure, it
    // is a key that only exact matching can address (see pm/resolver.cppm).
    std::size_t i = 0;
    while (true) {
        const std::size_t start = i;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
        if (start == i)
            return std::unexpected(std::format("version: not a number ('{}')", original));
        std::uint64_t n = 0;
        for (std::size_t k = start; k < i; ++k) {
            if (n > (static_cast<std::uint64_t>(
                         std::numeric_limits<std::int64_t>::max()) - 9) / 10)
                return std::unexpected(std::format(
                    "version: segment out of range ('{}')", original));
            n = n * 10 + static_cast<std::uint64_t>(s[k] - '0');
        }
        v.nums.push_back(static_cast<std::int64_t>(n));
        if (i == s.size()) break;
        if (s[i] != '.')
            return std::unexpected(std::format("version: not a number ('{}')", original));
        ++i;
        if (i == s.size())
            return std::unexpected(std::format("version: trailing '.' ('{}')", original));
    }

    // Pre-release identifiers: dot-separated, each non-empty and made of
    // [0-9A-Za-z-].
    while (!pre.empty()) {
        const auto dot = pre.find('.');
        const auto part = pre.substr(0, dot);
        if (part.empty())
            return std::unexpected(std::format(
                "version: empty pre-release identifier ('{}')", original));
        for (char c : part)
            if (!is_ident_char(c))
                return std::unexpected(std::format(
                    "version: invalid pre-release identifier '{}' ('{}')", part, original));
        v.prerelease.push_back(make_pre_id(part));
        if (dot == std::string_view::npos) break;
        pre = pre.substr(dot + 1);
        if (pre.empty())
            return std::unexpected(std::format(
                "version: trailing '.' in pre-release ('{}')", original));
    }

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

// SemVer/npm/Cargo pre-release visibility: a pre-release candidate is only
// eligible when the requirement itself names a pre-release at the SAME numeric
// tuple. Two bugs collapse into this one rule:
//
//   * `^1.92.8` must not silently pick `1.92.8-docking` — a different upstream
//     branch, a different tarball (mcpp#363).
//   * `^1.2.3` must not admit `2.0.0-alpha`, which the plain `v < upper` bound
//     lets through because 2.0.0-alpha sorts below 2.0.0.
bool prerelease_visible(const Requirement& r, const Version& v) {
    if (r.any) return false;               // `*` never reaches a pre-release
    for (auto& c : r.parts)
        if (c.v.isPrerelease() && c.v.same_numbers(v)) return true;
    return false;
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
    if (v.isPrerelease() && !prerelease_visible(r, v)) return false;
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
                // Every branch must also zero the segments AFTER the bumped one,
                // or the upper bound inherits the constraint's own tail and
                // wrongly excludes releases below it (^2026.7.27.3 would cut off
                // 2027.0.0.0..2). The bound is a release, never a pre-release:
                // `upper` drops any the constraint carried.
                Version upper = c.v;
                upper.prerelease.clear();
                auto bump_at = [&](std::size_t idx) {
                    upper.nums.resize(std::max(upper.nums.size(), idx + 1), 0);
                    ++upper.nums[idx];
                    upper.nums.resize(idx + 1);
                };
                if      (c.v.major() != 0) bump_at(0);
                else if (c.v.minor() != 0) bump_at(1);
                else                       bump_at(2);
                if (!(v >= c.v && v < upper)) return false;
                break;
            }
            case Op::Tilde: {
                // ~X.Y.Z = >=X.Y.Z, <X.(Y+1).0
                Version upper = c.v;
                upper.prerelease.clear();
                upper.nums.resize(std::max<std::size_t>(upper.nums.size(), 2), 0);
                ++upper.nums[1];
                upper.nums.resize(2);
                if (!(v >= c.v && v < upper)) return false;
                break;
            }
        }
    }
    return true;
}

std::vector<std::size_t>
choose_all(const Requirement& req, const std::vector<Version>& available) {
    std::vector<std::size_t> best;
    for (std::size_t i = 0; i < available.size(); ++i) {
        if (!matches(req, available[i])) continue;
        if (best.empty())                       { best.push_back(i); continue; }
        if (available[i] >  available[best.front()]) { best.assign(1, i); continue; }
        if (available[i] == available[best.front()])  best.push_back(i);
    }
    return best;
}

std::optional<std::size_t>
choose(const Requirement& req, const std::vector<Version>& available) {
    auto best = choose_all(req, available);
    if (best.empty()) return std::nullopt;
    return best.front();
}

} // namespace mcpp::version_req

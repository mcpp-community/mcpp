// mcpp.pm.index_contract — the index→client version contract.
//
// An index tree (a directory containing pkgs/) may carry an `index.toml`
// at its root:
//
//     [index]
//     spec        = "1"        # index layout spec
//     min_mcpp    = "0.0.85"   # oldest mcpp able to parse every descriptor
//     latest_mcpp = "0.0.85"   # optional: newest known-good mcpp (hint)
//
// The contract travels WITH the tree (git checkout, unpacked artifact,
// CI-restored cache, `[indices] path =` local dir), so one check at the
// index-open choke point covers every transport, offline included.
// Missing index.toml → no constraint (back-compat, third-party indices).
//
// Escape hatch: MCPP_INDEX_FLOOR=ignore (debugging).
// Design: .agents/docs/2026-07-08-index-version-semantics-and-descriptor-
// grammar-design.md (D3).

export module mcpp.pm.index_contract;

import std;
import mcpp.libs.toml;
import mcpp.version_req;
import mcpp.version;                 // MCPP_VERSION (leaf — see that module)

export namespace mcpp::pm {

struct IndexContract {
    std::string spec;         // index layout spec ("1")
    std::string minMcpp;      // floor: oldest client able to parse the tree
    std::string latestMcpp;   // optional upgrade hint
};

// Read <indexRoot>/index.toml. nullopt when absent or unreadable
// (absence is not an error — it simply means "no contract").
std::optional<IndexContract>
read_index_contract(const std::filesystem::path& indexRoot);

// Pure floor predicate: does `ownVersion` satisfy `minMcpp`?
// Returns the violation message when it does not; nullopt when fine
// (including unparsable versions — the contract must never brick a
// client by being malformed).
std::optional<std::string>
floor_violation(std::string_view minMcpp, std::string_view ownVersion);

// Pure predicate — no reporting, no registration, no dedup. For callers that
// need to ask "would this tree be usable?" without the side effects of
// check_index_floor (the refresh guard asks it twice per refresh).
bool index_usable(const std::filesystem::path& indexRoot);

// Open-time check for an index tree. Combines read + floor + escape
// hatch + once-per-root deduplication of the (expensive to spam) error.
// Returns the violation message the FIRST time a too-new tree is opened;
// nullopt otherwise. Also RECORDS the fact (see below).
std::optional<std::string>
check_index_floor(const std::filesystem::path& indexRoot);

// ── "this index is unusable" as a first-class, queryable fact ──────────
//
// A floor violation makes every descriptor read from that tree return nothing,
// which is indistinguishable from "the package genuinely is not in this index"
// at the call site. Two things downstream need to tell them apart:
//
//   * the refresh policy — a miss caused by an unusable index will NOT be
//     fixed by fetching the same tree again, and treating it as a normal miss
//     makes the unusable state drive repeated refreshes of itself;
//   * the final error — "not found" names neither the version nor the floor,
//     so the message that stops the build has to reach back for the real cause.
//
// Process-global on purpose: the fact depends only on (the tree on disk, this
// binary's version), and neither can change within a process. Recording it is
// what lets the two consumers above stay honest without threading a tri-state
// through every read_xpkg_lua entry point and all of their callers.
struct UnusableIndex {
    std::filesystem::path root;
    std::string           message;   // the full E0006 text, ready to print
};

// True when any index tree opened in this process failed its floor check.
bool any_index_unusable();

// True when THIS tree failed (exact root match).
bool index_marked_unusable(const std::filesystem::path& indexRoot);

// Every index that failed, in first-seen order.
std::vector<UnusableIndex> unusable_indexes();

// One line for appending to an unrelated failure ("… and by the way, an index
// was unusable, which is probably why"). Empty when nothing was unusable.
std::string unusable_index_hint();

// Testing only: forget everything recorded so far.
void reset_unusable_indexes_for_test();

} // namespace mcpp::pm

namespace mcpp::pm {

std::optional<IndexContract>
read_index_contract(const std::filesystem::path& indexRoot)
{
    std::error_code ec;
    auto file = indexRoot / "index.toml";
    if (!std::filesystem::exists(file, ec)) return std::nullopt;

    std::ifstream is{file};
    if (!is) return std::nullopt;
    std::string body{std::istreambuf_iterator<char>(is), {}};

    auto doc = mcpp::libs::toml::parse(body);
    if (!doc) return std::nullopt;

    IndexContract c;
    if (auto v = doc->get_string("index.spec"))        c.spec       = *v;
    if (auto v = doc->get_string("index.min_mcpp"))    c.minMcpp    = *v;
    if (auto v = doc->get_string("index.latest_mcpp")) c.latestMcpp = *v;
    return c;
}

std::optional<std::string>
floor_violation(std::string_view minMcpp, std::string_view ownVersion)
{
    if (minMcpp.empty()) return std::nullopt;
    auto need = mcpp::version_req::parse_version(minMcpp);
    auto have = mcpp::version_req::parse_version(ownVersion);
    if (!need || !have) return std::nullopt;   // malformed contract never bricks
    if (*have >= *need) return std::nullopt;
    return std::format(
        "index requires mcpp >= {} but this is mcpp {} [E0006]\n"
        "  Upgrade:  curl -fsSL https://github.com/mcpp-community/mcpp/"
        "releases/latest/download/install.sh | bash\n"
        "  Details:  mcpp explain E0006   "
        "(override for debugging: MCPP_INDEX_FLOOR=ignore)",
        minMcpp, ownVersion);
}

bool index_usable(const std::filesystem::path& indexRoot)
{
    if (const char* v = std::getenv("MCPP_INDEX_FLOOR");
        v && std::string_view(v) == "ignore")
        return true;
    auto c = read_index_contract(indexRoot);
    if (!c) return true;                       // no contract → no constraint
    return !floor_violation(c->minMcpp, mcpp::MCPP_VERSION);
}

namespace {
// See the header comment on UnusableIndex for why this is process-global.
std::vector<UnusableIndex>& unusable_registry() {
    static std::vector<UnusableIndex> reg;
    return reg;
}
} // namespace

bool any_index_unusable() { return !unusable_registry().empty(); }

bool index_marked_unusable(const std::filesystem::path& indexRoot) {
    for (auto& u : unusable_registry())
        if (u.root == indexRoot) return true;
    return false;
}

std::vector<UnusableIndex> unusable_indexes() { return unusable_registry(); }

std::string unusable_index_hint() {
    auto& reg = unusable_registry();
    if (reg.empty()) return {};
    // Name the index, not just the fact: with several repos configured, "an
    // index was too new" leaves the reader guessing which one to act on.
    std::string s = "note: this resolve ran with an index this mcpp cannot read:\n";
    for (auto& u : reg) {
        s += "  " + u.root.string() + "\n";
    }
    s += "      Packages served by it were reported as not found. See the "
         "[E0006] error above,\n"
         "      or run `mcpp explain E0006`.";
    return s;
}

void reset_unusable_indexes_for_test() { unusable_registry().clear(); }

std::optional<std::string>
check_index_floor(const std::filesystem::path& indexRoot)
{
    if (const char* v = std::getenv("MCPP_INDEX_FLOOR");
        v && std::string_view(v) == "ignore")
        return std::nullopt;

    auto c = read_index_contract(indexRoot);
    if (!c) return std::nullopt;
    auto violation = floor_violation(c->minMcpp, mcpp::MCPP_VERSION);
    if (!violation) return std::nullopt;

    // Record BEFORE the dedup return: the fact must be queryable no matter how
    // many times this root is opened, while the message is printed only once.
    // Deriving "was anything unusable?" from "did we print?" is what made the
    // second and later reads indistinguishable from an ordinary miss.
    if (!index_marked_unusable(indexRoot))
        unusable_registry().push_back({indexRoot, *violation});
    else
        return std::nullopt;          // already reported — stay quiet
    return violation;
}

} // namespace mcpp::pm

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
import mcpp.toolchain.fingerprint;   // MCPP_VERSION

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

// Open-time check for an index tree. Combines read + floor + escape
// hatch + once-per-root deduplication of the (expensive to spam) error.
// Returns the violation message the FIRST time a too-new tree is opened;
// nullopt otherwise.
std::optional<std::string>
check_index_floor(const std::filesystem::path& indexRoot);

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

std::optional<std::string>
check_index_floor(const std::filesystem::path& indexRoot)
{
    if (const char* v = std::getenv("MCPP_INDEX_FLOOR");
        v && std::string_view(v) == "ignore")
        return std::nullopt;

    // Once per root per process: the same index is opened many times in a
    // single resolve; report the violation once, stay quiet after.
    static std::set<std::filesystem::path> reported;
    auto c = read_index_contract(indexRoot);
    if (!c) return std::nullopt;
    auto violation = floor_violation(c->minMcpp, mcpp::toolchain::MCPP_VERSION);
    if (!violation) return std::nullopt;
    if (!reported.insert(indexRoot).second) return std::nullopt;
    return violation;
}

} // namespace mcpp::pm

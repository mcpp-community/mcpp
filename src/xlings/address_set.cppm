// mcpp.xlings.address_set — one package, one version.
//
// THE PROBLEM THIS EXISTS FOR. An xlings address is written
// `[<ns>:]<name>[@<version>]`, and mcpp had two definitions of when two such
// strings name the same thing. The conditional merge compared the PACKAGE, so
// `xim:glibc` and `xim:glibc@2.40` were one entry with the more specific
// spelling winning. The graph split compared the whole STRING, so the same two
// were two packages: both were installed, at two versions, several gigabytes
// each for a vendor toolkit — and the answer `xpkg_dir` gave to a build program
// was whichever one a later "keep the first value for a name" rule happened to
// reach. Installed twice, answered once, and nothing said so.
//
// THE RULE. A package's identity is `(namespace, name)`. A version is always a
// CONSTRAINT on that package, never part of its name. Everything else follows:
// "pin an exact version" and "pin a range" stop being two mechanisms and become
// two values of one, and there is no second place that has to learn about
// ranges because there is no second identity.
//
// ADJUDICATION AND VALIDATION ARE DIFFERENT QUESTIONS, and conflating them is
// what makes a version solver look necessary here.
//
//   - WHICH version — adjudicated by distance. The declaration nearer the
//     artifact wins: a project outranks a package it depends on. A rule package
//     owns "which packages, and no older than what"; the project owns "and
//     exactly this one". Both are stated, one is used, and the override is
//     reported rather than inferred from an install log.
//   - WHETHER it holds — validated by comparison. The winner is checked against
//     every REQUIREMENT that lost. That is one comparison against a known
//     version, not a search through an index for a version satisfying a set, so
//     the input a solver needs (which versions exist) is never consulted.
//
// A LOST EXACT PIN IS NOT A VIOLATION. Two exact pins that differ are two
// CHOICES, and choosing is what adjudication is for; the nearer one wins and
// the other is reported. A `>=`/`^`/`~` statement is a REQUIREMENT, and a
// winner that fails it is refused. The asymmetry is the point: an upgrade must
// not turn every dependency that pinned a tool into a hard failure, while a
// floor a rule package depends on must not be silently lowered.
export module mcpp.xlings.address_set;

import std;
import mcpp.manifest;
import mcpp.version_req;

export namespace mcpp::xlings::addrset {

// One declaration of one address, and where it came from.
struct Claim {
    std::string address;      // `[<ns>:]<name>[@<version>]`, exactly as authored
    std::string declaredBy;   // "this project", "mcpp:plugins" — named in messages
    // 0 = the consumer's own manifest, 1 = a package the consumer depends on.
    // Two levels, because that is the distinction the rule draws; a deeper
    // dependency is not nearer to the artifact than a shallower one in any
    // sense a manifest author can act on.
    int         distance = 0;
};

struct Winner {
    std::string address;   // the winning claim's address, verbatim
    std::size_t claim;     // index into the input, so callers can ask who won
};

struct Resolution {
    // One winner per package, in the order the packages were first claimed.
    std::vector<Winner>      winners;
    // One sentence per disagreement that was resolved rather than refused.
    // Returned rather than printed: this module has no output channel, which is
    // also what makes it testable without one.
    std::vector<std::string> overrides;
};

// `[<ns>:]<name>` with the namespace defaulted, which is the identity.
//
// The default is `xim`, matching `parse_xpkg_ref`: a manifest writing `ninja`
// and one writing `xim:ninja` name one package, and an identity that says
// otherwise would reintroduce the split this module removes.
std::string package_key(std::string_view address);

// The version position of `address`, or empty when it names none.
std::string version_of(std::string_view address);

// Refuses when a winner fails a requirement that lost. Never refuses for a
// spelling it cannot evaluate: an unparseable version is reported as an
// override, because a refusal manufactured from ignorance is worse than the
// duplicate install it would be preventing.
std::expected<Resolution, std::string> unify(std::span<const Claim> claims);

} // namespace mcpp::xlings::addrset

// ── implementation ──────────────────────────────────────────────────────────

namespace mcpp::xlings::addrset {

std::string package_key(std::string_view address) {
    const auto e = mcpp::manifest::parse_address(address);
    return (e.ns.empty() ? std::string("xim") : e.ns) + ":" + e.target;
}

std::string version_of(std::string_view address) {
    return mcpp::manifest::parse_address(address).version;
}

namespace {

enum class Verdict {
    Holds,      // the winner satisfies this statement, or the statement is silent
    Differs,    // two choices; the nearer one is used and the other reported
    Violated,   // the winner fails a stated requirement
};

Verdict check(std::string_view chosen, std::string_view stated) {
    if (stated.empty() || stated == chosen) return Verdict::Holds;
    if (!mcpp::version_req::is_constraint(stated)) return Verdict::Differs;
    // A requirement. It can only be tested against a version, so a winner that
    // is itself a range is not yet an answer to it.
    if (mcpp::version_req::is_constraint(chosen)) return Verdict::Differs;
    auto v = mcpp::version_req::parse_version(chosen);
    if (!v) return Verdict::Differs;
    auto r = mcpp::version_req::parse_req(stated);
    if (!r) return Verdict::Differs;
    return mcpp::version_req::matches(*r, *v) ? Verdict::Holds : Verdict::Violated;
}

} // namespace

std::expected<Resolution, std::string> unify(std::span<const Claim> claims) {
    Resolution out;
    std::vector<std::string> order;
    std::map<std::string, std::vector<std::size_t>> byPackage;
    for (std::size_t i = 0; i < claims.size(); ++i) {
        const auto key = package_key(claims[i].address);
        auto [it, fresh] = byPackage.try_emplace(key);
        if (fresh) order.push_back(key);
        it->second.push_back(i);
    }

    for (auto const& key : order) {
        auto const& idx = byPackage[key];
        // THE WINNER IS THE NEAREST CLAIM THAT NAMES A VERSION. A claim with no
        // version states that the package is wanted and nothing about which
        // version, so it abstains from a question it did not answer — without
        // this, a project naming a tool bare would silently drop a floor its
        // dependency depends on.
        std::optional<std::size_t> win;
        for (auto i : idx) {
            if (version_of(claims[i].address).empty()) continue;
            if (!win || claims[i].distance < claims[*win].distance) win = i;
        }
        if (!win) { out.winners.push_back({claims[idx.front()].address, idx.front()}); continue; }

        const auto chosen = version_of(claims[*win].address);
        for (auto i : idx) {
            if (i == *win) continue;
            const auto stated = version_of(claims[i].address);
            switch (check(chosen, stated)) {
                case Verdict::Holds: break;
                case Verdict::Differs:
                    out.overrides.push_back(std::format(
                        "'{}' is declared at two versions: '{}' by {}, and '{}' by "
                        "{}. One version is installed, and the declaration nearer "
                        "the artifact is the one used -- here '{}'. Drop the "
                        "nearer declaration to take the other.",
                        key, chosen, claims[*win].declaredBy, stated,
                        claims[i].declaredBy, chosen));
                    break;
                case Verdict::Violated:
                    return std::unexpected(std::format(
                        "`{}` is pinned to {} by {}, and {} requires {}.\n"
                        "       One version of a package is installed, so the two "
                        "cannot both hold.\n"
                        "       fix: pin a version satisfying {}, or drop the pin "
                        "and let the\n"
                        "       requirement decide.",
                        key, chosen, claims[*win].declaredBy,
                        claims[i].declaredBy, stated, stated));
            }
        }
        out.winners.push_back({claims[*win].address, *win});
    }
    return out;
}

} // namespace mcpp::xlings::addrset

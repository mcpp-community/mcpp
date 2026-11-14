// THE TOOLCHAIN PAYLOADS, READ (design 2026-11-12 §16, T3).
//
// data/builtin.toml states which package a family's toolchain is for a target
// on a host (`[[toolchain.payload]]`) and which packages a host is offered
// (`[[toolchain.offer]]`). This module answers the three questions the engine
// asks of those tables -- which payload a request takes, what an installed
// package is, and what a host is offered -- so that the forward and the reverse
// reading cannot disagree and no other code names a payload.

export module mcpp.toolchain.payloads;

import std;
import mcpplibs.os;
import mcpp.toolchain.platform;
import mcpp.toolchain.target_schema;
import mcpp.toolchain.target_rows;
import mcpp.toolchain.builtin_targets;
import mcpp.toolchain.triple;

export namespace mcpp::toolchain::payloads {

// The machine a question is asked on, as `<os>-<arch>` names it, and the suffix
// of its programs.
struct Host {
    std::string os, arch, exe;
    std::string pair() const { return os + "-" + arch; }
};
Host this_host();

// A placeholder value the caller supplies (a payload's legacy layout).
using Vars = std::vector<std::pair<std::string_view, std::string>>;

// An entry with its placeholders filled for one request.
struct Resolved {
    std::string name, family, frontendDir;
    std::vector<std::string> frontends;
    bool glibcFixup = false;
};

// The payload a family's toolchain is for `target` (empty: the host's) on
// `host`. `named`, when the request named a payload (`emsdk@6.0.9`), takes that
// payload's first entry whatever it serves. nullopt: no entry applies.
std::optional<Resolved> choose(std::string_view family, std::string_view named,
                               const triple::Triple& target, const Host& host = this_host(),
                               const Vars& vars = {});

// An installed package read back: its family and the target it builds for
// (empty: the host's). nullopt for a package no entry of a family's version
// names (ninja, glibc, ...).
struct Identity {
    std::string    payload;                // the entry's name
    std::string    family;
    triple::Triple target;
    bool           familyVersion = true;   // the payload's version is its family's
};
std::optional<Identity> identify(std::string_view package, const Host& host = this_host());

// The entry a request naming `package` (or one of its aliases) means, and the
// family and target it takes (`musl-gcc@16.1.0`, `mingw@16.1.0`, `emsdk@6.0.9`):
// nullopt when no entry has that name.
std::optional<Identity> named(std::string_view package, const Host& host = this_host());

// The packages `toolchain list --available` offers on `host`.
std::vector<std::string> offered(const Host& host = this_host());

// The names a request may give a payload of the table, in the table's order
// (those without a placeholder), for a message that has to list them.
std::vector<std::string> names();

} // namespace mcpp::toolchain::payloads

namespace mcpp::toolchain::payloads {

namespace {

namespace t = mcpp::toolchain::target;

// `{name}` replaced by its value; nullopt when a placeholder has none, which is
// how an entry that does not apply (`{gcc-triple}` of a row without one) says so.
std::optional<std::string> fill(std::string_view text, const Vars& vars) {
    std::string out;
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] != '{') { out += text[i++]; continue; }
        const auto end = text.find('}', i);
        if (end == std::string_view::npos) return std::nullopt;
        const auto key = text.substr(i + 1, end - i - 1);
        auto it = std::ranges::find_if(vars, [&](auto const& v) { return v.first == key; });
        if (it == vars.end()) return std::nullopt;
        out += it->second;
        i = end + 1;
    }
    return out;
}

Vars vars_for(const triple::Triple& target, const Host& host, const Vars& extra) {
    Vars v = extra;
    v.emplace_back("exe", host.exe);
    v.emplace_back("host-arch", host.arch);
    v.emplace_back("host-musl", host.arch + "-linux-musl");
    if (!target.empty()) {
        v.emplace_back("triple", target.str());
        if (auto const* r = t::find_row(target.str()); r && !r->gccTriple.empty())
            v.emplace_back("gcc-triple", std::string(r->gccTriple));
    }
    return v;
}

bool serves(const t::Payload& p, const triple::Triple& target) {
    for (auto s : p.serves) {
        if (s == "host") { if (target.empty()) return true; continue; }
        if (target.empty()) continue;
        if (s == "*" || t::serves_triple(s, target.str())) return true;
    }
    return false;
}

std::optional<Resolved> resolve(const t::Payload& p, const Vars& v) {
    auto name = fill(p.name, v);
    if (!name) return std::nullopt;
    Resolved r{ *name, std::string(p.family), "bin", {}, p.glibcFixup };
    if (!p.frontendDir.empty()) {
        auto dir = fill(p.frontendDir, v);
        if (!dir) return std::nullopt;
        r.frontendDir = *dir;
    }
    for (auto f : p.frontends)
        if (auto name = fill(f, v); name && std::ranges::find(r.frontends, *name) == r.frontends.end())
            r.frontends.push_back(*name);
    return r;
}

// The target an entry's `builds-for` names on `host`; empty: the host's.
std::optional<triple::Triple> builds_for(std::string_view what, const Host& host) {
    if (what.empty()) return std::nullopt;
    if (what == "host") return triple::Triple{};
    if (what == "host-musl") return triple::Triple{ host.arch, "linux", "musl" };
    return triple::parse(what);
}

// `name` matched against an entry's name pattern: the value its one
// placeholder takes, or the empty string for a name without one.
std::optional<std::string> capture(std::string_view pattern, std::string_view name) {
    const auto open = pattern.find('{');
    if (open == std::string_view::npos)
        return pattern == name ? std::optional<std::string>{""} : std::nullopt;
    const auto close = pattern.find('}', open);
    const auto head = pattern.substr(0, open), tail = pattern.substr(close + 1);
    if (name.size() <= head.size() + tail.size() || !name.starts_with(head) || !name.ends_with(tail))
        return std::nullopt;
    return std::string(name.substr(head.size(), name.size() - head.size() - tail.size()));
}

} // namespace

Host this_host() {
    return { std::string(mcpp::toolchain::platform::name), std::string(mcpp::toolchain::platform::host_arch),
             std::string(mcpplibs::os::exe_suffix) };
}

std::optional<Resolved> choose(std::string_view family, std::string_view named, const triple::Triple& target,
                               const Host& host, const Vars& extra) {
    const auto v = vars_for(target, host, extra);
    for (auto const& p : mcpp::toolchain::builtin::kPayloads) {
        if (p.family != family) continue;
        if (!named.empty() ? p.name != named
                           : !(serves(p, target) && t::hosts_allow(p.hosts, host.pair(), target.arch)))
            continue;
        if (auto r = resolve(p, v)) return r;
    }
    return std::nullopt;
}

std::optional<Identity> identify(std::string_view package, const Host& host) {
    // A name an entry states whole is read before a pattern's: `musl-gcc` is
    // not the cross GCC of a triple called `musl`.
    for (bool patterns : { false, true })
        for (auto const& p : mcpp::toolchain::builtin::kPayloads) {
            if (!p.familyVersion || (p.name.find('{') != std::string_view::npos) != patterns) continue;
            auto value = capture(p.name, package);
            if (!value) continue;
            if (!patterns) {
                if (auto target = builds_for(p.buildsFor, host))
                    return Identity{ std::string(p.name), std::string(p.family), *target };
                continue;
            }
            if (p.name.find("{gcc-triple}") != std::string_view::npos) {
                for (auto const* r : t::rows())
                    if (r->gccTriple == *value)
                        if (auto tt = triple::parse(r->triple))
                            return Identity{ std::string(package), std::string(p.family), *tt };
                continue;
            }
            if (auto tt = triple::parse(*value)) return Identity{ std::string(package), std::string(p.family), *tt };
        }
    return std::nullopt;
}

std::optional<Identity> named(std::string_view package, const Host& host) {
    for (auto const& p : mcpp::toolchain::builtin::kPayloads)
        if (p.name == package || std::ranges::find(p.aliases, package) != p.aliases.end())
            return Identity{ std::string(p.name), std::string(p.family),
                             builds_for(p.buildsFor, host).value_or(triple::Triple{}), p.familyVersion };
    return std::nullopt;
}

std::vector<std::string> names() {
    std::vector<std::string> out;
    for (auto const& p : mcpp::toolchain::builtin::kPayloads) {
        if (p.name.find('{') != std::string_view::npos || p.name == p.family) continue;
        if (std::ranges::find(out, p.name) == out.end()) out.emplace_back(p.name);
    }
    return out;
}

std::vector<std::string> offered(const Host& host) {
    const auto v = vars_for({}, host, {});
    std::vector<std::string> out;
    for (auto const& o : mcpp::toolchain::builtin::kOffers)
        if (t::hosts_allow(o.hosts, host.pair(), host.arch))
            if (auto name = fill(o.name, v)) out.push_back(*name);
    return out;
}

} // namespace mcpp::toolchain::payloads

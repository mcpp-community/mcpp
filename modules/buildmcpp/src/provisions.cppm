// mcpp.build.provisions — what a dependency hands to its consumer's BUILD
// PROGRAM, and how far it travels.
//
// WHY THIS MODULE EXISTS
//
// mcpp has long had a model for "what a dependency provides × who sees it":
// `UsageRequirements` (include dirs, defines, link flags, module names) with
// three scopes and a fixpoint that flows publicUsage into each consumer's
// privateBuild. Adding an entry there forces the author to answer "which
// scope?", which is why include dirs have never silently leaked.
//
// The provisions #355 introduced — host tools (`tools = [...]`) and host build
// rules (`host-module = true`) — went in beside that model rather than into
// it. Each got a hand-written destination: tools were recorded against the
// consumer of the *requesting* edge, host modules only against the root's
// direct dependencies, dependency directories only for direct dependencies.
// None of the three could be re-exported, so a library could not stand up a
// toolchain on its user's behalf, and the user had to name every tool the
// library needed (#359).
//
// The root cause is not a missing propagation. It is that NOTHING FORCED THE
// QUESTION. This codebase has repeatedly paid for "the same decision derived
// in N places" (#233/#240/#242/#344); this is its mirror image — a question
// that must be answered exactly once was answered in ZERO places, so each new
// provision invented its own reach. `directives::kTable` already solved this
// shape once by making Scope a required column.
//
// Here a provision KIND is one row of `kTable`, and every row states whether
// the kind is addressable by a bare name and whether it travels only on an
// explicitly re-exporting edge. Propagation is one fixpoint over the edge
// graph, shared by all kinds.
//
// WHY IT IS A SEPARATE MODULE RATHER THAN MORE OF prepare.cppm
//
// Same reason `mcpp.build.directives` and `mcpp.build.hostprogram` are
// separate: build_program.cppm's anonymous namespace miscompiles its own
// neighbours under clang 22 + C++20 modules + -O2 (PR#332, reproduced by
// PR#334), and prepare_build is already a single function of several thousand
// lines. New policy goes in its own module with its own tests.
//
// See .agents/docs/2026-08-06-provisions-and-build-inputs.md.

export module mcpp.build.provisions;

import std;
import mcpp.pm.dep_spec;
import mcpp.pm.mangle;

export namespace mcpp::build::provisions {

// ── The table ──────────────────────────────────────────────────────────────
//
// A provision is something a dependency makes available to a consumer's
// `build.mcpp` PROGRAM — not to its compile commands. That distinction is why
// these do not live in `UsageRequirements`: that type describes a consumer's
// command line and is consumed by the ninja graph, while a provision is
// consumed by a process mcpp runs during prepare. Merging them would leave
// `linkUsage.tools` as a field with no meaning.
enum class Kind {
    Tool,        // a dependency's `kind = "bin"` target, built for the host
    HostModule,  // a dependency's lib-root interface, compiled with build.mcpp
    DepDir,      // a dependency's resolved source directory
};

struct Def {
    Kind             kind;
    std::string_view name;
    // Does an unqualified spelling address this kind? Tools and dep dirs are
    // looked up by name from inside build.mcpp (`dep_bin("protobuf", …)`), so
    // they need the bare-name ladder below. A host module is addressed by
    // `import <module>;`, where the module name IS the package name and the
    // compiler — not mcpp — resolves it.
    bool             bareAddressable;
    // Does crossing one more edge require `reexport = true` on that edge?
    // Every kind does. The column exists so that a future kind cannot be added
    // without someone writing an answer down.
    bool             needsReexport;
};

inline constexpr Def kTable[] = {
    { Kind::Tool,       "tool",        true,  true },
    { Kind::HostModule, "host-module", false, true },
    { Kind::DepDir,     "dep-dir",     true,  true },
};

inline constexpr const Def& def_of(Kind k) {
    for (auto const& d : kTable)
        if (d.kind == k) return d;
    return kTable[0];  // unreachable: kTable covers the enum
}

// ── One provision instance ─────────────────────────────────────────────────

struct Provision {
    Kind        kind = Kind::Tool;
    std::size_t provider = 0;   // package index that produces it
    std::string tool;           // Kind::Tool only; empty otherwise

    // Written out rather than `= default`-ing the spaceship: a defaulted
    // operator<=> here made GCC 16 emit two different manglings for
    // `__gnu_cxx::operator<=>(__normal_iterator…)` across the module boundary
    // and reject the second ("conflicts with a previous mangle"). The ordering
    // is only needed to key a std::set, so an explicit `<` costs nothing.
    friend bool operator<(const Provision& a, const Provision& b) {
        if (a.kind != b.kind)         return a.kind < b.kind;
        if (a.provider != b.provider) return a.provider < b.provider;
        return a.tool < b.tool;
    }
    friend bool operator==(const Provision& a, const Provision& b) {
        return a.kind == b.kind && a.provider == b.provider && a.tool == b.tool;
    }
};

// ── Propagation ────────────────────────────────────────────────────────────
//
//   own(P→D)    = provisions this edge asks D for, plus D's own directory
//   exported(P) = ⋃ over edges P→D with reexport:  own(P→D) ∪ exported(D)
//   visible(P)  = ⋃ over all edges P→D:            own(P→D) ∪ exported(D)
//
// A package always sees what its direct dependencies hand it; it passes
// something on only when it says so. Monotone in both sets, so the fixpoint
// terminates, and a dependency cycle is harmless (sets stop growing).
//
// Templated on the edge type for the reason `dep_graph` is: `DependencyEdge`
// is a local struct inside prepare_build. An edge must expose
// `consumerPackageIndex`, `dependencyPackageIndex`, `reexport`,
// `requestedTools` and `hostModule`.
struct Propagation {
    std::vector<std::set<Provision>> visible;
    std::vector<std::set<Provision>> exported;
};

template <class Edge>
Propagation propagate(const std::vector<Edge>& edges, std::size_t packageCount) {
    Propagation p;
    p.visible.resize(packageCount);
    p.exported.resize(packageCount);

    auto own = [](const Edge& e) {
        std::set<Provision> s;
        // Every edge exposes the dependency's directory: that is today's
        // behaviour for direct dependencies (MCPP_DEP_<NAME>_DIR) and is kept
        // unconditional so nothing that works now stops working.
        s.insert(Provision{ Kind::DepDir, e.dependencyPackageIndex, {} });
        for (auto const& t : e.requestedTools)
            s.insert(Provision{ Kind::Tool, e.dependencyPackageIndex, t });
        if (e.hostModule)
            s.insert(Provision{ Kind::HostModule, e.dependencyPackageIndex, {} });
        return s;
    };

    bool changed = true;
    while (changed) {
        changed = false;
        for (auto const& e : edges) {
            if (e.consumerPackageIndex >= packageCount) continue;
            if (e.dependencyPackageIndex >= packageCount) continue;
            auto contributed = own(e);
            const auto& fromDep = p.exported[e.dependencyPackageIndex];
            contributed.insert(fromDep.begin(), fromDep.end());

            auto& vis = p.visible[e.consumerPackageIndex];
            for (auto const& pr : contributed)
                if (vis.insert(pr).second) changed = true;

            if (e.reexport) {
                auto& exp = p.exported[e.consumerPackageIndex];
                for (auto const& pr : contributed)
                    if (exp.insert(pr).second) changed = true;
            }
        }
    }
    return p;
}

// ── Bare-name binding ──────────────────────────────────────────────────────
//
// `dep_bin("protobuf", "protoc")` and `dep_dir("protobuf")` address a package
// by its namespace-stripped tail. That was safe while only the root's own
// declarations reached build.mcpp: a collision was between two lines the user
// had written. Once a library can re-export, two packages that have never
// heard of each other can both offer the tail `protobuf`, and "whichever was
// appended last" decides which binary runs — silently, and in exactly the
// place this feature exists to make version mismatch inexpressible.
//
// The binding therefore uses the same mechanism package identity already uses
// for an unqualified name: the fully-qualified spelling is the identity and is
// always available; the bare spelling is a convenience resolved through a
// fixed ladder.
//
//   1. (kDefaultNamespace, X)
//   2. (kCompatNamespace,  X)
//   3. (∅, X) — a package declared with no namespace at all
//   4. the single remaining candidate, if there is exactly one
//
// Rung 4 is the addition this context needs and package resolution does not: a
// package in some other namespace (grpc.grpc-plugin) must still be reachable
// as `grpc-plugin`, because that is the spelling rules already in the wild
// use. When it does not apply, the bare spelling is not bound at all and the
// caller must use the qualified one.

struct BareBinding {
    std::string              owner;       // FQN bound to this tail; empty = unbound
    std::vector<std::string> candidates;  // every FQN with this tail, sorted
    bool                     contested = false;  // more than one candidate
};

inline std::string namespace_of(std::string_view fqn) {
    auto dot = fqn.rfind('.');
    if (dot == std::string_view::npos) return {};
    return std::string(fqn.substr(0, dot));
}

inline std::string tail_of(std::string_view fqn) {
    auto dot = fqn.rfind('.');
    if (dot == std::string_view::npos || dot + 1 >= fqn.size())
        return std::string(fqn);
    return std::string(fqn.substr(dot + 1));
}

// tail → binding, over every package that provides something.
inline std::map<std::string, BareBinding>
bind_bare_names(const std::vector<std::string>& fqns) {
    std::map<std::string, std::vector<std::string>> byTail;
    for (auto const& f : fqns) {
        auto& v = byTail[tail_of(f)];
        if (std::find(v.begin(), v.end(), f) == v.end()) v.push_back(f);
    }

    const std::string ladder[] = {
        std::string(mcpp::pm::kDefaultNamespace),
        std::string(mcpp::pm::kCompatNamespace),
        std::string{},
    };

    std::map<std::string, BareBinding> out;
    for (auto& [tail, cands] : byTail) {
        std::ranges::sort(cands);
        BareBinding b;
        b.candidates = cands;
        b.contested  = cands.size() > 1;
        if (cands.size() == 1) {
            b.owner = cands.front();
        } else {
            for (auto const& ns : ladder) {
                std::vector<std::string> rung;
                for (auto const& c : cands)
                    if (namespace_of(c) == ns) rung.push_back(c);
                if (rung.size() == 1) { b.owner = rung.front(); break; }
                if (rung.size() > 1) break;  // ambiguous *within* a rung: unbind
            }
        }
        out.emplace(tail, std::move(b));
    }
    return out;
}

// The human-readable half of a contested binding. Empty when there is nothing
// to say, so callers can `if (auto m = …; !m.empty())`.
inline std::string contest_note(std::string_view tail, const BareBinding& b) {
    if (!b.contested) return {};
    std::string list;
    for (auto const& c : b.candidates) {
        if (!list.empty()) list += ", ";
        list += c;
    }
    if (b.owner.empty())
        return std::format(
            "the unqualified name '{}' is offered by more than one package "
            "({}) and none of them wins the namespace ladder, so it is NOT "
            "bound — address the one you mean by its full name",
            tail, list);
    return std::format(
        "the unqualified name '{}' is offered by more than one package ({}); "
        "it resolves to '{}' by the namespace ladder — use the full name to "
        "address a different one",
        tail, list, b.owner);
}

// ── Host module identity ───────────────────────────────────────────────────
//
// A rule's module name is AUTHORED API. prepare.cppm states that rule for
// ordinary packages already ("Module names are authored API and are not
// required to mirror package identity"), and the host-module path was the one
// place in the codebase that derived the name from `package.name` instead of
// reading what the source declares.
//
// The divergence was not merely inconsistent, it was a portability trap.
// `build_host_module` binds the name differently per compiler family: MSVC
// passes `/reference <name>=<ifc>` and Clang `-fmodule-file=<name>=<pcm>`,
// while GCC names nothing at all, because its BMIs are implicit under
// gcm.cache and keyed by the name the SOURCE declares. A rule whose lib root
// declared anything other than its package name therefore built under GCC and
// failed under the other two, reporting a module nobody had written.
//
// `fallback` is returned when the interface cannot be read or declares no
// named module. Both of those are real conditions with better diagnostics
// elsewhere — `build_host_module` reports a missing interface unit by path —
// and those diagnostics need a name to report the package WITH.
inline std::string host_module_name(const std::filesystem::path& interfacePath,
                                    std::string_view fallback)
{
    std::ifstream is(interfacePath);
    if (!is) return std::string(fallback);
    std::stringstream buf;
    buf << is.rdbuf();
    auto declared = mcpp::pm::declared_module_roots(buf.str());
    if (declared.empty()) return std::string(fallback);
    return declared.front();
}

// The name a PRIMARY module interface unit declares, or an empty string.
//
// `declared_module_roots` answers a different question -- every module a file
// names, implementation units included -- and is the right reader for the lib
// root, which `build_host_module` compiles whatever kind of unit it is. The
// question here is which OTHER files of a package are interface units at all.
// A package that exposes several rules through features lists their sources
// (mcpp 2026.9.5.3+), and an implementation unit or a partition among those
// cannot be compiled alone under a name of its own. Only `export module
// <name>;` qualifies: `module <name>;`, `export module <name>:<part>;` and the
// global module fragment `module;` do not, and a line that is a comment does
// not either -- a rule's own documentation may quote the declaration.
inline std::string declared_interface_name(std::string_view source)
{
    auto is_name = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '_' || c == '.';
    };
    auto is_ws = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
    std::size_t lineStart = 0;
    while (lineStart < source.size()) {
        auto eol = source.find('\n', lineStart);
        if (eol == std::string_view::npos) eol = source.size();
        auto line = source.substr(lineStart, eol - lineStart);
        lineStart = eol + 1;

        std::size_t i = 0;
        while (i < line.size() && is_ws(line[i])) ++i;
        if (line.substr(i).starts_with("//")) continue;
        if (!line.substr(i).starts_with("export")) continue;
        i += 6;
        if (i >= line.size() || !is_ws(line[i])) continue;
        while (i < line.size() && is_ws(line[i])) ++i;
        if (!line.substr(i).starts_with("module")) continue;
        i += 6;
        if (i >= line.size() || !is_ws(line[i])) continue;
        while (i < line.size() && is_ws(line[i])) ++i;
        std::size_t start = i;
        while (i < line.size() && is_name(line[i])) ++i;
        if (i == start) continue;                 // `export module;` or `:part`
        std::string name(line.substr(start, i - start));
        while (i < line.size() && is_ws(line[i])) ++i;
        if (i >= line.size() || line[i] != ';') continue;   // a partition, or noise
        return name;
    }
    return {};
}

// One host module as registered for one consumer's build program.
struct HostModule {
    std::string           module;     // what `import` in build.mcpp addresses
    std::string           package;    // fully-qualified package identity
    // Carried rather than split back out of `package`: the reserved-prefix
    // rule asks whether the package is in the official namespace, and
    // re-deriving that from the joined string would be the same decision made
    // in a second place.
    std::string           nameSpace;
    std::filesystem::path interface;
    // False when this module is in the list only because ANOTHER rule imports
    // it. It still has to be compiled — a rule cannot build without its own
    // imports — but the consumer never declared it, and provisions cross one
    // further edge only on a `reexport = true` edge.
    bool                  importable = true;
};

// Two rule packages registering the same module name fails, and it fails in a
// place that cannot be traced back to the cause. `build_host_module` derives
// both the BMI and the object filename from the registered name
// (`bdir / (stem + objExt)`), so the second compile OVERWRITES the first's
// object — silently — and then the one surviving object is handed to the link
// twice, once per registered module. Measured, with this check removed:
//
//     ld: …/target/.build-mcpp/sharedname.o: in function `initializer for
//     module sharedname': beta.cppm: multiple definition of `initializer for
//     module sharedname'; …/sharedname.o: beta.cppm: first defined here
//
// One file named twice, one package named twice, and no mention anywhere that
// two packages are involved or which two. The overwrite is the silent part;
// the diagnostic is merely unactionable. Refusing here is what turns it into a
// sentence the reader can act on.
//
// Scoped to what one `build.mcpp` can see. Deliberately NOT an index-wide
// uniqueness rule: `path` dependencies and private registries are invisible to
// that, and the index carries no such invariant for this to inherit.
inline std::optional<std::string>
host_module_collision(const std::vector<HostModule>& mods)
{
    std::map<std::string, const HostModule*> seen;
    for (auto const& m : mods) {
        auto [it, fresh] = seen.emplace(m.module, &m);
        if (fresh) continue;
        return std::format(
            "two build rules both provide the module '{}':\n"
            "  {}\n"
            "      {}\n"
            "  {}\n"
            "      {}\n"
            "A rule's module name is what `import` addresses, so these two are "
            "indistinguishable to the compiler and one would silently replace "
            "the other. Rename the module one of them declares, or depend on "
            "only one of these packages.",
            m.module,
            it->second->package, it->second->interface.string(),
            m.package,           m.interface.string());
    }
    return std::nullopt;
}

// `mcpp.*` reads to a user as an endorsement by the mcpp project, and handing
// out that impression is a supply-chain statement. It is a WARNING rather than
// an error because the engine cannot decide who is official: a path
// dependency, a private mirror and an internal fork are all legitimate and all
// indistinguishable from here. The message names both halves — the module name
// and the package identity — because exactly one of them is the surprising one
// and the reader is better placed to say which.
inline constexpr std::string_view kReservedModulePrefix = "mcpp.";
inline constexpr std::string_view kOfficialNamespace    = "mcpp";

inline std::optional<std::string>
reserved_prefix_warning(std::string_view moduleName,
                        std::string_view packageNamespace,
                        std::string_view packageFqn)
{
    if (!moduleName.starts_with(kReservedModulePrefix)) return std::nullopt;
    if (packageNamespace == kOfficialNamespace)         return std::nullopt;
    return std::format(
        "build rule '{}' declares the module '{}'; the '{}' prefix is reserved "
        "for rules maintained by the mcpp project. Nothing breaks — the name "
        "simply claims an origin the package does not have.",
        packageFqn, moduleName, kReservedModulePrefix);
}

} // namespace mcpp::build::provisions

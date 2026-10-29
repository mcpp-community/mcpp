// mcpp.manifest.platform -- the data a platform states (SPEC-013; design
// 2026-10-28-platforms-design.md §4).
//
// A PLATFORM is one reusable, versioned configuration of a class of targets:
// the toolchain, the default provider of each target-side layer, the tools the
// build runs on this machine, the compile and link conventions, the artifact's
// naming and sealing, and the runner. A platform package states it in its own
// manifest under `[platform]`; a project overrides it field by field under
// `[target.<selector>]` (and the host graph's under `[host]`) with the same key
// names; a build program states the parts that must be computed with
// directives of the same meaning. All three read into `Statement`, every field
// carrying the place it was stated, so the engine merges one structure and
// reports one source per value.
//
// This module is pure data. The parser is mcpp.manifest.toml's (it owns the
// TOML document and the dependency grammar); selection, merging, injection and
// reporting are the engine's (`src/platform`).

export module mcpp.manifest.platform;

import std;
import mcpp.pm.dep_spec;

export namespace mcpp::manifest::platform {

// The tier a value was stated at. Ordered by precedence where tiers compete:
// a later tier replaces an earlier one field by field (§4.5).
enum class Tier : std::uint8_t {
    BuiltIn,   // the engine's own conventions for a host target
    Platform,  // the platform package's `[platform]` section
    Program,   // the platform's plugin or a build program (directives)
    Config,    // config.toml: `[target.<selector>]`, `[host]`
    Manifest,  // the project's mcpp.toml: `[target.<selector>]`, `[host]`
    Env,       // MCPP_TARGET_<TRIPLE>_<KEY>, MCPP_HOST_PLATFORM
    Flag,      // --platform, --host-platform
    Graph,     // a package of the dependency graph provides it (§4.3)
};

constexpr std::string_view tier_name(Tier t) {
    switch (t) {
        case Tier::BuiltIn:  return "built-in";
        case Tier::Platform: return "platform";
        case Tier::Program:  return "build-program";
        case Tier::Config:   return "config.toml";
        case Tier::Manifest: return "mcpp.toml";
        case Tier::Env:      return "env";
        case Tier::Flag:     return "flag";
        case Tier::Graph:    return "graph";
    }
    return "platform";
}

// Where one value was stated: the tier, the file and line when there is one,
// and the key as written (`[target.'cfg(platform = "openkal")'].runner`,
// `MCPP_TARGET_X86_64_OPENKAL_PLATFORM`).
struct Origin {
    Tier        tier = Tier::Platform;
    std::string file;
    int         line = 0;
    std::string key;
};

// A package a platform names: the platform itself, a layer's provider, the
// package of a tool, or the carrier.
//
// String form `"[<namespace>.]<name>@<version>"`; table form the dependency
// grammar (`version`, `path`, `git` with `rev`/`tag`/`branch`, `features`,
// `default-features`) plus `name`, which the string form carries in front of
// the `@`. A path or git source may leave `name` out: the package's own
// manifest then names it.
struct PackageRef {
    std::string               name;      // "[ns.]name"; empty: the source names it
    mcpp::pm::DependencySpec  spec;      // the source and the requests on the edge
    // `tools.<name> = { program = "<program>" }`: a program found in a
    // declared payload or on PATH, not built.
    std::string               program;
    // `layers.<layer> = false`: the platform provides no package for the layer.
    bool                      off = false;

    bool empty() const {
        return name.empty() && program.empty() && !off && spec.path.empty()
            && spec.git.empty() && spec.version.empty();
    }
};

// A scalar or an argv with its origin. `set` distinguishes "absent" from
// "present and empty"; the struct carries no std::optional because an
// optional of a string as a member of an exported struct instantiates its
// special members in every importer, which MSVC's standard library under
// clang refuses (see TargetEntry::sysroot in mcpp.manifest.types).
struct Text {
    std::string value;
    Origin      origin;
    bool        set = false;
};

struct Words {
    std::vector<std::string> value;
    Origin                   origin;
    bool                     set = false;
};

struct Flag {
    bool   value = false;
    Origin origin;
    bool   set = false;
};

// `runner = ["okrun", "{}"]` or `runner = { cmd = [...], host = "same-arch",
// exclusive = true }`. `host`: "" (unstated), "same-arch" (the artifact runs
// on a machine of its architecture), "any" (the runner reaches it from any
// machine, an emulator or a probe).
struct Runner {
    std::vector<std::string> cmd;
    std::string              host;
    // The `<os>-<arch>` pairs the runner program exists for (design 2026-10-29
    // §4.4): where a package that needs it can run. Unset: unknown, and a
    // package's coverage names no pair through the runner.
    std::vector<std::string> hosts;
    bool                     hostsSet = false;
    bool                     exclusive    = false;
    bool                     exclusiveSet = false;
    Origin                   origin;
    bool                     set = false;
};

// The four flag lists of `[platform.build]` and of each of its
// `[platform.build.'cfg(...)']` tables.
struct Flags {
    std::vector<std::string> cflags, cxxflags, asmflags, ldflags;
    bool empty() const {
        return cflags.empty() && cxxflags.empty() && asmflags.empty() && ldflags.empty();
    }
};

struct CfgFlags {
    std::string predicate;    // the selector, `cfg(arch = "aarch64")`
    Flags       flags;
    Origin      origin;
};

// One input of the link besides the objects: a file of the platform package,
// or the standard output of a program saved as `as` (assembled when it ends
// in `.S` or `.s`, linked as it is otherwise).
struct LinkInput {
    std::string              file;   // absolute, inside the package that stated it
    std::vector<std::string> from;   // argv; `from[0]` is a program name
    std::string              as;     // the file name the output is saved under
};

// `[platform.link]`: how the artifact is produced (§4).
struct Link {
    Text  driver;    // "compiler" | "lld"
    Words flags;     // given to ld.lld itself when driver = "lld"
    Text  start;     // the entry symbol
    Text  script;    // a linker script, absolute, inside the stating package
    std::vector<LinkInput> inputs;
    Origin inputsOrigin;
    bool   inputsSet = false;
    Text  exe;       // "{name}.<ext>"
    Text  shared;    // "lib{name}.<ext>"
    // Post-link steps, each an argv run on the linked file (`{}`) before it
    // is published (§4.6).
    std::vector<std::vector<std::string>> seal;
    Origin sealOrigin;
    bool   sealSet = false;
};

// `carrier = { package = "<spec>", section = "<name>", main = "<file>" }` (M3,
// §4.7): the package a self-contained program of this machine is built over,
// the section the carried artifact is placed in, and the source of the
// program's entry, a file of the stating package that runs the artifact it
// finds between `mcpp_carried_begin` and `mcpp_carried_end`.
//
// A platform may state several (an array of such tables), each for the
// `<os>-<arch>` pairs in its `hosts`; an entry without `hosts` serves every
// pair (design 2026-10-29 §4.5). `build` names how the carrier program is
// built: "row" (the default), one program per `<os>-<arch>` over the
// platform's hosted arrangement. An entry of a model this mcpp does not know
// is skipped with a warning, so a platform can add one without breaking an
// older mcpp.
struct Carrier {
    PackageRef  package;
    std::string section;
    std::string main;     // absolute
    std::vector<std::string> hosts;
    std::string build = "row";
    Origin      origin;
    bool        set = false;
};

// The `<os>-<arch>` vocabulary of `runner.hosts`, `carrier.hosts` and a
// package's coverage: a system mcpp builds programs for, and an architecture.
inline bool is_host_pair(std::string_view s) {
    const auto dash = s.find('-');
    if (dash == std::string_view::npos || dash + 1 == s.size()) return false;
    const auto os = s.substr(0, dash);
    return (os == "linux" || os == "macos" || os == "windows")
        && s.find('-', dash + 1) == std::string_view::npos;
}

// The keys of a descriptor's per-platform blocks that `descriptor_keys` may
// name (the xpkg vocabulary, `macosx` for macOS).
inline constexpr std::string_view kDescriptorKeys[] = {
    "linux", "macosx", "windows", "android", "ios", "emscripten", "openkal", "none",
};

struct LayerEntry {
    PackageRef ref;
    Origin     origin;
};

struct ToolEntry {
    PackageRef ref;
    Origin     origin;
};

// `[platform.hosted]`: the part that applies when a HOST triple selects this
// platform (§5.4).
struct Hosted {
    // The host triples the arrangement is for (`x86_64-windows-gnu` and not
    // `-msvc`: a C library of the graph needs the GNU ABI on Windows). Unset:
    // every host triple.
    Words                             targets;
    Text                              toolchain;
    std::map<std::string, LayerEntry> layers;
    bool                              set = false;
};

// Everything one source states about a platform, in the seven groups of §4.
struct Statement {
    // [platform]
    Words                         targets;
    Text                          toolchain;
    Text                          sysroot;
    std::vector<std::string>      sysrootProvides;
    Runner                        runner;
    std::map<std::string, Words>  runners;
    // Every carrier entry, in the order stated; `carrier_for` picks one.
    std::vector<Carrier>          carriers;
    // The descriptor blocks the platform's targets read, in order (design
    // 2026-10-29 §5.1); unset: the rows' own.
    Words                         descriptorKeys;
    Text                          plugin;
    // `universal = true`: `mcpp pack` of any of the targets packs all of them
    // into one package (SPEC-014 §3).
    Flag                          universal;
    // [platform.layers], [platform.tools]
    std::map<std::string, LayerEntry> layers;
    std::map<std::string, ToolEntry>  tools;
    // [platform.build] and its cfg tables
    Flags                         build;
    Origin                        buildOrigin;
    std::vector<CfgFlags>         buildCfg;
    // [platform.link]
    Link                          link;
    // [platform.versions]
    std::map<std::string, std::string> versions;
    Origin                        versionsOrigin;
    // [platform.hosted]
    Hosted                        hosted;

    bool empty() const {
        return !targets.set && !toolchain.set && !sysroot.set && !runner.set
            && runners.empty() && carriers.empty() && !descriptorKeys.set && !plugin.set && !universal.set && layers.empty()
            && tools.empty() && build.empty() && buildCfg.empty()
            && !link.driver.set && !link.flags.set && !link.start.set && !link.script.set
            && !link.inputsSet && !link.exe.set && !link.shared.set && !link.sealSet
            && versions.empty() && !hosted.set;
    }
};

// The platform part of one `[target.<selector>]` table, or of `[host]`
// (selector empty): which platform, and the fields it overrides.
struct Override {
    std::string selector;
    PackageRef  platform;       // `platform = "<spec>"` or a table
    Origin      platformOrigin;
    bool        platformSet  = false;
    bool        platformNone = false;   // `platform = "none"`
    Statement   fields;
};

// The four target-side layers a platform may provide a package for (SPEC-002).
inline constexpr std::string_view kLayers[] = {
    "kernel-abi", "c-abi", "c++-abi", "compiler-runtime",
};

constexpr bool is_layer(std::string_view k) {
    for (auto l : kLayers) if (l == k) return true;
    return false;
}

// The keys a `[platform]` section may carry, and those of its sub-tables. One
// list per table, read by the parser's unknown-key sweep and by the tests.
inline constexpr std::string_view kPlatformKeys[] = {
    "targets", "toolchain", "sysroot", "runner", "runners", "carrier", "plugin",
    "universal", "descriptor_keys", "layers", "tools", "build", "link", "versions", "hosted",
};
inline constexpr std::string_view kBuildKeys[] = { "cflags", "cxxflags", "asmflags", "ldflags" };
inline constexpr std::string_view kLinkKeys[] = {
    "driver", "flags", "start", "script", "inputs", "exe", "shared", "seal",
};
inline constexpr std::string_view kHostedKeys[] = { "targets", "toolchain", "layers" };

// The `[target.<selector>]` keys a project or config.toml uses to select and
// override a platform. `runner`, `runners`, `toolchain` and `sysroot` are the
// table's existing keys and keep their parsers.
inline constexpr std::string_view kOverrideKeys[] = {
    "platform", "layers", "tools", "carrier", "link",
};

// The carrier entry of `s` that serves `pair` (`<os>-<arch>`): the first whose
// `hosts` names it, else the first without `hosts`. Null when none does.
inline const Carrier* carrier_for(const Statement& s, std::string_view pair) {
    const Carrier* open = nullptr;
    for (auto const& c : s.carriers) {
        if (c.hosts.empty()) { if (!open) open = &c; continue; }
        if (std::ranges::find(c.hosts, pair) != c.hosts.end()) return &c;
    }
    return open;
}

// `"[<namespace>.]<name>@<version>"` -> the ref; the error names what is
// missing. `none` is not a ref; callers test for it first.
inline std::expected<PackageRef, std::string> parse_ref_text(std::string_view text) {
    PackageRef r;
    const auto at = text.rfind('@');
    if (at == std::string_view::npos || at == 0 || at + 1 == text.size())
        return std::unexpected(std::format(
            "'{}' is not a package reference; write \"<name>@<version>\", for example "
            "\"openkal-platform@0.17.1\", or a table such as {{ path = \"<dir>\" }}", text));
    r.name = std::string(text.substr(0, at));
    r.spec.version = std::string(text.substr(at + 1));
    const auto dot = r.name.find('.');
    if (dot == std::string::npos) {
        r.spec.namespace_ = std::string(mcpp::pm::kDefaultNamespace);
        r.spec.shortName = r.name;
        r.spec.namespaceOmitted = true;
    } else {
        r.spec.namespace_ = r.name.substr(0, dot);
        r.spec.shortName = r.name.substr(dot + 1);
    }
    return r;
}

// The name a reference is shown under: `openkal-platform@0.17.1`,
// `openkal-musl (path ../openkal-musl)`, `okrun (program)`.
inline std::string display(const PackageRef& r) {
    if (r.off) return "false";
    if (!r.program.empty()) return std::format("{} (program)", r.program);
    std::string n = r.spec.shortName.empty() ? r.name : r.spec.shortName;
    if (!r.spec.path.empty())
        return n.empty() ? std::format("path {}", r.spec.path)
                         : std::format("{} (path {})", n, r.spec.path);
    if (!r.spec.git.empty())
        return std::format("{}{}{} ({} {})", n, n.empty() ? "" : " ", r.spec.git,
                           r.spec.gitRefKind, r.spec.gitRev);
    return std::format("{}@{}", n, r.spec.version);
}

// What a comparison of root positions keys an override on: its selector and
// the places its entries were written. One declaration is one key, so two
// workspace members whose plans an override would tell apart do not share one.
inline std::string identity(const Override& o) {
    std::string s = o.selector;
    auto add = [&](const Origin& g) { s += std::format("|{}:{}:{}", g.file, g.line, g.key); };
    if (o.platformSet) {
        s += o.platformNone ? std::string("|none") : "|" + display(o.platform);
        add(o.platformOrigin);
    }
    auto const& f = o.fields;
    for (auto const& [k, e] : f.layers) add(e.origin);
    for (auto const& [k, e] : f.tools) add(e.origin);
    for (auto const& [k, w] : f.runners) add(w.origin);
    for (auto const& c : f.carriers) add(c.origin);
    if (f.descriptorKeys.set) add(f.descriptorKeys.origin);
    if (f.runner.set) add(f.runner.origin);
    for (auto const* t : { &f.link.driver, &f.link.start, &f.link.script, &f.link.exe, &f.link.shared })
        if (t->set) add(t->origin);
    if (f.link.flags.set) add(f.link.flags.origin);
    if (f.link.inputsSet) add(f.link.inputsOrigin);
    if (f.link.sealSet) add(f.link.sealOrigin);
    return s;
}

// The name `cfg(platform = "<name>")` matches: the package name with a
// trailing `-platform` removed, so `openkal-platform` is the platform
// `openkal`. A selector may also name the package in full.
inline std::string platform_name(std::string_view packageShortName) {
    constexpr std::string_view kSuffix = "-platform";
    if (packageShortName.size() > kSuffix.size() && packageShortName.ends_with(kSuffix))
        return std::string(packageShortName.substr(0, packageShortName.size() - kSuffix.size()));
    return std::string(packageShortName);
}

} // namespace mcpp::manifest::platform

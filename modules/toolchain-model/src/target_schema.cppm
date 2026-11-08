// mcpp.toolchain.target_schema — the shape of a target row and of a provider.
//
// One schema for both sources of rows and providers: the built-in table
// (data/builtin.toml, compiled into mcpp.toolchain.builtin_targets by this
// package's build.mcpp) and a platform package's `[[platform.define]]` and
// `[[platform.provider]]` (SPEC-016). A built-in entry views static storage; an
// entry read from a platform package views the strings of the definition set
// that owns it.

export module mcpp.toolchain.target_schema;

import std;

export namespace mcpp::toolchain::target {

// A ROW DEFINES AN IDENTITY (SPEC-016 §2). The fields after `standard` are the
// facts of a row that the engine's conventions for a built-in system do not
// already answer: the ISA profile of a bare-metal row, and -- for a row a
// platform defines -- everything a build of an unfamiliar system needs.
struct Row {
    std::string_view triple;          // canonical `arch-os[-env]`
    std::string_view arch, os, env;   // its segments
    std::string_view tier;            // verified | preview | planned
    std::string_view note;            // display note
    bool             standard = true; // an industry-standard identity
    bool             exact = false;   // reached only by a request that names it whole
    std::string_view march;           // -march= of a bare-metal row
    std::string_view mabi;            // -mabi=; empty: none given
    std::string_view codeModel;       // -mcmodel=; empty: the driver's
    std::string_view multilib;        // a C library's multilib directory
    std::span<const std::string_view> flags;   // extra compile flags
    std::string_view linkEmulation;   // `ld.lld -m`, when the driver reaches no lld
    // The facts of a defined row (a platform's). Empty on a built-in row,
    // whose system's conventions answer them.
    std::string_view clangTriple;     // `{version}` / `{version:<default>}`
    std::string_view objectFormat;    // elf | mach-o | pe | wasm
    std::string_view family;          // unix | windows | ""
    std::string_view dataModel;       // lp64 | llp64 | ilp32
    std::string_view exeSuffix;
    std::string_view sharedSuffix;
    std::string_view loader;          // the program interpreter a dynamic program names
    std::span<const std::string_view> descriptorKeys;
    // The system the artifact runs on: the row's os by default, `loader` for an
    // image a loader binds, `none` for bare metal.
    std::string_view execution;
    bool             defined = false; // stated by a platform package
};

// A PROVIDER SERVES IDENTITIES (SPEC-016 §3). Several entries may share a name
// when one provider serves rows whose facts differ.
struct Provider {
    std::string_view name;
    std::span<const std::string_view> serves;   // `*` stands for the arch
    int              priority = 0;
    std::span<const std::string_view> hosts;    // `<os>-<arch>`, `*`, `{arch}`
    std::string_view probe;                     // msvc
    std::string_view toolchain;                 // empty: the host's default
    std::span<const std::string_view> pairs;    // empty: any family
    std::string_view sysroot;                   // a payload, or `toolchain`
    std::string_view linkage;                   // static | dynamic
    std::string_view platform;                  // a platform package reference
};

// THE PATTERN OF `serves`: segment by segment, `*` standing for any one segment
// (`*-linux-musl`, `x86_64-linux-*`); a pattern and a triple of different
// numbers of segments do not match.
constexpr bool serves_triple(std::string_view pattern, std::string_view triple) {
    if (pattern == triple) return true;
    for (;;) {
        const auto pd = pattern.find('-'), td = triple.find('-');
        const auto ps = pattern.substr(0, pd), ts = triple.substr(0, td);
        if (ps != "*" && ps != ts) return false;
        if ((pd == std::string_view::npos) != (td == std::string_view::npos)) return false;
        if (pd == std::string_view::npos) return true;
        pattern.remove_prefix(pd + 1);
        triple.remove_prefix(td + 1);
    }
}

constexpr bool serves(const Provider& p, std::string_view triple) {
    for (auto s : p.serves)
        if (serves_triple(s, triple)) return true;
    return false;
}

// WHERE A PROVIDER'S PAYLOAD EXISTS. `host` is `<os>-<arch>` of the build machine;
// a pattern segment `*` matches anything, `{arch}` the target's architecture.
constexpr bool host_matches(std::string_view pattern, std::string_view host,
                            std::string_view targetArch) {
    if (pattern == "*") return true;
    const auto pd = pattern.find('-');
    const auto hd = host.find('-');
    if (pd == std::string_view::npos || hd == std::string_view::npos) return pattern == host;
    const auto po = pattern.substr(0, pd), pa = pattern.substr(pd + 1);
    const auto ho = host.substr(0, hd), ha = host.substr(hd + 1);
    const bool os = po == "*" || po == ho;
    const bool arch = pa == "*" || pa == ha || (pa == "{arch}" && ha == targetArch);
    return os && arch;
}

constexpr bool available_on(const Provider& p, std::string_view host, std::string_view targetArch) {
    for (auto h : p.hosts)
        if (host_matches(h, host, targetArch)) return true;
    return false;
}

// A WORD OF A ROW'S DEFAULTS AND THE WORDS THAT STAND IN ITS PLACE (SPEC-016
// §5.2). A row's defaults are its machine description: `-march=`, `-mabi=`,
// `-mcmodel=`, its `flags`, and `-m <emulation>` of a link by ld.lld itself. A
// platform or a project replaces one by naming it as the row gives it; `with`
// empty removes it. The engine's conventions are not replaced, only added to.
// `origin` names the statement, for the reports.
struct Replacement {
    std::string              word;
    std::vector<std::string> with;
    std::string              origin;
};

// `words` with each replacement applied, in order. `met`, when given, receives
// the words of the replacements that met a word of the list.
std::vector<std::string> replaced(std::span<const std::string> words,
                                  std::span<const Replacement> replace,
                                  std::set<std::string>* met = nullptr);

} // namespace mcpp::toolchain::target

namespace mcpp::toolchain::target {

std::vector<std::string> replaced(std::span<const std::string> words,
                                  std::span<const Replacement> replace,
                                  std::set<std::string>* met) {
    std::vector<std::string> out;
    for (auto const& w : words) {
        auto r = std::ranges::find(replace, w, &Replacement::word);
        if (r == replace.end()) { out.push_back(w); continue; }
        if (met) met->insert(w);
        out.insert(out.end(), r->with.begin(), r->with.end());
    }
    return out;
}

} // namespace mcpp::toolchain::target

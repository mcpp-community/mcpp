// platform.cpp -- the platform of the target graph through the prepare phases
// (SPEC-013; design 2026-10-28 §5, §7-§9). Declared in `:state`.
//
//   P1   which platform (`step1_select_platform`): before the conditional
//        tables merge, so `cfg(platform = ...)` is answered with the rest of
//        the target's keys.
//   P4b  the platform package is the root's first dependency
//        (`step4b_seed_platform`); once the graph has been walked, its
//        `[platform]` is read and merged with this machine's and the
//        project's overrides, and the layers and tools the graph lacks are
//        added to it (`step4b_apply_platform`, walked again by the caller);
//        a version group the graph contradicts is refused
//        (`step4b_check_platform_versions`).
//   P5   its toolchain (`step5_platform_toolchain`).
//   P13  the statement every reader uses (`step13_finish_platform`):
//        directives merged in, flags evaluated for the target, tools located.

module mcpp.build.prepare;
import :state;

import std;
import mcpp.build.prepare_inputs;
import mcpp.build.refusal;
import mcpp.config;
import mcpp.invocation;
import mcpp.manifest;
import mcpp.platform;
import mcpp.provider;          // the synced index's roots (the platform column, design 2026-11-16 §6.2)
import mcpp.pm.refresh_policy; // a miss of the platform column refreshes as a dependency's does (mcpp#967)
import mcpp.pm.compat;
import mcpp.project;
import mcpp.targetside;
import mcpp.toolchain.triple;
import mcpp.toolchain.platform;
import mcpp.toolchain.target_schema;
import mcpp.toolchain.target_rows;
import mcpp.toolchain.target_select;
import mcpp.toolchain.builtin_targets;
import mcpp.ui;
import mcpp.diag;
import mcpp.version_req;
import mcpp.pm.resolver;
import mcpp.xpkg_version;
import mcpp.version;
import mcpp.freestanding.linkline;

namespace mcpp::build {

namespace fs = std::filesystem;
namespace mp = mcpp::manifest::platform;

namespace {

mcpp::toolchain::triple::Triple target_of(const PrepareState& state) {
    if (!state.overrides.target_triple.empty())
        if (auto t = mcpp::toolchain::triple::parse(state.overrides.target_triple)) return *t;
    return mcpp::toolchain::triple::host_triple();
}

// The package short name a reference names, before the package is fetched:
// the reference's own name (a git reference always has one), or the
// `[package]` of a directory.
std::string known_short_name(const mp::PackageRef& r) {
    if (!r.spec.shortName.empty()) return r.spec.shortName;
    if (!r.spec.path.empty()) {
        if (auto m = mcpp::manifest::load(fs::path(r.spec.path) / "mcpp.toml"))
            return mcpp::pm::compat::resolve_package_name(m->package.name, m->package.namespace_)
                .shortName;
        return {};
    }
    // A repository named by its URL alone is the package its last segment
    // names (`.../openkal-platform.git` is `openkal-platform`); the fetched
    // manifest's own name is checked against it as any dependency's is.
    if (!r.spec.git.empty()) {
        std::string_view url = r.spec.git;
        while (!url.empty() && (url.back() == '/' || url.back() == '\\')) url.remove_suffix(1);
        if (url.ends_with(".git")) url.remove_suffix(4);
        const auto cut = url.find_last_of("/\\:");
        return std::string(cut == std::string_view::npos ? url : url.substr(cut + 1));
    }
    return {};
}

// Whether this invocation's command line stated a variable (`--platform` is
// `MCPP_PLATFORM` for the invocation), rather than the environment.
bool stated_on_command_line(std::string_view name) {
    return std::ranges::any_of(mcpp::invocation::overrides(), [&](auto const& o) {
        return o.first == name && o.second.has_value();
    });
}

// The project's override tables: the plan root's, which hold the workspace
// layer's first (`merge_layer_root_position`).
std::vector<mp::Override> project_tables(const mcpp::manifest::Manifest& m) {
    return m.platformOverrides;
}

// Which layers each package of the graph provides, by layer.
std::map<std::string, std::string> provided_layers(const PrepareState& state) {
    std::map<std::string, std::string> out;
    for (std::size_t i = 1; i < state.packages.size(); ++i) {
        auto const& pm = state.packages[i].manifest;
        for (auto const& entry : pm.provides) {
            auto cap = mcpp::targetside::parse_capability(entry);
            if (!cap || !*cap) continue;
            auto const& c = **cap;
            std::string layer(mcpp::targetside::cap_layer_name(c.layer));
            if (!mp::is_layer(layer)) continue;
            out.try_emplace(layer, pm.package.version.empty()
                ? pm.package.name
                : std::format("{}@{}", pm.package.name, pm.package.version));
        }
    }
    return out;
}

std::string qualified(const mcpp::manifest::Manifest& m) {
    auto n = mcpp::pm::compat::resolve_package_name(m.package.name, m.package.namespace_);
    return mcpp::pm::compat::qualified_name(n.namespace_, n.shortName);
}

// A package the platform added, as `platformInjected` records it: qualified,
// the default namespace spelled out (the lock asks in that spelling).
std::string injected_name(const mcpp::manifest::DependencySpec& spec) {
    return mcpp::pm::compat::qualified_name(
        spec.namespace_.empty() ? std::string(mcpp::pm::kDefaultNamespace) : spec.namespace_, spec.shortName);
}

// The key a worklist item for a reference is written under: its name, or
// the short name its source gives.
std::string item_key(const mp::PackageRef& r) {
    if (!r.name.empty()) return r.name;
    return known_short_name(r);
}

// The selector evaluator the platform module asks: this graph's context.
mcpp::platform::Matches matcher(cfgpred::Ctx c) {
    return [c = std::move(c)](std::string_view sel) { return cfgpred::matches(std::string(sel), c); };
}

} // namespace

std::expected<std::string, std::string>
platform_selection_for(const mcpp::toolchain::triple::Triple& t, const BuildOverrides& overrides) {
    const auto flag = !overrides.platform_choice.empty()
        ? overrides.platform_choice
        : mcpp::invocation::get("MCPP_PLATFORM").value_or("");
    const auto envKey = target_env_name(t, "PLATFORM");
    const auto env = mcpp::invocation::get(envKey).value_or("");
    std::vector<mp::Override> project;
    std::error_code ec;
    auto root = !overrides.project_root.empty()
        ? std::optional<fs::path>(overrides.project_root)
        : mcpp::project::find_manifest_root(fs::current_path(ec));
    if (root)
        if (auto m = mcpp::manifest::load(*root / "mcpp.toml")) project = project_tables(*m);
    // Read, never initialised: asking which platform a target selects writes
    // nothing, and a machine without a configuration has no tables to apply.
    std::vector<mp::Override> config;
    if (auto c = mcpp::config::load_or_init(/*quiet=*/true, {}, {},
                                            mcpp::config::Initialization::ReadOnly))
        config = c->platformOverrides;
    // The built-in default is the platform of the provider the selection takes
    // for this identity on this host (SPEC-016 §4), not of the row's first.
    std::string column;
    {
        auto ctx = target_context(project, config, {}, {}, [] { return false; });
        auto s = mcpp::toolchain::target::select(t.str(), t.arch, t.os, t.env, true, ctx);
        if (s.provider) column = std::string(s.provider->platform);
    }
    mcpp::platform::ChoiceInputs in{ flag, env, envKey, project, config, column };
    if (overrides.platform_choice.empty() && !stated_on_command_line("MCPP_PLATFORM"))
        in.flagKey = "MCPP_PLATFORM";
    auto sel = mcpp::platform::select_target(in, matcher(cfgpred::context_for(t.str())));
    if (!sel) return std::unexpected(sel.error());
    if (!sel->builtin.empty()) return "builtin:" + sel->builtin;
    return sel->none ? std::string{} : mp::display(sel->ref);
}

// THE PLATFORM'S DEFINITIONS (SPEC-016 §2, §3): its rows, its providers, and
// `[platform]` and `[platform.hosted]` as the providers they are shorthands of,
// registered for the process so the parser, the selection and `cfg(os = …)`
// know them. `ref` is how a provider names the package (`<ns>.<name>@<version>`,
// or `path:<dir>`).
void define_platform(const mp::Statement& s, std::string_view shortName, std::string_view ref) {
    namespace target = mcpp::toolchain::target;
    target::DefinitionSet set;
    set.source = std::string(ref);
    const auto name = mp::platform_name(shortName);
    for (auto const& d : s.defines) {
        target::Row r;
        r.triple = set.keep(d.triple);
        std::vector<std::string> seg;
        for (auto part : std::views::split(d.triple, '-')) seg.emplace_back(part.begin(), part.end());
        r.arch = set.keep(seg[0]);
        r.os = set.keep(seg[1]);
        r.env = set.keep(seg.size() > 2 ? seg[2] : std::string{});
        r.tier = set.keep(d.tier);
        r.note = set.keep(d.note);
        r.standard = d.standard;
        r.exact = d.exact;
        r.march = set.keep(d.march);
        r.mabi = set.keep(d.mabi);
        r.codeModel = set.keep(d.codeModel);
        r.multilib = set.keep(d.multilib);
        r.flags = set.keep(d.flags);
        r.linkEmulation = set.keep(d.linkEmulation);
        r.clangTriple = set.keep(d.clangTriple);
        r.gccTriple = set.keep(d.gccTriple);
        r.objectFormat = set.keep(d.objectFormat);
        r.family = set.keep(d.family);
        r.dataModel = set.keep(d.dataModel);
        r.exeSuffix = set.keep(d.exeSuffix);
        r.sharedSuffix = set.keep(d.sharedSuffix);
        r.loader = set.keep(d.loader);
        r.descriptorKeys = set.keep(d.descriptorKeys);
        r.companions = set.keep(d.companions);
        r.linkWithoutMain = set.keep(d.linkWithoutMain);
        r.interfaces = set.keep(d.interfaces);
        r.interfacesStated = d.interfacesStated;
        r.exceptions = set.keep(d.exceptions);
        r.exeSuffixFixed = d.exeSuffixFixed;
        r.execution = set.keep(d.execution.empty() ? seg[1] : d.execution);
        r.defined = true;
        set.rows.push_back(r);
    }
    auto provider = [&](const mp::StatedProvider& sp) {
        target::Provider p;
        p.name = set.keep(sp.name);
        p.serves = set.keep(sp.serves);
        p.priority = sp.priority;
        p.hosts = set.keep(sp.hosts.empty() ? std::vector<std::string>{"*"} : sp.hosts);
        p.pairs = set.keep(sp.pairs);
        p.toolchain = set.keep(sp.toolchain);
        p.sysroot = set.keep(sp.sysroot);
        p.linkage = set.keep(sp.linkage);
        p.platform = set.keep(std::string(ref));
        p.tier = set.keep(sp.tier);
        p.systemHeaders = set.keep(sp.systemHeaders);
        p.runner = set.keep(sp.runner);
        p.runnerHosts = set.keep(sp.runnerHosts);
        p.runnerRequires = set.keep(sp.runnerRequires);
        p.runnerHost = set.keep(sp.runnerHost);
        p.runnerEnabled = sp.runnerEnabled;
        p.runnerExclusive = sp.runnerExclusive;
        set.providers.push_back(p);
    };
    for (auto const& sys : s.systems) {
        auto const& k = sys.kernelAbi;
        set.systems.push_back({ .name = set.keep(sys.name), .uname = set.keep(sys.uname),
                                .resources = sys.resources, .commandScripts = set.keep(sys.commandScripts),
                                .objectFormat = set.keep(sys.objectFormat), .exeSuffix = set.keep(sys.exeSuffix),
                                .sharedSuffix = set.keep(sys.sharedSuffix),
                                .kernelAbi = { .header = set.keep(k.header), .version = set.keep(k.version),
                                               .packed = set.keep(k.packed), .bits = set.keep(k.bits),
                                               .include = set.keep(k.include),
                                               .archInclude = set.keep(k.archInclude),
                                               .package = set.keep(k.package) },
                                .interfaces = set.keep(sys.interfaces),
                                .interfacesStated = sys.interfacesStated });
    }
    // A platform the engine's own providers name (openkal-platform) is served by
    // those; its shorthands are not registered twice.
    const bool builtin = std::ranges::any_of(mcpp::toolchain::builtin::kProviders, [&](auto const& b) {
        auto r = b.platform.empty() ? std::nullopt : std::optional(mp::parse_ref_text(b.platform));
        return r && *r && (*r)->spec.shortName == shortName;
    });
    for (auto const& p : mp::stated_providers(s, name))
        if (!p.shorthand || !builtin) provider(p);
    target::define(std::move(set));
}

// A PLATFORM IS SELECTED BY WHAT THE REQUEST SPELLS (SPEC-013 §3.1): `arch`,
// `os`, `env` and the families. A layer (`c-abi`, ...) is what the selected
// platform states, so a selector that names one holds only for a row whose
// own data states the layer. Measured on a project of 26 targets:
// `cfg(c-abi = "musl")` selected the platform for `x86_64-linux-musl` and not
// for `x86_64-macos-musl`, which then took the engine's default.
void warn_platform_selected_by_a_layer(const mcpp::manifest::Manifest& m) {
    for (auto const& o : project_tables(m)) {
        if (!o.platformSet || !o.selector.starts_with("cfg(")) continue;
        for (auto layer : mp::kLayers) {
            const auto at = o.selector.find(layer);
            if (at == std::string::npos) continue;
            // A whole word: `c-abi` is not found inside `c++-abi`.
            const auto word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '+' || c == '_'; };
            if ((at > 0 && word(o.selector[at - 1]))
                || (at + layer.size() < o.selector.size() && word(o.selector[at + layer.size()])))
                continue;
            mcpp::diag::warning("manifest/platform-selector", std::format(
                "[target.'{}'] platform: the selector names the layer `{}`, which the platform "
                "itself states; before a platform is selected it holds only for a row whose own "
                "data states it. Select by what the triple spells: `arch`, `os`, `env` "
                "(for example cfg(env = \"musl\")).", o.selector, layer));
            break;
        }
    }
}

void define_early_platform(PrepareState& state) {
    const auto raw = state.overrides.target_triple;
    if (state.m) warn_platform_selected_by_a_layer(*state.m);
    // The explicit choice for this request, by the words that need no parse: the
    // flag, the environment and the tables whose selector is the request itself.
    std::optional<mp::PackageRef> chosen;
    bool noneChosen = false;
    std::string chosenText = !state.overrides.platform_choice.empty() ? state.overrides.platform_choice
                           : mcpp::invocation::get("MCPP_PLATFORM").value_or("");
    if (chosenText.empty() && !raw.empty()) {
        std::string key = "MCPP_TARGET_";
        for (char c : raw) key += std::isalnum(static_cast<unsigned char>(c)) ? char(std::toupper(static_cast<unsigned char>(c))) : '_';
        chosenText = mcpp::invocation::get(key + "_PLATFORM").value_or("");
    }
    if (!chosenText.empty()) {
        if (auto sel = mcpp::platform::parse_choice(chosenText, mp::Origin{mp::Tier::Flag, {}, 0, "--platform"});
            sel && !sel->none)
            chosen = sel->ref;
    } else if (!raw.empty()) {
        // The tables whose selector the request answers by its words alone: the
        // triple, or a `cfg` of its arch, os and env, which a triple no row
        // defines yet (`x86_64-netbsd`) states as written. The project's tables
        // before the machine's, the most specific of each (SPEC-004 §3.1.1), as
        // the selection proper chooses once the row is defined (§3.1).
        std::vector<mp::Override> config;
        if (auto cfg = state.get_cfg(false); cfg) config = (*cfg)->platformOverrides;
        const auto project = project_tables(*state.m);
        auto lexical = cfgpred::context_for(raw);
        if (lexical.os.empty()) {
            // No row knows the system yet: `<arch>-<os>[-<env>]` as written.
            std::vector<std::string> seg;
            for (auto part : std::views::split(std::string_view(raw), '-')) seg.emplace_back(part.begin(), part.end());
            if (seg.size() >= 2 && seg.size() <= 3 && std::ranges::none_of(seg, [](auto const& x) { return x.empty(); })) {
                lexical.os = seg[1];
                if (seg.size() == 3) lexical.env = seg[2];
            }
        }
        const auto words = matcher(std::move(lexical));
        const std::array<const std::vector<mp::Override>*, 2> spans{&project, &config};
        for (auto const* tables : spans) {
            const mp::Override* most = nullptr;
            for (auto const* o : mcpp::platform::matching(*tables, words))
                if (o->platformSet) most = o;
            if (!most) continue;
            // `platform = "none"` in a table is a choice as `--platform none`
            // is: no platform, so the index's column is not asked (mcpp#996).
            // Before, only the flag's `none` ended the lookup.
            if (most->platformNone) noneChosen = true;
            else chosen = most->platform;
            break;
        }
    }
    fs::path root = state.overrides.platform_root;
    if (root.empty() && chosen && !chosen->spec.path.empty()) root = chosen->spec.path;
    if (!root.empty())
        if (auto m = mcpp::manifest::load(root / "mcpp.toml"); m && m->platformDeclared) {
            auto n = mcpp::pm::compat::resolve_package_name(m->package.name, m->package.namespace_);
            define_platform(m->platform, n.shortName, "path:" + root.generic_string());
        }
    // A request no definition knows, with a platform chosen for it that is not on
    // this machine yet: a provisional row lets the first pass fetch the platform.
    // With no choice, the index's platform column may answer (design 2026-11-16
    // §6.2).
    // A request that parses but names a row nothing defines (`x86_64-toyos`)
    // is answered so only by the column.
    if (raw.empty() || noneChosen) return;
    if (const auto parsed = mcpp::toolchain::triple::parse(raw);
        parsed && (chosen || !chosenText.empty() || parsed->defined()
                   || mcpp::toolchain::triple::find_known_target(*parsed)))
        return;
    if (!chosen && chosenText.empty())
        if (auto column = index_platform_for(state, raw, refused_without_platform(state, raw)); !column.empty())
            if (auto ref = mp::parse_ref_text(column)) chosen = *ref;
    if (!chosen) return;
    std::vector<std::string> seg;
    for (auto part : std::views::split(std::string_view(raw), '-')) seg.emplace_back(part.begin(), part.end());
    if (seg.size() < 2 || seg.size() > 3 || std::ranges::any_of(seg, [](auto const& x) { return x.empty(); })) return;
    namespace target = mcpp::toolchain::target;
    target::DefinitionSet set;
    set.provisional = true;
    set.source = "provisional:" + raw;
    target::Row r;
    r.triple = set.keep(raw);
    r.arch = set.keep(seg[0]);
    r.os = set.keep(seg[1]);
    r.env = set.keep(seg.size() > 2 ? seg[2] : std::string{});
    r.tier = set.keep("preview");
    r.objectFormat = set.keep("elf");
    r.family = set.keep("unix");
    r.execution = set.keep(seg[1]);
    r.defined = true;
    set.rows.push_back(r);
    target::define(std::move(set));
    state.provisionalTarget = true;
}

namespace {

// One claim of the platform column: the package whose index entry lists a row.
struct ColumnClaim {
    int rank;
    std::string package;
};
using ColumnClaims = std::map<std::string, std::vector<ColumnClaim>>;
// (root, rank, the namespace of a descriptor that names none)
using ColumnRoot = std::tuple<fs::path, int, std::string>;

// `<key> = ` at the start of a line or after `{` or `,`: the first such key is
// the package table's own (its keys precede its nested tables).
std::size_t lua_field(std::string_view lua, std::string_view key) {
    for (std::size_t pos = 0; (pos = lua.find(key, pos)) != std::string_view::npos; pos += key.size()) {
        std::size_t b = pos;
        while (b > 0 && (lua[b - 1] == ' ' || lua[b - 1] == '\t')) --b;
        std::size_t e = pos + key.size();
        while (e < lua.size() && (lua[e] == ' ' || lua[e] == '\t')) ++e;
        if ((b == 0 || lua[b - 1] == '\n' || lua[b - 1] == ',' || lua[b - 1] == '{') && e < lua.size()
            && lua[e] == '=' && (e + 1 >= lua.size() || lua[e + 1] != '='))
            return e + 1;
    }
    return std::string_view::npos;
}

std::string lua_quoted(std::string_view lua, std::size_t at) {
    std::string out;
    const auto q1 = lua.find('"', at), nl = lua.find('\n', at);
    if (q1 == std::string_view::npos || (nl != std::string_view::npos && q1 > nl)) return out;
    const auto q2 = lua.find('"', q1 + 1);
    if (q2 != std::string_view::npos) out = std::string(lua.substr(q1 + 1, q2 - q1 - 1));
    return out;
}

// The rows one descriptor's `platform_rows` lists, claimed for its package.
void claim_rows(std::string_view lua, int rank, const std::string& defaultNs, ColumnClaims& out) {
    const auto rows = lua_field(lua, "platform_rows");
    if (rows == std::string_view::npos) return;
    const auto open = lua.find('{', rows), close = lua.find('}', rows);
    if (open == std::string::npos || close == std::string::npos || close < open) return;
    const auto nameAt = lua_field(lua, "name"), nsAt = lua_field(lua, "namespace");
    if (nameAt == std::string_view::npos) return;
    const auto ns = nsAt == std::string_view::npos ? defaultNs : lua_quoted(lua, nsAt);
    auto name = lua_quoted(lua, nameAt);
    // A descriptor may spell its name qualified (`acme.plat`).
    if (name.starts_with(ns + ".")) name.erase(0, ns.size() + 1);
    if (ns.empty() || name.empty()) return;
    const auto pkg = ns + "." + name;
    for (std::size_t p = open; p < close; ++p) {
        if (lua[p] != '"') continue;
        const auto q = lua.find('"', p + 1);
        if (q == std::string::npos || q > close) break;
        auto& who = out[std::string(lua.substr(p + 1, q - p - 1))];
        if (std::ranges::none_of(who, [&](auto const& c) { return c.package == pkg; }))
            who.push_back({rank, pkg});
        p = q;
    }
}

ColumnClaims scan_column(const std::vector<ColumnRoot>& roots) {
    ColumnClaims built;
    for (auto const& [root, rank, defaultNs] : roots) {
        std::error_code ec;
        const auto pkgs = root / "pkgs";
        if (!fs::is_directory(pkgs, ec)) continue;
        for (auto it = fs::recursive_directory_iterator(pkgs, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (it->path().extension() != ".lua") continue;
            std::ifstream is(it->path(), std::ios::binary);
            claim_rows(std::string{std::istreambuf_iterator<char>(is), {}}, rank, defaultNs, built);
        }
    }
    return built;
}

// The project's own indices ([indices] of mcpp.toml) rank before the shared ones.
std::vector<ColumnRoot> column_roots(const PrepareState& state, const mcpp::config::GlobalConfig& cfg) {
    std::vector<ColumnRoot> roots;
    const auto add = [&](fs::path dir, int rank, std::string ns) {
        if (std::ranges::none_of(roots, [&](auto const& r) { return std::get<0>(r) == dir; }))
            roots.emplace_back(std::move(dir), rank, std::move(ns));
    };
    if (state.root && state.m) {
        const auto project = state.projectIndexRoot();
        for (auto const& [name, spec] : state.m->indices)
            if (spec.is_local()) add(mcpp::config::resolve_project_index_path(*state.root, spec), 0, name);
        for (auto const& dir : mcpp::provider::layout().index_roots_of(project / ".mcpp"))
            add(dir, 0, dir.filename().string());
        for (auto const& data : mcpp::config::project_xlings_data_roots(project))
            for (auto const& dir : mcpp::provider::layout().index_roots_of(data.parent_path()))
                add(dir, 0, dir.filename().string());
    }
    for (auto const& dir : mcpp::provider::open(mcpp::config::make_xlings_env(cfg))->index_roots())
        add(dir, 1, std::string(mcpp::pm::kDefaultNamespace));
    return roots;
}

std::mutex g_columnLock;
std::map<std::string, std::shared_ptr<const ColumnClaims>> g_columnScans;
// A row the column missed, with what its refusal adds about the index.
std::map<std::string, std::string> g_columnMisses;

std::shared_ptr<const ColumnClaims> column_claims(const PrepareState& state, const mcpp::config::GlobalConfig& cfg) {
    // The roots are found and the scan runs with no lock held: the
    // configuration and the provider may wait on the home's lock, which a leg
    // prepared beside this one may hold while it reaches this function.
    const auto roots = column_roots(state, cfg);
    std::string key;
    for (auto const& [root, rank, defaultNs] : roots) key += std::format("{}\t{}\t{}\n", root.string(), rank, defaultNs);
    {
        std::scoped_lock guard(g_columnLock);
        if (auto found = g_columnScans.find(key); found != g_columnScans.end()) return found->second;
    }
    auto built = std::make_shared<const ColumnClaims>(scan_column(roots));
    std::scoped_lock guard(g_columnLock);
    return g_columnScans.try_emplace(key, std::move(built)).first->second;
}

} // namespace

std::string index_platform_miss_note(std::string_view triple) {
    std::scoped_lock guard(g_columnLock);
    auto it = g_columnMisses.find(std::string(triple));
    return it == g_columnMisses.end() ? std::string{} : it->second;
}

std::string index_platform_for(const PrepareState& state, std::string_view triple, bool refreshOnMiss) {
    // One scan of a set of index roots per process: the column is consulted
    // only for a row the built-in table does not answer, and the answer does
    // not change while one command runs -- unless a miss refreshes the index.
    // One rank's claims are compared.
    auto cfg = state.get_cfg(false);
    if (!cfg) return {};
    auto claims = column_claims(state, **cfg);
    auto it = claims->find(std::string(triple));
    // A ROW THE LOCAL COPY DOES NOT LIST IS A MISS LIKE A DEPENDENCY'S
    // (mcpp#967): the refresh policy decides (`--offline`, `[index]
    // auto_refresh`, the debounce, one sync per run), and a refreshed index is
    // scanned again. Before, a row the index listed after the local copy was
    // taken was refused as unknown until `mcpp index update`. Decided once per
    // row per process; what was decided is what the refusal says. A caller for
    // which a miss is not a refusal (a row the table holds and serves without a
    // platform) asks without it; `refused_without_platform` is that question,
    // asked of the facts the refusal gates read.
    if (it == claims->end() || it->second.empty()) {
        if (!refreshOnMiss) return {};
        {
            std::scoped_lock guard(g_columnLock);
            if (!g_columnMisses.try_emplace(std::string(triple)).second) return {};
        }
        const auto env = mcpp::config::make_xlings_env(**cfg);
        const auto decision = mcpp::pm::decide_for_miss(
            mcpp::pm::policy_for(**cfg), env, std::format("the platform column for {}", triple));
        std::string note;
        if (auto applied = mcpp::pm::apply(decision, env); !applied) {
            note = std::format("the package index lists no platform for it, and its refresh failed: {}",
                               applied.error());
        } else if (!decision.shouldRefresh) {
            note = std::format("the package index lists no platform for it, and was not refreshed ({})",
                               mcpp::pm::reason_text(decision.reason));
        } else {
            {
                std::scoped_lock guard(g_columnLock);
                g_columnScans.clear();
            }
            claims = column_claims(state, **cfg);
            it = claims->find(std::string(triple));
            note = "the package index, refreshed by this command, lists no platform for it";
        }
        const bool found = it != claims->end() && !it->second.empty();
        std::scoped_lock guard(g_columnLock);
        if (found) g_columnMisses.erase(std::string(triple));
        else g_columnMisses[std::string(triple)] = note;
        if (!found) return {};
    }
    const int first = std::ranges::min(it->second, {}, &ColumnClaim::rank).rank;
    std::vector<std::string> who;
    for (auto const& c : it->second)
        if (c.rank == first) who.push_back(c.package);
    std::ranges::sort(who);
    if (who.size() > 1) {
        std::string names;
        for (auto const& w : who) names += (names.empty() ? "" : ", ") + w;
        mcpp::diag::warning("build", std::format("the package index lists {} for {}; {} is taken. The index's own check "
                                      "allows one platform for a row", names, triple, who.front()));
    }
    return who.front() + "@*";
}

std::string platform_short_name(std::string_view ref) {
    auto r = mp::parse_ref_text(ref);
    return r ? known_short_name(*r) : std::string{};
}

// A miss of the index's column refreshes the index only for a request the
// build refuses without a platform (mcpp#967). Through 2026.11.21.1 every row the
// engine does not know refreshed, and every `planned` row: a custom triple a
// `[target.<triple>]` section defines, and a `planned` row built with a named
// toolchain, are built without one, so each such build more than the debounce
// apart synced the index again (mcpp#996, 12 to 19 s a build).
bool refused_without_platform(const PrepareState& state, std::string_view request) {
    const auto parsed = mcpp::toolchain::triple::parse(request);
    const auto* section = find_request_entry(*state.m, request, parsed);
    const auto* known = parsed ? mcpp::toolchain::triple::find_known_target(*parsed) : nullptr;
    if (!known) return !section && !(parsed && parsed->defined());
    return known->tier == "planned" && !toolchain_named(state, section);
}

// WHETHER THE BUILT-IN TABLE ANSWERS THE PLATFORM COLUMN FOR A ROW (design
// 2026-11-16 §6.2). It does not for a row it does not hold (a platform defines
// it), holds as `planned`, or holds with no platform in its column and no
// built-in provider that serves that row here: the native FreeBSD rows, whose
// system a platform's sysroot provides (E4 of 2026-11-10; the selection takes
// them with no provider). A request the
// selection would complete to a sibling (`x86_64-freebsd` to openkal's
// `x86_64-freebsd-musl`) is not the table answering the row requested.
static bool table_answers(const PrepareState& state, const mcpp::toolchain::triple::Triple& t) {
    const auto* known = mcpp::toolchain::triple::find_known_target(t);
    if (!known || known->tier == "planned" || t.defined()) return false;
    if (!known->platform.empty()) return true;
    auto served = mcpp::toolchain::target::select(t.str(), t.arch, t.os, t.env, true,
        target_context(state.m->platformOverrides, {}, {}, {}, [&] {
            return state.msvc_usable_either_origin && state.msvc_usable_either_origin();
        }));
    return !served.candidates.empty() && !served.fallback && served.candidates.front().available
        && served.candidates.front().provider
        && served.candidates.front().row && served.candidates.front().row->triple == t.str();
}

std::string named_provider(const PrepareState& state, const mcpp::toolchain::triple::Triple& t,
                           std::span<const mp::Override> config, std::string* platform, std::string* chosenOut) {
    const auto flag = !state.overrides.platform_choice.empty()
        ? state.overrides.platform_choice
        : mcpp::invocation::get("MCPP_PLATFORM").value_or("");
    const auto envKey = target_env_name(t, "PLATFORM");
    const auto env = mcpp::invocation::get(envKey).value_or("");
    const auto project = project_tables(*state.m);
    mcpp::platform::ChoiceInputs in{ flag, env, envKey, project, config, {} };
    auto sel = mcpp::platform::select_target(in, matcher(cfgpred::context_for(t.str())));
    if (!sel) return {};
    // Nothing chose: the selection's answer is "none" from the (empty) column.
    const bool unchosen = sel->none && sel->origin.tier == mp::Tier::BuiltIn;
    if (!unchosen && sel->origin.tier == mp::Tier::BuiltIn) return {};
    if (!sel->builtin.empty()) return sel->builtin;
    if (sel->none && !unchosen) return "none";
    std::string chosen;
    if (!unchosen) {
        chosen = known_short_name(sel->ref);
    } else {
        // Nothing chose, and the table has no answer for a row it holds (a
        // `planned` row, or one no built-in provider serves here): the index's
        // column names the platform that serves it (design 2026-11-16 §6.2), as
        // a choice of the project would. A row nothing defines is answered by
        // `define_early_platform`.
            if (!mcpp::toolchain::triple::find_known_target(t) || table_answers(state, t)) return {};
        const auto column = index_platform_for(state, t.str(), refused_without_platform(state, t.str()));
        if (column.empty()) return {};
        const auto ref = mp::parse_ref_text(column);
        if (!ref) return {};
        chosen = known_short_name(*ref);
    }
    if (chosenOut) *chosenOut = chosen;
    for (auto const* p : mcpp::toolchain::target::providers()) {
        if (p->platform.empty()) continue;
        auto ref = mp::parse_ref_text(p->platform);
        if (ref && ref->spec.shortName == chosen) return std::string(p->name);
    }
    if (platform) *platform = chosen;
    return {};
}

cfgpred::Ctx platform_cfg_context(const PrepareState& state) {
    return cfgpred::context_for(target_of(state).str(), state.platformPackageShortName);
}

// THE TIERS ABOVE A PLATFORM (SPEC-013 §6): this machine's `[target.<selector>]`
// tables, then the project's, each matching one merged by key in specificity
// order. One merge for a platform's statement and for a row without one.
void merge_override_tables(const PrepareState& state, mp::Statement& into) {
    const auto ctx = platform_cfg_context(state);
    std::vector<mp::Override> config;
    if (auto cfg = state.get_cfg(false); cfg) config = (*cfg)->platformOverrides;
    for (auto const* o : mcpp::platform::matching(config, matcher(ctx))) mcpp::platform::merge(into, o->fields);
    auto project = project_tables(*state.m);
    for (auto const* o : mcpp::platform::matching(project, matcher(ctx))) mcpp::platform::merge(into, o->fields);
}

// A LAYER'S PREBUILT FORMS (design 2026-11-22 §3.1): an ecosystem payload
// (`xim:<name>@<version>`), or a directory that is no package (no mcpp.toml;
// a directory with one is a package of the graph, as a base package's is,
// SPEC-002 §2.5). Which layers accept them on a row is the system's data.
bool prebuilt_layer(const mp::LayerEntry& e) {
    if (!e.ref.payload.empty()) return true;
    if (e.ref.spec.path.empty()) return false;
    std::error_code ec;
    return !fs::is_regular_file(fs::path(e.ref.spec.path) / "mcpp.toml", ec);
}


// A `planned` row the chosen platform was to serve (`state.plannedRow`), checked
// against the platform's statement once it is read: served when a provider of the
// statement lists the row with a toolchain; refused, naming the platform, when not.
static std::expected<void, std::string> planned_row_served(PrepareState& state, const mp::Statement& s,
                                                           std::string_view platformName, std::string_view shown) {
    if (state.plannedRow.empty()) return {};
    const auto row = state.plannedRow;
    if (mp::serves_with_toolchain(s, platformName, row)) return {};
    refusal::record(refusal::Code::TierPlanned);
    return std::unexpected(std::format(
        "target '{}' is registered but not yet supported (planned), and the platform {} does not serve it:\n"
        "       its statement lists no provider of the row with a toolchain.\n"
        "       A toolchain named for the build opts in early: [target.{}] toolchain, or --toolchain.",
        row, shown, row));
}

std::expected<void, std::string> step1_select_platform(PrepareState& state) {
    // A host sub-build (build programs, tools) is the host graph: its platform
    // is the one its caller states (the build programs' graph under a host
    // platform), or the host conventions -- and a platform's own tools are
    // always built so (the bootstrap boundary, §6.2).
    const bool nested = state.overrides.host_target || state.overrides.tool_depth > 0
        || state.overrides.build_program_graph;
    if (nested && state.overrides.platform_choice.empty()) return {};
    // THE HOST GRAPH'S PLATFORM, chosen by the outermost prepare (§6.1).
    if (!nested) {
        mp::Override config;
        if (auto cfg = state.get_cfg(false); cfg) config = (*cfg)->hostPlatform;
        const auto hostFlag = mcpp::invocation::get("MCPP_HOST_PLATFORM").value_or("");
        mp::Override project = state.m->hostPlatform;
        auto hs = mcpp::platform::select_host(hostFlag, stated_on_command_line("MCPP_HOST_PLATFORM"),
                                              project, config);
        if (!hs) return std::unexpected(hs.error());
        state.hostPlatformSel = std::move(*hs);
    }
    const auto t = target_of(state);
    const auto flag = !state.overrides.platform_choice.empty()
        ? state.overrides.platform_choice
        : mcpp::invocation::get("MCPP_PLATFORM").value_or("");
    const auto envKey = target_env_name(t, "PLATFORM");
    const auto env = mcpp::invocation::get(envKey).value_or("");
    auto project = project_tables(*state.m);
    std::vector<mp::Override> config;
    if (auto cfg = state.get_cfg(false); cfg) config = (*cfg)->platformOverrides;
    // The built-in default: the platform of the provider the selection took.
    std::string column;
    if (state.selectedTarget) column = std::string(state.selectedTarget->platform);
    else if (auto* row = mcpp::toolchain::triple::find_known_target(t)) column = std::string(row->platform);
    // THE INDEX ANSWERS WHERE THE BUILT-IN TABLE DOES NOT (design 2026-11-16
    // §6.2, `table_answers`). A row the table answers -- a host row included,
    // which a platform's `targets` may also list -- is never taken from the
    // index.
    bool fromIndex = false;
    {
        const bool tableAnswers = table_answers(state, t);
        if (!tableAnswers)
            if (auto indexed = index_platform_for(state, t.str(), refused_without_platform(state, t.str())); !indexed.empty()) {
                column = std::move(indexed);
                fromIndex = true;
            }
    }
    mcpp::platform::ChoiceInputs in{ flag, env, envKey, project, config, column };
    in.columnFromIndex = fromIndex;
    if (state.overrides.platform_choice.empty() && !stated_on_command_line("MCPP_PLATFORM"))
        in.flagKey = "MCPP_PLATFORM";
    auto sel = mcpp::platform::select_target(in, matcher(cfgpred::context_for(t.str())));
    if (!sel) return std::unexpected(sel.error());
    state.platform = mcpp::platform::Resolved{};
    state.platform.selection = std::move(*sel);
    if (state.platform.selection.none && !state.plannedRow.empty()) {
        const auto column = index_platform_miss_note(t.str());
        refusal::record(refusal::Code::TierPlanned);
        return std::unexpected(std::format(
            "target '{}' is registered but not yet supported (planned) — no toolchain is published for it yet.\n"
            "       A toolchain named for the build opts in early: [target.{}] toolchain, or --toolchain.{}",
            state.plannedRow, state.plannedRow, column.empty() ? std::string{} : "\n       " + column));
    }
    if (!state.platform.selection.none) {
        auto const& ref = state.platform.selection.ref;
        // A directory is read now, and its manifest's own error is the answer
        // when it cannot be.
        if (!ref.spec.path.empty() && ref.name.empty()) {
            auto m = mcpp::manifest::load(fs::path(ref.spec.path) / "mcpp.toml");
            if (!m)
                return std::unexpected(std::format("{}: the platform {}: {}",
                    mcpp::platform::origin_text(state.platform.selection.origin),
                    mp::display(ref), m.error().format()));
        }
        state.platformPackageShortName = known_short_name(ref);
        if (state.platformPackageShortName.empty())
            return std::unexpected(std::format(
                "{}: the platform {} names no package; write {{ name = \"<package>\", ... }}",
                mcpp::platform::origin_text(state.platform.selection.origin),
                mp::display(state.platform.selection.ref)));
        // READ NOW WHEN IT IS ON DISK: a directory, or the package a first
        // pass fetched. The target base is decided before the graph exists,
        // and a platform may state one (§7).
        fs::path early = state.overrides.platform_root;
        if (early.empty() && !state.platform.selection.ref.spec.path.empty())
            early = state.platform.selection.ref.spec.path;
        if (!early.empty())
            if (auto m = mcpp::manifest::load(early / "mcpp.toml"); m && m->platformDeclared) {
                state.platformData = mcpp::platform::provider_view(m->platform, t.str());
                state.platformEarly = true;
                auto n = mcpp::pm::compat::resolve_package_name(m->package.name, m->package.namespace_);
                if (auto r = planned_row_served(state, m->platform, n.shortName, mp::display(state.platform.selection.ref)); !r)
                    return r;
            }
    }
    return {};
}

void step4b_seed_platform(PrepareState& state) {
    if (state.platform.selection.none) return;
    auto const& ref = state.platform.selection.ref;
    WorkItem item;
    item.name = item_key(ref);
    item.spec = ref.spec;
    if (item.spec.shortName.empty()) item.spec.shortName = state.platformPackageShortName;
    if (item.spec.namespace_.empty()) {
        item.spec.namespace_ = std::string(mcpp::pm::kDefaultNamespace);
        item.spec.namespaceOmitted = true;
    }
    if (item.spec.candidates.empty())
        item.spec.candidates = {{ .namespace_ = item.spec.namespace_, .shortName = item.spec.shortName }};
    item.spec.declaredIn = "the platform";
    item.requestedBy = state.m->package.name + " (platform)";
    item.originalConstraint = item.spec.version;
    // A SERIES TAKES THE RELEASE THE LOCK RECORDS (SPEC-013 §14): a platform
    // selected as `^0.24` resolves to the newest release of the series once,
    // and mcpp.lock keeps it there until `mcpp update` drops the entry; a
    // recorded release outside the request is not taken, and the build says
    // which it takes (the target table's exact default after an mcpp upgrade,
    // design 2026-11-07 §4.10).
    if (!item.spec.isPath() && !item.spec.isGit()) {
        const auto qualified = item.spec.namespace_ + "." + item.spec.shortName;
        if (auto anchor = state.platformLockAnchors.find(qualified);
            anchor != state.platformLockAnchors.end())
            if (auto req = mcpp::version_req::parse_req(item.spec.version))
                if (auto v = mcpp::version_req::parse_version(anchor->second); v) {
                    if (mcpp::version_req::matches(*req, *v)) {
                        if (item.spec.version != anchor->second) {
                            mcpp::ui::info("Platform", std::format("{} {} → v{} (mcpp.lock)",
                                item.spec.shortName, item.spec.version, anchor->second));
                            item.spec.version = "=" + anchor->second;
                        }
                    } else {
                        mcpp::ui::info("Platform", std::format("{} {} ({}; mcpp.lock recorded v{})",
                            item.spec.shortName, item.spec.version,
                            state.platform.selection.origin.tier == mcpp::manifest::platform::Tier::BuiltIn
                                ? std::format("mcpp {}'s default", mcpp::MCPP_VERSION)
                                : mcpp::platform::origin_text(state.platform.selection.origin),
                            anchor->second));
                    }
                }
    }
    item.consumerDepIndex = kMainConsumer;
    state.worklist.push_front(std::move(item));
}

namespace {
thread_local std::optional<fs::path> g_platformRestart;
thread_local std::optional<std::pair<std::string, std::string>> g_platformSeriesRestart;

// A REFUSAL BY THE VERSION GROUP OF A RELEASE A SERIES TOOK IS NOT THE BUILD'S
// LAST WORD (design 2026-11-19 §21 A5): the series' newest release is not the
// newest one this build can use, and `prepare_build` takes the series below
// it. `why` names the request that refused it.
void refuse_series_release(const PrepareState& state, std::string why) {
    if (!state.platformSeriesTook.empty())
        g_platformSeriesRestart = std::pair{state.platformSeriesTook, std::move(why)};
}
} // namespace

std::optional<fs::path> take_platform_restart() {
    auto r = std::move(g_platformRestart);
    g_platformRestart.reset();
    return r;
}

std::optional<std::pair<std::string, std::string>> take_platform_series_restart() {
    auto r = std::move(g_platformSeriesRestart);
    g_platformSeriesRestart.reset();
    return r;
}

std::expected<bool, std::string> step4b_apply_platform(PrepareState& state) {
    auto& P = state.platform;
    const auto t = target_of(state);

    // A ROW WITHOUT A PLATFORM HAS AN EMPTY STATEMENT, AND THE TIERS ABOVE IT
    // OVERRIDE IT AS THEY OVERRIDE A PLATFORM'S (design 2026-11-22 §3.2). The
    // keys of this machine's and the project's `[target.<selector>]` tables
    // used to be merged only once a platform package was read, so on a row
    // without one they were accepted and dropped; the layers and tools they
    // name are completed below as a platform's are. Merged again each round:
    // the merge replaces by key, and a round adds no table.
    if (P.selection.none) {
        P.fields = {};
        merge_override_tables(state, P.fields);
    }

    // ── The package, once: find it, read it, merge what overrides it ──────
    if (!P.selection.none && !P.active) {
        const mcpp::modgraph::PackageRoot* found = nullptr;
        const auto& ref = P.selection.ref;
        for (std::size_t i = 1; i < state.packages.size(); ++i) {
            auto const& pr = state.packages[i];
            auto n = mcpp::pm::compat::resolve_package_name(pr.manifest.package.name,
                                                             pr.manifest.package.namespace_);
            const bool byPath = !ref.spec.path.empty()
                && fs::weakly_canonical(pr.root) == fs::weakly_canonical(fs::path(ref.spec.path));
            if (byPath || n.shortName == state.platformPackageShortName) { found = &pr; break; }
        }
        if (!found)
            return std::unexpected(std::format(
                "the platform {} ({}) was not resolved into the graph",
                mp::display(ref), mcpp::platform::origin_text(P.selection.origin)));
        if (!found->manifest.platformDeclared) {
            refusal::record(refusal::Code::PlatformNotAPlatform);
            return std::unexpected(std::format(
                "{} is selected as the platform for {} ({}), and its mcpp.toml has no "
                "[platform] section.\n"
                "       A platform package states the configuration of its targets there "
                "(SPEC-013 §2); select a platform package, or none (--platform none).",
                qualified(found->manifest), t.str(),
                mcpp::platform::origin_text(P.selection.origin)));
        }
        P.package = qualified(found->manifest);
        P.version = found->manifest.package.version;
        P.root = found->root;
        // A planned row this platform was to serve: refused now when it does not;
        // when it does and the first pass fetched it, prepared again with the
        // platform read in P1, where its toolchain is the build's.
        if (!state.plannedRow.empty() && !state.platformEarly) {
            auto n = mcpp::pm::compat::resolve_package_name(found->manifest.package.name,
                                                            found->manifest.package.namespace_);
            if (auto r = planned_row_served(state, found->manifest.platform, n.shortName,
                                            std::format("{}@{}", P.package, P.version)); !r)
                return std::unexpected(r.error());
            g_platformRestart = P.root;
            return std::unexpected(std::string("mcpp:platform-restart"));
        }
        define_platform(found->manifest.platform, state.platformPackageShortName,
                        ref.spec.path.empty() ? std::format("{}@{}", P.package, P.version)
                                              : "path:" + ref.spec.path);
        // A REQUEST ONLY A PLATFORM DEFINES (mcpp#911 G1): the first pass assumed
        // the row while the platform was fetched; with its definitions read, the
        // build is prepared again from the start, or refused when it defines none.
        if (state.provisionalTarget) {
            const auto* row = mcpp::toolchain::target::find_row(state.overrides.target_triple);
            if (!row || !row->defined || std::ranges::none_of(found->manifest.platform.defines, [&](auto const& d) {
                    return d.triple == state.overrides.target_triple; })) {
                std::string defined;
                for (auto const& d : found->manifest.platform.defines)
                    defined += (defined.empty() ? "" : ", ") + d.triple;
                refusal::record(refusal::Code::UnknownTarget);
                return std::unexpected(std::format(
                    "unknown target '{}': no row defines it, and the platform {} ({}) defines {}.\n"
                    "       a platform defines a target with [[platform.define]] triple = \"{}\" (SPEC-016 §2)",
                    state.overrides.target_triple, P.package, mcpp::platform::origin_text(P.selection.origin),
                    defined.empty() ? std::string("none") : defined, state.overrides.target_triple));
            }
            if (!state.platformEarly) {
                g_platformRestart = P.root;
                return std::unexpected(std::string("mcpp:platform-restart"));
            }
        }
        auto hosted = mcpp::platform::serves(found->manifest.platform, P.package, t);
        if (!hosted) {
            refusal::record(refusal::Code::PlatformTargetUnserved);
            return std::unexpected(std::format("{} ({})", hosted.error(),
                mcpp::platform::origin_text(P.selection.origin)));
        }
        P.selection.hosted = *hosted;
        state.platformData = mcpp::platform::view_of(found->manifest.platform, t.str(), *hosted);
        P.fields = state.platformData;
        merge_override_tables(state, P.fields);
        P.active = true;
        state.platformInjected.insert(P.package);
        // A base, or descriptor keys, the platform states, which this pass
        // decided without: the next pass reads the package before deciding
        // them (§7; design 2026-10-29 §5.1).
        const auto platformStated = [](auto const& f) { return f.set && f.origin.tier == mp::Tier::Platform; };
        if ((platformStated(P.fields.sysroot) || platformStated(P.fields.descriptorKeys)) && !state.platformEarly) {
            g_platformRestart = P.root;
            return std::unexpected(std::string("mcpp:platform-restart"));
        }
    }

    // ── The layers the graph lacks (§4.3) ─────────────────────────────────
    bool pushed = false;
    const auto provided = provided_layers(state);
    std::map<std::string, std::vector<std::string>> byKey;   // item key -> layers
    std::map<std::string, const mp::LayerEntry*> entryOf;
    for (auto layer : mp::kLayers) {
        auto it = P.fields.layers.find(std::string(layer));
        if (it == P.fields.layers.end()) continue;
        auto const& e = it->second;
        if (e.ref.off) {
            P.layers[std::string(layer)] = { "none", e.origin.tier };
            continue;
        }
        // A prebuilt supplier is no package of the graph: the target side
        // decides and wires it (`step9_kernel_headers`).
        if (prebuilt_layer(e)) {
            P.layers[std::string(layer)] = { mp::display(e.ref), e.origin.tier };
            continue;
        }
        if (auto pv = provided.find(std::string(layer)); pv != provided.end()) {
            // Whose: the platform's when the provider is a package the
            // platform added, the graph's otherwise.
            const auto providerName = pv->second.substr(0, pv->second.find('@'));
            bool ours = false;
            for (auto const& q : state.platformInjected)
                if (q == providerName || q.ends_with("." + providerName)
                    || providerName.ends_with("." + q.substr(q.find('.') + 1)))
                    ours = true;
            P.layers[std::string(layer)] = { pv->second, ours ? e.origin.tier : mp::Tier::Graph };
            continue;
        }
        const auto key = item_key(e.ref);
        if (key.empty())
            return std::unexpected(std::format(
                "{}: the package for {} names no package; write {{ name = \"<package>\", ... }}",
                mcpp::platform::origin_text(e.origin), layer));
        if (state.platformInjected.contains("layer:" + key)) continue;   // already walked
        byKey[key].push_back(std::string(layer));
        entryOf[key] = &e;
    }
    for (auto const& [key, layers] : byKey) {
        auto const& e = *entryOf.at(key);
        WorkItem item;
        item.name = key;
        item.spec = e.ref.spec;
        if (item.spec.shortName.empty()) item.spec.shortName = known_short_name(e.ref);
        if (item.spec.namespace_.empty()) {
            item.spec.namespace_ = std::string(mcpp::pm::kDefaultNamespace);
            item.spec.namespaceOmitted = true;
        }
        if (item.spec.candidates.empty())
            item.spec.candidates = {{ .namespace_ = item.spec.namespace_,
                                      .shortName = item.spec.shortName }};
        std::string names;
        for (auto const& l : layers) { if (!names.empty()) names += ", "; names += l; }
        item.spec.declaredIn = std::format("the platform's {}", names);
        item.requestedBy = std::format("{} (platform: {})", state.m->package.name, names);
        item.originalConstraint = item.spec.version;
        item.consumerDepIndex = kMainConsumer;
        // The root declares it, as it would by hand: every reader of the
        // root's edges (the link's closure among them) sees it.
        state.m->dependencies[key] = item.spec;
        state.platformLayerSpecs[key] = item.spec;
        state.worklist.push_back(std::move(item));
        state.platformInjected.insert("layer:" + key);
        state.platformInjected.insert(injected_name(state.worklist.back().spec));
        pushed = true;
    }

    // ── The tools the graph does not build (§4.4) ─────────────────────────
    std::set<std::string> requested;   // tools the root already asks the graph for
    for (auto const& [k, spec] : state.m->buildDependencies)
        for (auto const& tool : spec.tools) requested.insert(tool);
    std::map<std::string, std::vector<std::string>> toolsByKey;
    std::map<std::string, const mp::ToolEntry*> toolEntryOf;
    for (auto const& [name, e] : P.fields.tools) {
        if (!e.ref.program.empty()) continue;            // found, not built
        if (requested.contains(name)) continue;          // the graph builds it already
        const auto key = item_key(e.ref);
        if (key.empty())
            return std::unexpected(std::format(
                "{}: the package for the tool {} names no package; write {{ name = \"<package>\", ... }}",
                mcpp::platform::origin_text(e.origin), name));
        if (state.platformInjected.contains("tool:" + name)) continue;
        toolsByKey[key].push_back(name);
        toolEntryOf[key] = &e;
    }
    for (auto const& [key, tools] : toolsByKey) {
        auto const& e = *toolEntryOf.at(key);
        WorkItem item;
        item.name = key;
        item.spec = e.ref.spec;
        if (item.spec.shortName.empty()) item.spec.shortName = known_short_name(e.ref);
        if (item.spec.namespace_.empty()) {
            item.spec.namespace_ = std::string(mcpp::pm::kDefaultNamespace);
            item.spec.namespaceOmitted = true;
        }
        if (item.spec.candidates.empty())
            item.spec.candidates = {{ .namespace_ = item.spec.namespace_,
                                      .shortName = item.spec.shortName }};
        item.spec.tools = tools;
        std::string names;
        for (auto const& n : tools) { if (!names.empty()) names += ", "; names += n; }
        item.spec.declaredIn = std::format("the platform's tools ({})", names);
        item.requestedBy = std::format("{} (platform tools: {})", state.m->package.name, names);
        item.originalConstraint = item.spec.version;
        item.consumerDepIndex = kMainConsumer;
        item.buildOnly = true;
        item.fromBuildTable = true;
        item.scope = root_program_scope(state);
        // The root asks for them as it would for a declared build dependency,
        // so the tool provisioning pass builds them for this machine.
        state.m->buildDependencies[key] = item.spec;
        for (auto const& n : tools) state.platformInjected.insert("tool:" + n);
        state.platformInjected.insert(injected_name(item.spec));
        state.worklist.push_back(std::move(item));
        pushed = true;
    }
    return pushed;
}

// THE CAPABILITY SLOTS OF ORDINARY PACKAGES (mcpp#911 G7; SPEC-002 §3.6). A
// package that requires a capability (`requires = ["mcpp:kernel-abi=openkal"]`)
// and states its default providers (`[defaults."mcpp:kernel-abi=openkal"] linux
// = "openkal-linux@^0.25"`) takes the default for the target only when no
// package of the graph provides the capability -- the consumer's choice, or
// the platform's layer, added in the rounds before. The default is the
// package's dependency, walked like one.
std::expected<bool, std::string> step4b_capability_defaults(PrepareState& state) {
    std::set<std::string> provided;
    for (auto const& p : state.packages)
        for (auto const& c : p.manifest.provides) provided.insert(c);
    const auto ctx = platform_cfg_context(state);
    const auto matches = matcher(ctx);
    bool pushed = false;
    for (std::size_t i = 0; i < state.packages.size(); ++i) {
        auto const& pm = state.packages[i].manifest;
        if (pm.capabilityDefaults.empty()) continue;
        // A selector that names the target wins over `*`, which is the default
        // of every other target.
        std::vector<const mcpp::manifest::Manifest::CapabilityDefault*> order;
        for (auto const& d : pm.capabilityDefaults) if (d.selector != "*") order.push_back(&d);
        for (auto const& d : pm.capabilityDefaults) if (d.selector == "*") order.push_back(&d);
        std::set<std::string> done;
        for (auto const* dp : order) {
            auto const& d = *dp;
            if (done.contains(d.capability) || provided.contains(d.capability)) continue;
            if (std::ranges::find(pm.requires_, d.capability) == pm.requires_.end()) continue;
            if (d.selector != "*" && !matches(d.selector)) continue;
            done.insert(d.capability);
            const auto key = item_key(d.ref);
            const auto mark = std::format("default:{}:{}", pm.package.name, d.capability);
            if (key.empty() || state.platformInjected.contains(mark)) continue;
            WorkItem item;
            item.name = key;
            item.spec = d.ref.spec;
            if (item.spec.shortName.empty()) item.spec.shortName = known_short_name(d.ref);
            if (item.spec.namespace_.empty()) {
                item.spec.namespace_ = std::string(mcpp::pm::kDefaultNamespace);
                item.spec.namespaceOmitted = true;
            }
            if (item.spec.candidates.empty())
                item.spec.candidates = {{ .namespace_ = item.spec.namespace_,
                                          .shortName = item.spec.shortName }};
            item.spec.declaredIn = std::format("{}'s default for {} ({})", pm.package.name,
                                               d.capability, d.selector);
            item.requestedBy = std::format("{} (default for {})", pm.package.name, d.capability);
            item.originalConstraint = item.spec.version;
            item.consumerDepIndex = i == 0 ? kMainConsumer : i - 1;
            item.resolveRoot = i == 0 ? std::filesystem::path{} : state.packages[i].root;
            // The package declares it, as it would by hand: the readers of its
            // edges (the link's closure among them) see it.
            if (i == 0) state.m->dependencies[key] = item.spec;
            else {
                state.packages[i].manifest.dependencies[key] = item.spec;
                if (i - 1 < state.dep_manifests.size() && state.dep_manifests[i - 1])
                    state.dep_manifests[i - 1]->dependencies[key] = item.spec;
            }
            state.platformInjected.insert(mark);
            mcpp::ui::info("Default", std::format("{} for {} of {} ({})", mp::display(d.ref),
                                                  d.capability, pm.package.name, d.selector));
            state.worklist.push_back(std::move(item));
            pushed = true;
        }
    }
    return pushed;
}

// THE PLATFORM'S VERSION GROUP IS A CONSTRAINT (SPEC-013 §7.5): a request of
// one of its members, by version, resolves to the group's version when the
// request admits it, and is refused naming both when it does not. A package
// built for this machine only, and a development tree (a path or a
// repository), are not bound by it, as the check after the walk leaves them.
//
// THE CONSTRAINT ENTERS THE MERGE (mcpp#956). The request was rewritten to the
// group's version and the rewriting stopped there: a second request of the
// same package was merged with the constraints as written, which the group's
// version was not among. A build program's request, which the group does not
// bind, took the newest version its range admits; the platform's layer then
// arrived, the merge found the kept version within both written ranges, and
// the check after the walk refused a version the resolver itself had chosen.
// One version of a package serves every request of it, and the group's
// constraint is one of those requests.
std::expected<void, std::string> step4b_pin_to_platform_group(PrepareState& state, WorkItem& item,
                                                              const std::string& name,
                                                              mcpp::manifest::DependencySpec& spec) {
    if (!state.platformGroup.empty() && !spec.isPath() && !spec.isGit() && !item.buildOnly) {
        const auto shortName = spec.shortName.empty() ? name : spec.shortName;
        const auto platformNs = state.platform.selection.ref.spec.namespace_;
        const bool sameNs = spec.namespace_.empty() || platformNs.empty()
                         || spec.namespace_ == platformNs;
        if (auto g = state.platformGroup.find(shortName); sameNs && g != state.platformGroup.end()) {
            if (step4b_root_overrides_group(state, item, shortName, g->second)) return {};
            const auto group = mcpp::version_req::parse_version(g->second);
            const auto asked = spec.version.empty() ? std::string("*") : spec.version;
            if (auto req = mcpp::version_req::parse_req(asked);
                req && group && !mcpp::version_req::matches(*req, *group)) {
                refusal::record(refusal::Code::PlatformVersionGroup);
                refuse_series_release(state, std::format("its version group names {} {}, and {} requests '{}'",
                                                         shortName, g->second, item.requestedBy, asked));
                return std::unexpected(std::format(
                    "{} is requested as '{}' by {}, and the platform {} is a version group\n"
                    "       that names {} {}.\n"
                    "       The group chooses one version of each package of the family for a\n"
                    "       build. Either select the platform release of the version you need\n"
                    "       ([target.<selector>] platform = \"{}@<version>\"), or admit {} where\n"
                    "       it is requested ({} = \"^{}\").",
                    shortName, asked, item.requestedBy, state.platformGroupFrom, shortName,
                    g->second, state.platformPackageShortName, g->second, shortName, g->second));
            }
            if (group) {
                spec.version = "=" + g->second;
                // The group's choice is this request's constraint from here on:
                // the record of the package keeps it, and a later request the
                // group does not bind is merged with it.
                item.groupConstraint = spec.version;
            }
        }
    }
    return {};
}

// The platform is the walk's first package: its version group binds every
// request after it (SPEC-013 §7.5).
void step4b_read_platform_group(PrepareState& state, const std::string& resolvedShortName) {
    if (state.platformGroupRead || state.platform.selection.none || state.dep_manifests.empty()
        || resolvedShortName != state.platformPackageShortName)
        return;
    state.platformGroupRead = true;
    state.platformGroup = state.dep_manifests.back()->platform.versions;
    const auto used = state.dep_manifests.back()->package.version;
    state.platformGroupFrom = std::format("{} {}", state.platformPackageShortName, used);
    // THE DEFAULT IS A RELEASE, AND A NEWER ONE IS THE PROJECT'S CHOICE (design
    // 2026-11-07 §4.10). The target table names one platform release, the one
    // this mcpp was qualified with; a project selects another in its manifest.
    // When the default is in use and the synced index holds a newer release, a
    // failed build says so once, with the line to write. It guesses no cause.
    auto const& sel = state.platform.selection;
    if (sel.origin.tier != mcpp::manifest::platform::Tier::BuiltIn || sel.ref.spec.isPath()
        || sel.ref.spec.isGit() || used.empty())
        return;
    auto cfg = state.get_cfg(true);
    if (!cfg) return;
    auto newer = mcpp::pm::resolve_semver(sel.ref.spec.namespace_.empty() ? std::string("mcpplibs")
                                                                          : sel.ref.spec.namespace_,
                                          state.platformPackageShortName, ">" + used,
                                          state.index_route(*cfg), *state.targetPlatform);
    if (!newer) return;
    if (auto tip = mcpp::platform::newer_release_tip(sel.origin, state.platformPackageShortName, used,
                                                     *newer, mcpp::MCPP_VERSION))
        mcpp::diag::on_failure("platform/newer", *tip);
}

// A SERIES TAKES ITS NEWEST RELEASE THIS ENGINE READS AND THIS BUILD CAN USE
// (SPEC-013 §14). A platform selected as a range (`^0.24`) skips a release
// whose `[package] mcpp` floor is above this mcpp, so an older engine keeps
// building with the newest platform it can read instead of being refused by a
// release made for a newer one; and a release whose version group a request
// of the build does not admit, which an earlier pass took and was refused for
// (design 2026-11-19 §21 A5). A pin (`=`) and a lock anchor are taken as they
// are.
std::expected<void, std::string> step4b_platform_within_engine(PrepareState& state) {
    if (state.platform.selection.none || state.worklist.empty()) return {};
    auto& item = state.worklist.front();
    auto& spec = item.spec;
    if (spec.isPath() || spec.isGit() || spec.version.starts_with("=")
        || !mcpp::pm::is_version_constraint(spec.version))
        return {};
    // The release the build that asked took (its build programs' graph,
    // mcpp#797) is a pin: the series names no release here, and a refusal
    // skips none.
    if (state.overrides.resolution_pins.contains(spec.namespace_.empty() ? spec.shortName
                                                                         : spec.namespace_ + "." + spec.shortName))
        return {};
    const auto have = mcpp::xpkg_version::parse(mcpp::MCPP_VERSION);
    if (!have) return {};
    auto cfg = state.get_cfg(true);
    if (!cfg) return std::unexpected(cfg.error());
    std::string constraint = spec.version;
    auto const& skipped = state.overrides.platform_series_skipped;
    for (auto const& [version, why] : skipped) constraint += ", <" + version;
    // Said once: each earlier pass said the releases it skipped before.
    if (!skipped.empty())
        mcpp::ui::info("Platform", std::format("{} v{} is not usable by this build: {}",
                                               spec.shortName, skipped.back().first, skipped.back().second));
    for (int attempt = 0; attempt < 16; ++attempt) {
        auto found = mcpp::pm::resolve_semver(spec.namespace_, spec.shortName, constraint,
                                              state.index_route(*cfg), *state.targetPlatform);
        if (!found) {
            if (skipped.empty()) return {};   // the walk reports it, with the request that made it
            refusal::record(refusal::Code::PlatformVersionGroup);
            std::string each;
            for (auto const& [version, why] : skipped) each += std::format("\n         v{}: {}", version, why);
            return std::unexpected(std::format(
                "no release of the platform {} {} can be used by this build:{}\n"
                "       Each release's version group names a package version a request of the\n"
                "       build does not admit. Admit the group's version where it is requested,\n"
                "       or select a release outside the series ([target.<selector>] platform =\n"
                "       \"{}@<version>\").",
                spec.shortName, spec.version, each, spec.shortName));
        }
        auto loaded = state.loadVersionDep(item.name, spec.namespace_, spec.shortName, *found);
        if (!loaded) return {};
        auto const& floor = loaded->second.package.mcppFloor;
        const auto need = mcpp::xpkg_version::parse(floor);
        if (floor.empty() || !need || mcpp::xpkg_version::compare(*have, *need) >= 0) {
            if (attempt > 0 || !skipped.empty()) {
                mcpp::ui::info("Platform", std::format("{} {} → v{} (the newest {})",
                    spec.shortName, spec.version, *found,
                    skipped.empty() ? "this mcpp reads" : "this build can use"));
                spec.version = "=" + *found;
            }
            state.platformSeriesTook = *found;
            return {};
        }
        mcpp::ui::info("Platform", std::format("{} v{} needs mcpp >= {}; this is mcpp {}",
            spec.shortName, *found, floor, mcpp::MCPP_VERSION));
        constraint = std::format("{}, <{}", constraint, *found);
    }
    return {};
}

std::expected<void, std::string> step4b_check_platform_versions(PrepareState& state) {
    auto const& P = state.platform;
    if (!P.active || P.fields.versions.empty()) return {};
    // Asked of the edges as they stand: the check runs before each round's
    // drain, before `buildTimeOnly` is decided.
    const auto instances = package_instances(state);
    for (std::size_t i = 1; i < state.packages.size(); ++i) {
        // A package built for this machine only (a tool's) is not linked with
        // the target's, and a group's exact pins are about one link.
        if (!instances.target[i] && instances.host[i]) continue;
        auto const& pm = state.packages[i].manifest;
        auto n = mcpp::pm::compat::resolve_package_name(pm.package.name, pm.package.namespace_);
        auto it = P.fields.versions.find(n.shortName);
        if (it == P.fields.versions.end()) continue;
        if (pm.package.version.empty() || pm.package.version == it->second) continue;
        // A package taken from a directory or a repository is a development
        // tree, which the version group does not bind.
        if (auto r = state.resolved.find({n.namespace_, n.shortName});
            r != state.resolved.end() && r->second.source != "version")
            continue;
        std::string by;
        if (auto r = state.resolved.find({n.namespace_, n.shortName}); r != state.resolved.end())
            by = r->second.requestedBy;
        refusal::record(refusal::Code::PlatformVersionGroup);
        refuse_series_release(state, std::format("its version group names {} {}, and {} {} is in this build (requested by {})",
                                                 n.shortName, it->second, n.shortName, pm.package.version,
                                                 by.empty() ? std::string("the graph") : by));
        return std::unexpected(std::format(
            "{} {} is in this build, and the platform {} is a version group that names {} {}.\n"
            "       requested by: {}\n"
            "       The family's packages pin one another exactly, so two versions of one of\n"
            "       them cannot be linked into one program. Either select the platform release\n"
            "       of that version ([target.<selector>] platform = \"{}@<version>\"), or write\n"
            "       {} = \"{}\" where it is requested.",
            n.shortName, pm.package.version, P.display(), n.shortName, it->second,
            by.empty() ? std::string("the graph") : by,
            P.package.substr(P.package.find('.') + 1), n.shortName, it->second));
    }
    return {};
}

// A WORKSPACE'S MEMBERS STAND ON THE PLATFORM'S LAYERS AS A ROOT DOES (§4.3).
// The packages the platform added are the virtual root's dependencies, and a
// member links its own closure; so each member of the plan is given an edge to
// each layer provider the platform added, as if it had declared it. (Measured:
// a build programs' graph under a host platform linked its programs without
// the C library and the C++ run-time, and `okld check` refused `free`.)
void step4b_platform_member_edges(PrepareState& state) {
    if (!state.workspacePlan() || state.platformLayerSpecs.empty()) return;
    // The packages of the plan by short name, so a member's edge to a layer
    // provider is a lookup.
    std::map<std::string, std::vector<std::size_t>, std::less<>> byName;
    for (std::size_t l = 1; l < state.packages.size(); ++l) {
        auto const& pm = state.packages[l].manifest;
        byName[mcpp::pm::compat::resolve_package_name(pm.package.name, pm.package.namespace_)
                   .shortName].push_back(l);
    }
    auto give = [&](std::size_t member, const std::string& key, const mcpp::manifest::DependencySpec& spec) {
        auto& deps = state.packages[member].manifest.dependencies;
        if (deps.contains(key)) return;
        deps[key] = spec;
        auto it = byName.find(spec.shortName);
        if (it == byName.end()) return;
        for (auto const l : it->second) {
            DependencyEdge e;
            e.consumerPackageIndex = member;
            e.dependencyPackageIndex = l;
            e.requestedFeatures = spec.features;
            e.defaultFeatures = spec.defaultFeatures;
            state.dependencyEdges.push_back(std::move(e));
        }
    };
    for (std::size_t m = 1; m < state.packages.size(); ++m) {
        if (!state.packages[m].selectedMember) continue;
        for (auto const& [key, spec] : state.platformLayerSpecs) give(m, key, spec);
    }
}

void step5_platform_toolchain(PrepareState& state) {
    auto const& P = state.platform;
    if (!P.active || !P.fields.toolchain.set) return;
    // The platform's toolchain stands where the target row's pin stands: it
    // replaces a default, never a toolchain the project or this machine named
    // for the target (§4.5), and the graph supplying the system does not
    // cancel it -- the platform is what supplies it.
    if (tc_origin_is_user_explicit(state.tcOrigin)) return;
    state.targetPinCandidate = P.fields.toolchain.value;
    state.targetPinIsCapability = true;
    state.platformToolchainPin = true;
}

} // namespace mcpp::build

namespace mcpp::build {

namespace {

// A program a platform names, as a runner's program is found: the programs
// the graph built for this machine, then the declared payloads, then PATH.
std::optional<fs::path> locate_program(const PrepareState& state, const BuildContext& ctx,
                                       const std::string& name) {
    if (auto it = state.rootTools.find(name); it != state.rootTools.end()) return it->second;
    for (auto const& dir : ctx.xlingsDepBinDirs)
        for (auto const& n : { name, name + ".exe" }) {
            std::error_code ec;
            if (fs::is_regular_file(dir / n, ec)) return dir / n;
        }
    return mcpp::invocation::which(name);
}

std::vector<std::string> split_words(std::string_view v) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : v) {
        if (c == ' ' || c == '\t') { if (!cur.empty()) out.push_back(std::exchange(cur, {})); }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

} // namespace

// THE PLATFORM'S NAMES OF ITS ARTIFACTS STAND (mcpp#911 B2): what follows
// `{name}` in `link.exe` and `link.shared` is the platform's format (`.oke`,
// `.okso`), which its own steps read. A build program's `artifact-name` that
// would change it -- written for the host's suffix, without knowing the
// platform -- is reported, and the platform's pattern is kept.
namespace {
void keep_platform_naming(mp::Statement& s, const mp::Statement& platform, std::string_view display,
                          std::vector<std::string>* conflicts) {
    auto suffix = [](std::string_view pattern) {
        const auto at = pattern.find("{name}");
        return at == std::string_view::npos ? pattern : pattern.substr(at + 6);
    };
    for (auto [field, base, kind] : { std::tuple{ &s.link.exe, &platform.link.exe, "exe" },
                                      std::tuple{ &s.link.shared, &platform.link.shared, "shared" } }) {
        if (!base->set || base->origin.tier != mp::Tier::Platform || suffix(base->value).empty()) continue;
        // A build program's `artifact-name` only: the project and the machine
        // state a name on purpose, and theirs stands as any field's does.
        if (!field->set || field->origin.tier != mp::Tier::Program || suffix(field->value) == suffix(base->value))
            continue;
        if (conflicts)
            conflicts->push_back(std::format(
                "{} names the {} `{}`, and the platform {} names it `{}`: the platform's name stands, "
                "since its own steps read it", mcpp::platform::origin_text(field->origin) == "build-program"
                    ? std::string("a build program") : mcpp::platform::origin_text(field->origin),
                kind == std::string_view("exe") ? "program" : "shared library", field->value, display,
                base->value));
        *field = *base;
    }
}
} // namespace

// ── The statement every reader uses: platform → build programs → this
// machine → the project → the environment (§4.5) ──────────────────────────
mp::Statement platform_statement(const PrepareState& state, std::vector<std::string>* conflicts) {
    auto const& P = state.platform;
    const auto t = target_of(state);
    const auto cfgCtx = platform_cfg_context(state);
    // THE PROVIDER'S RUNNER STANDS BENEATH EVERY OTHER STATEMENT (SPEC-016 §6):
    // how an artifact of the arrangement is started when nothing nearer says,
    // on the hosts the provider names for it.
    mp::Statement s;
    if (auto const* p = chosen_provider(state); p && !p->runner.empty()) {
        const auto host = mcpp::toolchain::target::host_pair(mcpp::toolchain::platform::name,
                                                             mcpp::toolchain::platform::host_arch);
        const bool here = p->runnerHosts.empty()
            || std::ranges::any_of(p->runnerHosts, [&](std::string_view h) {
                   return mcpp::toolchain::target::host_matches(h, host, t.arch);
               });
        if (here) {
            s.runner.cmd.assign(p->runner.begin(), p->runner.end());
            s.runner.requires_.assign(p->runnerRequires.begin(), p->runnerRequires.end());
            s.runner.host = std::string(p->runnerHost);
            s.runner.enabled = p->runnerEnabled;
            s.runner.enabledSet = !p->runnerEnabled;
            s.runner.exclusive = p->runnerExclusive;
            s.runner.exclusiveSet = p->runnerExclusive;
            s.runner.origin = mp::Origin{ mp::Tier::BuiltIn, {}, 0,
                std::format("the provider '{}' of {}", p->name, state.targetChoice.identity) };
            s.runner.set = true;
        }
    }
    // The platform's statement is taken whole over it: its fields are merged
    // field by field, and what a statement of a platform alone holds (the rows
    // it defines, its providers and systems) is the platform's as it is.
    if (P.active) {
        mp::Statement whole = state.platformData;
        mcpp::platform::merge(s, state.platformData);
        whole.runner = s.runner;
        s = std::move(whole);
    }
    mp::Statement program = state.programPlatform;
    // The root's own program states them as well (applied into its manifest);
    // it is the last program to run, and it speaks for the project.
    mcpp::platform::merge(program, state.m->buildConfig.programPlatform);
    // A build program's runner and named runners (`mcpp::runner`), the BSP
    // spelling of the same fields.
    // The rest of a runner's table (`runner_spec`, R2): the same fields as
    // data, so `{pkg:<name>}` and `enabled` mean in a build program what they
    // mean in a platform.
    auto table = [](mp::Runner& r, const mcpp::manifest::NamedRunner& f) {
        r.requires_ = f.requires_;
        if (!f.hosts.empty()) { r.hosts = f.hosts; r.hostsSet = true; }
        if (!f.host.empty()) r.host = f.host;
        if (!f.enabled) { r.enabled = false; r.enabledSet = true; }
        if (f.exclusive) { r.exclusive = true; r.exclusiveSet = true; }
    };
    if (!state.m->buildConfig.runner.empty()) {
        program.runner.cmd = state.m->buildConfig.runner;
        program.runner.set = true;
        program.runner.origin = mp::Origin{ mp::Tier::Program, {}, 0,
            state.runnerProvider.empty() ? state.m->package.name : state.runnerProvider };
        if (state.m->buildConfig.runExclusive) {
            program.runner.exclusive = true;
            program.runner.exclusiveSet = true;
        }
        table(program.runner, state.m->buildConfig.runnerFields);
    }
    for (auto const& [name, nr] : state.m->buildConfig.namedRunners) {
        auto it = state.namedRunnerProvider.find(name);
        mp::Runner r;
        r.cmd = nr.argv;
        r.origin = mp::Origin{ mp::Tier::Program, {}, 0,
            it == state.namedRunnerProvider.end() ? state.m->package.name : it->second };
        r.set = !nr.argv.empty();
        r.longlived = nr.longLived;
        r.longlivedSet = nr.longLived;
        table(r, nr);
        program.runners[name] = std::move(r);
    }
    mcpp::platform::merge(s, program);
    merge_override_tables(state, s);
    if (P.active) keep_platform_naming(s, state.platformData, P.display(), conflicts);
    const auto envKey = target_env_name(t, "RUNNER");
    if (auto e = mcpp::invocation::get(envKey); e && !e->empty()) {
        if (*e == "none") {
            s.runner = mp::Runner{};
        } else {
            s.runner.cmd = split_words(*e);
            // The command replaces the runner whole, as every other tier's does
            // (`merge`): the packages the replaced one was installed from are
            // not this command's, and it is enabled unless stated otherwise.
            s.runner.requires_.clear();
            s.runner.enabled = true;
            s.runner.enabledSet = false;
            s.runner.origin = mp::Origin{ mp::Tier::Env, {}, 0, envKey };
            s.runner.set = !s.runner.cmd.empty();
        }
    }
    // THE ONE-OFF ENABLING (R1): `MCPP_TARGET_<TRIPLE>_RUNNER_ENABLED`, the
    // spelling of the variables beside it, nearest of all.
    const auto enabledKey = target_env_name(t, "RUNNER_ENABLED");
    if (auto e = mcpp::invocation::get(enabledKey); e && !e->empty()) {
        if (*e == "true" || *e == "1") { s.runner.enabled = true; s.runner.enabledSet = true; }
        else if (*e == "false" || *e == "0") { s.runner.enabled = false; s.runner.enabledSet = true; }
        else mcpp::diag::warning("runner/enabled", std::format(
            "{}={} is not true or false (ignored)", enabledKey, *e));
    }
    return s;
}

void step13_platform_statement(PrepareState& state) {
    auto& P = state.platform;
    std::vector<std::string> conflicts;
    P.fields = platform_statement(state, &conflicts);
    for (auto const& c : conflicts) mcpp::diag::warning("platform/artifact-name", c);
    P.flags = mcpp::platform::evaluate_flags(P.fields, matcher(platform_cfg_context(state)));
}

// A unit's compile words the platform states (`[platform.build]` and its cfg
// tables), as they stand when they are asked for: the std module is compiled
// before the statement is final, and is a unit like any other.
std::vector<std::string> platform_unit_cxxflags(const PrepareState& state) {
    return mcpp::platform::evaluate_flags(platform_statement(state), matcher(platform_cfg_context(state))).cxxflags;
}

std::expected<void, std::string> step13_finish_platform(PrepareState& state, BuildContext& ctx) {
    auto P = state.platform;
    const auto t = target_of(state);

    // A preparation links when it compiles and does not only describe: the
    // run of an archive (RunBuilt) links nothing, and its link and seal
    // programs were the machine's that wrote the archive.
    const bool linksSomething = state.overrides.compiles() && !state.overrides.describes()
        && std::ranges::any_of(ctx.plan.linkUnits, [](auto const& lu) {
               return lu.kind != mcpp::build::LinkUnit::StaticLibrary;
           });

    // Who states the link: the platform, or on a row without one the tiers
    // above it (design 2026-11-22 §3.2). The fields are read wherever they are
    // stated; only the platform's identity needs a platform.
    const std::string stater = P.active ? std::format("the platform {}", P.display())
                                        : std::string("this build");
    // `carrier` is a form of the platform's image (SPEC-013 §11): on a row
    // without a platform it describes nothing, and a configuration that serves
    // both kinds of row may state it, so it is said and not refused.
    if (!P.active && !P.fields.carriers.empty())
        mcpp::diag::warning("platform/carrier", std::format(
            "`carrier` ({}) is stated for {}, which uses no platform; a carrier is a form of a "
            "platform's image, so this build has none and the key is not used",
            mcpp::platform::origin_text(P.fields.carriers.front().origin), t.str()));

    // ── The programs the link and the seal run (§4.4, §4.6) ───────────────
    {
        std::set<std::string> needed;
        for (auto const& in : P.fields.link.inputs)
            if (!in.from.empty()) needed.insert(in.from.front());
        for (auto const& step : P.fields.link.seal)
            if (!step.empty()) needed.insert(step.front());
        for (auto const& [name, e] : P.fields.tools)
            if (auto found = locate_program(state, ctx, e.ref.program.empty() ? name : e.ref.program))
                P.tools[name] = found->generic_string();
        for (auto const& name : needed) {
            if (P.tools.contains(name)) continue;
            if (auto found = locate_program(state, ctx, name)) {
                P.tools[name] = found->generic_string();
                continue;
            }
            if (!linksSomething) continue;
            refusal::record(refusal::Code::PlatformToolAbsent);
            auto it = P.fields.tools.find(name);
            const std::string from = it == P.fields.tools.end()
                ? std::string("no package that builds it is named")
                : std::format("{} builds it ({})", mp::display(it->second.ref),
                              mcpp::platform::origin_text(it->second.origin));
            return std::unexpected(std::format(
                "{} links {} with `{}`, and it is neither built by the graph nor found "
                "on this machine; {}.\n"
                "       Name the package that builds it for this machine:\n"
                "\n"
                "           [target.{}.tools]\n"
                "           {} = \"<package>@<version>\"      # or {{ path = \"<dir>\" }}, {{ program = \"<name>\" }}",
                stater, t.str(), name, from,
                P.active ? std::format("'cfg(platform = \"{}\")'",
                                       mp::platform_name(state.platformPackageShortName))
                         : t.str(),
                name));
        }
    }
    // A platform that links with ld.lld needs one beside the compiler; its
    // absence is said here, not as a link line that starts with a flag.
    if (P.fields.link.driver.value == "lld" && linksSomething
        && mcpp::freestanding::resolve_lld_any(state.tc->binaryPath).empty()) {
        refusal::record(refusal::Code::LldRequiredAbsent);
        return std::unexpected(std::format(
            "{} links {} with ld.lld itself ({}), and the toolchain has no ld.lld "
            "beside {}.\n"
            "       Use a toolchain whose payload carries it -- the platform's own is {} -- with "
            "`[target.<selector>] toolchain`, or remove the line that names this one.",
            stater, t.str(), mcpp::platform::origin_text(P.fields.link.driver.origin),
            state.tc->binaryPath.generic_string(),
            P.fields.toolchain.set ? P.fields.toolchain.value : std::string("an llvm toolchain")));
    }
    // `link.flags` are words for ld.lld itself; a platform that links through
    // the compiler gives them to no program, and says so rather than dropping
    // them (`[platform.build] ldflags` reach the compiler's link).
    if (P.fields.link.flags.set && P.fields.link.driver.value != "lld")
        mcpp::diag::warning("build", std::format(
            "{} states `{}` ({}), and it links through the "
            "compiler: those words are given to ld.lld only, so this link uses none of them.\n"
            "       Words for the compiler's link belong in `{}`.",
            stater, P.active ? "[platform.link] flags" : "[target.<selector>.link] flags",
            mcpp::platform::origin_text(P.fields.link.flags.origin),
            P.active ? "[platform.build] ldflags" : "[target.<selector>.build] ldflags"));
    // THE PLATFORM, REPORTED (SPEC-013 §9): which one and why, then what it
    // contributes beyond the layers the Target lines already showed.
    if (P.active) {
        mcpp::ui::info("Platform", std::format("{} ({}{})", P.display(),
                       mcpp::platform::origin_text(P.selection.origin),
                       P.selection.hosted ? ", hosted" : ""));
        for (auto const& [label, value] : mcpp::platform::report_rows(P))
            if (!mp::is_layer(label))
                mcpp::ui::info("", std::format("{:<17} {}", label, value));
    }
    ctx.plan.platform = std::move(P);
    // THE HOST GRAPH'S, when the build programs were built under it (§6.1).
    ctx.hostPlatform = state.hostPlatform;
    if (ctx.hostPlatform.active)
        mcpp::ui::info("Host", std::format("platform {} ({}): build programs are {} images run by {}",
            ctx.hostPlatform.display(), mcpp::platform::origin_text(ctx.hostPlatform.selection.origin),
            state.hostPlatformTarget,
            state.hostPlatformRunner.empty() ? std::string("its runner")
                : std::filesystem::path(state.hostPlatformRunner.front()).filename().string()));
    if (state.overrides.configures()) {
        if (ctx.hostPlatform.active) {
            ctx.hostTriple = state.hostPlatformTarget;
            ctx.hostCompiler = ctx.hostPlatform.fields.toolchain.value;
            ctx.hostCompilerOrigin = "platform";
        } else if (auto h = state.host_tc_for_build_program()) {
            ctx.hostTriple = h->second.targetTriple;
            ctx.hostCompiler = h->second.binaryPath.generic_string();
            ctx.hostCompilerOrigin = "host";
        } else {
            return std::unexpected(h.error());
        }
    }
    return {};
}

} // namespace mcpp::build

namespace mcpp::build {

// THE HOST GRAPH'S PLATFORM, LOADED (SPEC-013 §6): the package, and the
// target among its own that has this machine's architecture -- what the build
// programs are compiled for and run as. Read when the build programs are
// built; the package is fetched as any dependency is.
std::expected<void, std::string> load_host_platform(PrepareState& state) {
    auto& H = state.hostPlatform;
    auto const& sel = state.hostPlatformSel;
    if (sel.none || H.active) return {};
    const auto& ref = sel.ref;
    fs::path root;
    std::optional<mcpp::manifest::Manifest> m;
    if (!ref.spec.path.empty()) {
        root = ref.spec.path;
        auto r = mcpp::manifest::load(root / "mcpp.toml");
        if (!r) return std::unexpected(std::format("{}: the host platform {}: {}",
            mcpp::platform::origin_text(sel.origin), mp::display(ref), r.error().format()));
        m = std::move(*r);
    } else if (!ref.spec.version.empty()) {
        if (!state.loadVersionDep)
            return std::unexpected("the host platform is read once the graph's loader exists");
        auto r = state.loadVersionDep(ref.name, ref.spec.namespace_, ref.spec.shortName, ref.spec.version);
        if (!r) return std::unexpected(std::format("{}: the host platform {}: {}",
            mcpp::platform::origin_text(sel.origin), mp::display(ref), r.error()));
        root = r->first;
        m = std::move(r->second);
    } else {
        return std::unexpected(std::format(
            "{}: the host platform {} is named by a version or a directory",
            mcpp::platform::origin_text(sel.origin), mp::display(ref)));
    }
    if (!m->platformDeclared) {
        refusal::record(refusal::Code::PlatformNotAPlatform);
        return std::unexpected(std::format(
            "{} is selected as the host graph's platform ({}), and its mcpp.toml has no "
            "[platform] section", qualified(*m), mcpp::platform::origin_text(sel.origin)));
    }
    const std::string arch(mcpp::toolchain::platform::host_arch);
    for (auto const& w : m->platform.targets.value) {
        auto t = mcpp::toolchain::triple::parse(w);
        if (t && t->arch == arch) { state.hostPlatformTarget = t->str(); break; }
    }
    if (state.hostPlatformTarget.empty()) {
        refusal::record(refusal::Code::PlatformTargetUnserved);
        return std::unexpected(std::format(
            "the host graph's platform {} ({}) serves no {} target, so this machine's build "
            "programs cannot be built for it; its targets are {}",
            qualified(*m), mcpp::platform::origin_text(sel.origin), arch,
            [&] { std::string l; for (auto const& w : m->platform.targets.value)
                      l += (l.empty() ? "" : ", ") + w; return l; }()));
    }
    H.active = true;
    H.selection = sel;
    H.package = qualified(*m);
    H.version = m->package.version;
    H.root = root;
    H.fields = m->platform;
    return {};
}

// The spelling a sub-build is told the host platform by (`platform_choice`).
std::string host_platform_choice(const PrepareState& state) {
    auto const& r = state.hostPlatformSel.ref;
    if (!r.spec.path.empty()) return "path:" + r.spec.path;
    return std::format("{}.{}@{}", r.spec.namespace_.empty() ? std::string(mcpp::pm::kDefaultNamespace)
                                                             : r.spec.namespace_,
                       r.spec.shortName, r.spec.version);
}

} // namespace mcpp::build

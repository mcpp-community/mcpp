// mcpp.platform -- the engine's half of platforms (SPEC-013; design
// 2026-10-28-platforms-design.md §3-§9).
//
// The engine holds no knowledge of any one platform. It provides the
// mechanism: which platform a graph uses (selection), what the platform's
// statement is once every tier has spoken (merge), the flags it contributes to
// a target (evaluation of its cfg tables), how an artifact is named and sealed
// (patterns and argv templates), and how all of it is reported. The data comes
// from a platform package's `[platform]` section (mcpp.manifest.platform) and
// from the tables that override it; the prepare phases apply the result to the
// graph, the toolchain, the flags, the link, the runner and the pack.
//
// Everything here is a pure function of its arguments, and the unit tests
// state the rules as tables (tests/unit/test_platform_*.cpp). Whether a
// `[target.<selector>]` applies is asked of the caller (`Matches`), which
// holds the one evaluator of selectors (mcpp.build.prepare_inputs), so this
// module depends on the data model alone. (Importing prepare_inputs here also
// made GCC 16.1 crash importing mcpp.cli, mcpp#848.)

export module mcpp.platform;

import std;
import mcpp.manifest;
import mcpp.toolchain.triple;
import mcpp.invocation;

export namespace mcpp::platform {

// Whether a selector (`cfg(...)`, a triple, an alias) applies to the target
// of this graph.
using Matches = std::function<bool(std::string_view selector)>;

// ── Selection (§5.1, §6.1) ──────────────────────────────────────────────────

// Which platform one graph uses. `none` is the engine's own conventions for
// the target (a host target's, or nothing at all).
struct Selection {
    mcpp::manifest::platform::PackageRef ref;
    mcpp::manifest::platform::Origin     origin;
    bool           none = true;
    // A host triple selected the platform: only its `[platform.hosted]` part
    // applies (§5.4).
    bool           hosted = false;
};

// The value a flag or an environment variable gives: `none`, a package
// reference `<name>@<version>`, or a directory (`path:<dir>`, or anything
// spelled as a path), relative to where mcpp was started.
std::expected<Selection, std::string> parse_choice(std::string_view value, mcpp::manifest::platform::Origin origin);

// The tables of `tables` whose selector the context meets, least specific
// first (SPEC-004 §3.1.1), so that applying them in order lets the most
// specific one decide.
std::vector<const mcpp::manifest::platform::Override*> matching(std::span<const mcpp::manifest::platform::Override> tables,
                                          const Matches& matches);

// Everything a selection reads, in precedence order.
struct ChoiceInputs {
    std::string_view              flag;      // MCPP_PLATFORM (`--platform`)
    std::string_view              env;       // MCPP_TARGET_<TRIPLE>_PLATFORM
    std::string                   envKey;
    std::span<const mcpp::manifest::platform::Override> project;   // the plan root's tables, the workspace layer's first
    std::span<const mcpp::manifest::platform::Override> config;    // config.toml's
    std::string_view              column;    // the target table's platform column
};

// The target graph's platform: the flag, the environment, the project, this
// machine's configuration, the target table, in that order; the first to
// answer decides.
std::expected<Selection, std::string> select_target(const ChoiceInputs& in, const Matches& matches);

// The host graph's platform: `--host-platform` / MCPP_HOST_PLATFORM, the
// project's `[host]`, config.toml's `[host]`; otherwise the host conventions.
std::expected<Selection, std::string> select_host(std::string_view flagOrEnv, bool fromFlag,
                                                  const mcpp::manifest::platform::Override& project,
                                                  const mcpp::manifest::platform::Override& config);

// ── Merge (§4.5) ────────────────────────────────────────────────────────────

// `over` replaces `base` field by field, with two exceptions: flag lists
// concatenate (the platform's first, so a convention cannot be removed, only
// added to), and `link.inputs` and `link.seal` are replaced whole (they are
// one sequence each). Maps (`layers`, `tools`, `runners`, `versions`) are
// merged key by key.
void merge(mcpp::manifest::platform::Statement& base, const mcpp::manifest::platform::Statement& over);

// The part of a platform that applies when a host triple selects it: its
// `[platform.hosted]` toolchain and layers, and nothing else (§5.4).
mcpp::manifest::platform::Statement hosted_view(const mcpp::manifest::platform::Statement& platform);

// Whether the platform serves the triple, or refuses with the reason. A host
// triple is served by a platform with a `[platform.hosted]` table.
std::expected<bool /*hosted*/, std::string>
serves(const mcpp::manifest::platform::Statement& platform, std::string_view package,
       const mcpp::toolchain::triple::Triple& t);

// The four flag lists the platform gives every unit of a target: the
// unconditional ones, then each cfg table the context meets, in file order.
struct TargetFlags {
    std::vector<std::string> cflags, cxxflags, asmflags, ldflags;
};
TargetFlags evaluate_flags(const mcpp::manifest::platform::Statement& s, const Matches& matches);

// ── Artifacts (§4, §4.6) ────────────────────────────────────────────────────

// `"lib{name}.okso"` -> {"lib", ".okso"}. A pattern names `{name}` exactly once.
std::expected<std::pair<std::string, std::string>, std::string>
split_name_pattern(std::string_view pattern);

// One seal step for one artifact: `{}` is the artifact; an argument `{needed}`
// stands for the shared artifacts it needs, one argument each, and when the
// argument before it is an option that takes a value (`--with`: no `=`, no
// `{}`), that option is repeated before each of them (both are left out when
// it needs none).
std::vector<std::string> expand_step(const std::vector<std::string>& step,
                                     std::string_view artifact,
                                     std::span<const std::string> needed);

// ── What the build uses (§9) ────────────────────────────────────────────────

// The platform of one graph after every tier has spoken: what the flags, the
// link, the runner, the pack and the report read.
struct Resolved {
    bool                  active = false;   // a platform package is in use
    Selection             selection;
    std::string           package;          // "mcpplibs.openkal-platform"
    std::string           version;          // "0.17.1"
    std::filesystem::path root;             // its directory
    mcpp::manifest::platform::Statement         fields;           // merged
    TargetFlags           flags;            // evaluated for the target
    // Per layer the platform names: who provides it in this build, and
    // whether that is the platform's package or one the graph already had.
    struct Layer { std::string provider; mcpp::manifest::platform::Tier tier = mcpp::manifest::platform::Tier::Platform; };
    std::map<std::string, Layer> layers;
    // Per tool: the program the build runs.
    std::map<std::string, std::string> tools;

    // The platform's part of a configuration's identity: name, version and
    // source, so that another platform is another build directory (§8.3).
    std::string identity() const;
    // `openkal-platform 0.17.1`
    std::string display() const;
};

// The `source` column of the report and of JSON: `built-in`, `platform`,
// `config.toml`, `mcpp.toml`, `env`, `flag`, `graph`, `build-program`.
std::string origin_text(const mcpp::manifest::platform::Origin& o);

// The lines `Platform …` and its fields, as the target report prints them.
std::vector<std::pair<std::string, std::string>> report_rows(const Resolved& r);

// The JSON object of the `plan` event's `platform` / `hostPlatform`.
std::string to_json(const Resolved& r);

} // namespace mcpp::platform

// ── implementation ──────────────────────────────────────────────────────────

namespace mcpp::platform {

namespace {

namespace mp = mcpp::manifest::platform;

bool names_a_path(std::string_view v) {
    if (v.empty()) return false;
    if (v.starts_with("path:")) return true;
    if (v.starts_with("/") || v.starts_with("./") || v.starts_with("../") || v.starts_with("~")
        || v == "." || v == "..")
        return true;
    if (v.size() > 2 && v[1] == ':' && (v[2] == '\\' || v[2] == '/')) return true;   // C:\…
    return v.find('@') == std::string_view::npos
        && (v.find('/') != std::string_view::npos || v.find('\\') != std::string_view::npos);
}

void append(std::vector<std::string>& to, const std::vector<std::string>& from) {
    to.insert(to.end(), from.begin(), from.end());
}

std::string json_escape(std::string_view s) {
    std::string o;
    o.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\t': o += "\\t"; break;
            case '\r': o += "\\r"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) o += std::format("\\u{:04x}", int(c));
                else o += c;
        }
    }
    return o;
}

std::string q(std::string_view s) { return "\"" + json_escape(s) + "\""; }

// An option that takes the next argument as its value (`--with`, `-L`): no
// `=` and no `{}` of its own.
bool is_bare_option(std::string_view a) {
    return a.starts_with("-") && a.find('=') == std::string_view::npos
        && a.find("{}") == std::string_view::npos;
}

std::string argv_text(const std::vector<std::string>& v) {
    std::string out;
    for (auto const& a : v) {
        if (!out.empty()) out += ' ';
        out += a.find(' ') == std::string::npos ? a : "\"" + a + "\"";
    }
    return out;
}

} // namespace

std::expected<Selection, std::string> parse_choice(std::string_view value, mp::Origin origin) {
    Selection s;
    s.origin = std::move(origin);
    if (value == "none") return s;
    if (names_a_path(value)) {
        std::string dir(value.starts_with("path:") ? value.substr(5) : value);
        if (dir.starts_with("~"))
            if (auto h = mcpp::invocation::get("HOME"); h && !h->empty()) dir = *h + dir.substr(1);
        std::error_code ec;
        auto p = std::filesystem::absolute(std::filesystem::path(dir), ec).lexically_normal();
        s.ref.spec.path = p.generic_string();
        s.none = false;
        return s;
    }
    auto r = mp::parse_ref_text(value);
    if (!r) return std::unexpected(std::format("{}: {}", s.origin.key, r.error()));
    s.ref = std::move(*r);
    s.none = false;
    return s;
}

std::vector<const mp::Override*> matching(std::span<const mp::Override> tables,
                                          const Matches& matches) {
    std::vector<const mp::Override*> out;
    for (auto const& t : tables)
        if (matches(t.selector)) out.push_back(&t);
    std::ranges::stable_sort(out, [](const mp::Override* a, const mp::Override* b) {
        return mcpp::manifest::cfg::specificity(a->selector)
             < mcpp::manifest::cfg::specificity(b->selector);
    });
    return out;
}

std::expected<Selection, std::string> select_target(const ChoiceInputs& in, const Matches& matches) {
    if (!in.flag.empty())
        return parse_choice(in.flag, mp::Origin{mp::Tier::Flag, {}, 0, "--platform"});
    if (!in.env.empty())
        return parse_choice(in.env, mp::Origin{mp::Tier::Env, {}, 0, in.envKey});
    for (auto span : {in.project, in.config}) {
        const mp::Override* chosen = nullptr;
        for (auto const* o : matching(span, matches))
            if (o->platformSet) chosen = o;
        if (!chosen) continue;
        Selection s;
        s.origin = chosen->platformOrigin;
        if (s.origin.key.empty())
            s.origin.key = std::format("[target.{}].platform", chosen->selector);
        if (chosen->platformNone) return s;
        s.ref = chosen->platform;
        s.none = false;
        return s;
    }
    if (!in.column.empty()) {
        auto s = parse_choice(in.column, mp::Origin{mp::Tier::BuiltIn, {}, 0, "the target table"});
        if (!s) return s;
        return s;
    }
    Selection none;
    none.origin = mp::Origin{mp::Tier::BuiltIn, {}, 0, "the target table"};
    return none;
}

std::expected<Selection, std::string> select_host(std::string_view flagOrEnv, bool fromFlag,
                                                  const mp::Override& project,
                                                  const mp::Override& config) {
    if (!flagOrEnv.empty())
        return parse_choice(flagOrEnv, mp::Origin{fromFlag ? mp::Tier::Flag : mp::Tier::Env, {}, 0,
                                                  fromFlag ? "--host-platform" : "MCPP_HOST_PLATFORM"});
    for (auto const* o : {&project, &config}) {
        if (!o->platformSet) continue;
        Selection s;
        s.origin = o->platformOrigin;
        if (s.origin.key.empty()) s.origin.key = "[host].platform";
        if (o->platformNone) return s;
        s.ref = o->platform;
        s.none = false;
        return s;
    }
    Selection none;
    none.origin = mp::Origin{mp::Tier::BuiltIn, {}, 0, "host conventions"};
    return none;
}

void merge(mp::Statement& b, const mp::Statement& o) {
    auto text = [](mp::Text& to, const mp::Text& from) { if (from.set) to = from; };
    auto words = [](mp::Words& to, const mp::Words& from) { if (from.set) to = from; };
    words(b.targets, o.targets);
    text(b.toolchain, o.toolchain);
    if (o.sysroot.set) { b.sysroot = o.sysroot; b.sysrootProvides = o.sysrootProvides; }
    // Field by field: an argv-only statement keeps the platform's `host` and
    // `exclusive`; a table, or a program's `run-host`, states what it states.
    if (o.runner.set) {
        b.runner.cmd = o.runner.cmd;
        b.runner.origin = o.runner.origin;
        b.runner.set = true;
    }
    if (!o.runner.host.empty()) b.runner.host = o.runner.host;
    if (o.runner.exclusiveSet) {
        b.runner.exclusive = o.runner.exclusive;
        b.runner.exclusiveSet = true;
    }
    for (auto const& [k, v] : o.runners) b.runners[k] = v;
    if (o.carrier.set) b.carrier = o.carrier;
    text(b.plugin, o.plugin);
    for (auto const& [k, v] : o.layers) b.layers[k] = v;
    for (auto const& [k, v] : o.tools) b.tools[k] = v;
    append(b.build.cflags, o.build.cflags);
    append(b.build.cxxflags, o.build.cxxflags);
    append(b.build.asmflags, o.build.asmflags);
    append(b.build.ldflags, o.build.ldflags);
    for (auto const& c : o.buildCfg) b.buildCfg.push_back(c);
    text(b.link.driver, o.link.driver);
    if (o.link.flags.set) {
        append(b.link.flags.value, o.link.flags.value);
        if (!b.link.flags.set) b.link.flags.origin = o.link.flags.origin;
        b.link.flags.set = true;
    }
    text(b.link.start, o.link.start);
    text(b.link.script, o.link.script);
    if (o.link.inputsSet) {
        b.link.inputs = o.link.inputs;
        b.link.inputsOrigin = o.link.inputsOrigin;
        b.link.inputsSet = true;
    }
    text(b.link.exe, o.link.exe);
    text(b.link.shared, o.link.shared);
    if (o.link.sealSet) {
        b.link.seal = o.link.seal;
        b.link.sealOrigin = o.link.sealOrigin;
        b.link.sealSet = true;
    }
    for (auto const& [k, v] : o.versions) b.versions[k] = v;
    if (!o.versions.empty()) b.versionsOrigin = o.versionsOrigin;
    if (o.hosted.set) {
        text(b.hosted.toolchain, o.hosted.toolchain);
        for (auto const& [k, v] : o.hosted.layers) b.hosted.layers[k] = v;
        b.hosted.set = true;
    }
}

mp::Statement hosted_view(const mp::Statement& p) {
    mp::Statement s;
    s.targets = p.targets;
    s.toolchain = p.hosted.toolchain;
    s.layers = p.hosted.layers;
    s.versions = p.versions;
    s.versionsOrigin = p.versionsOrigin;
    return s;
}

std::expected<bool, std::string>
serves(const mp::Statement& p, std::string_view package, const mcpp::toolchain::triple::Triple& t) {
    for (auto const& w : p.targets.value) {
        auto pt = mcpp::toolchain::triple::parse(w);
        if (pt ? pt->str() == t.str() : w == t.str()) return false;
    }
    std::string list;
    for (auto const& w : p.targets.value) { if (!list.empty()) list += ", "; list += w; }
    if (p.hosted.set) return true;
    return std::unexpected(std::format(
        "platform {} serves {} and not {}: it has no [platform.hosted] table, the part a host "
        "triple may take (SPEC-013 §5.4).\n"
        "       Build for one of its targets (--target {}), or select another platform for {}",
        package, list, t.str(), p.targets.value.empty() ? std::string("<target>") : p.targets.value.front(),
        t.str()));
}

TargetFlags evaluate_flags(const mp::Statement& s, const Matches& matches) {
    TargetFlags f;
    append(f.cflags, s.build.cflags);
    append(f.cxxflags, s.build.cxxflags);
    append(f.asmflags, s.build.asmflags);
    append(f.ldflags, s.build.ldflags);
    for (auto const& t : s.buildCfg) {
        if (!matches(t.predicate)) continue;
        append(f.cflags, t.flags.cflags);
        append(f.cxxflags, t.flags.cxxflags);
        append(f.asmflags, t.flags.asmflags);
        append(f.ldflags, t.flags.ldflags);
    }
    return f;
}

std::expected<std::pair<std::string, std::string>, std::string>
split_name_pattern(std::string_view pattern) {
    constexpr std::string_view kName = "{name}";
    const auto at = pattern.find(kName);
    if (at == std::string_view::npos || pattern.find(kName, at + 1) != std::string_view::npos)
        return std::unexpected(std::format(
            "'{}' names the artifact as {{name}} exactly once, for example \"{{name}}.oke\"", pattern));
    auto prefix = std::string(pattern.substr(0, at));
    auto suffix = std::string(pattern.substr(at + kName.size()));
    if (prefix.find_first_of("/\\") != std::string::npos || suffix.find_first_of("/\\") != std::string::npos)
        return std::unexpected(std::format("'{}' is a file name pattern; it names no directory", pattern));
    return std::pair{ std::move(prefix), std::move(suffix) };
}

std::vector<std::string> expand_step(const std::vector<std::string>& step,
                                     std::string_view artifact,
                                     std::span<const std::string> needed) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < step.size(); ++i) {
        auto const& a = step[i];
        if (a == "{needed}") {
            // The option in front of it, if any, was held back below.
            const bool optioned = i > 0 && is_bare_option(step[i - 1]);
            for (auto const& n : needed) {
                if (optioned) out.push_back(step[i - 1]);
                out.push_back(n);
            }
            continue;
        }
        if (i + 1 < step.size() && step[i + 1] == "{needed}" && is_bare_option(a)) continue;
        if (a == "{}") { out.emplace_back(artifact); continue; }
        // `{}` inside an argument (`--image={}`) stands for the artifact too.
        std::string v = a;
        for (std::size_t p = v.find("{}"); p != std::string::npos; p = v.find("{}", p + artifact.size()))
            v.replace(p, 2, artifact);
        out.push_back(std::move(v));
    }
    return out;
}

std::string Resolved::identity() const {
    if (!active) return {};
    std::string id = std::format("{}@{}", package, version);
    if (!selection.ref.spec.path.empty()) id += "|path:" + selection.ref.spec.path;
    if (!selection.ref.spec.git.empty())
        id += std::format("|git:{}#{}", selection.ref.spec.git, selection.ref.spec.gitRev);
    return id;
}

std::string Resolved::display() const {
    if (!active) return "host conventions";
    auto dot = package.find('.');
    return std::format("{} {}", dot == std::string::npos ? package : package.substr(dot + 1), version);
}

std::string origin_text(const mp::Origin& o) {
    switch (o.tier) {
        case mp::Tier::Env:
        case mp::Tier::Flag:   return o.key;
        case mp::Tier::Config:
        case mp::Tier::Manifest: {
            std::string file = std::filesystem::path(o.file).filename().generic_string();
            if (file.empty()) file = std::string(mp::tier_name(o.tier));
            return o.line > 0 ? std::format("{}:{}", file, o.line) : file;
        }
        default: return std::string(mp::tier_name(o.tier));
    }
}

std::vector<std::pair<std::string, std::string>> report_rows(const Resolved& r) {
    std::vector<std::pair<std::string, std::string>> rows;
    if (!r.active) return rows;
    auto const& f = r.fields;
    for (auto const& [layer, l] : r.layers)
        rows.emplace_back(layer, std::format("{} ({})", l.provider, mp::tier_name(l.tier)));
    if (!f.tools.empty()) {
        // Tools grouped by the package that builds them.
        std::map<std::string, std::pair<std::vector<std::string>, std::string>> by;
        for (auto const& [name, t] : f.tools) {
            auto key = mp::display(t.ref);
            by[key].first.push_back(name);
            by[key].second = origin_text(t.origin);
        }
        for (auto const& [pkg, v] : by) {
            std::string names;
            for (auto const& n : v.first) { if (!names.empty()) names += ", "; names += n; }
            rows.emplace_back("tools", std::format("{} ({}, {})", names, pkg, v.second));
        }
    }
    if (f.runner.set)
        rows.emplace_back("runner", std::format("{} ({})", f.runner.cmd.empty() ? std::string() : f.runner.cmd.front(),
                                                origin_text(f.runner.origin)));
    for (auto const& [name, w] : f.runners)
        rows.emplace_back("runner " + name, std::format("{} ({})", argv_text(w.value), origin_text(w.origin)));
    if (f.link.driver.set && f.link.driver.value != "compiler")
        rows.emplace_back("link", std::format("{}{} ({})", f.link.driver.value,
                                              f.link.start.set ? ", start " + f.link.start.value : "",
                                              origin_text(f.link.driver.origin)));
    return rows;
}

std::string to_json(const Resolved& r) {
    if (!r.active) return "null";
    auto const& f = r.fields;
    std::string o = "{";
    o += "\"name\":" + q(r.package);
    o += ",\"version\":" + q(r.version);
    o += ",\"source\":" + q(origin_text(r.selection.origin));
    o += ",\"hosted\":" + std::string(r.selection.hosted ? "true" : "false");
    o += ",\"fields\":{";
    bool first = true;
    auto field = [&](std::string_view key, std::string value, const mp::Origin& origin) {
        if (!first) o += ",";
        first = false;
        o += q(key) + ":{\"value\":" + value + ",\"source\":" + q(origin_text(origin)) + "}";
    };
    auto arr = [](const std::vector<std::string>& v) {
        std::string s = "[";
        for (std::size_t i = 0; i < v.size(); ++i) { if (i) s += ","; s += q(v[i]); }
        return s + "]";
    };
    if (f.toolchain.set) field("toolchain", q(f.toolchain.value), f.toolchain.origin);
    if (f.runner.set) field("runner", arr(f.runner.cmd), f.runner.origin);
    for (auto const& [k, w] : f.runners) field("runners." + k, arr(w.value), w.origin);
    for (auto const& [layer, l] : r.layers) {
        mp::Origin origin{l.tier, {}, 0, {}};
        if (auto it = f.layers.find(layer); it != f.layers.end() && l.tier != mp::Tier::Graph)
            origin = it->second.origin;
        field("layers." + layer, q(l.provider), origin);
    }
    for (auto const& [name, t] : f.tools) {
        auto it = r.tools.find(name);
        field("tools." + name, it == r.tools.end() ? q(mp::display(t.ref)) : q(it->second), t.origin);
    }
    if (f.link.driver.set) field("link.driver", q(f.link.driver.value), f.link.driver.origin);
    if (f.link.start.set) field("link.start", q(f.link.start.value), f.link.start.origin);
    if (f.link.exe.set) field("link.exe", q(f.link.exe.value), f.link.exe.origin);
    if (f.link.shared.set) field("link.shared", q(f.link.shared.value), f.link.shared.origin);
    if (f.carrier.set) field("carrier", q(mp::display(f.carrier.package)), f.carrier.origin);
    o += "}}";
    return o;
}

} // namespace mcpp::platform

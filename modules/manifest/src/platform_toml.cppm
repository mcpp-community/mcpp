// mcpp.manifest.platform_toml -- the TOML reading of a platform statement
// (SPEC-013 §2; design 2026-10-28 §4, §5.2, §6.1).
//
// One reader for the three places the same keys are written: a platform
// package's `[platform]` section, a `[target.<selector>]` table of a project or
// of config.toml, and `[host]`. The tier and the file travel with every value,
// so a merged statement reports where each of its fields came from.
//
// Type errors are refusals naming the key. A key this engine does not know is
// recorded as a warning (`--strict` refuses it), as `[target.<selector>]`
// does: a package written for a newer engine still loads, and states its floor
// with `[package] mcpp` (R9.8).

export module mcpp.manifest.platform_toml;

import std;
import mcpp.libs.toml;
import mcpp.manifest.platform;
import mcpp.pm.dep_spec;

export namespace mcpp::manifest::platform {

struct Reader {
    std::filesystem::path     file;       // the file the values are written in
    Tier                      tier = Tier::Platform;
    std::vector<std::string>* warnings = nullptr;

    Origin at(const mcpp::libs::toml::Value& v, std::string key) const;
    std::filesystem::path absolute(const std::string& written) const;

    std::expected<std::vector<std::string>, std::string>
    strings(const mcpp::libs::toml::Value& v, std::string_view where) const;

    // A package: `"<name>@<version>"`, a dependency table with `name`, or —
    // where allowed — `false` (a layer the platform does not provide) and
    // `{ program = "<name>" }` (a tool found rather than built).
    std::expected<PackageRef, std::string>
    ref(const mcpp::libs::toml::Value& v, std::string_view where, bool allowOff, bool allowProgram) const;

    std::expected<Runner, std::string> runner(const mcpp::libs::toml::Value& v, std::string_view where) const;

    std::expected<void, std::string>
    flags(const mcpp::libs::toml::Table& t, std::string_view where, Flags& out,
          std::vector<CfgFlags>* cfg) const;

    std::expected<void, std::string>
    link(const mcpp::libs::toml::Table& t, std::string_view where, Link& out) const;

    // One entry of `link.inputs`: `{ file = "..." }` or `{ from = [...], as = "..." }`.
    std::expected<LinkInput, std::string>
    link_input(const mcpp::libs::toml::Value& e, std::string_view key) const;

    std::expected<void, std::string>
    layers(const mcpp::libs::toml::Value& v, std::string_view where,
           std::map<std::string, LayerEntry>& out) const;

    std::expected<void, std::string>
    tools(const mcpp::libs::toml::Value& v, std::string_view where,
          std::map<std::string, ToolEntry>& out) const;

    std::expected<Carrier, std::string> carrier(const mcpp::libs::toml::Value& v, std::string_view where) const;
    std::expected<std::vector<Carrier>, std::string>
    carriers(const mcpp::libs::toml::Value& v, std::string_view where) const;

    // A list of `<os>-<arch>` pairs (`runner.hosts`, `carrier.hosts`).
    std::expected<std::vector<std::string>, std::string>
    host_pairs(const mcpp::libs::toml::Value& v, std::string_view where) const;

    // `[platform]` of a platform package.
    std::expected<Statement, std::string> statement(const mcpp::libs::toml::Table& t) const;

    // The platform part of one `[target.<selector>]` table (the keys of
    // `kOverrideKeys`, plus `runner` and `runners`, which the table already
    // had) or of `[host]` (selector empty: `platform` only).
    std::expected<Override, std::string>
    override_(const mcpp::libs::toml::Table& t, std::string_view selector) const;

    void warn(std::string text) const { if (warnings) warnings->push_back(std::move(text)); }
};

} // namespace mcpp::manifest::platform

// ── implementation ──────────────────────────────────────────────────────────

namespace mcpp::manifest::platform {

namespace {

namespace toml = mcpp::libs::toml;

std::string list_of(std::span<const std::string_view> keys) {
    std::string out;
    for (auto k : keys) { if (!out.empty()) out += ", "; out += k; }
    return out;
}

bool contains(std::span<const std::string_view> keys, std::string_view k) {
    return std::ranges::find(keys, k) != keys.end();
}

void split_name(PackageRef& r) {
    if (r.name.empty()) return;
    const auto dot = r.name.find('.');
    if (dot == std::string::npos) {
        r.spec.namespace_ = std::string(mcpp::pm::kDefaultNamespace);
        r.spec.shortName = r.name;
        r.spec.namespaceOmitted = true;
    } else {
        r.spec.namespace_ = r.name.substr(0, dot);
        r.spec.shortName = r.name.substr(dot + 1);
        r.spec.namespaceOmitted = false;
    }
}

} // namespace

Origin Reader::at(const toml::Value& v, std::string key) const {
    return Origin{ tier, file.generic_string(), static_cast<int>(v.position.line), std::move(key) };
}

std::filesystem::path Reader::absolute(const std::string& written) const {
    std::filesystem::path p(written);
    if (p.is_relative()) {
        std::error_code ec;
        auto dir = file.parent_path();
        if (dir.is_relative()) dir = std::filesystem::absolute(dir, ec);
        p = dir / p;
    }
    return p.lexically_normal();
}

std::expected<std::vector<std::string>, std::string>
Reader::strings(const toml::Value& v, std::string_view where) const {
    if (!v.is_array())
        return std::unexpected(std::format("{} must be an array of strings", where));
    std::vector<std::string> out;
    for (auto const& e : v.as_array()) {
        if (!e.is_string())
            return std::unexpected(std::format("{} must contain only strings", where));
        out.push_back(e.as_string());
    }
    return out;
}

std::expected<PackageRef, std::string>
Reader::ref(const toml::Value& v, std::string_view where, bool allowOff, bool allowProgram) const {
    PackageRef r;
    if (v.is_bool()) {
        if (allowOff && !v.as_bool()) { r.off = true; return r; }
        return std::unexpected(std::format(
            "{} must be a package reference{}", where, allowOff ? " or false" : ""));
    }
    if (v.is_string()) {
        auto parsed = parse_ref_text(v.as_string());
        if (!parsed) return std::unexpected(std::format("{}: {}", where, parsed.error()));
        return std::move(*parsed);
    }
    if (!v.is_table())
        return std::unexpected(std::format(
            "{} must be \"<name>@<version>\" or a table such as {{ path = \"<dir>\" }}", where));
    auto const& t = v.as_table();
    if (auto it = t.find("program"); it != t.end()) {
        if (!allowProgram)
            return std::unexpected(std::format(
                "{}: `program` names a program found on this machine and is accepted "
                "for a tool only", where));
        if (!it->second.is_string() || it->second.as_string().empty())
            return std::unexpected(std::format("{}.program must be a program name", where));
        if (t.size() != 1)
            return std::unexpected(std::format(
                "{}: {{ program = \"...\" }} takes no other key", where));
        r.program = it->second.as_string();
        return r;
    }
    for (auto const& [k, val] : t) {
        auto key = std::format("{}.{}", where, k);
        auto str = [&]() -> std::expected<std::string, std::string> {
            if (!val.is_string() || val.as_string().empty())
                return std::unexpected(std::format("{} must be a non-empty string", key));
            return val.as_string();
        };
        if (k == "name") {
            auto s = str(); if (!s) return std::unexpected(s.error());
            r.name = *s;
        } else if (k == "version") {
            auto s = str(); if (!s) return std::unexpected(s.error());
            r.spec.version = *s;
        } else if (k == "path") {
            auto s = str(); if (!s) return std::unexpected(s.error());
            r.spec.path = absolute(*s).generic_string();
        } else if (k == "git") {
            auto s = str(); if (!s) return std::unexpected(s.error());
            r.spec.git = *s;
        } else if (k == "rev" || k == "tag" || k == "branch") {
            auto s = str(); if (!s) return std::unexpected(s.error());
            if (!r.spec.gitRefKind.empty())
                return std::unexpected(std::format(
                    "{}: one of rev / tag / branch, not both {} and {}", where, r.spec.gitRefKind, k));
            r.spec.gitRev = *s;
            r.spec.gitRefKind = k;
        } else if (k == "features") {
            auto s = strings(val, key); if (!s) return std::unexpected(s.error());
            r.spec.features = std::move(*s);
        } else if (k == "default-features") {
            if (!val.is_bool())
                return std::unexpected(std::format("{} must be true or false", key));
            r.spec.defaultFeatures = val.as_bool();
            r.spec.defaultFeaturesDeclared = true;
        } else {
            warn(std::format(
                "{}: '{}' is not a key of a package reference (ignored); the keys are name, version, "
                "path, git, rev, tag, branch, features, default-features{}",
                where, k, allowProgram ? ", or program alone" : ""));
        }
    }
    const int sources = int(!r.spec.version.empty()) + int(!r.spec.path.empty())
                      + int(!r.spec.git.empty());
    if (sources != 1)
        return std::unexpected(std::format(
            "{} must name exactly one source: version, path or git", where));
    if (!r.spec.git.empty() && r.spec.gitRev.empty())
        return std::unexpected(std::format("{}: a git source needs one of rev / tag / branch", where));
    if (r.spec.git.empty() && !r.spec.gitRev.empty())
        return std::unexpected(std::format("{}: {} applies to a git source", where, r.spec.gitRefKind));
    if (!r.spec.version.empty() && r.name.empty())
        return std::unexpected(std::format(
            "{}: a version names the package too: {{ name = \"<name>\", version = \"<version>\" }}, "
            "or \"<name>@<version>\"", where));
    // A repository is not read before its package is fetched, and its URL does
    // not say the package's name; a git reference names it.
    if (!r.spec.git.empty() && r.name.empty())
        return std::unexpected(std::format(
            "{}: a git source names the package too: {{ name = \"<name>\", git = \"<url>\", ... }}",
            where));
    split_name(r);
    return r;
}

std::expected<Runner, std::string> Reader::runner(const toml::Value& v, std::string_view where) const {
    Runner r;
    r.origin = at(v, std::string(where));
    r.set = true;
    if (v.is_array()) {
        auto c = strings(v, where);
        if (!c) return std::unexpected(c.error());
        r.cmd = std::move(*c);
    } else if (v.is_table()) {
        for (auto const& [k, val] : v.as_table()) {
            auto key = std::format("{}.{}", where, k);
            if (k == "cmd") {
                auto c = strings(val, key);
                if (!c) return std::unexpected(c.error());
                r.cmd = std::move(*c);
            } else if (k == "host") {
                if (!val.is_string() || (val.as_string() != "same-arch" && val.as_string() != "any"))
                    return std::unexpected(std::format(
                        "{} must be \"same-arch\" (the artifact runs on a machine of its "
                        "architecture) or \"any\" (the runner reaches it from any machine)", key));
                r.host = val.as_string();
            } else if (k == "exclusive") {
                if (!val.is_bool()) return std::unexpected(std::format("{} must be true or false", key));
                r.exclusive = val.as_bool();
                r.exclusiveSet = true;
            } else if (k == "hosts") {
                auto h = host_pairs(val, key);
                if (!h) return std::unexpected(h.error());
                r.hosts = std::move(*h);
                r.hostsSet = true;
            } else {
                warn(std::format(
                    "{}: '{}' is not a key of a runner (ignored); the keys are cmd, host, hosts, exclusive",
                    where, k));
            }
        }
    } else {
        return std::unexpected(std::format(
            "{} must be an array of strings or {{ cmd = [...], host = \"same-arch\" }}", where));
    }
    if (r.cmd.empty())
        return std::unexpected(std::format(
            "{} has an empty command; an empty template would run nothing and report success", where));
    return r;
}

std::expected<void, std::string>
Reader::flags(const toml::Table& t, std::string_view where, Flags& out,
              std::vector<CfgFlags>* cfg) const {
    for (auto const& [k, v] : t) {
        auto key = std::format("{}.{}", where, k);
        if (cfg && v.is_table() && k.starts_with("cfg(") && k.ends_with(")")) {
            CfgFlags c;
            c.predicate = k;
            c.origin = at(v, std::format("{}.'{}'", where, k));
            if (auto r = flags(v.as_table(), std::format("{}.'{}'", where, k), c.flags, nullptr); !r)
                return r;
            cfg->push_back(std::move(c));
            continue;
        }
        std::vector<std::string>* dst =
              k == "cflags"   ? &out.cflags
            : k == "cxxflags" ? &out.cxxflags
            : k == "asmflags" ? &out.asmflags
            : k == "ldflags"  ? &out.ldflags
            : nullptr;
        if (!dst) {
            warn(std::format(
                "{} has unsupported key '{}' (ignored); the keys are {}{}", where, k, list_of(kBuildKeys),
                cfg ? ", and 'cfg(...)' tables of the same keys" : ""));
            continue;
        }
        auto s = strings(v, key);
        if (!s) return std::unexpected(s.error());
        dst->insert(dst->end(), s->begin(), s->end());
    }
    return {};
}

std::expected<LinkInput, std::string>
Reader::link_input(const toml::Value& e, std::string_view key) const {
    if (!e.is_table())
        return std::unexpected(std::format(
            "{} must contain only tables: {{ file = \"...\" }} or "
            "{{ from = [...], as = \"...\" }}", key));
    LinkInput in;
    for (auto const& [ik, iv] : e.as_table()) {
        auto ikey = std::format("{}.{}", key, ik);
        if (ik == "file") {
            if (!iv.is_string() || iv.as_string().empty())
                return std::unexpected(std::format("{} must be a path", ikey));
            in.file = absolute(iv.as_string()).generic_string();
        } else if (ik == "from") {
            auto s = strings(iv, ikey);
            if (!s) return std::unexpected(s.error());
            if (s->empty())
                return std::unexpected(std::format("{} is empty; it names a program", ikey));
            in.from = std::move(*s);
        } else if (ik == "as") {
            if (!iv.is_string() || iv.as_string().empty()
                || iv.as_string().find_first_of("/\\") != std::string::npos)
                return std::unexpected(std::format("{} must be a file name", ikey));
            in.as = iv.as_string();
        } else {
            warn(std::format(
                "{}: '{}' is not a key of a link input (ignored); the keys are file, "
                "or from and as", key, ik));
        }
    }
    if (in.file.empty() == in.from.empty())
        return std::unexpected(std::format(
            "{}: an input is {{ file = \"...\" }} or {{ from = [...], as = \"...\" }}", key));
    if (!in.from.empty() && in.as.empty())
        return std::unexpected(std::format(
            "{}: an input made by a program names the file it is saved as (`as`)", key));
    return in;
}

std::expected<void, std::string>
Reader::link(const toml::Table& t, std::string_view where, Link& out) const {
    for (auto const& [k, v] : t) {
        auto key = std::format("{}.{}", where, k);
        auto text = [&](Text& dst, bool needsName) -> std::expected<void, std::string> {
            if (!v.is_string() || v.as_string().empty())
                return std::unexpected(std::format("{} must be a non-empty string", key));
            // A name pattern holds `{name}` exactly once and names no directory
            // (the rule `mcpp::platform::split_name_pattern` applies).
            const std::string_view s = v.as_string();
            const auto first = s.find("{name}");
            if (needsName && (first == std::string_view::npos
                              || s.find("{name}", first + 1) != std::string_view::npos))
                return std::unexpected(std::format(
                    "{} = \"{}\" names the artifact as {{name}} exactly once; "
                    "for example \"{{name}}.oke\"", key, s));
            if (needsName && s.find_first_of("/\\") != std::string_view::npos)
                return std::unexpected(std::format(
                    "{} = \"{}\" is a file name pattern; it names no directory", key, s));
            dst = Text{ v.as_string(), at(v, key), true };
            return {};
        };
        if (k == "driver") {
            if (!v.is_string() || (v.as_string() != "compiler" && v.as_string() != "lld"))
                return std::unexpected(std::format(
                    "{} must be \"compiler\" (the compiler driver links) or \"lld\" "
                    "(ld.lld itself links, with `flags`)", key));
            out.driver = Text{ v.as_string(), at(v, key), true };
        } else if (k == "flags") {
            auto s = strings(v, key);
            if (!s) return std::unexpected(s.error());
            out.flags = Words{ std::move(*s), at(v, key), true };
        } else if (k == "start") {
            if (auto r = text(out.start, false); !r) return r;
        } else if (k == "script") {
            if (auto r = text(out.script, false); !r) return r;
            out.script.value = absolute(out.script.value).generic_string();
        } else if (k == "exe") {
            if (auto r = text(out.exe, true); !r) return r;
            if (!out.exe.value.starts_with("{name}"))
                return std::unexpected(std::format(
                    "{} = \"{}\": an executable's name begins with the target's name; "
                    "write \"{{name}}<suffix>\", for example \"{{name}}.oke\"", key, out.exe.value));
        } else if (k == "shared") {
            if (auto r = text(out.shared, true); !r) return r;
        } else if (k == "inputs") {
            if (!v.is_array())
                return std::unexpected(std::format(
                    "{} must be an array of {{ file = \"...\" }} or {{ from = [...], as = \"...\" }}", key));
            out.inputs.clear();
            for (auto const& e : v.as_array()) {
                auto in = link_input(e, key);
                if (!in) return std::unexpected(in.error());
                out.inputs.push_back(std::move(*in));
            }
            out.inputsOrigin = at(v, key);
            out.inputsSet = true;
        } else if (k == "seal") {
            if (!v.is_array())
                return std::unexpected(std::format(
                    "{} must be an array of commands, for example [[\"okld\", \"check\", \"{{}}\"]]", key));
            out.seal.clear();
            for (auto const& e : v.as_array()) {
                auto s = strings(e, key + "[]");
                if (!s) return std::unexpected(std::format(
                    "{} must be an array of commands, each an array of strings", key));
                if (s->empty())
                    return std::unexpected(std::format("{} has an empty command", key));
                out.seal.push_back(std::move(*s));
            }
            out.sealOrigin = at(v, key);
            out.sealSet = true;
        } else {
            warn(std::format(
                "{} has unsupported key '{}' (ignored); the keys are {}", where, k, list_of(kLinkKeys)));
        }
    }
    return {};
}

std::expected<void, std::string>
Reader::layers(const toml::Value& v, std::string_view where,
               std::map<std::string, LayerEntry>& out) const {
    if (!v.is_table())
        return std::unexpected(std::format(
            "{} must be a table of <layer> = <package> | false, for example "
            "c-abi = \"openkal-musl@0.22.6\"", where));
    for (auto const& [k, val] : v.as_table()) {
        auto key = std::format("{}.{}", where, k);
        if (!is_layer(k))
            return std::unexpected(std::format(
                "{}: '{}' is not a target-side layer; the layers are {}", where, k, list_of(kLayers)));
        auto r = ref(val, key, /*allowOff=*/true, /*allowProgram=*/false);
        if (!r) return std::unexpected(r.error());
        out[k] = LayerEntry{ std::move(*r), at(val, key) };
    }
    return {};
}

std::expected<void, std::string>
Reader::tools(const toml::Value& v, std::string_view where,
              std::map<std::string, ToolEntry>& out) const {
    if (!v.is_table())
        return std::unexpected(std::format(
            "{} must be a table of <program> = <package> | {{ program = \"<name>\" }}", where));
    for (auto const& [k, val] : v.as_table()) {
        auto key = std::format("{}.{}", where, k);
        auto r = ref(val, key, /*allowOff=*/false, /*allowProgram=*/true);
        if (!r) return std::unexpected(r.error());
        out[k] = ToolEntry{ std::move(*r), at(val, key) };
    }
    return {};
}

std::expected<std::vector<std::string>, std::string>
Reader::host_pairs(const toml::Value& v, std::string_view where) const {
    auto w = strings(v, where);
    if (!w) return std::unexpected(w.error());
    for (auto const& p : *w)
        if (!mcpp::manifest::platform::is_host_pair(p))
            return std::unexpected(std::format(
                "{}: '{}' is not a system and an architecture; write \"<os>-<arch>\" with "
                "<os> one of linux, macos, windows, for example \"linux-x86_64\"", where, p));
    return w;
}

// `carrier` is one table or an array of them (design 2026-10-29 §4.5).
std::expected<std::vector<Carrier>, std::string>
Reader::carriers(const toml::Value& v, std::string_view where) const {
    std::vector<Carrier> out;
    if (v.is_table()) {
        auto c = carrier(v, where);
        if (!c) return std::unexpected(c.error());
        out.push_back(std::move(*c));
        return out;
    }
    if (!v.is_array())
        return std::unexpected(std::format(
            "{} must be a table {{ package, section, main }} or an array of them", where));
    std::size_t i = 0;
    for (auto const& e : v.as_array()) {
        auto c = carrier(e, std::format("{}[{}]", where, i++));
        if (!c) return std::unexpected(c.error());
        if (c->build != "row") {
            warn(std::format(
                "{}[{}]: build = \"{}\" is a carrier model this mcpp does not build (skipped); "
                "it builds \"row\"", where, i - 1, c->build));
            continue;
        }
        out.push_back(std::move(*c));
    }
    return out;
}

std::expected<Carrier, std::string> Reader::carrier(const toml::Value& v, std::string_view where) const {
    if (!v.is_table())
        return std::unexpected(std::format(
            "{} must be {{ package = \"<name>@<version>\", section = \"<name>\" }}", where));
    Carrier c;
    c.origin = at(v, std::string(where));
    c.set = true;
    for (auto const& [k, val] : v.as_table()) {
        auto key = std::format("{}.{}", where, k);
        if (k == "package") {
            auto r = ref(val, key, false, false);
            if (!r) return std::unexpected(r.error());
            c.package = std::move(*r);
        } else if (k == "section") {
            if (!val.is_string() || val.as_string().empty())
                return std::unexpected(std::format("{} must be a section name", key));
            c.section = val.as_string();
        } else if (k == "main") {
            if (!val.is_string() || val.as_string().empty())
                return std::unexpected(std::format("{} must be a source file of the package", key));
            c.main = absolute(val.as_string()).generic_string();
        } else if (k == "hosts") {
            auto h = host_pairs(val, key);
            if (!h) return std::unexpected(h.error());
            c.hosts = std::move(*h);
        } else if (k == "build") {
            if (!val.is_string() || val.as_string().empty())
                return std::unexpected(std::format("{} must name a carrier model, for example \"row\"", key));
            c.build = val.as_string();
        } else {
            warn(std::format(
                "{}: '{}' is not a key of a carrier (ignored); the keys are package, section, main, "
                "hosts, build", where, k));
        }
    }
    if (c.package.empty() || c.section.empty() || c.main.empty())
        return std::unexpected(std::format(
            "{} names `package`, `section` and `main` (the program's entry source)", where));
    return c;
}

std::expected<Statement, std::string> Reader::statement(const toml::Table& t) const {
    Statement s;
    for (auto const& [k, v] : t) {
        auto key = std::format("[platform].{}", k);
        if (k == "targets") {
            auto w = strings(v, key);
            if (!w) return std::unexpected(w.error());
            if (w->empty()) return std::unexpected(std::format("{} names no target", key));
            s.targets = Words{ std::move(*w), at(v, key), true };
        } else if (k == "toolchain") {
            if (!v.is_string() || v.as_string().empty())
                return std::unexpected(std::format(
                    "{} must be a toolchain, for example \"llvm@23.1.3\"", key));
            s.toolchain = Text{ v.as_string(), at(v, key), true };
        } else if (k == "sysroot") {
            if (v.is_string()) {
                s.sysroot = Text{ v.as_string(), at(v, key), true };
            } else if (v.is_table()) {
                std::string path;
                for (auto const& [sk, sv] : v.as_table()) {
                    if (sk == "path" && sv.is_string()) path = sv.as_string();
                    else if (sk == "provides") {
                        auto p = strings(sv, key + ".provides");
                        if (!p) return std::unexpected(p.error());
                        s.sysrootProvides = std::move(*p);
                    } else warn(std::format(
                        "{}: '{}' is not a key of a sysroot table (ignored); the keys are path, provides",
                        key, sk));
                }
                if (path.empty())
                    return std::unexpected(std::format("{} is a table without `path`", key));
                s.sysroot = Text{ "path:" + absolute(path).generic_string(), at(v, key), true };
            } else {
                return std::unexpected(std::format("{} must be a string or {{ path = \"<dir>\" }}", key));
            }
        } else if (k == "runner") {
            auto r = runner(v, key);
            if (!r) return std::unexpected(r.error());
            s.runner = std::move(*r);
        } else if (k == "runners") {
            if (!v.is_table())
                return std::unexpected(std::format("{} must be a table of <name> = [argv]", key));
            for (auto const& [rk, rv] : v.as_table()) {
                auto rkey = std::format("{}.{}", key, rk);
                auto w = strings(rv, rkey);
                if (!w) return std::unexpected(w.error());
                if (w->empty()) return std::unexpected(std::format("{} is empty", rkey));
                s.runners[rk] = Words{ std::move(*w), at(rv, rkey), true };
            }
        } else if (k == "carrier") {
            auto c = carriers(v, key);
            if (!c) return std::unexpected(c.error());
            s.carriers = std::move(*c);
        } else if (k == "descriptor_keys") {
            auto w = strings(v, key);
            if (!w) return std::unexpected(w.error());
            if (w->empty()) return std::unexpected(std::format("{} names no key", key));
            for (auto const& d : *w)
                if (std::ranges::find(kDescriptorKeys, std::string_view(d)) == std::end(kDescriptorKeys))
                    return std::unexpected(std::format(
                        "{}: '{}' is not a descriptor platform; the keys are {}", key, d,
                        list_of(kDescriptorKeys)));
            s.descriptorKeys = Words{ std::move(*w), at(v, key), true };
        } else if (k == "plugin") {
            // The platform's program is its package's build program, which a
            // package names `build.mcpp` (SPEC-007); no other file is run.
            if (!v.is_string() || v.as_string() != "build.mcpp")
                return std::unexpected(std::format(
                    "{} must be \"build.mcpp\": the platform's program is its package's build program", key));
            s.plugin = Text{ absolute(v.as_string()).generic_string(), at(v, key), true };
        } else if (k == "universal") {
            if (!v.is_bool())
                return std::unexpected(std::format("{} must be true or false", key));
            s.universal = Flag{ v.as_bool(), at(v, key), true };
        } else if (k == "layers") {
            if (auto r = layers(v, "[platform.layers]", s.layers); !r) return std::unexpected(r.error());
        } else if (k == "tools") {
            if (auto r = tools(v, "[platform.tools]", s.tools); !r) return std::unexpected(r.error());
        } else if (k == "build") {
            if (!v.is_table()) return std::unexpected(std::format("{} must be a table", key));
            s.buildOrigin = at(v, "[platform.build]");
            if (auto r = flags(v.as_table(), "[platform.build]", s.build, &s.buildCfg); !r)
                return std::unexpected(r.error());
        } else if (k == "link") {
            if (!v.is_table()) return std::unexpected(std::format("{} must be a table", key));
            if (auto r = link(v.as_table(), "[platform.link]", s.link); !r)
                return std::unexpected(r.error());
        } else if (k == "versions") {
            if (!v.is_table())
                return std::unexpected(std::format("{} must be a table of <package> = \"<version>\"", key));
            for (auto const& [vk, vv] : v.as_table()) {
                if (!vv.is_string() || vv.as_string().empty())
                    return std::unexpected(std::format("[platform.versions].{} must be a version", vk));
                s.versions[vk] = vv.as_string();
            }
            s.versionsOrigin = at(v, "[platform.versions]");
        } else if (k == "hosted") {
            if (!v.is_table()) return std::unexpected(std::format("{} must be a table", key));
            s.hosted.set = true;
            for (auto const& [hk, hv] : v.as_table()) {
                auto hkey = std::format("[platform.hosted].{}", hk);
                if (hk == "targets") {
                    auto w = strings(hv, hkey);
                    if (!w) return std::unexpected(w.error());
                    s.hosted.targets = Words{ std::move(*w), at(hv, hkey), true };
                } else if (hk == "toolchain") {
                    if (!hv.is_string() || hv.as_string().empty())
                        return std::unexpected(std::format("{} must be a toolchain", hkey));
                    s.hosted.toolchain = Text{ hv.as_string(), at(hv, hkey), true };
                } else if (hk == "layers") {
                    if (auto r = layers(hv, "[platform.hosted.layers]", s.hosted.layers); !r)
                        return std::unexpected(r.error());
                } else {
                    warn(std::format(
                        "[platform.hosted] has unsupported key '{}' (ignored); a host triple takes "
                        "the platform's {} only", hk, list_of(kHostedKeys)));
                }
            }
        } else {
            warn(std::format("[platform] has unsupported key '{}' (ignored). The keys are: {}.",
                             k, list_of(kPlatformKeys)));
        }
    }
    return s;
}

std::expected<Override, std::string>
Reader::override_(const toml::Table& t, std::string_view selector) const {
    Override o;
    o.selector = std::string(selector);
    const std::string where = selector.empty()
        ? std::string("[host]") : std::format("[target.{}]", selector);
    for (auto const& [k, v] : t) {
        auto key = std::format("{}.{}", where, k);
        if (k == "platform") {
            if (v.is_string() && v.as_string() == "none") {
                o.platformNone = true;
            } else {
                auto r = ref(v, key, false, false);
                if (!r) return std::unexpected(r.error());
                o.platform = std::move(*r);
            }
            o.platformOrigin = at(v, key);
            o.platformSet = true;
        } else if (selector.empty()) {
            // `[host]` selects the host graph's platform and nothing else.
            warn(std::format(
                "[host] has unsupported key '{}' (ignored); it holds `platform`, the host graph's platform",
                k));
        } else if (k == "layers") {
            if (auto r = layers(v, key, o.fields.layers); !r) return std::unexpected(r.error());
        } else if (k == "tools") {
            if (auto r = tools(v, key, o.fields.tools); !r) return std::unexpected(r.error());
        } else if (k == "carrier") {
            auto c = carriers(v, key);
            if (!c) return std::unexpected(c.error());
            o.fields.carriers = std::move(*c);
        } else if (k == "link") {
            if (!v.is_table()) return std::unexpected(std::format("{} must be a table", key));
            if (auto r = link(v.as_table(), key, o.fields.link); !r) return std::unexpected(r.error());
        } else if (k == "runner") {
            auto r = runner(v, key);
            if (!r) return std::unexpected(r.error());
            o.fields.runner = std::move(*r);
        } else if (k == "runners") {
            if (!v.is_table()) continue;   // the table's own reader refuses the shape
            for (auto const& [rk, rv] : v.as_table()) {
                auto rkey = std::format("{}.{}", key, rk);
                auto w = strings(rv, rkey);
                if (!w) continue;          // ditto
                o.fields.runners[rk] = Words{ std::move(*w), at(rv, rkey), true };
            }
        }
        // Every other key belongs to the table's other readers.
    }
    // `platform` decides what `cfg(platform = ...)` means, so it cannot be
    // stated under a selector that asks about it (§5.1).
    if (o.platformSet && selector.find("platform") != std::string_view::npos
        && selector.starts_with("cfg("))
        return std::unexpected(std::format(
            "{}.platform: a platform cannot be selected under a selector that asks which "
            "platform is selected; write it under a triple or a cfg() of os/arch/env", where));
    return o;
}

} // namespace mcpp::manifest::platform

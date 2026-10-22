// base.cpp -- the target base: which statement of `sysroot` applies, and the
// one resolution every reader reads (design 2026-10-23, "target base and
// execution host", §3.3 and §4.2). Declared in `:state`.
//
// FOUR TIERS, IN #755'S ORDER: an override (`--sysroot`, `MCPP_TARGET_<T>_
// SYSROOT`, `MCPP_SYSROOT`) outranks the project (`[target.<t>].sysroot`, then
// `[toolchain] { path, sysroot }` or a toolchain phase), which outranks this
// machine's defaults (`config.toml`, the user's then the system's), which
// outrank the engine's (the target row). The host graph reads none of them:
// a build program runs here, on the host's own base.

module mcpp.build.prepare;
import :state;

import std;
import mcpp.build.refusal;
import mcpp.config;
import mcpp.fetcher;
import mcpp.fetcher.progress;
import mcpp.manifest;
import mcpp.platform;
import mcpp.provider;
import mcpp.runtime.binding;
import mcpp.toolchain.base;
import mcpp.toolchain.base_probe;
import mcpp.toolchain.triple;
import mcpp.ui;

namespace mcpp::build {

namespace fs = std::filesystem;
namespace tb = mcpp::toolchain::base;

std::string target_env_name(const mcpp::toolchain::triple::Triple& t, std::string_view key) {
    std::string out = "MCPP_TARGET_";
    for (char c : t.str())
        out += (c >= 'a' && c <= 'z') ? char(c - 'a' + 'A')
             : ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ? c : '_';
    out += '_';
    out += key;
    return out;
}

namespace {

// An environment or command-line value: empty is unset, `none` is the empty
// form, a filesystem path is a path (relative to where mcpp was started), and
// anything else is the manifest's string grammar.
std::optional<std::expected<tb::Statement, std::string>>
override_value(std::string_view v) {
    if (v.empty()) return std::nullopt;
    if (v == "none") return tb::parse_text("");
    if (!v.starts_with("path:") && names_a_file_path(v)) {
        std::error_code ec;
        auto p = fs::absolute(fs::path(std::string(v)), ec).lexically_normal();
        return tb::parse_text("path:" + p.generic_string());
    }
    return tb::parse_text(v);
}

std::string shown_statement(const tb::Statement& s, const fs::path& relativeTo) {
    if (s.form == tb::Form::Empty) return "\"\" (no prebuilt C library)";
    if (s.form != tb::Form::Path) return s.text;
    if (!relativeTo.empty()) {
        std::error_code ec;
        auto rel = fs::relative(s.path, relativeTo, ec);
        if (!ec && !rel.empty() && !rel.generic_string().starts_with(".."))
            return rel.generic_string();
    }
    return s.path.generic_string();
}

SourceClass class_of(const tb::Statement& s) {
    if (s.originKind == "build-program") return SourceClass::Program;
    switch (s.form) {
        case tb::Form::Path:    return SourceClass::Custom;
        case tb::Form::Locator: return s.version == "system" ? SourceClass::Host : SourceClass::Pinned;
        case tb::Form::Package: return s.text.find('@') != std::string::npos ? SourceClass::Pinned
                                                                             : SourceClass::Managed;
        case tb::Form::Empty:   return SourceClass::Custom;
        case tb::Form::Default: return SourceClass::Managed;
    }
    return SourceClass::Managed;
}

std::string origin_text(const tb::Statement& s) {
    if (s.originKind == "env") return s.originKey;
    if (s.originFile.empty()) return s.originKey;
    return s.originLine > 0 ? std::format("{}:{}", s.originFile, s.originLine) : s.originFile;
}

// A sysroot the project names and that is not there (design §3.8): where the
// statement is, what it resolved to, the three ways a team makes it exist,
// and how to use another one on this machine only.
std::string missing_path_message(const mcpp::toolchain::triple::Triple& t, const tb::Statement& s) {
    return std::format(
        "{}: [target.{}].sysroot = {{ path = ... }}\n"
        "       resolves to {}, which does not exist.\n"
        "       A sysroot a project names is the project's to provide. Common ways:\n"
        "         · publish it as a package in your index:  sysroot = \"acme:sysroot@1.0\"\n"
        "         · keep it in the repository (submodule/LFS) at that relative path\n"
        "         · agree on a path for the team (e.g. /opt/acme/sysroot)\n"
        "       To use another one on this machine only: {}=path:<dir>, or\n"
        "       [target.{}] sysroot = {{ path = \"<dir>\" }} in ~/.mcpp/config.toml",
        origin_text(s), t.str(), s.path.string(), target_env_name(t, "SYSROOT"), t.str());
}

} // namespace

std::expected<tb::Statement, std::string>
base_statement(PrepareState& state, const mcpp::toolchain::triple::Triple& t) {
    const auto key = t.str();
    if (state.baseStatement && state.baseStatementFor == key) return *state.baseStatement;
    auto settle = [&](std::expected<tb::Statement, std::string> s)
        -> std::expected<tb::Statement, std::string> {
        if (!s) return s;
        *s = tb::normalise(t, std::move(*s));
        if (auto why = tb::refusal(t, *s)) {
            refusal::record(refusal::Code::TargetBase);
            return std::unexpected(std::format("{} (stated in {})", *why, origin_text(*s)));
        }
        state.baseStatement = *s;
        state.baseStatementFor = key;
        return s;
    };
    auto stamped = [&](std::expected<tb::Statement, std::string> s, std::string kind,
                       std::string file, int line, std::string originKey)
        -> std::expected<tb::Statement, std::string> {
        if (!s) return std::unexpected(std::format("{}{}: {}", file.empty() ? originKey : file,
                                                   line > 0 ? std::format(":{}", line) : "", s.error()));
        s->originKind = std::move(kind);
        s->originFile = std::move(file);
        s->originLine = line;
        s->originKey  = std::move(originKey);
        return s;
    };
    // The host graph never reads the target's base (§3.6).
    if (state.overrides.host_target || state.overrides.tool_depth > 0)
        return settle(tb::Statement{});

    // 1. Overrides (`--sysroot` is `MCPP_SYSROOT`).
    for (auto name : {target_env_name(t, "SYSROOT"), std::string("MCPP_SYSROOT")}) {
        const char* e = std::getenv(name.c_str());
        if (auto v = override_value(e ? std::string_view(e) : std::string_view{}))
            return settle(stamped(std::move(*v), "env", {}, 0, name));
    }
    // 2. The project: the target row, then the toolchain's own statement.
    if (auto* e = find_target_entry(*state.m, t); e && e->sysrootDeclared) {
        auto s = stamped(tb::parse_text(e->sysroot), "manifest", state.m->sourcePath.string(),
                         e->sysrootLine, std::format("[target.{}].sysroot", key));
        if (s) s->provides = e->sysrootProvides;
        return settle(std::move(s));
    }
    if (state.localToolchain && !state.localToolchain->sysroot.empty()) {
        auto const& o = state.localToolchainOrigin;
        return settle(stamped(tb::parse_text("path:" + state.localToolchain->sysroot),
                              o.originKind == "build-program" ? "build-program" : "toolchain",
                              o.originFile, o.originLine, "[toolchain] sysroot"));
    }
    if (!state.programSysroot.empty()) {
        auto const& o = state.programSysrootOrigin;
        auto text = state.programSysroot;
        auto parsed = names_a_file_path(text) && !text.starts_with("path:")
            ? tb::parse_text("path:" + fs::path(text).lexically_normal().generic_string())
            : tb::parse_text(text);
        return settle(stamped(std::move(parsed), "build-program", o.originFile, o.originLine,
                              "mcpp:toolchain=sysroot"));
    }
    // 3. This machine's defaults.
    if (auto cfg = state.get_cfg(false); cfg) {
        for (auto const& [k, d] : (*cfg)->targetDefaults) {
            if (!d.sysrootDeclared) continue;
            auto kt = mcpp::toolchain::triple::parse(k);
            if (!(kt ? kt->str() == key : k == key)) continue;
            auto s = stamped(tb::parse_text(d.sysroot), "config", d.sysrootFile.string(),
                             d.sysrootLine, std::format("[target.{}].sysroot", key));
            if (s) s->provides = d.sysrootProvides;
            return settle(std::move(s));
        }
    }
    // 4. The engine's: the row, read where it always was (`effective_sysroot`).
    return settle(tb::Statement{});
}

const std::string* base_statement_text(PrepareState& state, const mcpp::toolchain::triple::Triple& t) {
    auto s = base_statement(state, t);
    if (!s || !s->stated()) return nullptr;
    // A `std::string` that outlives the call: the memoised statement's.
    return &state.baseStatement->text;
}

namespace {

// The driver and the flags that choose its target, without any sysroot: what
// a measurement asks.
std::vector<std::string> driver_base(const mcpp::toolchain::Toolchain& tc) {
    std::vector<std::string> v{tc.binaryPath.string()};
    if (mcpp::toolchain::is_clang(tc)) v.push_back("--no-default-config");
    if (!tc.crossTargetFlag.empty()) v.push_back(tc.crossTargetFlag);
    return v;
}

bool provides_c_abi(const tb::Statement& s, std::string* impl) {
    for (auto const& p : s.provides) {
        constexpr std::string_view k = "mcpp:c-abi=";
        if (p.starts_with(k)) { if (impl) *impl = p.substr(k.size()); return true; }
    }
    return false;
}

// The directory a package statement names, installed through the channel a
// row's toolchain pin uses (and `MCPP_NO_AUTO_INSTALL` / offline are the
// fetcher's to decide, as they are there).
std::expected<fs::path, std::string>
package_dir(PrepareState& state, const std::string& ref) {
    auto cfg = state.get_cfg(true);
    if (!cfg) return std::unexpected(cfg.error());
    mcpp::fetcher::Fetcher f(**cfg);
    mcpp::fetcher::InstallProgressHandler progress;
    if (auto p = f.resolve_xpkg_path(ref, mcpp::fetcher::PayloadIntent::Use, &progress))
        return p->root;
    auto xl = mcpp::config::make_xlings_env(**cfg);
    if (auto d = mcpp::provider::payload(*mcpp::provider::open(xl), mcpp::provider::parse_ref(ref)))
        return *d;
    return std::unexpected(std::format("the package {} could not be installed", ref));
}

} // namespace

// THE ONE RESOLUTION (§4.2 steps 5-7) for the rows whose base the engine
// renders itself: a gnu-sysroot (Linux), and the identity and report of every
// other stated base. The Apple, MSVC and multilib rows locate theirs where
// they always did, now from the same statement; this records what they found.
std::expected<void, std::string> step2_resolve_base(PrepareState& state) {
    if (!state.tc) return {};
    auto& tc = *state.tc;
    auto tt = mcpp::toolchain::triple::parse(tc.targetTriple);
    if (!tt) return {};
    const auto row = tb::row_base(*tt);
    tc.base.address = row.address;
    auto stmt = base_statement(state, *tt);
    if (!stmt) return std::unexpected(stmt.error());
    if (!stmt->stated()) return {};
    tc.base.form = stmt->form;
    tc.base.text = stmt->text;
    // A NAMED SubOS AND A STATED BASE ANSWER DIFFERENT QUESTIONS (§3.3): the
    // SubOS supplies what the compiler and the build's tools run on, the base
    // what the artifact is built against. Said once, so neither is mistaken
    // for having been ignored.
    if (state.runtimeBindingSnapshot.selection.mode
            == mcpp::provider::runtime::RuntimeSelection::Mode::NamedSubos
        && row.address == tb::Address::GnuSysroot)
        mcpp::ui::info("note", std::format(
            "[xlings] subos '{}' supplies the host's tools; the artifact is built against "
            "the stated sysroot {}", state.runtimeBindingSnapshot.selection.subosName,
            stmt->text));

    const auto root = state.root ? *state.root : fs::path{};
    SourceDecision d;
    d.subject    = "sysroot";
    d.value      = shown_statement(*stmt, root);
    d.cls        = class_of(*stmt);
    d.originKind = stmt->originKind;
    d.originFile = stmt->originFile;
    d.originLine = stmt->originLine;
    d.originKey  = stmt->originKey;

    // The identity a non-rendered base contributes: its statement and where it
    // was found. The Linux branch below replaces it with a measured one.
    auto record_identity = [&](std::string measured, std::vector<fs::path> evidence,
                               std::vector<fs::path> hostDirs = {}) {
        tc.base.identity = tb::identity_of(tc.base.text, tc.base.root, evidence, measured);
        auto target = state.targetRuntimeBinding.value_or(state.runtimeBindingSnapshot);
        // The host's base loads through the host's loader, which searches the
        // host's directories itself: the binding names no private loader, so
        // closure validation models those directories.
        const bool hostBase = !hostDirs.empty();
        mcpp::platform::runtime::bind_target_base(target, {
            .runtimeId = tc.base.runtime == tb::Runtime::Binding && tc.base.libc == "glibc"
                             && !tc.base.libcVersion.empty()
                           ? std::format("glibc@{}", tc.base.libcVersion) : std::string{},
            .loader = tc.base.loader,
            .libraryDirs = hostBase ? hostDirs : tc.base.runtimeDirs,
            .identity = tc.base.identity,
            .binding = tc.base.runtime == tb::Runtime::Binding,
            .host = hostBase,
        });
        state.targetRuntimeBinding = std::move(target);
        tc.runtimeContractHash = state.targetRuntimeBinding->contractHash;
    };

    if (row.address == tb::Address::AppleIsysroot) {
        tc.base.runtime = tb::Runtime::Floor;
        const bool cross = !tc.base.apple.root.empty();
        tc.base.root = cross ? tc.base.apple.root : state.appleSdkLocated.value_or(fs::path{});
        tc.base.libc = "libSystem";
        d.detail = std::format("{} {}", tc.base.apple.name.empty() ? std::string(row.family) : tc.base.apple.name,
                               tc.base.apple.version);
        record_identity(tc.base.apple.version, {tc.base.root / "SDKSettings.json"});
        record_source(state, std::move(d));
        return {};
    }
    if (row.address == tb::Address::MsvcRoots) {
        tc.base.runtime = tb::Runtime::Floor;
        tc.base.root = tc.base.msvc.toolsDir;
        d.detail = std::format("msvc {}", tc.base.msvc.toolsVersion);
        record_identity(tc.base.msvc.toolsVersion + " " + tc.base.msvc.sdkVersion,
                        {tc.base.msvc.toolsDir, tc.base.msvc.sdkRoot});
        record_source(state, std::move(d));
        return {};
    }
    if (row.address == tb::Address::Multilib) {
        tc.base.runtime = tb::Runtime::None;
        tc.base.root = tc.base.multilibRoot;
        d.detail = tc.base.package;
        record_identity(tc.base.package, {tc.base.multilibInclude, tc.base.multilibLib});
        record_source(state, std::move(d));
        return {};
    }
    if (row.address != tb::Address::GnuSysroot || stmt->form == tb::Form::Empty) {
        // The empty form, and a package on a row whose SDK resolves its own
        // system: accepted as they always were, and stated.
        record_source(state, std::move(d));
        return {};
    }

    // ── gnu-sysroot ────────────────────────────────────────────────────────
    fs::path dir;
    std::error_code ec;
    if (stmt->form == tb::Form::Locator) {
        dir = "/";
    } else if (stmt->form == tb::Form::Path) {
        dir = stmt->path;
        if (!fs::is_directory(dir, ec)) {
            refusal::record(refusal::Code::TargetBase);
            return std::unexpected(missing_path_message(*tt, *stmt));
        }
    } else {
        auto p = package_dir(state, stmt->text);
        if (!p) return std::unexpected(std::format("[target.{}].sysroot = \"{}\": {}",
                                                   tt->str(), stmt->text, p.error()));
        dir = *p;
    }
    const auto ecosystemLinux = tc.base.payload ? tc.base.payload->linuxInclude : fs::path{};
    const bool native = tt->os == "linux" && tt->arch == mcpp::platform::host_arch
                        && mcpp::platform::is_linux;
    const bool staticLink = state.m->buildConfig.linkage == "static";
    const auto probeDir = state.workRoot / "target" / ".mcpp-base";

    tc.base.libc.clear();
    tc.base.libcVersion.clear();
    std::vector<fs::path> evidence;
    std::vector<fs::path> hostLibcDirs;
    std::string impl;
    if (tb::is_payload_layout(dir)) {
        // The ecosystem's own layout: a glibc package (`xim:glibc@2.39`). It
        // is linked as the default base is, from its own directories.
        tc.base.payload = tb::payload_layout_paths(dir, ecosystemLinux);
        tc.base.root.clear();
        auto v = driver_base(tc);
        v.push_back("-isystem" + tc.base.payload->glibcInclude.string());
        fs::create_directories(probeDir, ec);
        { std::ofstream os(probeDir / "mcpp-base-probe.c"); os << "#include <stdio.h>\n"; }
        v.insert(v.end(), {"-E", "-dM", "-x", "c", (probeDir / "mcpp-base-probe.c").string()});
        auto r = mcpp::platform::process::capture_stdout(v);
        if (r.exit_code == 0)
            if (auto f = tb::classify_macros(tb::parse_defines(r.output))) {
                tc.base.libc = f->impl;
                tc.base.libcVersion = f->version;
            }
        tc.base.loader = mcpp::toolchain::resolve_loader(tc.base.payload->glibcLib, tt->str());
        tc.base.runtimeDirs = {tc.base.payload->glibcLib};
        evidence = {tc.base.payload->glibcInclude / "features.h", tc.base.loader,
                    tc.base.payload->glibcLib / "libc.so.6"};
    } else {
        auto m = tb::measure_gnu(driver_base(tc), *tt, dir,
                                 mcpp::toolchain::is_clang(tc), probeDir);
        if (!m.cAbiFound && !provides_c_abi(*stmt, &impl)) {
            refusal::record(refusal::Code::TargetBase);
            return std::unexpected(std::format(
                "[target.{}].sysroot = {} ({}): {} supplies no C library the compiler can find "
                "(no crt1.o), so it is a library prefix rather than a sysroot.\n"
                "       A library prefix is a dependency; a sysroot supplies the C library. If this "
                "directory does supply one in a layout the driver does not know, say which:\n"
                "         sysroot = {{ path = \"{}\", provides = [\"mcpp:c-abi=<impl>\"] }}",
                tt->str(), d.value, origin_text(*stmt), dir.string(), dir.generic_string()));
        }
        tc.base.payload.reset();
        tc.base.root = dir;
        tc.base.multiarch = m.multiarch;
        tc.base.multiarchTuple = m.tuple;
        if (m.fact) { tc.base.libc = m.fact->impl; tc.base.libcVersion = m.fact->version; }
        tc.base.loader = m.loader;
        tc.base.runtimeDirs = m.runtimeDirs;
        evidence = m.evidence;
        // The host's own root binds through the standard loader path and the
        // loader's own search: nothing to name in RUNPATH, and nothing private
        // about the loader. The libc directory is kept for closure validation.
        if (stmt->form == tb::Form::Locator) {
            hostLibcDirs = tc.base.runtimeDirs;
            tc.base.runtimeDirs.clear();
            if (auto std = mcpp::toolchain::distro_loader_path(tt->str());
                !std.empty() && fs::exists(std, ec))
                tc.base.loader = std;
        }
        // Kernel headers are additive (§2.4): the engine supplies the
        // ecosystem's for a native base that lacks them.
        if (native && !ecosystemLinux.empty()
            && !fs::exists(dir / "usr" / "include" / "linux" / "limits.h", ec))
            tc.base.addedIncludes.push_back(ecosystemLinux);
    }
    // A declaration checks a measurement and stands in for one that cannot be
    // made (§3.4).
    if (std::string declared; provides_c_abi(*stmt, &declared)) {
        if (!tc.base.libc.empty() && declared != tc.base.libc) {
            refusal::record(refusal::Code::TargetBase);
            return std::unexpected(std::format(
                "[target.{}].sysroot ({}) provides mcpp:c-abi={}, and the C library measured there "
                "is {} {}", tt->str(), origin_text(*stmt), declared, tc.base.libc, tc.base.libcVersion));
        }
        if (tc.base.libc.empty()) tc.base.libc = declared;
    }
    const auto rowLibc = std::string(row.family);
    if (!rowLibc.empty() && !tc.base.libc.empty() && tc.base.libc != rowLibc
        && (tc.base.libc == "glibc" || tc.base.libc == "musl")) {
        refusal::record(refusal::Code::TargetBase);
        return std::unexpected(std::format(
            "[target.{}].sysroot ({}) supplies {} {}; a {} row is built on {}. Name the row that "
            "matches the C library (e.g. --target {}-linux-{}).",
            tt->str(), origin_text(*stmt), tc.base.libc, tc.base.libcVersion, tt->str(), rowLibc,
            tt->arch, tc.base.libc == "glibc" ? "gnu" : "musl"));
    }
    tc.base.runtime = !native || staticLink ? (native ? tb::Runtime::None : tb::Runtime::Floor)
                                            : tb::Runtime::Binding;
    if (tc.base.runtime == tb::Runtime::Binding && tc.base.loader.empty()) {
        refusal::record(refusal::Code::TargetBase);
        return std::unexpected(std::format(
            "[target.{}].sysroot = {} ({}): no dynamic loader ({}) was found in {}, and a "
            "native dynamic artifact loads its C library through the sysroot's own loader.\n"
            "       Link statically ([target.{}] linkage = \"static\"), or use a sysroot that "
            "carries its loader.",
            tt->str(), d.value, origin_text(*stmt), mcpp::toolchain::loader_filename(tt->str()),
            dir.string(), tt->str()));
    }

    // THE LAYERS BELOW MUST FIT (§4.8): the compiler's own runtime requires a
    // C library version, and a base older than that cannot serve it. Refused
    // here, naming both, rather than inside `std.cc` (scenario I).
    if (tc.base.libc == "glibc" && !tc.base.libcVersion.empty()) {
        auto floor = tb::compiler_runtime_glibc_floor(driver_base(tc));
        if (!floor.version.empty() && tb::compare_versions(floor.version, tc.base.libcVersion) > 0) {
            refusal::record(refusal::Code::TargetBase);
            return std::unexpected(std::format(
                "[target.{}].sysroot ({}): {} {}'s runtime requires glibc >= {}\n"
                "       ({}: GLIBC_{}), and this sysroot provides glibc {}.\n"
                "       Use a compiler built against glibc {} or older, or a newer sysroot.",
                tt->str(), origin_text(*stmt), tc.compiler_name(), tc.version, floor.version,
                floor.from.filename().string(), floor.version, tc.base.libcVersion,
                tc.base.libcVersion));
        }
    }

    d.detail = tc.base.libcVersion.empty() ? tc.base.libc
                                           : std::format("{} {}", tc.base.libc, tc.base.libcVersion);
    record_identity(std::format("{} {} {} {}", tc.base.libc, tc.base.libcVersion,
                                tc.base.multiarch ? tc.base.multiarchTuple : "",
                                tb::runtime_name(tc.base.runtime)),
                    std::move(evidence), std::move(hostLibcDirs));
    record_source(state, std::move(d));
    return {};
}

// THE EXECUTION HOST'S WRAPPER (§3.7): the same tiers as the base, minus the
// engine's (an execution host is never a default). In the environment, empty
// is unset and `none` clears what the project or the machine said.
std::expected<void, std::string> step2_resolve_run_under(PrepareState& state) {
    state.runUnder.clear();
    if (!state.tc || state.overrides.host_target || state.overrides.tool_depth > 0) return {};
    auto tt = mcpp::toolchain::triple::parse(state.tc->targetTriple);
    if (!tt) return {};
    const auto key = tt->str();
    SourceDecision d;
    d.subject = "run_under";
    d.cls = SourceClass::Custom;
    d.announce = false;   // announced when something is run through it
    auto decide = [&](std::vector<std::string> argv) {
        state.runUnder = std::move(argv);
        if (state.runUnder.empty()) return;
        std::string shown;
        for (auto const& a : state.runUnder) shown += (shown.empty() ? "" : " ") + a;
        d.value = std::move(shown);
        state.runUnderOrigin = d;
        record_source(state, std::move(d));
    };
    const auto envName = target_env_name(*tt, "RUN_UNDER");
    if (const char* e = std::getenv(envName.c_str()); e && *e) {
        std::string_view v(e);
        if (v == "none") return {};
        std::vector<std::string> argv;
        std::string cur;
        for (char c : v) {
            if (c == ' ' || c == '\t') { if (!cur.empty()) argv.push_back(std::exchange(cur, {})); }
            else cur += c;
        }
        if (!cur.empty()) argv.push_back(cur);
        d.originKind = "env";
        d.originKey = envName;
        decide(std::move(argv));
        return {};
    }
    if (auto* e = find_target_entry(*state.m, *tt); e && !e->runUnder.empty()) {
        d.originKind = "manifest";
        d.originFile = state.m->sourcePath.string();
        d.originLine = e->runUnderLine;
        decide(e->runUnder);
        return {};
    }
    if (auto cfg = state.get_cfg(false); cfg) {
        for (auto const& [k, td] : (*cfg)->targetDefaults) {
            if (td.runUnder.empty()) continue;
            auto kt = mcpp::toolchain::triple::parse(k);
            if (!(kt ? kt->str() == key : k == key)) continue;
            d.originKind = "config";
            d.originFile = td.runUnderFile.string();
            d.originLine = td.runUnderLine;
            decide(td.runUnder);
            return {};
        }
    }
    return {};
}

} // namespace mcpp::build

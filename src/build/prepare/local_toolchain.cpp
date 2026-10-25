// local_toolchain.cpp -- a toolchain named by path, the toolchain that runs
// build programs when it differs, and the root build program's toolchain
// phase (mcpp#755). Declared in `:state`.
//
// A TOOLCHAIN NAMED BY PATH IS NOT A PATH COMPILER. `[toolchain] = "system"`
// stays refused: it is whatever PATH happens to hold. A `[toolchain]` table
// names a tree, and mcpp probes the drivers in it, identifies them (version,
// triple, standard library, `import std`), drives them with its own link
// model, and records the source on every line that reports it -- the shape
// `msvc@system` already has, generalised to gcc and llvm.

module;
#include <cstdlib>

module mcpp.build.prepare;
import :state;

import std;
import mcpp.build.refusal;
import mcpp.build.build_program;
import mcpp.config;
import mcpp.home;
import mcpp.manifest;
import mcpp.platform;
import mcpp.toolchain.registry;
import mcpp.toolchain.triple;
import mcpp.toolchain.post_install;
import mcpp.ui;

namespace mcpp::build {

namespace fs = std::filesystem;

namespace {

// The statement a toolchain phase made, carried to the second pass.
struct ToolchainRestart {
    std::vector<std::string> statement;
    std::string              bootstrap;
};
thread_local std::optional<ToolchainRestart> g_restart;

fs::path absolute_against(const fs::path& base, const std::string& p) {
    if (p.empty()) return {};
    fs::path v(p);
    if (v.is_relative()) v = base / v;
    return v.lexically_normal();
}

// A description read from `key=value` lines: the build program's statement.
std::expected<mcpp::manifest::LocalToolchain, std::string>
read_statement(const std::vector<std::string>& lines, std::string& spec,
               std::string& originFile, int& originLine) {
    mcpp::manifest::LocalToolchain lt;
    for (auto const& line : lines) {
        const auto eq = line.find('=');
        if (eq == std::string::npos || eq == 0)
            return std::unexpected(std::format("`mcpp:toolchain={}` is not `<key>=<value>`", line));
        const auto key = line.substr(0, eq);
        const auto val = line.substr(eq + 1);
        if      (key == "spec")     spec = val;
        else if (key == "path")     lt.path = val;
        else if (key == "prefix")   lt.prefix = val;
        else if (key == "sysroot")  lt.sysroot = val;
        else if (key == "launcher") lt.launcher = val;
        else if (key == "family") {
            if (val != "gcc" && val != "llvm")
                return std::unexpected(std::format("family = '{}': a toolchain is \"gcc\" or \"llvm\"", val));
            lt.family = val;
        } else if (key.starts_with("tool.")) {
            lt.tools.emplace_back(key.substr(5), val);
        } else if (key == "origin") {
            const auto colon = val.rfind(':');
            originFile = colon == std::string::npos ? val : val.substr(0, colon);
            if (colon != std::string::npos) originLine = std::atoi(val.c_str() + colon + 1);
        } else {
            return std::unexpected(std::format(
                "`mcpp:toolchain={}`: '{}' is not a key; the keys are spec, path, prefix, "
                "sysroot, family, launcher, tool.<role> and origin", line, key));
        }
    }
    if (spec.empty() && lt.path.empty())
        return std::unexpected(std::string(
            "the toolchain phase stated neither `spec` (a managed toolchain) nor `path`"));
    if (!spec.empty() && !lt.path.empty())
        return std::unexpected(std::string(
            "the toolchain phase stated both `spec` and `path`; a build toolchain is one of them"));
    // A MANAGED TOOLCHAIN HAS NO TREE TO DESCRIBE. `prefix`, `launcher`,
    // `tool.<role>` and `family` describe a toolchain named by path, and
    // stated beside `spec` they were dropped without a word. `sysroot` is the
    // exception: it is the TARGET's base, which a program may state for a
    // managed compiler too (design 2026-10-23 §3.5).
    if (!spec.empty()) {
        std::vector<std::string> pathOnly;
        if (!lt.prefix.empty())   pathOnly.push_back("prefix");
        if (!lt.launcher.empty()) pathOnly.push_back("launcher");
        if (!lt.family.empty())   pathOnly.push_back("family");
        for (auto const& [role, _] : lt.tools) pathOnly.push_back("tool." + role);
        if (!pathOnly.empty()) {
            std::string names;
            for (auto const& n : pathOnly) names += (names.empty() ? "" : ", ") + n;
            return std::unexpected(std::format(
                "the toolchain phase stated `spec` with {}; those describe a toolchain named by "
                "`path`, and a managed toolchain ({}) has no tree for them to describe. State "
                "`path` instead, or drop them (`sysroot` may accompany either)", names, spec));
        }
    }
    return lt;
}

// What makes a toolchain named by path a different toolchain without the
// version changing: a rebuilt trunk driver, a replaced linker. The path, size
// and modification time of each program, not their bytes -- a driver is
// hundreds of megabytes and this is read on every prepare.
std::string local_identity(const mcpp::toolchain::Toolchain& tc) {
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&](std::string_view s) {
        for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
        h ^= 0x1f; h *= 1099511628211ull;
    };
    auto stamp = [&](const fs::path& p) {
        std::error_code ec;
        mix(p.generic_string());
        mix(std::format("{}", static_cast<std::uint64_t>(fs::file_size(p, ec))));
        auto t = fs::last_write_time(p, ec);
        mix(std::format("{}", static_cast<std::int64_t>(t.time_since_epoch().count())));
    };
    stamp(tc.binaryPath);
    for (auto const& [role, p] : tc.toolOverrides) { mix(role); stamp(p); }
    mix(tc.toolPrefix);
    return std::format("{:016x}", h);
}

} // namespace

std::optional<std::pair<std::vector<std::string>, std::string>> take_toolchain_restart() {
    if (!g_restart) return std::nullopt;
    auto r = std::move(*g_restart);
    g_restart.reset();
    return std::pair{std::move(r.statement), std::move(r.bootstrap)};
}

std::expected<void, std::string> step1_local_toolchain(PrepareState& state) {
    // THE SECOND PASS OF A TOOLCHAIN PHASE: the build program stated it.
    if (!state.overrides.toolchain_statement.empty()) {
        std::string spec, file;
        int line = 0;
        auto lt = read_statement(state.overrides.toolchain_statement, spec, file, line);
        if (!lt) {
            refusal::record(refusal::Code::LocalToolchain);
            return std::unexpected(std::format("the root build program's toolchain phase: {}", lt.error()));
        }
        state.tcOrigin = TcOrigin::BuildProgram;
        state.bootstrapSpec = state.overrides.bootstrap_spec;
        SourceDecision d;
        d.subject    = "toolchain.build";
        d.originKind = "build-program";
        d.originFile = file.empty() ? (*state.root / "build.mcpp").string() : file;
        d.originLine = line;
        if (!spec.empty()) {
            state.tcSpec = spec;
            d.cls = SourceClass::Pinned;
            d.value = spec;
            if (!lt->sysroot.empty()) {
                // The program stated the target's base beside a managed
                // compiler: a path relative to the project, or a spelling.
                auto text = lt->sysroot;
                if (names_a_file_path(text) && !text.starts_with("path:"))
                    text = absolute_against(*state.root, text).generic_string();
                state.programSysroot = std::move(text);
                state.programSysrootOrigin = d;
                state.programSysrootOrigin.subject = "sysroot";
            }
            state.localToolchainOrigin = std::move(d);
            return {};
        }
        const auto base = *state.root;
        lt->path    = absolute_against(base, lt->path).generic_string();
        lt->sysroot = absolute_against(base, lt->sysroot).generic_string();
        for (auto& [role, p] : lt->tools) p = absolute_against(base, p).generic_string();
        state.tcSpec = "path:" + lt->path;
        state.localToolchain = std::move(*lt);
        d.cls = SourceClass::Program;
        state.localToolchainOrigin = std::move(d);
        return {};
    }
    if (!state.tcSpec) {
        if (!state.m->toolchain.bootstrap.empty()) state.bootstrapSpec = state.m->toolchain.bootstrap;
        return {};
    }
    // THE FIRST PASS OF A TOOLCHAIN PHASE: the bootstrap toolchain builds
    // until the root build program has stated the build toolchain.
    if (state.tcSpec->starts_with("configure:")) {
        state.toolchainConfigure = true;
        if (!state.m->toolchain.bootstrap.empty()) {
            state.tcSpec   = state.m->toolchain.bootstrap;
            state.tcOrigin = TcOrigin::ManifestToolchain;
        } else if (auto cfg = state.get_cfg(true); cfg && !(*cfg)->defaultToolchain.empty()) {
            state.tcSpec   = (*cfg)->defaultToolchain;
            state.tcOrigin = TcOrigin::GlobalDefault;
        } else {
            state.tcSpec.reset();
            state.tcOrigin = TcOrigin::None;
        }
        return {};
    }
    if (!state.m->toolchain.bootstrap.empty()) state.bootstrapSpec = state.m->toolchain.bootstrap;
    if (!state.tcSpec->starts_with("path:")) return {};

    // A toolchain named by path: by the manifest's table, by
    // `MCPP_TOOLCHAIN=path:<dir>`, or handed to a host-tool sub-build.
    const mcpp::manifest::LocalToolchain* table =
        (state.tcFromCommandLine || state.tcFromConsumer) ? nullptr
            : state.m->toolchain.local_for(kCurrentPlatform);
    mcpp::manifest::LocalToolchain lt = table ? *table : mcpp::manifest::LocalToolchain{};
    if (!table) lt.path = state.tcSpec->substr(5);
    const auto base = (state.tcFromCommandLine || state.tcFromConsumer)
        ? fs::current_path() : state.m->sourcePath.parent_path();
    lt.path    = absolute_against(base, lt.path).generic_string();
    lt.sysroot = absolute_against(base, lt.sysroot).generic_string();
    for (auto& [role, p] : lt.tools) p = absolute_against(base, p).generic_string();
    state.tcSpec = "path:" + lt.path;
    SourceDecision d;
    d.subject = "toolchain.build";
    d.cls     = SourceClass::Custom;
    if (state.tcFromCommandLine) {
        d.originKind = "env";
        d.originKey  = "MCPP_TOOLCHAIN";
    } else if (state.tcFromConsumer) {
        d.originKind = "graph";
        d.originKey  = state.overrides.tool_chain;
    } else {
        d.originKind = "manifest";
        d.originFile = state.m->sourcePath.string();
        d.originLine = state.m->toolchain.line_for(kCurrentPlatform);
        d.originKey  = "[toolchain]";
    }
    state.localToolchain = std::move(lt);
    state.localToolchainOrigin = std::move(d);
    return {};
}

std::expected<void, std::string>
step2_use_local_toolchain(PrepareState& state, const mcpp::toolchain::ToolchainSpec& spec) {
    if (!state.localToolchain) {
        state.localToolchain = mcpp::manifest::LocalToolchain{};
        state.localToolchain->path = spec.localRoot.generic_string();
    }
    auto& lt = *state.localToolchain;
    const fs::path root(lt.path);
    std::error_code ec;
    auto refuse = [&](std::string why) -> std::unexpected<std::string> {
        refusal::record(refusal::Code::LocalToolchain);
        return std::unexpected(std::format(
            "the toolchain named by path '{}' cannot be used: {}", root.generic_string(), why));
    };
    if (!fs::is_directory(root, ec)) return refuse("the directory does not exist");
    std::string family = lt.family;
    if (family.empty())
        family = spec.family == mcpp::toolchain::Family::Llvm ? "llvm" : "gcc";
    fs::path driver;
    for (auto const& [role, p] : lt.tools) if (role == "cxx") driver = p;
    if (driver.empty())
        driver = root / "bin" / (lt.prefix + (family == "llvm" ? "clang++" : "g++")
                                 + std::string(mcpp::platform::exe_suffix));
    if (!fs::exists(driver, ec))
        return refuse(std::format("no C++ driver at '{}'; a {} toolchain keeps it in "
                                  "`<path>/bin/{}{}`, or names it with `tools = {{ cxx = ... }}`",
                                  driver.generic_string(), family, lt.prefix,
                                  family == "llvm" ? "clang++" : "g++"));
    for (auto const& [role, p] : lt.tools)
        if (!fs::exists(p, ec))
            return refuse(std::format("tools.{} names '{}', which does not exist", role, p));
    // A STATED LINKER REACHES THE LINK THROUGH CLANG'S `--ld-path`, which gcc
    // does not have: gcc selects a linker by the name `ld` inside a `-B`
    // directory, so a program named anything else cannot be chosen that way.
    // Refused here rather than ignored in the link: the alternative is a stated
    // tool that enters the fingerprint and takes no part in the build, which is
    // what this mechanism exists to prevent.
    if (family == "gcc")
        for (auto const& [role, p] : lt.tools)
            if (role == "ld")
                return refuse(std::format(
                    "tools.ld names '{}', and this is a gcc toolchain: a linker is stated "
                    "through clang's `--ld-path`, which gcc has no counterpart for. Either "
                    "name an llvm toolchain, or place a program called `ld` in the tree "
                    "gcc searches", p));
    state.explicit_compiler = driver;
    // THE C LIBRARY A NATIVE BUILD LINKS IS THE ECOSYSTEM'S, AS FOR A MANAGED
    // TOOLCHAIN: the binding's payload, linked by mcpp's own link model. A
    // toolchain that states its sysroot, a target whose base is stated
    // anywhere else, or a prefixed (cross) toolchain that carries one, links
    // that instead -- and needs no payload of the ecosystem's.
    const auto tt = state.overrides.target_triple.empty()
        ? std::optional{mcpp::toolchain::triple::host_triple()}
        : mcpp::toolchain::triple::parse(state.overrides.target_triple);
    bool baseStated = !lt.sysroot.empty();
    if (tt && !baseStated) {
        auto s = base_statement(state, *tt);
        if (!s) return std::unexpected(s.error());
        baseStated = s->stated();
    }
    if (!baseStated && lt.prefix.empty()) {
        mcpp::toolchain::XimToolchainPackage pkg;
        pkg.ximName = family;
        pkg.family  = family == "llvm" ? mcpp::toolchain::Family::Llvm : mcpp::toolchain::Family::Gcc;
        pkg.needsGccPostInstallFixup = family == "gcc";
        if (auto provided = state.provide_runtime_payload(pkg); !provided)
            return std::unexpected(provided.error());
    }
    return {};
}

std::expected<void, std::string> step2_apply_local_toolchain(PrepareState& state) {
    if (!state.localToolchain || !state.tc) return {};
    auto& lt = *state.localToolchain;
    auto& tc = *state.tc;
    if (!lt.family.empty()) {
        const bool isLlvm = tc.compiler == mcpp::toolchain::CompilerId::Clang;
        if ((lt.family == "llvm") != isLlvm) {
            refusal::record(refusal::Code::LocalToolchain);
            return std::unexpected(std::format(
                "the toolchain at '{}' is stated as family \"{}\", and its driver '{}' is {} {}",
                lt.path, lt.family, tc.binaryPath.generic_string(), tc.compiler_name(), tc.version));
        }
    }
    tc.localRoot  = lt.path;
    tc.toolPrefix = lt.prefix;
    for (auto const& [role, p] : lt.tools)
        if (role != "cxx") tc.toolOverrides.emplace_back(role, p);
    if (!lt.launcher.empty()) {
        fs::path l(lt.launcher);
        if (!l.has_parent_path()) {
            auto found = mcpp::platform::fs::which(lt.launcher);
            if (!found) {
                refusal::record(refusal::Code::LocalToolchain);
                return std::unexpected(std::format(
                    "launcher '{}' is not found on PATH", lt.launcher));
            }
            l = *found;
        }
        tc.launcher = l.generic_string();
    }
    // A stated sysroot is the target's base, resolved with every other
    // statement of it (base.cpp), not here.
    tc.driverIdent += "\nmcpp-local-toolchain " + local_identity(tc);
    // Announced beside the resolution, with what the probe found.
    auto d = state.localToolchainOrigin;
    d.subject = "toolchain.build";
    d.value   = lt.path;
    d.detail  = std::format("{} {}", tc.compiler_name(), tc.version);
    d.considered = {tc.binaryPath.generic_string()};
    if (!tc.launcher.empty()) d.considered.push_back("launcher " + tc.launcher);
    for (auto const& [role, p] : tc.toolOverrides)
        d.considered.push_back(std::format("{} {}", role, p.generic_string()));
    record_source(state, std::move(d));
    return {};
}

std::expected<void, std::string> step6_toolchain_phase(PrepareState& state) {
    if (!state.toolchainConfigure || !state.overrides.toolchain_statement.empty()) return {};
    if (!std::filesystem::exists(*state.root / "build.mcpp")) {
        refusal::record(refusal::Code::LocalToolchain);
        return std::unexpected(std::string(
            "[toolchain] says `configure = \"build.mcpp\"`, and this project has no build.mcpp"));
    }
    auto host = state.host_tc_for_build_program();
    if (!host) return std::unexpected(host.error());
    mcpp::build::BuildProgramEnv bpEnv;
    bpEnv.targetTriple = state.resolvedTargetCanonical;
    fill_target_build_env(bpEnv, *state.m, state.tc ? &*state.tc : nullptr,
                          state.cfg_opt ? &*state.cfg_opt : nullptr);
    apply_graph_cxx_abi(bpEnv, state.packages);
    bpEnv.toolsBin = state.projectSubosBin;
    bpEnv.profile  = state.effectiveProfile;
    fill_package_build_env(bpEnv, *state.m);
    bpEnv.languageModules = state.m->language.modules;
    // ITS OWN RECORD: the toolchain phase and the build phase are two runs of
    // one program with two contracts, and one record would make each
    // invalidate the other on every build.
    bpEnv.artifactsDir = state.workRoot / "target" / ".build-mcpp" / "toolchain-phase";
    bpEnv.features = feature_closure(*state.m, parse_feature_request(state.overrides.features));
    state.fillDepDirs(bpEnv, 0, nullptr);
    // The program, compiled by the build programs' graph with the bootstrap
    // toolchain (mcpp#797).
    auto program = build_program_binary(state, 0);
    if (!program) return std::unexpected(std::format("the root build program's toolchain phase: {}",
                                                     program.error()));
    bpEnv.programBinary   = program->path;
    bpEnv.programIdentity = program->identity;
    bpEnv.programCompile  = program->compile;
    bpEnv.phase = "toolchain";
    // A copy: the first pass is discarded, and what the phase states is the
    // whole of what is carried to the second.
    auto manifest = *state.m;
    auto r = mcpp::build::run_build_program(manifest, *state.root, host->first, host->second,
                                            manifest.cppStandard, bpEnv);
    if (!r) return std::unexpected(std::format("the root build program's toolchain phase: {}", r.error()));
    if (manifest.buildConfig.toolchainStatement.empty()) {
        refusal::record(refusal::Code::LocalToolchain);
        return std::unexpected(std::string(
            "[toolchain] says `configure = \"build.mcpp\"`, and the root build program stated no\n"
            "       toolchain in its toolchain phase. While `mcpp::phase()` is \"toolchain\", it\n"
            "       states one with `mcpp::plugins::toolchain::use(...)` (or `mcpp::toolchain(k, v)`)."));
    }
    g_restart = ToolchainRestart{manifest.buildConfig.toolchainStatement,
                                 state.tcSpec.value_or(std::string{})};
    return std::unexpected(std::string("mcpp:toolchain-phase-restart"));
}

} // namespace mcpp::build

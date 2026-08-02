// mcpp.build.ninja — Ninja-backed implementation of Backend.
//
// Layout produced under plan.outputDir = target/<triple>/<fp>/:
//   build.ninja
//   gcm.cache/std.gcm           (symlink/copy of plan.stdBmiPath)
//   gcm.cache/<module>.gcm      (created by GCC during compile)
//   obj/<unit>.o
//   obj/std.o                   (symlink/copy of plan.stdObjectPath)
//   bin/<target>
//
// All compile commands are run with cwd = plan.outputDir, so GCC's implicit
// gcm.cache/ lookup finds both std and our package modules.

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.build.ninja;

import std;
import mcpp.build.backend;
import mcpp.build.distribution;
import mcpp.build.plan;
import mcpp.build.flags;
import mcpp.build.hermetic;
import mcpp.build.compile_commands;
import mcpp.diag;
import mcpp.dyndep;
import mcpp.toolchain.detect;
import mcpp.toolchain.dialect;
import mcpp.toolchain.provider;
import mcpp.toolchain.registry;
import mcpp.xlings;
import mcpp.platform;
import mcpp.ui;

export namespace mcpp::build {

class NinjaBackend final : public Backend {
public:
    std::string_view name() const override {
        return "ninja";
    }

    std::expected<BuildResult, BuildError> build(const BuildPlan& plan,
                                                 const BuildOptions& opts) override;
};

// Factory for this backend implementation.
std::unique_ptr<Backend> make_ninja_backend();

// Helper exposed for testing / debugging
std::string emit_ninja_string(const BuildPlan& plan);
std::string filter_ninja_output(std::string_view output,
                                std::span<const std::string> commandPrefixes);

}  // namespace mcpp::build

namespace mcpp::build {

namespace {

std::string escape_ninja_path(const std::filesystem::path& p) {
    // Ninja escapes: $ → $$, : → $:, space → $ (with leading space).
    // For simplicity we wrap in case-by-case.
    //
    // generic_string(): ninja node names must be forward-slash even on
    // Windows (#247). rspfile_content = $in copies node names verbatim into
    // a response file that gcc/clang/GNU ar tokenize GNU-style, where
    // backslash is an ESCAPE character — `obj\cli.o` would arrive as
    // `objcli.o`. Every Windows consumer of these strings (CreateProcess
    // path resolution, cl.exe/link.exe, `mcpp stage`, ninja itself)
    // accepts forward slashes; POSIX output is byte-identical.
    std::string s = p.generic_string();
    std::string out;
    for (char c : s) {
        if (c == '$')
            out += "$$";
        else if (c == ':')
            out += "$:";
        else if (c == ' ')
            out += "$ ";
        else
            out.push_back(c);
    }
    return out;
}

std::string escape_flag_path(const std::filesystem::path& p) {
    // generic_string() for the same reason node names use it (#247): on
    // Windows these -I/-idirafter paths are copied verbatim into a response
    // file (#261), which the drivers tokenize GNU-style, where backslash is
    // an ESCAPE character — a path like C:\src\inc would lose its separators.
    // Every Windows consumer accepts forward slashes; POSIX is unchanged.
    auto s = p.generic_string();
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == ' ' || c == '$' || c == ':')
            out.push_back('$');
        out.push_back(c);
    }
    return out;
}

bool is_nasm_source(const std::filesystem::path& src) {
    return src.extension() == ".asm";
}

std::string local_include_flags(const CompileUnit& cu,
                                const mcpp::toolchain::CommandDialect& d) {
    const bool nasmUnit    = is_nasm_source(cu.source);
    const bool msvcDialect = d.includePrefix == std::string_view("/I");
    std::string flags;
    for (auto const& inc : cu.localIncludeDirs) {
        // #331: this used to hardcode `-I` and apply only ninja's `$`
        // escaping — no shell quoting — while the global channel in
        // flags.cppm quoted properly. Same manifest include_dirs, two
        // derivations, and a directory with a space in it split into
        // separate shell words on this path only. Both channels now go
        // through mcpp::build::include_token.
        //
        // Generic form is REQUIRED here and only here: on Windows ninja
        // copies $local_includes into a response file (#261), which the
        // drivers tokenize GNU-style — a backslash there is an escape
        // character, inside quotes as well, so `C:\src\inc` would lose its
        // separators and every dependency header would go missing.
        flags += ' ';
        flags += mcpp::build::include_token(d, inc, {},
                                            mcpp::build::PathForm::Generic);
    }
    // #249: after-dirs are searched AFTER the toolchain's system dirs
    // (-idirafter, gcc+clang), so a dep source root that contains a file
    // named like a standard header (ffmpeg's VERSION vs libc++'s <version>
    // on case-insensitive macOS) can't shadow it, while its real headers
    // are still found. Appended after the -I entries — the flag's
    // semantics, not its position, carry the priority; the ordering is
    // just for readability. Two degradations, both safe because only the
    // C/C++ frontends have a system-header chain to protect:
    //   • cl.exe has no -idirafter → plain /I at the END of the list
    //     (clang targeting MSVC uses the gnu dialect and gets the real flag);
    //   • nasm_object edges share $local_includes but NASM would parse
    //     `-idirafter<p>` as its `-i` option with value `dirafter<p>` —
    //     a silently wrong search dir — so nasm units get plain -I.
    for (auto const& inc : cu.localIncludeDirsAfter) {
        std::string_view pfx =
            nasmUnit ? "-I" : (msvcDialect ? "/I" : "-idirafter");
        flags += ' ';
        flags += mcpp::build::include_token(d, inc, pfx,
                                            mcpp::build::PathForm::Generic);
    }
    return flags;
}

std::string join_flags(const std::vector<std::string>& flags) {
    // mcpp#234: a manifest `defines = ["T=long long"]` arrives as the single
    // element `-DT=long long` (pushed whole by apply_glob_flags) — a space
    // that is genuinely PART of one argv token. Joining with a bare space let
    // the shell split it into two words (`-DT=long` + `long`) once ninja
    // handed the command line to the shell, so a define value with a space
    // must be shell-quoted.
    //
    // But quote ONLY that case. Every OTHER flag element is passed through
    // verbatim, because two other kinds of element legitimately contain a
    // space that MUST stay a token boundary, and quoting them breaks the build:
    //   • raw descriptor flags pack multiple argv tokens into one string
    //     (compat.lua's `-include mcpp_lua_platform_config.h` — the space
    //     separates `-include` from its argument; quoting made gcc see one
    //     malformed arg → "No such file"; broke aarch64/macos/windows builds);
    //   • mcpp-generated link flags are already shell-quoted + ninja-escaped
    //     (`-Wl,-rpath,'$$ORIGIN'` — single quotes stop shell $-expansion,
    //     `$$` is ninja's literal `$`); re-quoting baked a literal `'$ORIGIN'`
    //     into RUNPATH so dependency .so's next to the exe couldn't resolve.
    // The `defines` channel is the ONLY producer of a `-D`/`/D`-prefixed token
    // whose space is intra-value, so gate the quoting on exactly that shape.
    std::string out;
    for (auto const& flag : flags) {
        out += ' ';
        const bool defineWithSpace =
            (flag.starts_with("-D") || flag.starts_with("/D"))
            && flag.find(' ') != std::string::npos;
        out += defineWithSpace ? shell_quote_arg(flag) : flag;
    }
    return out;
}

std::string shared_soname_flag(const LinkUnit& lu) {
    if (lu.kind != LinkUnit::SharedLibrary || lu.soname.empty()) return "";
#if defined(__APPLE__)
    return "-Wl,-install_name,@rpath/" + lu.soname;
#elif defined(__linux__)
    return "-Wl,-soname," + lu.soname;
#else
    return "";
#endif
}

void write_file(const std::filesystem::path& p, std::string_view content) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream os(p);
    os << content;
}

bool run(const std::string& cmd, std::string& output_capture, bool capture_output = true) {
    output_capture.clear();
    if (capture_output) {
        auto r = mcpp::platform::process::capture(cmd);
        output_capture = r.output;
        return r.exit_code == 0;
    }
    return mcpp::platform::process::run_passthrough(cmd) == 0;
}

bool dyndep_mode_enabled() {
    // M4 #7: dyndep is now the default. Set MCPP_NINJA_DYNDEP=0 to opt
    // OUT and fall back to the static-deps emission path.
    const char* v = std::getenv("MCPP_NINJA_DYNDEP");
    if (!v)
        return true;
    std::string_view sv(v);
    return !(sv == "0" || sv == "off" || sv == "false");
}

std::filesystem::path mcpp_exe_path() {
    return mcpp::platform::fs::self_exe_path();
}

bool is_c_source(const std::filesystem::path& src) {
    auto ext = src.extension();
    return ext == ".c" || ext == ".m";
}

bool is_gas_source(const std::filesystem::path& src) {
    auto ext = src.extension();
    return ext == ".S" || ext == ".s";
}

// TUs the P1689 module scan must skip: C-family and assembly units cannot
// contain `import`/`module` declarations, and feeding them to the scanner
// would route them through the C++ frontend.
bool is_scan_exempt(const std::filesystem::path& src) {
    return is_c_source(src) || is_gas_source(src) || is_nasm_source(src);
}

// Per-unit flags an assembler can take: the -D/-U/-I subset of the unit's C
// flags (feature defines land there). NASM shares the GNU -D/-U/-I spelling
// (and ≥2.14 inserts a missing -I path separator itself), so one filter
// serves both asm rules. Explicit per-glob asmflags (G4) append after the
// filtered subset — author-directed flags win.
std::vector<std::string> asm_unit_flags(const CompileUnit& cu) {
    std::vector<std::string> out;
    for (auto& f : cu.packageCflags) {
        if (f.starts_with("-D") || f.starts_with("-U") || f.starts_with("-I"))
            out.push_back(f);
    }
    out.insert(out.end(), cu.packageAsmflags.begin(), cu.packageAsmflags.end());
    return out;
}

std::string ltrim_copy(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    return std::string(s);
}

bool is_ninja_progress_line(std::string_view line) {
    if (line.size() < 5 || line.front() != '[') return false;
    std::size_t i = 1;
    if (i >= line.size() || !std::isdigit(static_cast<unsigned char>(line[i])))
        return false;
    while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) ++i;
    if (i >= line.size() || line[i] != '/') return false;
    ++i;
    if (i >= line.size() || !std::isdigit(static_cast<unsigned char>(line[i])))
        return false;
    while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) ++i;
    return i < line.size() && line[i] == ']';
}

bool starts_with_any(std::string_view line,
                     std::span<const std::string> prefixes) {
    for (auto& prefix : prefixes) {
        if (!prefix.empty() && line.starts_with(prefix))
            return true;
    }
    return false;
}

bool contains_any(std::string_view line,
                  std::span<const std::string> needles) {
    for (auto& needle : needles) {
        if (!needle.empty() && line.find(needle) != std::string_view::npos)
            return true;
    }
    return false;
}

std::vector<std::string> command_prefixes(const CompileFlags& flags,
                                          const BuildPlan& plan) {
    std::vector<std::string> prefixes;
    auto add = [&](const std::filesystem::path& p) {
        if (p.empty()) return;
        auto s = p.string();
        if (std::find(prefixes.begin(), prefixes.end(), s) == prefixes.end())
            prefixes.push_back(std::move(s));
    };
    add(flags.cxxBinary);
    add(flags.ccBinary);
    add(flags.arBinary);
    add(plan.scanDepsPath);
    // mcpp itself drives the dyndep and stage_file rules; its echoed command
    // line is noise, while the message it prints on failure is the diagnostic
    // we want to keep.
    add(mcpp_exe_path());
    return prefixes;
}

bool is_command_line(std::string_view trimmed,
                     std::span<const std::string> commandPrefixes) {
    if (starts_with_any(trimmed, commandPrefixes)) return true;

    if (trimmed.starts_with("env ")
        && (trimmed.find("LD_LIBRARY_PATH=") != std::string_view::npos
            || trimmed.find("DYLD_LIBRARY_PATH=") != std::string_view::npos
            || contains_any(trimmed, commandPrefixes))) {
        return true;
    }

    if ((trimmed.starts_with("cmd /c ") || trimmed.starts_with("if [ "))
        && contains_any(trimmed, commandPrefixes)) {
        return true;
    }

    return false;
}

std::optional<std::pair<std::string, std::string>>
runtime_env_for_dirs(const std::vector<std::filesystem::path>& dirs) {
    auto key = mcpp::platform::env::runtime_library_path_key();
    auto value = mcpp::platform::env::prepend_path_list(key, dirs);
    if (key.empty() || value.empty()) return std::nullopt;
    return std::pair{std::move(key), std::move(value)};
}

}  // namespace

std::string filter_ninja_output(std::string_view output,
                                std::span<const std::string> commandPrefixes) {
    std::string filtered;
    std::string line;
    std::istringstream in{std::string(output)};
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        auto trimmed = ltrim_copy(line);
        if (trimmed.starts_with("ninja: Entering directory")
            || trimmed.starts_with("ninja: build stopped")
            || is_ninja_progress_line(trimmed)
            || is_command_line(trimmed, commandPrefixes)) {
            continue;
        }
        // Keep WHICH output failed. Dropping this line entirely (as we used to)
        // is why #311's report couldn't tell a BMI staging failure from a
        // compile error. Normalized to lowercase `failed:` so it reads as part
        // of mcpp's own diagnostics, and `[code=N]` is dropped as noise.
        if (trimmed.starts_with("FAILED:")) {
            auto target = ltrim_copy(trimmed.substr(std::string_view("FAILED:").size()));
            if (target.starts_with("[code=")) {
                if (auto close = target.find(']'); close != std::string::npos)
                    target = ltrim_copy(target.substr(close + 1));
            }
            while (!target.empty()
                   && std::isspace(static_cast<unsigned char>(target.back())))
                target.pop_back();
            if (target.empty()) continue;
            filtered += "failed: ";
            filtered += target;
            filtered.push_back('\n');
            continue;
        }
        filtered += line;
        filtered.push_back('\n');
    }
    return filtered;
}

std::string emit_ninja_string(const BuildPlan& plan) {
    // dyndep requires P1689 scanning capability:
    //   GCC: built-in -fdeps-format=p1689r5
    //   Clang: external clang-scan-deps tool (same P1689 output format)
    //   (MSVC /scanDependencies is the future third driver — scanner design §3a)
    auto caps = mcpp::toolchain::capabilities_for(plan.toolchain);
    bool has_scanner = caps.has_builtin_p1689_scan || !plan.scanDepsPath.empty();
    bool dyndep = dyndep_mode_enabled() && has_scanner;
    auto traits = mcpp::toolchain::bmi_traits(plan.toolchain);
    const auto& dial = mcpp::toolchain::dialect_for(plan.toolchain);
    std::string out;
    auto append = [&](std::string s) { out += std::move(s); };

    append("# Auto-generated by mcpp v0.0.1. Do not edit by hand.\n");
    append("ninja_required_version = 1.11\n\n");

    // All compile/link flags are computed once via flags.cppm.
    auto flags = compute_flags(plan);

    bool need_c_rule = false, need_asm_rule = false, need_nasm_rule = false;
    for (auto& cu : plan.compileUnits) {
        if (is_c_source(cu.source))         need_c_rule = true;
        else if (is_gas_source(cu.source))  need_asm_rule = true;
        else if (is_nasm_source(cu.source)) need_nasm_rule = true;
    }

    // The macOS initializer-ordering shim (#336) is a C translation unit, so
    // it needs the C driver bindings even in a project with no .c sources.
    const bool need_ios_init_shim = flags.needsStreamInitShim;
    append(std::format("cxx       = {}\n", escape_ninja_path(flags.cxxBinary)));
    append(std::format("cxxflags  = {}\n", flags.cxx));
    if (need_c_rule || need_asm_rule || need_ios_init_shim) {  // asm_object drives the C compiler too
        append(std::format("cc        = {}\n", escape_ninja_path(flags.ccBinary)));
    }
    if (need_c_rule || need_ios_init_shim) {
        append(std::format("cflags    = {}\n", flags.cc));
    }
    if (need_asm_rule) {
        append(std::format("asmflags  ={}\n", flags.as));
    }
    if (need_nasm_rule) {
        append(std::format("nasm      = {}\n", escape_ninja_path(plan.nasmPath)));
        append(std::format("nasmfmt   = {}\n", plan.nasmFormat));
        append(std::format("nasmflags ={}\n", flags.nasm));
    }
    append(std::format("ldflags   ={}\n", flags.ld));

    // `ar` for cxx_archive.
    if (!flags.arBinary.empty()) {
        append(std::format("ar        = {}\n", escape_ninja_path(flags.arBinary)));
    } else {
        append("ar        = ar\n");
    }
    // Separate linker (link.exe) for the msvc dialect.
    const bool separateLinker =
        dial.linkStyle == mcpp::toolchain::CommandDialect::LinkStyle::SeparateLinker;
    if (separateLinker) {
        append(std::format("ld        = {}\n",
            flags.ldBinary.empty() ? std::string("link.exe")
                                   : escape_ninja_path(flags.ldBinary)));
    }
    // `$mcpp` is needed by stage_file in EVERY configuration (dyndep or not),
    // so the binding cannot live inside the `if (dyndep)` below.
    append(std::format("mcpp      = {}\n", escape_ninja_path(mcpp_exe_path())));
    if (dyndep) {
        if (!plan.scanDepsPath.empty()) {
            append(std::format("scan_deps = {}\n", escape_ninja_path(plan.scanDepsPath)));
        }
    }
    append("\n");

    // Staging (cache → build dir) runs through mcpp itself instead of a
    // per-platform shell copy: it skips the write when the destination is
    // already equivalent, writes out-of-place + renames when it isn't, retries
    // transient sharing violations, and fails with a diagnostic that names the
    // likely holder. #311: `Copy-Item -Force` overwrote in place, so a std BMI
    // that clangd had memory-mapped failed the whole build with error 1224.
    // `restat = 1` is what makes the no-write path actually pay off — a skipped
    // stage must not dirty every importer of the staged BMI.
    // $verify is per-edge: empty (⇒ content comparison, the safe default) for
    // payloads whose bytes can change under an unchanged name — a rebuilt DLL
    // keeps its size, PE sections are page-padded — and `--verify size` for the
    // std artifacts, which are fingerprint-scoped: the cache dir and this build
    // dir share the fp that covers compiler identity, target triple, stdlib,
    // std source hash and the dialect flags, so equal size IS equivalence.
    //
    // That distinction is not a micro-optimization: reading the destination
    // needs to OPEN it, and a holder may deny even that (Windows CI: a
    // MemoryMappedFile opened with FileShare.None → ERROR_SHARING_VIOLATION on
    // open, so content can't be compared and staging fails). Size comes from
    // directory metadata, which stays readable — so the fingerprint-scoped
    // edges survive a lock that would otherwise be unsurvivable.
    append("rule stage_file\n");
    append("  command = $mcpp stage $verify --output $out $in\n");
    append("  description = STAGE $out\n");
    append("  restat = 1\n\n");

    // P1: per-file dyndep rule. Converts one .ddi → .dd independently.
    append(std::format(
        "rule cxx_dyndep\n"
        "  command = $mcpp dyndep --single --bmi-dir {} --bmi-ext {} $expect --output $out $in\n"
        "  description = DYNDEP $out\n"
        "  restat = 1\n\n",
        traits.bmiDir, traits.bmiExt));

    // P2: cxx_module preserves BMI timestamps when interface is unchanged.
    // GCC always updates the .gcm timestamp even if content is identical.
    // We backup the BMI before compilation, compile, then restore the old
    // file if content is byte-identical. Combined with restat = 1 in the
    // dyndep file, this prevents cascading rebuilds when only the module
    // implementation changed (not the interface).
    //
    // $bmi_out is set per build edge to the BMI path (gcm.cache/<module>.gcm).
    // If $bmi_out is empty (no module provided), we just compile normally.
    // Runtime library paths for private toolchain executables are scoped onto
    // the ninja subprocess instead of being emitted into each visible rule.

    // Command spellings come from the toolchain's CommandDialect (gnu vs
    // msvc); the rule *structure* is shared across compilers.
    std::string module_output_flag = traits.needsExplicitModuleOutput
        ? std::string(traits.moduleOutputPrefix) + "$bmi_out" : "";
    // msvc: /showIncludes feeds ninja's deps=msvc header tracking; the
    // stable-English prefix is guaranteed by VSLANG=1033 in envOverrides.
    const bool msvcDeps = dial.ninjaDepsMode == std::string_view("msvc");
    const std::string compile_tail = std::format(
        "{}{} $in {}$out",
        msvcDeps ? "/showIncludes " : "", dial.compileOnly, dial.outputObjPrefix);
    auto append_deps = [&] {
        if (msvcDeps) append("  deps = msvc\n");
    };
    // mcpp#235: cxx_module/cxx_object had NO depfile at all on non-MSVC —
    // only the msvcDeps branch tracked header deps (via /showIncludes). So
    // editing a file #include'd inside a module's purview (or a plain
    // header pulled in by a .cpp) never invalidated the compile edge: the
    // P1689 scan already emits a `.dep`/`.d`-shaped list of textual
    // includes, but it was generated and discarded.
    //
    // Naively mirroring the nasm rule below (`-MD $out.d` + `deps = gcc` +
    // `depfile = $out.d`) does NOT work here: GCC's `-fmodules` bolts extra
    // "reversed" rules onto ANY -M*/-MF depfile for a TU that imports or
    // provides a module — e.g. for a module interface unit:
    //   obj/m.m.o gcm.cache/m.gcm: src/m.cppm src/vals.inc
    //   m.c++-module: gcm.cache/m.gcm
    //   .PHONY: m.c++-module
    //   gcm.cache/m.gcm:| obj/m.m.o
    // and for an importing TU:
    //   obj/main.o: src/main.cpp gcm.cache/std.gcm gcm.cache/m.gcm
    //   obj/main.o: m.c++-module std.c++-module
    //   CXX_IMPORTS += m.c++-module std.c++-module
    // Those extra records describe a Make-style dependency graph WITHIN the
    // depfile itself (e.g. gcm.cache/m.gcm "having its own inputs"), which
    // collides with gcm.cache/m.gcm already being a declared ninja-graph
    // OUTPUT of this same edge (`| gcm.cache/m.gcm`) — ninja's depfile
    // loader rejects that outright ("inputs may not also have inputs"),
    // confirmed empirically (e2e 118 failed the fresh build this way before
    // the filter below was added). GCC has no flag to suppress this.
    //
    // Fix: compile to a scratch `$out.d.raw`, then keep only the FIRST
    // record (target + its indented continuation lines — the plain textual
    // #include graph, which is all #235 needs) as `$out.d`; module BMI
    // deps stay tracked independently via the existing per-edge `dyndep`
    // binding, so dropping the reversed/module lines here loses nothing.
    // POSIX-only (`awk`): native Windows has no POSIX shell/toolset here
    // (see the existing "Windows: skip BMI restat optimization" branch
    // below), so a non-MSVC Windows toolchain keeps the pre-#235
    // behavior (no depfile) rather than depend on an unavailable filter —
    // msvcDeps (cl.exe) is unaffected either way (deps=msvc, no -MMD).
    //
    // #257: these are TWO decisions, and 0.0.97 conflated them. Emitting a
    // depfile at all is the minimum correctness contract for textual include
    // tracking — without it, editing a file #include'd in a module purview
    // silently reuses a stale BMI. Stripping GCC's reversed make-rules is a
    // GCC-shaped detail of HOW that depfile arrives. Gating the first on the
    // second left Clang with no include tracking for four releases.
    //
    // Measured (bundled 20.1.7 / 22.1.8 vs gcc 16.1.0, module TU with a
    // purview #include):
    //   clang:  `x.o: x.cppm ops.inc`               — one plain rule
    //   gcc:    `x.o gcm.cache/x.gcm: x.cppm ops.inc`
    //           `x.c++-module: gcm.cache/x.gcm` + .PHONY + `gcm.cache/x.gcm:| x.o`
    // So Clang emits nothing the filter would need to remove, and the
    // conflated gate was protecting against a shape that does not exist.
    const bool posixDepfile = !msvcDeps && !mcpp::platform::is_windows;
    const bool needsGnuModuleFilter =
        posixDepfile && plan.toolchain.compiler == mcpp::toolchain::CompilerId::GCC;
    const std::string mmd_flag =
        posixDepfile ? (needsGnuModuleFilter ? "-MMD -MF $out.d.raw "
                                             : "-MMD -MF $out.d ")
                     : "";
    const std::string mmd_filter = needsGnuModuleFilter
        ? " && awk 'NR==1{print;next} /^[^ ]/{exit} {print}' "
          "\"$out.d.raw\" > \"$out.d\" && rm -f \"$out.d.raw\""
        : "";
    auto append_cxx_deps = [&] {
        if (posixDepfile) {
            append("  deps = gcc\n");
            append("  depfile = $out.d\n");
        } else {
            append_deps();
        }
    };
    // C and GAS units include headers too, and had no depfile on ANY
    // toolchain — the second half of the same asymmetry #257 reports. They
    // never carry module reversed-rules, so they need the flag but not the
    // filter.
    const std::string c_mmd_flag = posixDepfile ? "-MMD -MF $out.d " : "";
    // Windows non-MSVC (mingw gcc / clang) is the one combination left with
    // no include tracking: the GCC filter needs awk, which is not available
    // there. cl.exe is fine — deps=msvc via /showIncludes is the equivalent
    // mechanism, not a degradation.
    if (!posixDepfile && !msvcDeps) {
        mcpp::diag::degraded("build/depfile",
            "this toolchain and platform combination emits no GNU depfile",
            "editing a file #include'd inside a module interface purview (or a "
            "header pulled into a .cpp) will not trigger a rebuild, so the build "
            "may reuse a stale BMI or object",
            "touch the including .cppm/.cpp after editing such a file");
    }
    // #261: the flag payload of every compile/scan rule is unbounded — one
    // -I per dependency include dir — and on Windows ninja spawns through
    // CreateProcess, whose command line caps at 32767 chars. Route the
    // payload through a response file: the same mitigation the link rules
    // got in #247, for the same reason (there it was thousands of objects).
    //
    // Safe for every consumer: the gcc and clang drivers and cl.exe all
    // expand @file, and clang-scan-deps passes an @file in its post-`--`
    // command straight through to the driver (verified on the bundled
    // 20.1.7). nasm_object is deliberately NOT converted — NASM spells
    // response files `-@ file`, not `@file`, and its rule already sits
    // outside CommandDialect for that class of reason.
    //
    // POSIX keeps the inline form byte-identical: ARG_MAX is ample and an
    // inline command is far easier to re-run by hand when debugging.
    //
    // ONLY $local_includes goes in. Response-file content is tokenized
    // GNU-style, where backslash is an ESCAPE character, so every path that
    // moves off the command line must be forward-slashed — and the only
    // paths whose form this file controls are the -I/-idirafter entries it
    // builds via escape_flag_path(). $cxxflags carries paths produced
    // elsewhere (flags.cppm's -fprebuilt-module-path=, module-file mappings)
    // that are still native-separated; routing those through the rsp ate the
    // separators of the std.pcm path and broke every `import std;` on the
    // Windows CI leg. They stay inline, where a backslash is just a
    // character. Sufficient, too: the unbounded axis #261 is about is one -I
    // per dependency include dir, and the rest of the payload is bounded.
    // Also keyed on the msvc dialect, which only ever runs on Windows: it
    // makes the response-file shape reachable from a non-Windows test host,
    // the same over-approximation the link rules use (`separateLinker ||
    // is_windows`).
    const bool useCompileRsp = mcpp::platform::is_windows || msvcDeps;
    // Both take the payload with its leading space, so callers read as
    // `command = $cxx{payload} ...` exactly like the inline form did.
    auto rsp_ref = [&](const std::string& payload) {
        return useCompileRsp ? std::string(" @$out.rsp") : payload;
    };
    auto append_rspfile = [&](const std::string& payload) {
        if (!useCompileRsp) return;
        append("  rspfile = $out.rsp\n");
        append(std::format("  rspfile_content ={}\n", payload));
    };

    // cl.exe needs /TP (our module interfaces are .cppm, unknown to cl) and
    // /interface to treat the TU as a module interface unit.
    const std::string module_src_flags = msvcDeps ? " /interface /TP" : "";
    append("rule cxx_module\n");
    if constexpr (mcpp::platform::is_windows) {
        // Windows: skip BMI restat optimization (requires POSIX shell).
        const std::string payload = " $local_includes";
        append(std::format("  command = $cxx{} $cxxflags $unit_cxxflags{}{} {}\n",
                           rsp_ref(payload), module_output_flag,
                           module_src_flags, compile_tail));
        append_rspfile(payload);
        append_cxx_deps();
    } else {
        append(std::format("  command = "
               "if [ -n \"$bmi_out\" ] && [ -f \"$bmi_out\" ]; then "
                 "cp -p \"$bmi_out\" \"$bmi_out.bak\"; "
               "fi && "
               "$cxx $local_includes $cxxflags $unit_cxxflags{} {}{}{} && "
               "if [ -n \"$bmi_out\" ] && [ -f \"$bmi_out.bak\" ] && "
                  "cmp -s \"$bmi_out\" \"$bmi_out.bak\"; then "
                 "mv \"$bmi_out.bak\" \"$bmi_out\"; "
               "else "
                 "rm -f \"$bmi_out.bak\"; "
               "fi\n", module_output_flag, mmd_flag, compile_tail, mmd_filter));
        append_cxx_deps();
    }
    append("  description = MOD $out\n");
    if (dyndep)
        append("  restat = 1\n");
    append("\n");

    append("rule cxx_object\n");
    if constexpr (mcpp::platform::is_windows) {
        const std::string payload = " $local_includes";
        append(std::format("  command = $cxx{} $cxxflags $unit_cxxflags {}\n",
                           rsp_ref(payload), compile_tail));
        append_rspfile(payload);
    } else {
        append(std::format(
            "  command = $cxx $local_includes $cxxflags $unit_cxxflags {}{}{}\n",
            mmd_flag, compile_tail, mmd_filter));
    }
    append("  description = OBJ $out\n");
    append_cxx_deps();
    if (dyndep)
        append("  restat = 1\n");
    append("\n");

    if (need_c_rule) {
        append("rule c_object\n");
        const std::string payload = " $local_includes";
        append(std::format("  command = $cc{} $cflags $unit_cflags {}{}\n",
                           rsp_ref(payload), c_mmd_flag, compile_tail));
        append_rspfile(payload);
        append("  description = CC $out\n");
        append_cxx_deps();
        if (dyndep)
            append("  restat = 1\n");
        append("\n");
    }

    if (need_asm_rule) {
        // GAS assembly (.S/.s) through the C driver: it preprocesses .S (cpp)
        // and assembles both, dispatching by extension. $asmflags is the
        // asm-safe flag subset (no -std / no -O — see flags.cppm).
        // TWO rules, split by case: the C driver preprocesses `.S` but not
        // `.s`. Asking for a depfile on a `.s` unit is not merely useless —
        // clang emits `argument unused during compilation: '-MMD'` for every
        // such file and writes nothing, and ninja's `deps = gcc` treats an
        // absent depfile as an error. So `.s` keeps the pre-#257 shape.
        const std::string payload = " $local_includes";
        append("rule asm_object\n");     // .S — preprocessed, tracks #include
        append(std::format("  command = $cc{} $asmflags $unit_asmflags {}{}\n",
                           rsp_ref(payload), c_mmd_flag, compile_tail));
        append_rspfile(payload);
        append("  description = AS $out\n");
        append_cxx_deps();
        append("\n");

        append("rule asm_object_raw\n");  // .s — not preprocessed, no depfile
        append(std::format("  command = $cc{} $asmflags $unit_asmflags {}\n",
                           rsp_ref(payload), compile_tail));
        append_rspfile(payload);
        append("  description = AS $out\n\n");
    }

    if (need_nasm_rule) {
        // NASM (.asm): its own fixed flag spelling — deliberately outside
        // CommandDialect. -MD/-MQ feed ninja's header tracking for %include.
        append("rule nasm_object\n");
        append("  command = $nasm -f $nasmfmt $local_includes $nasmflags "
               "$unit_asmflags -MD $out.d -MQ $out -o $out $in\n");
        append("  deps = gcc\n");
        append("  depfile = $out.d\n");
        append("  description = NASM $out\n\n");
    }

    // Link/archive/shared: driver-style (g++/clang++ are the linker) vs the
    // msvc dialect's separate link.exe/lib.exe. One emitter owns the rule
    // shape; `useRsp` decides whether $in is inlined or routed through a
    // response file (`$in → @$out.rsp` — the only `$i…` variable in any
    // link/archive command; revisit the first-match replace if a dialect
    // ever grows another).
    //
    // rsp is used when the command spawns through CreateProcess (32 KiB
    // command-line ceiling): always for the separate-linker msvc dialect,
    // and on Windows for driver-style too (#247 — ffmpeg/opencv-class
    // packages link thousands of objects; clang/gcc drivers and GNU/llvm ar
    // all accept @rspfile). POSIX driver-style keeps the inline form
    // byte-identical: ARG_MAX is ample and the plain command is easier to
    // reproduce by hand.
    {
        const bool useRsp = separateLinker || mcpp::platform::is_windows;
        auto link_rule = [&](std::string_view name, std::string cmd,
                             std::string_view desc) {
            append(std::format("rule {}\n", name));
            if (useRsp) {
                if (auto pos = cmd.find("$in"); pos != std::string::npos)
                    cmd.replace(pos, 3, "@$out.rsp");
                append(std::format("  command = {}\n", cmd));
                append("  rspfile = $out.rsp\n");
                append("  rspfile_content = $in\n");
            } else {
                append(std::format("  command = {}\n", cmd));
            }
            append(std::format("  description = {} $out\n\n", desc));
        };
        if (separateLinker) {
            link_rule("cxx_link",
                      "$ld /nologo /OUT:$out $in $ldflags $unit_ldflags",
                      "LINK");
            link_rule("cxx_archive", std::string(dial.archiveCmd), "AR");
            link_rule("cxx_shared",
                      "$ld /nologo /DLL /OUT:$out /IMPLIB:$out.lib "
                      "$in $ldflags $unit_ldflags",
                      "SHARED");
        } else {
            link_rule("cxx_link",
                      "$cxx $in -o $out $ldflags $unit_ldflags", "LINK");
            link_rule("cxx_archive", std::string(dial.archiveCmd), "AR");
            link_rule("cxx_shared",
                      "$cxx -shared $in -o $out $ldflags $soname_flag $unit_ldflags",
                      "SHARED");
        }
    }

    // #336: one extra object, compiled from a generated C TU, whose only job
    // is to run first. Its own rule rather than the C rule above because it
    // has no includes (so no depfile machinery) and must not be perturbed by
    // whatever that rule grows next.
    if (need_ios_init_shim) {
        append("rule ios_init_object\n");
        append("  command = $cc $cflags -c $in -o $out\n");
        append("  description = CC $out\n\n");
    }

    append("rule runtime_alias\n");
    if constexpr (mcpp::platform::is_windows) {
        // PE has no soname symlink, so the alias is a copy — and a copy of a
        // just-rebuilt DLL is exactly the hazard #311 is about (a program still
        // running from a previous `mcpp run` holds the old one). Route it
        // through the same staging primitive rather than a second in-place
        // PowerShell copy. Content-verified, so a same-size rebuild still
        // refreshes the alias.
        append("  command = $mcpp stage --output $out $in\n");
    } else {
        append("  command = mkdir -p $$(dirname $out) && rm -f $out && ln -s $$(basename $in) $out\n");
    }
    append("  description = ALIAS $out\n");
    if constexpr (mcpp::platform::is_windows) {
        append("  restat = 1\n");
    }
    append("\n");

    if (dyndep) {
        // Scan rule: produce P1689 .ddi for one TU.
        // GCC: built-in -fdeps-format=p1689r5 flags during preprocessing.
        // Clang: external clang-scan-deps tool with -format=p1689.
        append("rule cxx_scan\n");
        // The scan command is strictly LONGER than the compile command for
        // the same TU (it wraps it), so it carries the same unbounded
        // include payload through the same response file (#261).
        const std::string scanPayload = " $local_includes";
        if (msvcDeps) {
            // MSVC: compiler-integrated P1689 via /scanDependencies (scan
            // only — no codegen); /TP because our module units are .cppm.
            append(std::format("  command = $cxx{} $cxxflags $unit_cxxflags "
                   "/scanDependencies $out /TP /c $in /Fo:$compile_target\n",
                   rsp_ref(scanPayload)));
        } else if (plan.scanDepsPath.empty()) {
            // GCC path: compiler-integrated P1689 scanning.
            append(std::format("  command = $cxx{} $cxxflags $unit_cxxflags -fmodules "
                   "-fdeps-format=p1689r5 "
                   "-fdeps-file=$out -fdeps-target=$compile_target "
                   "-M -MM -MF $out.dep -E $in -o $compile_target\n",
                   rsp_ref(scanPayload)));
        } else {
            // Clang path: clang-scan-deps writes the P1689 JSON itself via -o
            // (LLVM 17+), like the GCC and MSVC branches above write theirs
            // via -fdeps-file= / /scanDependencies. Shell redirection would
            // force a `cmd /c` wrapper on Windows, and cmd.exe caps a command
            // line at 8191 chars — a quarter of the 32767 ninja gets from
            // CreateProcess — which any package with a large include-dir list
            // overruns (#261: 48 -I entries at a deep consumer path).
            append(std::format(
                   "  command = $scan_deps -format=p1689 -o $out -- "
                   "$cxx{} $cxxflags $unit_cxxflags -c $in -o $compile_target\n",
                   rsp_ref(scanPayload)));
        }
        append_rspfile(scanPayload);
        append("  description = SCAN $out\n\n");

        // Aggregate .ddi files into a Ninja dyndep file.
        append(std::format(
            "rule cxx_collect\n"
            "  command = $mcpp dyndep --bmi-dir {} --bmi-ext {} --output $out $in\n"
            "  description = COLLECT $out\n"
            "  restat = 1\n\n",
            traits.bmiDir, traits.bmiExt));
    }

    // Stage prebuilt std artifacts into the compiler-specific BMI cache.
    // These four are fingerprint-scoped (see the stage_file rule comment), so
    // they carry the size-only equivalence check.
    // (ninja trims trailing whitespace in variable values, hence the space
//  lives in the rule's command string, not here)
    static constexpr std::string_view kFpScopedVerify = "  verify = --verify size\n";
    auto std_bmi_dst = mcpp::toolchain::staged_std_bmi_path(plan.toolchain, {});
    auto std_o_dst = std::filesystem::path("obj")
                   / std::format("std{}", dial.objExt);

    // #336 shim: source is written next to build.ninja by NinjaBackend::build.
    auto ios_init_src = std::filesystem::path("obj") / "mcpp_ios_init.c";
    auto ios_init_obj = std::filesystem::path("obj")
                      / std::format("mcpp_ios_init{}", dial.objExt);
    if (need_ios_init_shim) {
        append(std::format("build {} : ios_init_object {}\n",
                           escape_ninja_path(ios_init_obj),
                           escape_ninja_path(ios_init_src)));
        append("\n");
    }

    bool has_std_artifacts = !plan.stdBmiPath.empty() && !plan.stdObjectPath.empty();
    if (has_std_artifacts) {
        append(std::format("build {} : stage_file {}\n", escape_ninja_path(std_bmi_dst),
                           escape_ninja_path(plan.stdBmiPath)));
        append(std::string(kFpScopedVerify));
        append(std::format("build {} : stage_file {}\n", escape_ninja_path(std_o_dst),
                           escape_ninja_path(plan.stdObjectPath)));
        append(std::string(kFpScopedVerify));
        append("\n");
    }

    bool has_std_compat = !plan.stdCompatBmiPath.empty() && !plan.stdCompatObjectPath.empty();
    auto compat_bmi_dst = std::filesystem::path(traits.bmiDir)
                        / std::format("std.compat{}", traits.bmiExt);
    auto compat_o_dst = std::filesystem::path("obj")
                      / std::format("std.compat{}", dial.objExt);
    if (has_std_compat) {
        // std.compat.pcm depends on std.pcm — ensure std.pcm is staged first
        // so clang can resolve the transitive dependency when loading std.compat.pcm.
        append(std::format("build {} : stage_file {} | {}\n", escape_ninja_path(compat_bmi_dst),
                           escape_ninja_path(plan.stdCompatBmiPath),
                           escape_ninja_path(std_bmi_dst)));
        append(std::string(kFpScopedVerify));
        append(std::format("build {} : stage_file {}\n", escape_ninja_path(compat_o_dst),
                           escape_ninja_path(plan.stdCompatObjectPath)));
        append(std::string(kFpScopedVerify));
        append("\n");
    }

    // Aggregate target for everything staged out of the global cache. Named
    // with a leading underscore so it cannot collide with a module or target
    // name (both of which are identifiers or paths).
    constexpr std::string_view kStagedCachePhony = "_mcpp_staged_cache";

    auto bmi_path = [&traits](std::string_view name) {
        std::string s(traits.bmiDir);
        s += '/';
        for (char c : name)
            s.push_back(c == ':' ? '-' : c);
        s += traits.bmiExt;
        return s;
    };

    auto pick_rule = [](const std::filesystem::path& src) -> std::string {
        auto ext = src.extension();
        if (ext == ".cppm")
            return "cxx_module";
        if (ext == ".c" || ext == ".m")
            return "c_object";
        if (ext == ".S")
            return "asm_object";
        if (ext == ".s")
            return "asm_object_raw";
        if (ext == ".asm")
            return "nasm_object";
        return "cxx_object";
    };

    // ── Cache-served units: stage edges instead of compile edges ────────
    //
    // A unit whose outputs are already in the global cache gets one stage_file
    // edge per artifact and is then skipped by every loop below — no scan edge,
    // no dyndep edge, no compile edge. Its outputs land at exactly the paths a
    // compile edge would have produced, so the link edges, the BMI implicit
    // inputs of consuming TUs and the runtime deployment edges are unchanged.
    //
    // This indirection is the whole point. The cache used to copy artifacts
    // into the build dir from inside prepare, leaving them declared as compile
    // edge outputs — and ninja treats an output with no command line in
    // .ninja_log as dirty ("command line not found in log"), so a fresh build
    // dir recompiled every one of them while the CLI printed "Cached". Making
    // the staging an edge is what gives ninja a record to compare against.
    //
    // `--verify size` for the same reason the std artifacts use it: the entry
    // directory is named by a key covering the toolchain, dialect, profile,
    // this package's own config and its dependencies' keys, so for a given key
    // equal size IS equivalence — and size comes from directory metadata, which
    // stays readable even when a holder denies opening the file.
    //
    // ORDERING. Replacing a package's compile edges with stage edges also
    // removes the ordering those compile edges carried. A module partition is
    // the case that breaks: a consumer imports `pkg`, so its dyndep declares
    // `pkg`'s BMI and nothing else — the partition BMI `pkg:part` was reached
    // only because `pkg`'s own compile edge depended on it. With independent
    // stage edges, ninja may finish staging `pkg` and start the consumer while
    // `pkg:part` is still unstaged, and Clang then fails with
    // `failed to find module file for module 'pkg:part'`. (Observed on macOS
    // CI; Linux happened to win the race, which is exactly why this is stated
    // as an invariant rather than left to scheduling.)
    //
    // So every staged artifact becomes an ORDER-ONLY prerequisite of every edge
    // that is not itself staged. Order-only (`||`) is the right strength: the
    // real content dependencies are still declared where they always were (a
    // dyndep-supplied implicit input, or the static-mode implicit list), so a
    // changed BMI still invalidates its consumers — this adds sequencing, not
    // dirtiness. The cost is that a handful of copies finish before compilation
    // starts, which is what used to happen anyway when those units were built.
    std::string stagedOrderOnly;
    {
        std::vector<std::string> staged;
        for (auto& cu : plan.compileUnits) {
            if (!cu.servedFromCache) continue;
            if (cu.cachedObject.empty()) continue;
            auto obj = escape_ninja_path(cu.object);
            append(std::format("build {} : stage_file {}\n", obj,
                               escape_ninja_path(cu.cachedObject)));
            append("  verify = --verify size\n");
            staged.push_back(obj);
            if (cu.providesModule && !cu.cachedBmi.empty()) {
                auto bmi = bmi_path(*cu.providesModule);
                append(std::format("build {} : stage_file {}\n", bmi,
                                   escape_ninja_path(cu.cachedBmi)));
                append("  verify = --verify size\n");
                staged.push_back(bmi);
            }
        }
        if (!staged.empty()) {
            append("\n");
            // One phony aggregates them so each consuming edge names a single
            // prerequisite instead of repeating the whole list (mcpp#274: long
            // ninja lines are how a 50781-character command line blew past
            // cmd.exe's 8191 limit on Windows).
            append("build " + std::string(kStagedCachePhony) + " : phony");
            for (auto& s2 : staged) append(" " + s2);
            append("\n\n");
            stagedOrderOnly = " || " + std::string(kStagedCachePhony);
        }
    }

    if (dyndep) {
        // ── Phase 1: scan edges (one .ddi per TU). ──────────────────────
        // .ddi is placed beside the object so multi-version mangling can
        // namespace by package without producing two `build` rules with
        // the same `.ddi` output (mcpp#233: plan.cppm switches `cu.object`
        // from the flat `obj/<file>.o` to a path mirroring the source's
        // relative directory under a sanitized-package prefix whenever a
        // basename collides — `.ddi` follows that placement).
        // Skip .c files: they have no `import`s and don't need P1689 scan;
        // running them through cxx_scan would route them through g++ /
        // -fmodules which is exactly what C support is here to avoid.
        std::vector<std::string> ddi_paths;
        ddi_paths.reserve(plan.compileUnits.size());
        for (auto& cu : plan.compileUnits) {
            if (cu.servedFromCache) continue;   // staged, never scanned
            if (is_scan_exempt(cu.source))
                continue;
            auto ddi = (cu.object.parent_path() / cu.source.filename()).string() + ".ddi";
            ddi_paths.push_back(ddi);
            append(std::format("build {} : cxx_scan {}{}\n", escape_ninja_path(ddi),
                               escape_ninja_path(cu.source), stagedOrderOnly));
            append(std::format("  compile_target = {}\n", escape_ninja_path(cu.object)));
            if (auto includes = local_include_flags(cu, dial); !includes.empty())
                append(std::format("  local_includes ={}\n", includes));
            if (auto flags = join_flags(cu.packageCxxflags); !flags.empty())
                append(std::format("  unit_cxxflags ={}\n", flags));
        }
        append("\n");

        // ── Phase 2: per-file dyndep (P1 optimization). ────────────────
        // Each .ddi → .dd independently, so modifying one source file only
        // invalidates that file's .dd and its compile edge, not all edges.
        // Map ddi path → dd path for Phase 3 reference.
        std::map<std::string, std::string> ddi_to_dd;
        // Plan-vs-ddi reconciliation (design 2026-07-08 scanner doc §3d):
        // scan_overrides units ALWAYS carry their planned (provides, imports)
        // on the dyndep edge — the compiler's own P1689 scan audits the
        // author's assertion, per TU, failing the edge on divergence.
        // MCPP_VERIFY_MODGRAPH=1 (read at generation time) extends the
        // check to every module unit.
        const bool verifyAll = [] {
            const char* v = std::getenv("MCPP_VERIFY_MODGRAPH");
            return v && std::string_view(v) == "1";
        }();
        std::map<std::string, std::string> ddi_expect;
        for (auto& cu : plan.compileUnits) {
            if (cu.servedFromCache) continue;
            if (is_scan_exempt(cu.source)) continue;
            if (!cu.scanOverridden && !verifyAll) continue;
            auto ddi = (cu.object.parent_path() / cu.source.filename()).string() + ".ddi";
            std::string exp;
            if (cu.providesModule)
                exp += std::format("--expect-provides {}", *cu.providesModule);
            if (!cu.imports.empty()) {
                std::string csv;
                for (auto& m : cu.imports) {
                    if (!csv.empty()) csv += ",";
                    csv += m;
                }
                if (!exp.empty()) exp += " ";
                exp += std::format("--expect-imports {}", csv);
            }
            if (exp.empty()) exp = "--expect-none";
            ddi_expect[ddi] = std::move(exp);
        }
        for (auto& ddi : ddi_paths) {
            auto dd = ddi + ".dd";   // e.g. obj/cli.cppm.ddi.dd
            ddi_to_dd[ddi] = dd;
            append(std::format("build {} : cxx_dyndep {}\n", dd, ddi));
            if (auto it = ddi_expect.find(ddi); it != ddi_expect.end())
                append(std::format("  expect = {}\n", it->second));
        }
        append("\n");

        // ── Phase 3: compile edges with per-file dyndep. ────────────────
        // Each compile edge references its OWN .dd file instead of a global one.
        // P2: module compile edges get a $bmi_out variable for BMI preservation.
        for (auto& cu : plan.compileUnits) {
            if (cu.servedFromCache) continue;   // a stage_file edge owns these outputs
            std::string rule = pick_rule(cu.source);

            std::string out_line = "build " + escape_ninja_path(cu.object);
            if (cu.providesModule) {
                out_line += " | " + bmi_path(*cu.providesModule);
            }
            out_line += std::format(" : {} {}", rule, escape_ninja_path(cu.source));
            if (!is_scan_exempt(cu.source)) {
                auto ddi = (cu.object.parent_path() / cu.source.filename()).string() + ".ddi";
                auto it = ddi_to_dd.find(ddi);
                if (it != ddi_to_dd.end()) {
                    out_line += " | " + it->second;
                    out_line += stagedOrderOnly;
                    out_line += "\n  dyndep = " + it->second;
                    // P2: set bmi_out for the copy_if_different logic in cxx_module.
                    if (cu.providesModule) {
                        out_line += "\n  bmi_out = " + bmi_path(*cu.providesModule);
                    }
                    out_line += "\n";
                } else {
                    out_line += stagedOrderOnly + "\n";
                }
            } else {
                out_line += stagedOrderOnly + "\n";
            }
            if (auto includes = local_include_flags(cu, dial); !includes.empty())
                out_line += "  local_includes =" + includes + "\n";
            if (is_gas_source(cu.source) || is_nasm_source(cu.source)) {
                if (auto flags = join_flags(asm_unit_flags(cu)); !flags.empty())
                    out_line += "  unit_asmflags =" + flags + "\n";
            } else if (is_c_source(cu.source)) {
                if (auto flags = join_flags(cu.packageCflags); !flags.empty())
                    out_line += "  unit_cflags =" + flags + "\n";
            } else {
                if (auto flags = join_flags(cu.packageCxxflags); !flags.empty())
                    out_line += "  unit_cxxflags =" + flags + "\n";
            }
            append(std::move(out_line));
        }
        append("\n");
    } else {
        // ── Static-deps mode (M3.2 and earlier). ────────────────────────
        for (auto& cu : plan.compileUnits) {
            if (cu.servedFromCache) continue;   // a stage_file edge owns these outputs
            std::string rule = pick_rule(cu.source);

            std::string implicit;
            // C/asm files don't `import` modules; skip BMI implicit inputs.
            if (!is_scan_exempt(cu.source)) {
                for (auto& imp : cu.imports) {
                    if (imp == "std") {
                        if (has_std_artifacts)
                            implicit += " " + escape_ninja_path(std_bmi_dst);
                        continue;
                    }
                    if (imp == "std.compat") {
                        if (has_std_compat)
                            implicit += " " + escape_ninja_path(compat_bmi_dst);
                        else if (has_std_artifacts)
                            implicit += " " + escape_ninja_path(std_bmi_dst);
                        continue;
                    }
                    implicit += " " + bmi_path(imp);
                }
            }

            std::string out_line = "build " + escape_ninja_path(cu.object);
            if (cu.providesModule) {
                // Use implicit output (|) so $out only contains the .o file.
                // GCC writes BMI implicitly; Clang uses -fmodule-output=$bmi_out.
                out_line += " | " + bmi_path(*cu.providesModule);
            }
            out_line += std::format(" : {} {}", rule, escape_ninja_path(cu.source));
            if (!implicit.empty())
                out_line += " |" + implicit;
            out_line += stagedOrderOnly;
            out_line += "\n";
            if (auto includes = local_include_flags(cu, dial); !includes.empty())
                out_line += "  local_includes =" + includes + "\n";
            if (is_gas_source(cu.source) || is_nasm_source(cu.source)) {
                if (auto flags = join_flags(asm_unit_flags(cu)); !flags.empty())
                    out_line += "  unit_asmflags =" + flags + "\n";
            } else if (is_c_source(cu.source)) {
                if (auto flags = join_flags(cu.packageCflags); !flags.empty())
                    out_line += "  unit_cflags =" + flags + "\n";
            } else {
                if (auto flags = join_flags(cu.packageCxxflags); !flags.empty())
                    out_line += "  unit_cxxflags =" + flags + "\n";
            }
            // Clang needs $bmi_out to emit -fmodule-output=$bmi_out
            if (cu.providesModule) {
                out_line += "  bmi_out = " + bmi_path(*cu.providesModule) + "\n";
            }
            append(std::move(out_line));
        }
        append("\n");
    }

    // Link units
    for (auto& lu : plan.linkUnits) {
        std::string ins;
        // FIRST, before every other object. Mach-O runs __init_offsets in
        // LINK order and has no priority-ordered init section, so "runs
        // before the user's global constructors" is spelled "is the first
        // input" — nothing else about this edge achieves it (#336).
        if (need_ios_init_shim && lu.kind != LinkUnit::StaticLibrary)
            ins += " " + escape_ninja_path(ios_init_obj);
        for (auto& o : lu.objects) {
            ins += " " + escape_ninja_path(o);
        }

        std::string rule;
        switch (lu.kind) {
            case LinkUnit::Binary:
            case LinkUnit::TestBinary:
                if (has_std_artifacts)
                    ins += " " + escape_ninja_path(std_o_dst);
                if (has_std_compat)
                    ins += " " + escape_ninja_path(compat_o_dst);
                rule = "cxx_link";
                break;
            case LinkUnit::StaticLibrary:
                rule = "cxx_archive";
                break;
            case LinkUnit::SharedLibrary:
                if (has_std_artifacts)
                    ins += " " + escape_ninja_path(std_o_dst);
                if (has_std_compat)
                    ins += " " + escape_ninja_path(compat_o_dst);
                rule = "cxx_shared";
                break;
        }
        std::string implicit;
        for (auto& input : lu.implicitInputs) {
            implicit += " " + escape_ninja_path(input);
        }
        // Windows runtime-DLL deployment: an executable takes an implicit
        // dependency on each staged dep DLL (bin/<dll>), so ninja copies them
        // beside the .exe before the build is considered done. Empty on RPATH
        // platforms (no *.dll deps), so other targets are unaffected.
        if (lu.kind == LinkUnit::Binary || lu.kind == LinkUnit::TestBinary) {
            for (auto const& d : plan.runtimeDeployFiles)
                implicit += " " + escape_ninja_path(d.dest);
        }

        std::string out_line = std::format("build {} : {}{}{}\n",
            escape_ninja_path(lu.output), rule, ins,
            implicit.empty() ? std::string{} : " |" + implicit);
        if (auto flag = shared_soname_flag(lu); !flag.empty())
            out_line += "  soname_flag = " + flag + "\n";
        {
            // Per-unit C++ runtime link, by ROLE. The kind→role map is the
            // only place that knows a TestBinary runs on the build machine
            // while everything else leaves it; which flags that implies is
            // the contract table's business, not this emitter's (#336 —
            // before, this switch WAS the policy, and `static_stdlib = false`
            // could not reach the test side of it).
            std::string unit = join_flags(lu.linkFlags);
            unit += flags.ldStdlibFor(role_of(lu.kind));
            if (!unit.empty())
                out_line += "  unit_ldflags =" + unit + "\n";
        }
        append(std::move(out_line));

        for (auto const& alias : lu.runtimeAliases) {
            append(std::format("build {} : runtime_alias {}\n",
                escape_ninja_path(alias),
                escape_ninja_path(lu.output)));
        }
    }
    append("\n");

    // Windows runtime-DLL deployment: one copy edge per staged dep DLL. Emitted
    // once (deduped by dest in BuildPlan), reusing the generic stage_file rule
    // — which also means a DLL still loaded by a running program from a
    // previous `mcpp run` gets the skip-if-equivalent treatment instead of a
    // hard "cannot copy" failure.
    // Inert on RPATH platforms where runtimeDeployFiles is empty.
    for (auto const& d : plan.runtimeDeployFiles) {
        append(std::format("build {} : stage_file {}\n",
            escape_ninja_path(d.dest),
            escape_ninja_path(d.source)));
    }
    if (!plan.runtimeDeployFiles.empty())
        append("\n");

    if (!plan.linkUnits.empty()) {
        std::string defaults;
        for (auto& lu : plan.linkUnits) {
            defaults += " " + escape_ninja_path(lu.output);
            for (auto const& alias : lu.runtimeAliases) {
                defaults += " " + escape_ninja_path(alias);
            }
        }
        for (auto const& d : plan.runtimeDeployFiles) {
            defaults += " " + escape_ninja_path(d.dest);
        }
        append("default" + defaults + "\n");
    }

    return out;
}

// Name of the phony edge that aggregates an explicit goal set. Not a path —
// ninja resolves it in the build dir, and no rule produces a file by this name.
constexpr std::string_view kGoalPhony = "mcpp-requested-goals";

// Explicit goals go into the MANIFEST, not onto ninja's command line.
//
// `mcpp test` names every shared prerequisite as a goal so a broken package
// source fails once, as a package error, rather than N times as identical
// per-test compile failures (see execute.cppm phase A). For a large package
// that is thousands of object paths: FFmpeg's 2281 translation units produced
// a 50,781-character argv. Windows joins argv into a single command string for
// cmd.exe, which truncates at 8191 characters — the command never ran, and the
// bare 127 came back with no output at all, from cmd.exe rather than from
// ninja or mcpp.
//
// A phony edge expresses exactly the same goal set with a one-word command
// line. The manifest has no length limit, and ninja still builds the whole set
// in one invocation, so parallelism is unchanged.
//
// Returns the goal to put on the command line, or empty for "build default".
std::string append_goal_phony(std::string& manifest,
                              const std::vector<std::string>& goals) {
    if (goals.empty()) return {};
    manifest += std::format("\nbuild {} : phony", kGoalPhony);
    for (auto const& g : goals) {
        manifest += ' ';
        manifest += escape_ninja_path(g);
    }
    manifest += '\n';
    return std::string(kGoalPhony);
}

std::expected<BuildResult, BuildError> NinjaBackend::build(const BuildPlan& plan,
                                                           const BuildOptions& opts) {
    auto t0 = std::chrono::steady_clock::now();

    std::error_code ec;
    std::filesystem::create_directories(plan.outputDir, ec);
    if (ec)
        return std::unexpected(BuildError{std::format("cannot create output dir '{}': {}",
                                                      plan.outputDir.string(), ec.message()),
                                          plan.outputDir});

    auto ninja_path = plan.outputDir / "build.ninja";
    auto manifest = emit_ninja_string(plan);
    auto goalArg = append_goal_phony(manifest, opts.ninjaTargets);
    write_file(ninja_path, manifest);

    // compile_commands.json — via the dedicated module.
    auto flags = compute_flags(plan);
    write_compile_commands(plan, flags);

    // A distribution contract that could not be honored is reported, never
    // silently downgraded — the whole point of the model (INV-1/INV-4 in
    // .agents/docs/2026-08-02-issue336-pr142-analysis.md). Emitted here rather
    // than inside compute_flags, which runs twice per build.
    for (auto const& d : flags.diagnostics)
        mcpp::ui::warning(std::format("cxx_runtime: {}", d));

    // #336: the generated initializer-ordering TU. Written before ninja runs,
    // since the link edge lists its object as an input.
    if (flags.needsStreamInitShim) {
        std::error_code sec;
        std::filesystem::create_directories(plan.outputDir / "obj", sec);
        write_file(plan.outputDir / "obj" / "mcpp_ios_init.c",
                   mcpp::build::dist::stream_init_shim_source());
    }

    if (opts.dryRun) {
        BuildResult r;
        r.exitCode = 0;
        r.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0);
        return r;
    }

    // Hermetic link check: assert the sandbox toolchain resolves its CRT
    // objects + dynamic linker inside the sandbox BEFORE running the build —
    // catches both the bare-CRT link failure (#195) and silent host-library
    // contamination, cached per flag-set.
    if (auto h = verify_hermetic_link(plan.toolchain, flags.ld, plan.outputDir,
                                      plan.manifest.buildConfig.allowHostLibs); !h) {
        return std::unexpected(BuildError{h.error(), {}});
    }

    // When the toolchain comes from mcpp's private sandbox, use the
    // sandbox-local ninja absolute path (skip the system xlings ninja
    // shim which requires per-tool version pin activation).
    //
    // The compiler's internal `as`/`ld` lookup is handled via the
    // -B<binutils-bin> flag we emit into cxxflags/ldflags (see
    // emit_ninja_string). No PATH injection needed here.
    std::filesystem::path ninjaBin;
    auto ninja_name = std::string("ninja") + std::string(mcpp::platform::exe_suffix);
    if (auto nb = mcpp::xlings::paths::find_sibling_binary(
            plan.toolchain.binaryPath, "ninja", ninja_name)) {
        ninjaBin = *nb;
    }

    // Raw program path (no shell quoting): recorded in the fast-path cache and
    // exec'd directly via capture_exec/execvp, which take argv (not a shell
    // string). Shell-using call sites must quote it locally.
    std::string ninjaProgram = ninjaBin.empty() ? std::string("ninja")
                                                 : ninjaBin.string();

    // Record ninja binary for P0 fast-path cache.
    BuildResult r;
    r.ninjaProgram = ninjaProgram;
    if (!plan.toolchain.envOverrides.empty()) {
        // Toolchain-declared env (MSVC INCLUDE/LIB/PATH/VSLANG). Encode all
        // pairs (plus any runtime-dirs pair) into the fast-path cache's
        // single env slot: "@env" key + \x1f-separated k=v records — the
        // fast path must re-create this exact environment for ninja.
        r.runtimeEnvKey = "@env";
        std::string joined;
        auto add = [&](const std::string& k, const std::string& v) {
            if (!joined.empty()) joined += '\x1f';
            joined += k; joined += '='; joined += v;
        };
        if (auto runtimeEnv = runtime_env_for_dirs(plan.toolchain.compilerRuntimeDirs))
            add(runtimeEnv->first, runtimeEnv->second);
        for (auto& ev : plan.toolchain.envOverrides) add(ev.key, ev.value);
        r.runtimeEnvValue = std::move(joined);
    } else if (auto runtimeEnv = runtime_env_for_dirs(plan.toolchain.compilerRuntimeDirs)) {
        r.runtimeEnvKey = runtimeEnv->first;
        r.runtimeEnvValue = runtimeEnv->second;
    } else {
        r.runtimeEnvKey = "-";
    }

    // Direct exec (no /bin/sh): argv, not a shell string. capture_exec merges
    // stderr into the captured output (replacing the old `2>&1`), and applies
    // the runtime env to the child ONLY — so a bundled-glibc LD_LIBRARY_PATH
    // can never poison the host shell (the newer-glibc `sh:` crash class).
    std::vector<std::string> nargv{ninjaProgram};
    if (!opts.verbose)
        nargv.push_back("--quiet");
    nargv.push_back("-C");
    nargv.push_back(plan.outputDir.string());
    if (opts.verbose)
        nargv.push_back("-v");
    if (opts.parallelJobs)
        nargv.push_back(std::format("-j{}", opts.parallelJobs));

    if (opts.keepGoing) {
        nargv.push_back("-k");
        nargv.push_back("0");
    }

    // Explicit goal targets: ninja builds only these outputs (and their
    // prerequisites). Used by `mcpp test` to isolate per-test compiles. The set
    // travels through the manifest as a phony edge (append_goal_phony), so the
    // command line stays one word no matter how many goals there are.
    if (!goalArg.empty())
        nargv.push_back(goalArg);

    // Real env pairs for THIS run (the "@env" cache encoding above is only
    // for the fast path's later re-creation of the same environment).
    std::vector<std::pair<std::string, std::string>> nenv;
    if (auto runtimeEnv = runtime_env_for_dirs(plan.toolchain.compilerRuntimeDirs))
        nenv.emplace_back(runtimeEnv->first, runtimeEnv->second);
    for (auto& ev : plan.toolchain.envOverrides)
        nenv.emplace_back(ev.key, ev.value);

    bool buildTimedOut = false;
    auto cap = mcpp::platform::process::capture_exec_deadline(
        nargv, nenv,
        std::chrono::milliseconds(static_cast<long long>(opts.buildTimeoutSecs) * 1000),
        &buildTimedOut);
    std::string out = cap.output;
    bool ok = (cap.exit_code == 0) && !buildTimedOut;

    if (buildTimedOut) {
        // Report as a build failure with the partial ninja output attached —
        // the last edge ninja printed is the one that hung, which is the whole
        // point of having a deadline here.
        r.exitCode = 1;
        r.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0);
        return std::unexpected(BuildError{
            std::format("build timed out after {}s", opts.buildTimeoutSecs),
            plan.outputDir / "build.ninja", out, /*timedOut=*/true});
    }

    r.exitCode = ok ? 0 : 1;
    r.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);

    if (ok) {
        if (opts.verbose && !out.empty())
            std::fputs(out.c_str(), stdout);
        std::set<std::string> want(opts.ninjaTargets.begin(), opts.ninjaTargets.end());
        for (auto& lu : plan.linkUnits) {
            if (!want.empty() && !want.contains(lu.output.generic_string())) continue;
            r.producedArtifacts.push_back(plan.outputDir / lu.output);
            for (auto const& alias : lu.runtimeAliases) {
                r.producedArtifacts.push_back(plan.outputDir / alias);
            }
        }
    } else {
        auto prefixes = command_prefixes(flags, plan);
        auto diagnostics = opts.verbose ? out : filter_ninja_output(out, prefixes);
        return std::unexpected(BuildError{"build failed", plan.outputDir / "build.ninja",
                                          std::move(diagnostics)});
    }
    return r;
}

std::unique_ptr<Backend> make_ninja_backend() {
    return std::make_unique<NinjaBackend>();
}

}  // namespace mcpp::build

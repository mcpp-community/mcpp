// mcpp.toolchain.probe - common compiler probing helpers.
//
// A compiler is asked by argument vector, with the environment it needs as
// additions (mcpp.run): no command interpreter stands between mcpp and the
// compiler it queries.

export module mcpp.toolchain.probe;

import std;
import mcpp.toolchain.model;
import mcpp.toolchain.triple;
import mcpp.provider;
import mcpp.provider.select;
import mcpplibs.os.platform;
import mcpp.toolchain.apple_sdk;
import mcpp.invocation;
import mcpp.effects;
import mcpp.build.command_line;
import mcpp.run;
import mcpp.log;
import mcpp.home;                    // the probe cache lives under the home's cache
import mcpp.toolchain.fingerprint;   // hash_string
import mcpp.fallback.sysroot_complete;
import mcpp.fallback.probe_sysroot;

export namespace mcpp::toolchain {

// A compiler query: `argv` with `env` added, its standard output (with its
// standard error when `withErrors`), answered from the probe cache when the
// same query of the same files was answered before.
std::expected<std::string, DetectError> run_capture(const std::vector<std::string>& argv,
                                                    const mcpp::run::Env& env = {},
                                                    bool withErrors = false);

// Whether the compiler writes compressed debug sections (`-gz=zlib`), and the
// linker of its toolchain reads them -- a property of how the toolchain was
// built: an LLVM built without zlib warns and writes them uncompressed, and its
// `ld.lld` refuses `--compress-debug-sections` (measured: the Windows-hosted
// LLVM 23.1.3 of the xim index); a GCC configured without support refuses
// `-gz`. Asked of the driver alone (`-###`), which exits 0 either way, so the
// answer is kept in the probe cache (design 2026-10-31 §4.1).
bool compresses_debug_sections(const std::filesystem::path& compiler, const mcpp::run::Env& env = {});

std::string extract_version(std::string_view s);
std::string first_line_of(std::string_view s);
std::string lower_copy(std::string_view s);
std::string trim_line(std::string s);
std::string normalize_driver_output(std::string_view s);

std::vector<std::filesystem::path>
discover_compiler_runtime_dirs(const std::filesystem::path& compilerBin);

std::vector<std::filesystem::path>
discover_link_runtime_dirs(const std::filesystem::path& compilerBin,
                           std::string_view targetTriple);

// What a compiler of this toolchain needs added to its environment: the
// directories of its own runtime libraries on the loader search path (Linux).
mcpp::run::Env compiler_env(const Toolchain& tc);
// The same, as the prefix of a command line composed as text (a std module's
// build commands, recorded in its metadata).
std::string compiler_env_prefix(const Toolchain& tc);

std::expected<std::filesystem::path, DetectError>
probe_compiler_binary(const std::filesystem::path& explicit_compiler = {});

std::expected<std::string, DetectError>
probe_target_triple(const std::filesystem::path& compilerBin,
                    const mcpp::run::Env& env);

std::filesystem::path
probe_sysroot(const std::filesystem::path& compilerBin,
              const mcpp::run::Env& env);

// Resolve the payload paths for an EXPLICIT runtime binding ("glibc@2.39").
//
// The version is named by the caller's authority, never searched for. An
// empty binding returns nullopt: declining PayloadFirst is correct, guessing
// is what split the compile and run halves apart (see §3.2 of
// 2026-08-08-payload-version-and-contract-drift-design.md).
std::optional<PayloadPaths>
probe_payload_paths(const std::filesystem::path& compilerBin,
                    std::string_view runtimeBinding);

// Ensure sysroot directory has complete headers by symlinking from
// payload xpkgs. Called when GCC's probed sysroot exists but may
// be missing linux kernel headers or glibc headers.
void ensure_sysroot_complete(const std::filesystem::path& sysroot,
                             const PayloadPaths& pp);

} // namespace mcpp::toolchain

namespace mcpp::toolchain {

namespace {

// The directories an LLVM payload keeps its runtime libraries in beside lib/:
// the triple-named ones on Linux, darwin's on macOS.
std::vector<std::filesystem::path> llvm_runtime_lib_dirs(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> dirs;
    std::error_code ec;
    auto add = [&](const std::filesystem::path& p) {
        if (std::filesystem::exists(p, ec)) dirs.push_back(p);
    };
    if constexpr (mcpplibs::os::is_linux) {
        add(root / "lib" / "x86_64-unknown-linux-gnu");
        add(root / "lib" / "aarch64-unknown-linux-gnu");
        add(root / "lib" / "aarch64-linux-gnu");
    } else if constexpr (mcpplibs::os::is_macos) {
        add(root / "lib" / "aarch64-apple-darwin");
        add(root / "lib" / "darwin");
    }
    return dirs;
}

void append_existing_unique(std::vector<std::filesystem::path>& out,
                            const std::filesystem::path& p) {
    std::error_code ec;
    if (p.empty() || !std::filesystem::exists(p, ec)) return;
    auto abs = std::filesystem::absolute(p, ec);
    if (ec) abs = p;
    if (std::find(out.begin(), out.end(), abs) == out.end())
        out.push_back(abs);
}

std::string join_colon_paths(const std::vector<std::filesystem::path>& dirs) {
    std::string joined;
    for (auto& d : dirs) {
        if (!joined.empty()) joined += ':';
        joined += d.string();
    }
    return joined;
}


} // namespace

namespace {
// THE ANSWERS OF A COMPILER'S QUERIES, CACHED (design 2026-10-26 §8). Every
// plan asked the same compiler `--version`, `-dumpmachine` and `-print-sysroot`
// through a shell and an `env` -- three processes each, about 20 ms of a 100 ms
// query -- and a compiler answers them identically until its files change.
// Keyed by the command text and by the size and modification time of every
// file the command names (the compiler, its configuration), so a payload
// replaced in place, a different environment prefix or another compiler is a
// different key. Only a successful answer is kept; a failed query runs again.
std::string probe_cache_key(const std::vector<std::string>& argv, const mcpp::run::Env& env,
                            bool withErrors) {
    std::string key = withErrors ? "merged" : "output";
    for (auto const& [k, v] : env) key += std::format("\x1e{}={}", k, v);
    for (auto const& a : argv) key += "\x1f" + a;
    for (auto const& a : argv) {
        const std::filesystem::path p(a);
        std::error_code ec;
        if (p.is_absolute() && std::filesystem::is_regular_file(p, ec)) {
            const auto size = std::filesystem::file_size(p, ec);
            const auto time = std::filesystem::last_write_time(p, ec);
            key += std::format("\n{}\t{}\t{}", p.generic_string(),
                               static_cast<std::uint64_t>(size),
                               static_cast<std::int64_t>(time.time_since_epoch().count()));
        }
    }
    return key;
}
std::filesystem::path probe_cache_file(const std::string& key) {
    return mcpp::home::root() / "cache" / "probe" / (mcpp::toolchain::hash_string(key) + ".out");
}
}  // namespace

bool compresses_debug_sections(const std::filesystem::path& compiler, const mcpp::run::Env& env) {
    // An LLVM toolchain links with its own lld, which is what refuses the
    // option without zlib -- its clang does not even warn (measured on the
    // Windows-hosted LLVM 23.1.3). Asked by one real link: an empty unit with
    // debug information, compiled for an ELF target and linked compressed. The
    // answer, either way, is kept beside the probe cache under the identity of
    // that lld (its path, size and time).
    if (compiler.filename().string().find("clang") != std::string::npos) {
        const auto dir = compiler.parent_path();
        for (std::string_view name : {"ld.lld", "ld.lld.exe", "lld", "lld.exe"}) {
            const auto lld = dir / name;
            std::error_code lec;
            if (!std::filesystem::exists(lld, lec)) continue;
            const auto work = mcpp::home::root() / "cache" / "probe" / "debug-compression";
            const auto identity = std::format("{}|{}|{}", lld.generic_string(),
                std::filesystem::file_size(lld, lec),
                std::filesystem::last_write_time(lld, lec).time_since_epoch().count());
            const auto answer = work / (mcpp::toolchain::hash_string(identity) + ".answer");
            if (std::ifstream in(answer); in) {
                std::string v;
                in >> v;
                if (v == "yes" || v == "no") return v == "yes";
            }
            std::filesystem::create_directories(work, lec);
            const auto src = work / "empty.c";
            if (!std::filesystem::exists(src, lec)) std::ofstream(src, std::ios::binary).flush();
            const auto obj = work / "empty.o";
            const auto out = work / "empty.so";
            auto compiled = mcpp::run::execute(
                {compiler.string(), "--target=x86_64-unknown-linux-gnu", "-g", "-c", "-x", "c",
                 src.string(), "-o", obj.string()},
                {.env = env, .host_tool = true, .streams = mcpp::run::Streams::merged,
                 .report_spawn_failure = false});
            if (compiled.exit_code != 0) return false;   // not asked; nothing kept
            std::vector<std::string> link{lld.string()};
            if (name.starts_with("lld")) { link.push_back("-flavor"); link.push_back("gnu"); }
            for (auto const& a : {std::string("--compress-debug-sections=zlib"), std::string("-shared"),
                                  obj.string(), std::string("-o"), out.string()})
                link.push_back(a);
            auto linked = mcpp::run::execute(link, {.env = env, .host_tool = true,
                                                    .streams = mcpp::run::Streams::merged,
                                                    .report_spawn_failure = false});
            const bool yes = linked.exit_code == 0;
            mcpp::log::verbose("toolchain", std::format("{} writes compressed debug sections: {}{}",
                               lld.string(), yes ? "yes" : "no",
                               yes ? std::string{} : " (" + first_line_of(linked.output) + ")"));
            if (mcpp::effects::writes_global_cache()) std::ofstream(answer) << (yes ? "yes\n" : "no\n");
            return yes;
        }
    }
    // An empty C file the driver may name: `-###` reads no input, but a driver
    // checks that it exists.
    const auto empty = mcpp::home::root() / "cache" / "probe" / "empty.c";
    std::error_code ec;
    if (!std::filesystem::exists(empty, ec)) {
        std::filesystem::create_directories(empty.parent_path(), ec);
        std::ofstream(empty, std::ios::binary).flush();
    }
    auto r = run_capture({compiler.string(), "-gz=zlib", "-###", "-c", "-x", "c",
                          empty.string(), "-o", (empty.parent_path() / "empty.o").string()},
                         env, /*withErrors=*/true);
    if (!r) return false;
    return r->find("cannot compress debug sections") == std::string::npos
        && r->find("-gz is not supported") == std::string::npos
        && r->find("unsupported option '-gz") == std::string::npos;
}

std::expected<std::string, DetectError> run_capture(const std::vector<std::string>& argv,
                                                    const mcpp::run::Env& env, bool withErrors) {
    const auto key = probe_cache_key(argv, env, withErrors);
    const auto cached = probe_cache_file(key);
    if (std::ifstream in(cached, std::ios::binary); in) {
        std::string recordedKey;
        std::getline(in, recordedKey, '\0');
        if (recordedKey == key) return std::string(std::istreambuf_iterator<char>(in), {});
    }
    auto r = mcpp::run::execute(argv, { .env = env, .host_tool = true,
                                        .streams = withErrors ? mcpp::run::Streams::merged
                                                              : mcpp::run::Streams::output,
                                        .report_spawn_failure = false });
    const auto cmd = mcpp::build::command_line::display(argv);
    // Written only by a run that writes the global cache: a query (`self
    // doctor`, `toolchain list`) reads the answer and leaves the home as it
    // found it, and a cold home is never created for it.
    std::error_code homeEc;
    if (r.exit_code == 0 && mcpp::effects::writes_global_cache()
        && std::filesystem::is_directory(mcpp::home::root(), homeEc)) {
        std::error_code ec;
        std::filesystem::create_directories(cached.parent_path(), ec);
        auto tmp = cached;
        tmp += std::format(".{:x}", std::random_device{}());
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out << key << '\0' << r.output;
        }
        std::filesystem::rename(tmp, cached, ec);
        if (ec) std::filesystem::remove(tmp, ec);
    }
    if (r.exit_code != 0 && r.output.empty()) {
        return std::unexpected(DetectError{std::format("failed to execute: {}", cmd)});
    }
    if (r.exit_code != 0) {
        return std::unexpected(DetectError{
            std::format("'{}' exited with status {}", cmd, r.exit_code)});
    }
    return r.output;
}

std::string extract_version(std::string_view s) {
    std::string out;
    bool seen_digit = false;
    int dots = 0;
    for (char c : s) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            out.push_back(c);
            seen_digit = true;
        } else if (c == '.' && seen_digit && dots < 2) {
            out.push_back('.');
            ++dots;
        } else if (seen_digit) {
            break;
        }
    }
    return out;
}

std::string first_line_of(std::string_view s) {
    auto end = s.find('\n');
    return std::string(s.substr(0, end));
}

std::string lower_copy(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

std::string trim_line(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    while (!s.empty() && (s.front() == '\n' || s.front() == '\r' || s.front() == ' '))
        s.erase(s.begin());
    return s;
}

std::string normalize_driver_output(std::string_view s) {
    auto replace_local_paths = [](std::string line) {
        static constexpr std::array<std::string_view, 3> prefixes{
            "/home/", "/tmp/", "/var/"
        };
        for (auto prefix : prefixes) {
            std::size_t pos = 0;
            while ((pos = line.find(prefix, pos)) != std::string::npos) {
                auto end = pos;
                while (end < line.size()) {
                    unsigned char c = static_cast<unsigned char>(line[end]);
                    if (std::isspace(c) || line[end] == '\'' || line[end] == '"')
                        break;
                    ++end;
                }
                line.replace(pos, end - pos, "<PATH>");
                pos += std::string_view("<PATH>").size();
            }
        }
        return line;
    };

    std::string out;
    std::istringstream is(std::string{s});
    std::string line;
    while (std::getline(is, line)) {
        line = trim_line(std::move(line));
        if (line.empty()) continue;
        if (line.starts_with("PWD=")) continue;
        line = replace_local_paths(std::move(line));
        if (!out.empty()) out.push_back('\n');
        out += line;
    }
    return out;
}

std::vector<std::filesystem::path>
discover_compiler_runtime_dirs(const std::filesystem::path& compilerBin) {
    std::vector<std::filesystem::path> dirs;
    auto root = compilerBin.parent_path().parent_path();

    auto rootStr = root.string();
    auto exe = compilerBin.filename().string();
    bool looksLikeLlvm = mcpp::provider::layout().entry_of(root).name == "llvm"
                      || exe.find("clang") != std::string::npos;
    if (looksLikeLlvm) {
        append_existing_unique(dirs, root / "lib");
        for (auto& d : llvm_runtime_lib_dirs(root))
            append_existing_unique(dirs, d);
    }

    if (auto rt = mcpp::provider::beside_tool(compilerBin, "gcc-runtime")) {
        append_existing_unique(dirs, *rt / "lib64");
        append_existing_unique(dirs, *rt / "lib");
    }
    return dirs;
}

std::vector<std::filesystem::path>
discover_link_runtime_dirs(const std::filesystem::path& compilerBin,
                           std::string_view targetTriple) {
    std::vector<std::filesystem::path> dirs;

    // THE COMPILER'S OWN RUNTIME IS NOT THE ARTIFACT'S, AND FOR EVERY ROW
    // BEFORE THESE TWO THE DISTINCTION DID NOT MATTER.
    //
    // What this returns goes on the ARTIFACT's link line as `-L` and `-rpath`
    // (flags.cppm's runtime_dirs). For a payload that targets its own host
    // that is right: the libstdc++ beside the compiler is the libstdc++ the
    // artifact links. An SDK that cross-compiles breaks the coincidence --
    // `<emsdk>/lib` holds the host x86-64 libraries its own clang needs to
    // RUN, and its sibling `xim:gcc-runtime` is a runtime dependency of the
    // toolchain, not of a wasm module.
    //
    // Measured on `--target wasm32-emscripten`, after the sysroot and link
    // model were already gated:
    //
    //   wasm-ld: error: unknown file type:
    //     <xim-x-gcc-runtime>/lib64/libatomic.so
    //
    // and `atomic_link_flag` had found that libatomic precisely because this
    // function had put its directory on the link dirs. The compiler's own
    // needs are `compilerRuntimeDirs`, which this does not touch and which
    // still gets them.
    if (auto tt = triple::parse(targetTriple); tt && tt->has_own_sysroot())
        return dirs;

    auto root = compilerBin.parent_path().parent_path();
    if (!targetTriple.empty())
        append_existing_unique(dirs, root / "lib" / std::string(targetTriple));
    for (auto& d : llvm_runtime_lib_dirs(root))
        append_existing_unique(dirs, d);
    append_existing_unique(dirs, root / "lib");

    if constexpr (mcpplibs::os::is_linux) {
        if (auto rt = mcpp::provider::beside_tool(compilerBin, "gcc-runtime")) {
            append_existing_unique(dirs, *rt / "lib64");
            append_existing_unique(dirs, *rt / "lib");
        }
    }
    return dirs;
}

mcpp::run::Env compiler_env(const Toolchain& tc) {
    if constexpr (!mcpplibs::os::is_linux) return {};
    if (tc.compilerRuntimeDirs.empty()) return {};
    std::string joined;
    for (auto const& d : tc.compilerRuntimeDirs) {
        if (!joined.empty()) joined += ':';
        joined += d.string();
    }
    return { { "LD_LIBRARY_PATH", joined } };
}

std::string compiler_env_prefix(const Toolchain& tc) {
    return mcpp::build::command_line::clean_ld_library_path_prefix(tc.compilerRuntimeDirs);
}

std::expected<std::filesystem::path, DetectError>
probe_compiler_binary(const std::filesystem::path& explicit_compiler) {
    if (!explicit_compiler.empty()) {
        mcpp::log::verbose("probe", std::format("explicit compiler: {}", explicit_compiler.string()));
        if (!std::filesystem::exists(explicit_compiler)) {
            return std::unexpected(DetectError{std::format(
                "explicit compiler path does not exist: {}",
                explicit_compiler.string())});
        }
        return explicit_compiler;
    }

    std::string cxx;
    if (auto e = mcpp::invocation::get("CXX")) {
        cxx = *e;
    } else {
        cxx = "g++";
    }

    auto found = mcpp::invocation::which(cxx);
    if (!found) {
        return std::unexpected(DetectError{std::format("compiler '{}' not found in PATH", cxx)});
    }
    mcpp::log::verbose("probe", std::format("resolved compiler: {} → {}", cxx, found->string()));
    return *found;
}

std::expected<std::string, DetectError>
probe_target_triple(const std::filesystem::path& compilerBin,
                    const mcpp::run::Env& env) {
    auto triple_r = run_capture({ compilerBin.string(), "-dumpmachine" }, env);
    if (!triple_r) return std::unexpected(triple_r.error());
    return trim_line(*triple_r);
}

std::filesystem::path
probe_sysroot(const std::filesystem::path& compilerBin,
              const mcpp::run::Env& env) {
    // A sysroot is only usable if it actually carries the C library headers.
    // A merely-existing directory (e.g. a partially-bootstrapped sandbox
    // subos) would silently shadow the payload -isystem fallback and produce
    // "stdlib.h: No such file or directory" deep inside the std module build.
    auto usable = [](const std::filesystem::path& root) {
        return std::filesystem::exists(root / "usr" / "include" / "stdlib.h")   // glibc layout
            || std::filesystem::exists(root / "include" / "stdlib.h");          // musl layout
    };

    // 1. Ask the compiler directly (works for GCC; Clang often doesn't support it).
    auto r = run_capture({ compilerBin.string(), "-print-sysroot" }, env);
    if (r) {
        auto s = trim_line(*r);

        // A usable sysroot that belongs to somebody else is still somebody
        // else's. gcc records this path as a string when it is built and
        // reports it forever after; on a machine with several checkouts the
        // recorded one routinely exists AND carries headers, so accepting it
        // on usability alone hands the build another project's tree. Measured
        // right here: this repo's gcc reported a sysroot under an unrelated
        // one, and every build took its headers.
        //
        // The ownership test lives inside remap_xlings_baked_sysroot, which is
        // exactly why the order matters -- the early return below reached it
        // only when the path was missing, so the case the remap exists for was
        // the one case it never saw.
        const bool ownedByThisHome =
            !s.empty() && mcpp::fallback::sysroot_is_owned(s, compilerBin);

        if (ownedByThisHome && usable(s)) return s;
        if (!s.empty() && std::filesystem::exists(s) && !usable(s))
            mcpp::log::debug("probe", std::format(
                "sysroot '{}' exists but lacks usr/include/stdlib.h — ignoring", s));

        // GCC bakes the build-time sysroot into the binary. For xlings-built
        // GCC this is a path like <buildhost>/.xlings/subos/default that
        // doesn't exist on the user's machine -- or exists and belongs to a
        // different one. Remap via fallback module.
        if (auto remapped = mcpp::fallback::remap_xlings_baked_sysroot(s, compilerBin)) {
            if (usable(*remapped)) return *remapped;
            mcpp::log::debug("probe", std::format(
                "remapped sysroot '{}' lacks usr/include/stdlib.h — ignoring",
                remapped->string()));
        }

        // Last resort: a foreign but usable sysroot beats no sysroot. This is
        // the pre-existing behaviour, kept for machines that have no registry
        // subos to remap to -- it is a worse answer, not a wrong one, and
        // taking nothing here would break them outright.
        if (!s.empty() && std::filesystem::exists(s) && usable(s)) {
            mcpp::log::verbose("probe", std::format(
                "using sysroot '{}', which is outside this toolchain's "
                "registry — no equivalent found under it", s));
            return std::filesystem::path(s);
        }
    }

    // 2. macOS fallback: use xcrun to discover the SDK path.
    //
    // NOTE: mcpp used to also mine the Clang driver cfg for --sysroot here.
    // That trust was dead code walking: the cfg is an install-time-generated
    // artifact that mcpp's own fixup pipeline now REGENERATES without a
    // --sysroot line (the C library comes from the payload link model), so
    // the mined value existed only on never-fixed-up installs and pointed at
    // an environment directory the payload doesn't own. The cfg serves
    // direct driver invocations only; builds derive everything from the
    // link model. Kept as a diagnostic only.
    if (auto cfg = mcpp::fallback::parse_clang_cfg_sysroot(compilerBin)) {
        mcpp::log::debug("probe", std::format(
            "clang cfg declares sysroot '{}' — ignored (payload-first model)",
            cfg->string()));
    }
    if (auto sdk = mcpp::fallback::probe_macos_sdk_sysroot())
        return *sdk;

    mcpp::log::debug("probe", "no sysroot found");
    return {};
}

// Resolve `<name>@<version>` to the payload ROOT for that exact version.
//
// The version is named, never searched: the caller has an authority (the
// resolved runtime binding) and this turns it into an address. Two payload
// roots are tried in the order the rest of the file uses -- the compiler's own
// siblings first, then the active home -- because an inherited or symlinked
// compiler resolves into its owner home while the active home may be the one
// that just installed the payload.
std::optional<std::filesystem::path>
payload_root_for_binding(const std::filesystem::path& compilerBin,
                         std::string_view binding) {
    const auto at = binding.find('@');
    if (at == std::string_view::npos || at == 0 || at + 1 >= binding.size())
        return std::nullopt;
    const auto name    = binding.substr(0, at);
    const auto version = std::string(binding.substr(at + 1));

    // THE DIRECTORY IS NAMED BY THE EXACT VERSION. The binding carries the
    // version whose payload `ensure_declared_runtime` installed, so no nearby
    // version is accepted in its place (mcpp#660).
    //
    // THIS SITE'S FAILURE DOES NOT NAME A VERSION. The fixup says which
    // payload it wanted; here the include directory is simply never added,
    // and what the user reads comes from inside libstdc++:
    //
    //     bits/os_defines.h:39: fatal error: features.h: No such file
    const auto exact = [&](const std::filesystem::path& xpkgs)
        -> std::optional<std::filesystem::path> {
        auto root = mcpp::provider::layout().tool_in(xpkgs, name) / version;
        std::error_code ec;
        if (std::filesystem::is_directory(root, ec)) return root;
        return std::nullopt;
    };
    // Compiler siblings: <...>/xpkgs/xim-x-<name>/<version>
    if (auto xpkgs = mcpp::provider::store_of(compilerBin)) {
        if (auto root = exact(*xpkgs)) return *root;
    }
    // Active home.
    if (auto xpkgs = mcpp::provider::home_store()) {
        if (auto root = exact(*xpkgs)) return *root;
    }
    return std::nullopt;
}

// `runtimeBinding` is the libc identity from the root-selected immutable
// RuntimeBinding snapshot. Empty is a refusal, not a licence to guess: a guess
// here is how the compile side and the artifact's interpreter came to name
// different glibc versions, and nothing about the resulting binary looks
// wrong until it loads a library built against the other one.
std::optional<PayloadPaths>
probe_payload_paths(const std::filesystem::path& compilerBin,
                    std::string_view runtimeBinding) {
    if (runtimeBinding.empty()) {
        mcpp::log::verbose("probe",
            "no runtime binding resolved — declining PayloadFirst rather than "
            "picking a libc by directory order");
        return std::nullopt;
    }
    // Only a libc family has a payload of this shape. `macos_sdk`/`ucrt` are
    // resolved elsewhere, and a musl target uses the sysroot mode.
    if (!runtimeBinding.starts_with("glibc@")) {
        mcpp::log::verbose("probe", std::format(
            "runtime '{}' is not a glibc payload — no PayloadFirst",
            std::string(runtimeBinding)));
        return std::nullopt;
    }

    auto glibc = payload_root_for_binding(compilerBin, runtimeBinding);
    if (!glibc) {
        mcpp::log::verbose("probe", std::format(
            "runtime '{}' is not installed in this home",
            std::string(runtimeBinding)));
        return std::nullopt;
    }

    // Layout WITHIN the chosen payload. This convention stays: it answers
    // "where inside", not "which one".
    auto glibcInclude = *glibc / "include";
    if (!std::filesystem::exists(glibcInclude / "features.h"))
        return std::nullopt;

    auto glibcLib = *glibc / "lib64";
    if (!std::filesystem::exists(glibcLib))
        glibcLib = *glibc / "lib";
    if (!std::filesystem::exists(glibcLib))
        return std::nullopt;

    PayloadPaths pp;
    pp.glibcInclude = glibcInclude;
    pp.glibcLib     = glibcLib;

    // Find linux kernel headers (optional — search across index prefixes,
    // then the active home registry). Require the actual payload: a
    // delegating index package (xim:linux-headers → scode:linux-headers)
    // leaves a metadata-only husk under its own prefix, and the discovery
    // must skip it instead of giving up (issue #120: glibc's local_lim.h
    // needs <linux/limits.h>, so a silent miss breaks every glibc build).
    constexpr std::string_view kLinuxLimits = "include/linux/limits.h";
    auto linuxHeaders =
        mcpp::provider::beside(compilerBin, "linux-headers", kLinuxLimits);
    if (!linuxHeaders)
        linuxHeaders = mcpp::provider::in_home_store("linux-headers", kLinuxLimits);
    if (linuxHeaders) {
        pp.linuxInclude = *linuxHeaders / "include";
    } else {
        mcpp::log::verbose("probe",
            "linux-headers payload not found under any index prefix — "
            "glibc builds will fail at <linux/limits.h>");
    }

    mcpp::log::verbose("probe", std::format(
        "payload paths: glibcLib='{}' linuxInclude='{}'",
        pp.glibcLib.string(),
        pp.linuxInclude.empty() ? "(none)" : pp.linuxInclude.string()));
    return pp;
}

void ensure_sysroot_complete(const std::filesystem::path& sysroot,
                             const PayloadPaths& pp) {
    mcpp::fallback::ensure_sysroot_complete(sysroot, pp);
}

} // namespace mcpp::toolchain

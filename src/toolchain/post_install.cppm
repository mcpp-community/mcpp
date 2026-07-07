// mcpp.toolchain.post_install — toolchain payload post-install fixups (patchelf / specs / cfg)
//
// Extracted verbatim from cli.cppm (cli modularization, see
// .agents/docs/2026-06-10-cli-modularization.md). Zero behavior change:
// bodies are byte-identical moves; only the surrounding module/namespace
// changed (mcpp::cli::detail -> mcpp::cli).

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.toolchain.post_install;

import std;
import mcpp.config;
import mcpp.libs.json;
import mcpp.log;
import mcpp.platform;
import mcpp.toolchain.linkmodel;
import mcpp.toolchain.registry;
import mcpp.ui;
import mcpp.xlings;

namespace mcpp::toolchain {

// Run patchelf on every dynamic ELF in `dir` (recursively):
//   - Set PT_INTERP to `loader` (the sandbox-local glibc loader).
//   - Set RUNPATH to `rpath` (colon-separated list of sandbox lib dirs).
// Idempotent; skips static binaries and shared libs without PT_INTERP.
//
// TODO(xlings/libxpkg-upstream): xim 0.4.10's `elfpatch.auto({interpreter=...})`
// is supposed to do this in install hooks but currently scans 0 files for
// some packages (verified empirically: `binutils: elfpatch auto: 0 0 0`).
// Once the upstream legacy elfpatch path is fixed, this mcpp-side walker
// can be deleted.
export void patchelf_walk(const std::filesystem::path& dir,
                   const std::filesystem::path& loader,
                   const std::string& rpath,
                   const std::filesystem::path& patchelfBin)
{
    if (!std::filesystem::exists(dir) || !std::filesystem::exists(patchelfBin))
        return;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
         it != std::filesystem::recursive_directory_iterator{}; it.increment(ec))
    {
        if (ec) { ec.clear(); continue; }
        if (!it->is_regular_file(ec)) continue;
        auto path = it->path();
        // Skip non-ELF (cheap magic check)
        std::ifstream is(path, std::ios::binary);
        char m[4]{};
        is.read(m, 4);
        if (!is || m[0] != 0x7f || m[1] != 'E' || m[2] != 'L' || m[3] != 'F')
            continue;
        is.close();
        // Probe PT_INTERP — skip static binaries (no interp).
        auto probe = std::format("{} --print-interpreter {} 2>/dev/null",
                                 mcpp::platform::shell::quote(patchelfBin.string()),
                                 mcpp::platform::shell::quote(path.string()));
        auto probeResult = mcpp::platform::process::capture(probe);
        bool hasInterp = (probeResult.exit_code == 0 && !probeResult.output.empty());

        // Patch a COPY and atomically rename it into place. The payload can
        // contain libraries the CURRENT process has mmapped (a self-hosted
        // mcpp links the sandbox glibc/libgcc_s, and since the fixup
        // pipeline runs on every install path, the patching process may BE
        // such a consumer). In-place patchelf rewrites the backing file of
        // those live mappings and corrupts the running process — observed
        // on CI as an exit-time SIGSEGV in _dl_fini jumping to an
        // unrelocated address. rename() gives the patched content a fresh
        // inode while live processes keep the old one.
        auto tmp = path;
        tmp += ".mcpp-patch.tmp";
        {
            std::error_code cec;
            std::filesystem::copy_file(
                path, tmp, std::filesystem::copy_options::overwrite_existing, cec);
            if (cec) continue;
            std::filesystem::permissions(
                tmp, std::filesystem::status(path, cec).permissions(),
                std::filesystem::perm_options::replace, cec);
        }
        bool patched = true;
        if (hasInterp) {
            patched = (mcpp::platform::process::run_silent(std::format(
                "{} --set-interpreter {} {} 2>/dev/null",
                mcpp::platform::shell::quote(patchelfBin.string()),
                mcpp::platform::shell::quote(loader.string()),
                mcpp::platform::shell::quote(tmp.string()))) == 0) && patched;
        }
        // Always set RUNPATH (works on .so too — they need to find deps).
        if (!rpath.empty()) {
            patched = (mcpp::platform::process::run_silent(std::format(
                "{} --set-rpath {} {} 2>/dev/null",
                mcpp::platform::shell::quote(patchelfBin.string()),
                mcpp::platform::shell::quote(rpath),
                mcpp::platform::shell::quote(tmp.string()))) == 0) && patched;
        }
        std::error_code rec;
        if (patched) std::filesystem::rename(tmp, path, rec);
        if (!patched || rec) std::filesystem::remove(tmp, rec);
    }
}

// xim bakes the installing user's XLINGS_HOME into gcc specs at install
// time (as `--dynamic-linker` and `-rpath`). When mcpp uses its own
// isolated sandbox (MCPP_HOME/registry/), the baked-in paths point to
// xlings' home, not mcpp's sandbox glibc — binaries would fail to exec.
//
// Mcpp does a post-install spec rewrite:
//   - Dynamically detects the baked-in loader path from the specs file
//   - Replaces it with the sandbox glibc payload's loader
//   - Replaces the rpath with <glibc_lib>:<gcc_lib64>
// Idempotent — skips if already pointing at the correct glibc.
// Extract the baked-in glibc loader path (".../ld-linux-<arch>.so.N") from a
// gcc specs file. xim bakes the installing user's XLINGS_HOME into specs at
// install time, so the DIR varies per machine, and the loader NAME varies
// per arch — detect both instead of hardcoding either.
export std::string detect_baked_loader(const std::string& specsContent) {
    // Path-character whitelist. Specs embed loader paths inside %-spec
    // syntax (`%{mmusl:...;:/baked/dir/ld-linux-x86-64.so.2}`), so scanning
    // to "whitespace or :;" is NOT a valid boundary — it would swallow the
    // closing braces, and replacing that string corrupts the spec grammar
    // ("braced spec body ... is invalid" from every subsequent g++ run).
    auto is_path_char = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c))
            || c == '/' || c == '.' || c == '-' || c == '_' || c == '+';
    };

    // The baked GNU loader is the ld-linux entry whose directory is NOT a
    // standard /lib* location — specs also contain pristine defaults
    // (/lib/ld-linux.so.2, /libx32/…) for other multilib branches that must
    // never be rewritten.
    constexpr std::string_view kLoaderMark = "/ld-linux-";
    for (std::size_t pos = specsContent.find(kLoaderMark);
         pos != std::string::npos;
         pos = specsContent.find(kLoaderMark, pos + 1)) {
        auto start = pos;
        while (start > 0 && is_path_char(specsContent[start - 1])) --start;
        auto end = pos + 1;
        while (end < specsContent.size() && is_path_char(specsContent[end])) ++end;
        auto loader = specsContent.substr(start, end - start);
        if (loader.empty() || loader[0] != '/') continue;
        auto dir = std::filesystem::path(loader).parent_path().string();
        if (dir == "/lib" || dir == "/lib64" || dir == "/lib32" || dir == "/libx32")
            continue;  // pristine multilib default, not a baked path
        return loader;
    }
    return "";
}

void fixup_gcc_specs(const std::filesystem::path& gccPkgRoot,
                     const std::filesystem::path& glibcLibDir,
                     const std::filesystem::path& gccLibDir)
{
    std::filesystem::path specsParent;
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(gccPkgRoot / "lib" / "gcc", ec);
         !ec && it != std::filesystem::directory_iterator{}; it.increment(ec)) {
        if (it->is_directory(ec)) { specsParent = it->path(); break; }
    }
    if (specsParent.empty()) return;

    auto loaderReplacement = resolve_loader(glibcLibDir, /*targetTriple=*/{}).string();
    if (loaderReplacement.empty()) return;
    auto rpathReplacement  = std::format("{}:{}",
                                         glibcLibDir.string(),
                                         gccLibDir.string());

    auto replace_all = [](std::string& s, std::string_view needle,
                          std::string_view rep)
    {
        for (std::size_t pos = 0;
             (pos = s.find(needle, pos)) != std::string::npos;) {
            s.replace(pos, needle.size(), rep);
            pos += rep.size();
        }
    };

    for (auto& sub : std::filesystem::directory_iterator(specsParent)) {
        auto specs = sub.path() / "specs";
        if (!std::filesystem::exists(specs)) continue;

        std::ifstream is(specs);
        std::stringstream ss;  ss << is.rdbuf();
        std::string content = ss.str();

        auto bakedLoader = detect_baked_loader(content);
        if (bakedLoader.empty()) continue;
        auto bakedDir = std::filesystem::path(bakedLoader).parent_path().string();
        // Already pointing at the right place — no fixup needed.
        if (bakedDir == glibcLibDir.string()) continue;

        // Order matters: replace the full loader file path first so the
        // shorter dir pattern doesn't eat its prefix.
        replace_all(content, bakedLoader, loaderReplacement);
        replace_all(content, bakedDir,    rpathReplacement);

        std::ofstream os(specs);
        os << content;
    }
}

// Regenerate the clang driver cfg files after the LLVM payload landed in the
// sandbox. The cfg xlings authored at install time is a per-machine,
// per-install-path artifact (its content depended on what existed when the
// package was installed); mcpp's builds bypass it entirely
// (--no-default-config), so its only remaining job is to make a HUMAN
// running `clang++` directly get a working, hermetic compiler. We therefore
// regenerate it deterministically from the same link model the builds use,
// instead of line-patching whatever a given install produced:
//   C + C++:  -B/-L glibc payload, payload dynamic linker + rpath,
//             lld / compiler-rt / libunwind
//   C++ only: -nostdinc++ -stdlib=libc++ + payload libc++ headers/libs
// On macOS the C library comes from the SDK: --sysroot=<sdk> + libc++ headers.
export void fixup_clang_cfg(const std::filesystem::path& payloadRoot,
                     const std::filesystem::path& glibcLibDir) {
    auto binDir = payloadRoot / "bin";
    if (!std::filesystem::exists(binDir)) return;

    // Target triple from the payload layout (lib/<triple>), used for the
    // loader lookup and the per-target libc++ include/lib dirs.
    std::string triple;
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(payloadRoot / "lib", ec);
         !ec && it != std::filesystem::directory_iterator{}; it.increment(ec)) {
        auto name = it->path().filename().string();
        if (it->is_directory(ec) && name.find("-linux-") != std::string::npos) {
            triple = name;
            break;
        }
    }

    std::string common, cxxOnly;
    auto cxxInclude = payloadRoot / "include" / "c++" / "v1";
    if constexpr (mcpp::platform::is_macos) {
        // macOS keeps its historical cfg semantics: the C library and the
        // C++ runtime LINK both come from the SDK; only the libc++ HEADERS
        // come from the payload. Do NOT add -nostdinc++/-stdlib=libc++
        // here — a bare cfg-driven link has no libc++abi handling (that
        // lives in the main build's needs_explicit_libcxx path) and dies
        // with undefined __cxa_* / __gxx_personality_v0.
        if (auto sdk = mcpp::platform::macos::sdk_path())
            common += "--sysroot=" + sdk->string() + "\n";
        if (std::filesystem::exists(cxxInclude))
            cxxOnly += "-isystem " + cxxInclude.string() + "\n";
    } else {
        if (!glibcLibDir.empty()) {
            auto loader = resolve_loader(glibcLibDir, triple);
            common += "-B" + glibcLibDir.string() + "\n";
            common += "-L" + glibcLibDir.string() + "\n";
            if (!loader.empty())
                common += "-Wl,--dynamic-linker=" + loader.string() + "\n";
            common += "-Wl,--enable-new-dtags,-rpath," + glibcLibDir.string() + "\n";
        }
        common += "-fuse-ld=lld\n--rtlib=compiler-rt\n--unwindlib=libunwind\n";

        if (std::filesystem::exists(cxxInclude)) {
            cxxOnly += "-nostdinc++\n-stdlib=libc++\n";
            cxxOnly += "-isystem " + cxxInclude.string() + "\n";
        }
        if (!triple.empty()) {
            auto tripleInclude = payloadRoot / "include" / triple / "c++" / "v1";
            if (std::filesystem::exists(tripleInclude))
                cxxOnly += "-isystem " + tripleInclude.string() + "\n";
            auto tripleLib = payloadRoot / "lib" / triple;
            if (std::filesystem::exists(tripleLib)) {
                cxxOnly += "-L" + tripleLib.string() + "\n";
                cxxOnly += "-Wl,-rpath," + tripleLib.string() + "\n";
            }
        }
    }

    // Regenerate every existing cfg in bin/ (clang.cfg, clang++.cfg, and any
    // versioned clang-<major>.cfg xlings created), classified C vs C++ by
    // whether the driver name contains "++".
    for (auto it = std::filesystem::directory_iterator(binDir, ec);
         !ec && it != std::filesystem::directory_iterator{}; it.increment(ec)) {
        auto name = it->path().filename().string();
        if (!name.ends_with(".cfg")) continue;
        const bool isCxx = name.find("++") != std::string::npos;
        std::ofstream os(it->path());
        os << common << (isCxx ? cxxOnly : std::string{});
    }
}

// Locate the sandbox glibc payload's lib dir (the newest installed version
// that actually carries a dynamic loader). Shared by the gcc and llvm fixups.
std::filesystem::path find_sandbox_glibc_lib(const mcpp::xlings::Env& xlEnv) {
    auto glibcRoot = mcpp::xlings::paths::xim_tool_root(xlEnv, "glibc");
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(glibcRoot, ec);
         !ec && it != std::filesystem::directory_iterator{}; it.increment(ec)) {
        if (auto lib = payload_lib_dir_with_loader(it->path()); !lib.empty())
            return lib;
    }
    return {};
}

// Post-install fixup for a freshly-installed GNU gcc payload: patchelf
// PT_INTERP/RUNPATH for gcc/binutils binaries + linker-specs wiring against
// the sandbox glibc — without it a fresh-sandbox glibc gcc cannot find the
// C library (stdlib.h not found).
void gcc_post_install_fixup(const mcpp::config::GlobalConfig& cfg,
                            const std::filesystem::path& payloadRoot,
                            const std::filesystem::path& glibcLibDir) {
    auto xlEnv = mcpp::config::make_xlings_env(cfg);
    auto gccLibDir = payloadRoot / "lib64";
    auto patchelfBin = mcpp::xlings::paths::xim_tool(xlEnv, "patchelf",
        mcpp::xlings::pinned::kPatchelfVersion) / "bin" / "patchelf";

    if (!glibcLibDir.empty() && std::filesystem::exists(gccLibDir)
        && std::filesystem::exists(patchelfBin))
    {
        auto loader = resolve_loader(glibcLibDir, /*targetTriple=*/{});
        auto rpath = std::format("{}:{}",
            glibcLibDir.string(), gccLibDir.string());

        mcpp::log::verbose("toolchain", std::format(
            "gcc fixup: patchelf_walk rpath='{}'", rpath));
        auto binutilsRoot = mcpp::xlings::paths::xim_tool_root(xlEnv, "binutils");
        if (std::filesystem::exists(binutilsRoot)) {
            for (auto& v : std::filesystem::directory_iterator(binutilsRoot))
                patchelf_walk(v.path(), loader, rpath, patchelfBin);
        }
        patchelf_walk(payloadRoot, loader, rpath, patchelfBin);

        mcpp::log::verbose("toolchain", "gcc fixup: fixup_gcc_specs");
        fixup_gcc_specs(payloadRoot, glibcLibDir, gccLibDir);
    } else {
        mcpp::ui::warning(
            "could not locate sandbox glibc/gcc/patchelf paths; "
            "gcc-built binaries may have unresolved PT_INTERP/RUNPATH");
    }
}

// LLVM payload fixup: RUNPATH for the bundled runtime shared libraries
// (libc++.so / libunwind.so need to find siblings like libatomic.so.1 after
// the payload moved) + deterministic cfg regeneration. Only lib/ dirs are
// walked — NOT bin/: the clang++ binary's own RUNPATH (zlib, libxml2, …) was
// set by xlings and must be preserved.
void llvm_post_install_fixup(const mcpp::config::GlobalConfig& cfg,
                             const std::filesystem::path& payloadRoot,
                             const std::filesystem::path& glibcLibDir) {
    auto xlEnv = mcpp::config::make_xlings_env(cfg);
    auto patchelfBin = mcpp::xlings::paths::xim_tool(xlEnv, "patchelf",
        mcpp::xlings::pinned::kPatchelfVersion) / "bin" / "patchelf";
    if (!glibcLibDir.empty() && std::filesystem::exists(patchelfBin)) {
        auto loader = resolve_loader(glibcLibDir, /*targetTriple=*/{});
        auto llvmLib = payloadRoot / "lib";
        std::string rpath;
        std::error_code ec;
        for (auto it = std::filesystem::directory_iterator(llvmLib, ec);
             !ec && it != std::filesystem::directory_iterator{}; it.increment(ec)) {
            if (it->is_directory(ec)
                && it->path().filename().string().find("-linux-") != std::string::npos)
                rpath += it->path().string() + ":";
        }
        rpath += llvmLib.string() + ":" + glibcLibDir.string();
        mcpp::log::verbose("toolchain", std::format(
            "llvm fixup: patchelf_walk lib/ rpath='{}'", rpath));
        patchelf_walk(llvmLib, loader, rpath, patchelfBin);
    }
    mcpp::log::verbose("toolchain", "llvm fixup: fixup_clang_cfg");
    fixup_clang_cfg(payloadRoot, glibcLibDir);
}

// ── the single fixup pipeline entry ──────────────────────────────────────
//
// Called from the payload-resolution seam shared by ALL toolchain install
// paths (explicit `mcpp toolchain install`, default-toolchain auto-install,
// and manifest `[toolchain]` auto-install). Previously each path remembered
// (or forgot) its own subset of fixups: the manifest path ran none, which is
// how a fresh llvm install kept a stale install-time cfg and unpatched
// runtime libs. Idempotent via a content-fingerprinted marker.
//
// Bump when the fixup logic changes so existing installs re-run it.
constexpr std::string_view kFixupRev = "hermetic-2";

export void ensure_post_install_fixup(const mcpp::config::GlobalConfig& cfg,
                                      const std::filesystem::path& payloadRoot,
                                      const XimToolchainPackage& pkg) {
    std::string kind;
    if (pkg.needsGccPostInstallFixup) kind = "gcc";
    else if (pkg.ximName == "llvm")   kind = "llvm";
    else return;
    if constexpr (mcpp::platform::is_windows) return;  // PE world: no fixups

    // Ownership guard: payloads inherited via symlink from another MCPP_HOME
    // are not ours to patch — their owner already ran the fixup, and patching
    // through the symlink would rewrite the canonical files against OUR
    // (possibly ephemeral) paths, bricking the owner's toolchain.
    {
        std::error_code ec;
        auto canonicalRoot = std::filesystem::weakly_canonical(payloadRoot, ec);
        auto homeRegistry  = std::filesystem::weakly_canonical(cfg.registryDir, ec);
        if (!ec && !canonicalRoot.string().starts_with(homeRegistry.string())) {
            mcpp::log::verbose("toolchain", std::format(
                "skip {} fixup: payload '{}' resolves outside this home ('{}') — "
                "inherited payload, owner is responsible for its fixup",
                kind, payloadRoot.string(), canonicalRoot.string()));
            return;
        }
    }

    auto xlEnv = mcpp::config::make_xlings_env(cfg);
    std::filesystem::path glibcLibDir;
    if constexpr (mcpp::platform::is_linux)
        glibcLibDir = find_sandbox_glibc_lib(xlEnv);

    // Content-fingerprinted marker: a marker whose INPUTS drifted (different
    // glibc payload, newer fixup logic) re-runs the fixup — "a process once
    // exited 0" is not evidence the current inputs were ever applied.
    auto markerPath = payloadRoot / ".mcpp-fixup.json";
    nlohmann::json expected;
    expected["schema"]   = 1;
    expected["kind"]     = kind;
    expected["rev"]      = std::string(kFixupRev);
    expected["glibcLib"] = glibcLibDir.generic_string();
    {
        std::ifstream is(markerPath);
        if (is) {
            try {
                nlohmann::json actual;
                is >> actual;
                if (actual == expected) return;  // fixup already applied
            } catch (...) { /* corrupt marker → re-run */ }
        }
    }

    if (kind == "gcc")  gcc_post_install_fixup(cfg, payloadRoot, glibcLibDir);
    else                llvm_post_install_fixup(cfg, payloadRoot, glibcLibDir);

    std::ofstream os(markerPath);
    os << expected.dump(2) << "\n";
}

} // namespace mcpp::toolchain

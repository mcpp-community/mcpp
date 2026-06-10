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
import mcpp.log;
import mcpp.platform;
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
        if (hasInterp) {
            (void)mcpp::platform::process::run_silent(std::format(
                "{} --set-interpreter {} {} 2>/dev/null",
                mcpp::platform::shell::quote(patchelfBin.string()),
                mcpp::platform::shell::quote(loader.string()),
                mcpp::platform::shell::quote(path.string())));
        }
        // Always set RUNPATH (works on .so too — they need to find deps).
        if (!rpath.empty()) {
            (void)mcpp::platform::process::run_silent(std::format(
                "{} --set-rpath {} {} 2>/dev/null",
                mcpp::platform::shell::quote(patchelfBin.string()),
                mcpp::platform::shell::quote(rpath),
                mcpp::platform::shell::quote(path.string())));
        }
    }
}

// xim bakes the installing user's XLINGS_HOME into gcc specs at install
// time (as `--dynamic-linker` and `-rpath`). When mcpp uses its own
// isolated sandbox (MCPP_HOME/registry/), the baked-in paths point to
// xlings' home, not mcpp's sandbox glibc — binaries would fail to exec.
//
// Mcpp does a post-install spec rewrite:
//   - Dynamically detects the baked-in lib dir from the specs file
//   - Replaces the dynamic-linker path with <glibc_lib64>/ld-linux-x86-64.so.2
//   - Replaces the rpath with <glibc_lib64>:<gcc_lib64>
// Idempotent — skips if already pointing at the correct glibc.
// Extract the baked-in lib directory from a gcc specs file by finding
// the dynamic-linker path that ends with `/ld-linux-x86-64.so.2`.
// xim bakes the installing user's XLINGS_HOME into specs at install
// time, so the path varies per machine — we cannot hardcode it.
std::string detect_baked_lib_dir(const std::string& specsContent) {
    constexpr std::string_view kLoader = "/ld-linux-x86-64.so.2";
    auto pos = specsContent.find(kLoader);
    if (pos == std::string::npos) return "";
    // Walk backwards to find start of the absolute path
    auto start = pos;
    while (start > 0 && specsContent[start - 1] != ' '
                     && specsContent[start - 1] != ':'
                     && specsContent[start - 1] != ';'
                     && specsContent[start - 1] != '\n') {
        --start;
    }
    auto dir = specsContent.substr(start, pos - start);
    // Sanity: must be absolute
    if (dir.empty() || dir[0] != '/') return "";
    // Skip if it already points to the target glibc (no fixup needed)
    return dir;
}

void fixup_gcc_specs(const std::filesystem::path& gccPkgRoot,
                     const std::filesystem::path& glibcLibDir,
                     const std::filesystem::path& gccLibDir)
{
    auto specsParent = gccPkgRoot / "lib" / "gcc" / "x86_64-linux-gnu";
    if (!std::filesystem::exists(specsParent)) return;

    auto loaderReplacement = (glibcLibDir / "ld-linux-x86-64.so.2").string();
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

        auto bakedDir = detect_baked_lib_dir(content);
        if (bakedDir.empty()) continue;
        // Already pointing at the right place — no fixup needed.
        if (bakedDir == glibcLibDir.string()) continue;

        auto bakedLoader = bakedDir + "/ld-linux-x86-64.so.2";

        // Order matters: replace the full loader file path first so the
        // shorter dir pattern doesn't eat its prefix.
        replace_all(content, bakedLoader, loaderReplacement);
        replace_all(content, bakedDir,    rpathReplacement);

        std::ofstream os(specs);
        os << content;
    }
}

// Rewrite clang++.cfg paths after the LLVM payload has been copied to the
// mcpp sandbox. The cfg was authored by xlings at install time and contains
// absolute paths pointing to ~/.xlings/. We rewrite them to point to the
// actual payload location + sibling xpkgs (glibc, linux-headers).
export void fixup_clang_cfg(const std::filesystem::path& payloadRoot,
                     const std::filesystem::path& glibcLibDir) {
    for (auto cfgName : {"clang++.cfg", "clang.cfg"}) {
        auto cfgPath = payloadRoot / "bin" / cfgName;
        if (!std::filesystem::exists(cfgPath)) continue;

        std::ifstream is(cfgPath);
        std::stringstream ss;  ss << is.rdbuf();
        std::string content = ss.str();
        is.close();

        auto llvmRoot = payloadRoot;
        auto replace_line_prefix = [&](std::string& s, std::string_view prefix,
                                       const std::string& newValue) {
            std::istringstream lines(s);
            std::string result, line;
            while (std::getline(lines, line)) {
                if (line.starts_with(prefix)) {
                    result += std::string(prefix) + newValue + '\n';
                } else {
                    result += line + '\n';
                }
            }
            s = result;
        };

        // Rewrite --sysroot to remove (mcpp provides this explicitly).
        // Rewrite -isystem to point to payload's libc++ headers.
        // Rewrite -L and -rpath to point to payload's lib dir.
        // Rewrite dynamic-linker to use glibc payload's ld-linux.
        std::istringstream lines(content);
        std::string result, line;
        while (std::getline(lines, line)) {
            if (line.starts_with("--sysroot=")) {
                // Remove — mcpp provides sysroot via payload paths.
                continue;
            }
            if (line.starts_with("-isystem ")) {
                auto oldPath = line.substr(9);
                if (oldPath.find("include/c++/v1") != std::string::npos) {
                    auto relative = oldPath.substr(oldPath.find("include/c++/v1"));
                    result += "-isystem " + (llvmRoot / relative).string() + '\n';
                    continue;
                }
                if (oldPath.find("include/x86_64") != std::string::npos ||
                    oldPath.find("include/aarch64") != std::string::npos) {
                    // Target-specific libc++ include.
                    auto includePos = oldPath.find("include/");
                    auto relative = oldPath.substr(includePos);
                    result += "-isystem " + (llvmRoot / relative).string() + '\n';
                    continue;
                }
            }
            if (line.starts_with("-L")) {
                auto oldPath = line.substr(2);
                if (oldPath.find("lib/x86_64") != std::string::npos ||
                    oldPath.find("lib/aarch64") != std::string::npos) {
                    auto libPos = oldPath.find("lib/");
                    auto relative = oldPath.substr(libPos);
                    result += "-L" + (llvmRoot / relative).string() + '\n';
                    continue;
                }
            }
            if (line.starts_with("-Wl,-rpath,")) {
                auto oldPath = line.substr(11);
                // Rpath for LLVM lib dir
                if (oldPath.find("lib/x86_64") != std::string::npos ||
                    oldPath.find("lib/aarch64") != std::string::npos) {
                    auto libPos = oldPath.find("lib/");
                    auto relative = oldPath.substr(libPos);
                    result += "-Wl,-rpath," + (llvmRoot / relative).string() + '\n';
                    continue;
                }
                // Rpath for subos/glibc — rewrite to glibc payload.
                if (!glibcLibDir.empty()) {
                    auto parentDir = std::filesystem::path(oldPath).parent_path();
                    // subos rpath lines like -Wl,-rpath,<subos>/lib
                    if (oldPath.find("subos") != std::string::npos) {
                        result += "-Wl,-rpath," + glibcLibDir.string() + '\n';
                        continue;
                    }
                }
            }
            if (line.starts_with("-Wl,--dynamic-linker=")) {
                // Rewrite to glibc payload's ld-linux.
                if (!glibcLibDir.empty()) {
                    result += "-Wl,--dynamic-linker=" +
                              (glibcLibDir / "ld-linux-x86-64.so.2").string() + '\n';
                    continue;
                }
            }
            if (line.starts_with("-Wl,--enable-new-dtags,-rpath,")) {
                if (!glibcLibDir.empty()) {
                    result += "-Wl,--enable-new-dtags,-rpath," + glibcLibDir.string() + '\n';
                    continue;
                }
            }
            if (line.starts_with("-Wl,-rpath-link,")) {
                if (!glibcLibDir.empty()) {
                    result += "-Wl,-rpath-link," + glibcLibDir.string() + '\n';
                    continue;
                }
            }
            result += line + '\n';
        }

        // Remove trailing newline
        while (!result.empty() && result.back() == '\n') result.pop_back();
        result += '\n';

        std::ofstream os(cfgPath);
        os << result;
    }
}

// Post-install fixup for a freshly-installed GNU gcc payload: patchelf
// PT_INTERP/RUNPATH for gcc/binutils binaries + linker-specs wiring against
// the sandbox glibc. ONE pipeline shared by `mcpp toolchain install` and the
// first-run auto-install (the latter previously skipped this, leaving a
// fresh-sandbox glibc gcc unable to find the C library: stdlib.h not found).
export void gcc_post_install_fixup(const mcpp::config::GlobalConfig& cfg,
                            const std::filesystem::path& payloadRoot) {
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
                "skip gcc fixup: payload '{}' resolves outside this home ('{}') — "
                "inherited payload, owner is responsible for its fixup",
                payloadRoot.string(), canonicalRoot.string()));
            return;
        }
    }
    auto xlEnv = mcpp::config::make_xlings_env(cfg);
    auto glibcRoot = mcpp::xlings::paths::xim_tool_root(xlEnv, "glibc");
    std::filesystem::path glibcLibDir;
    if (std::filesystem::exists(glibcRoot)) {
        for (auto& v : std::filesystem::directory_iterator(glibcRoot)) {
            auto candidate = v.path() / "lib64";
            if (std::filesystem::exists(candidate / "ld-linux-x86-64.so.2")) {
                glibcLibDir = candidate;
                break;
            }
        }
    }
    auto gccLibDir = payloadRoot / "lib64";
    auto patchelfBin = mcpp::xlings::paths::xim_tool(xlEnv, "patchelf",
        mcpp::xlings::pinned::kPatchelfVersion) / "bin" / "patchelf";

    if (!glibcLibDir.empty() && std::filesystem::exists(gccLibDir)
        && std::filesystem::exists(patchelfBin))
    {
        auto loader = glibcLibDir / "ld-linux-x86-64.so.2";
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

} // namespace mcpp::toolchain

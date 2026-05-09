// mcpp.cli — top-level command dispatch.
//
// MVP commands:
//   mcpp new <name>
//   mcpp build [--verbose] [--print-fingerprint] [--no-cache]
//   mcpp run [target] [-- args...]
//   mcpp clean [--bmi-cache]
//   mcpp emit xpkg [--version V] [--output FILE]   (M2)
//   mcpp --help / mcpp --version

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.cli;

import std;
import mcpp.manifest;
import mcpp.modgraph.graph;
import mcpp.modgraph.scanner;
import mcpp.modgraph.validate;
import mcpp.toolchain.detect;
import mcpp.toolchain.fingerprint;
import mcpp.toolchain.stdmod;
import mcpp.build.plan;
import mcpp.build.backend;
import mcpp.build.ninja;
import mcpp.lockfile;
import mcpp.publish.xpkg_emit;
import mcpp.pack;
import mcpp.config;
import mcpp.fetcher;
import mcpp.pm.resolver;   // PR-R4: extracted from cli.cppm
import mcpp.pm.commands;   // PR-R5: cmd_add / cmd_remove / cmd_update live here now
import mcpp.pm.mangle;     // Level 1 multi-version fallback (cross-major coexistence)
import mcpp.ui;
import mcpp.bmi_cache;
import mcpp.dyndep;
import mcpp.version_req;   // SemVer constraint resolution
import mcpplibs.cmdline;   // M6.1: dogfooded CLI parser

export namespace mcpp::cli {

int run(int argc, char** argv);

} // namespace mcpp::cli

namespace mcpp::cli::detail {

// ----- helpers -----
//
// As of M6.1 phase 3, all CLI commands dispatch through a single
// `cmdline::App` declared in `run()` below. The previous per-command
// `cl::App` build + `parse_cmd_args(...)` double-parse is gone; each
// `cmd_*` now takes the already-parsed `ParsedArgs` and reads from it.
// `cmdline` handles `--help` / `--version` / unknown-option errors itself.

// Custom top-level help. cmdline's auto-generated `print_help` is a fine
// default but its layout (`USAGE:`, no command-specific blurbs) doesn't
// match what the e2e tests assert against — they check for `Usage:`
// (mixed case) plus `mcpp new` / `mcpp build` literals. We keep the
// canonical printer here so the docs/CHANGELOG examples don't drift
// every time cmdline tweaks its formatting.
void print_usage() {
    std::println("mcpp v{} — modern C++23 build tool", mcpp::toolchain::MCPP_VERSION);
    std::println("");
    std::println("Usage:");
    std::println("Project commands:");
    std::println("  mcpp new <name>                      Create a new package skeleton");
    std::println("  mcpp build [options]                 Build the current package");
    std::println("  mcpp run [target] [-- args...]       Build + run a binary target");
    std::println("  mcpp test [-- args...]               Build + run all tests/**/*.cpp");
    std::println("  mcpp clean [--bmi-cache]             Remove target/ (and optionally BMI cache)");
    std::println("  mcpp add <pkg>[@<ver>]               Add a dependency to mcpp.toml");
    std::println("  mcpp remove <pkg>                    Remove a dependency from mcpp.toml");
    std::println("  mcpp update [pkg]                    Re-resolve deps and rewrite mcpp.lock");
    std::println("  mcpp search <keyword>                Search packages in registries");
    std::println("  mcpp publish [--dry-run]             Publish package to default registry");
    std::println("  mcpp pack [--mode <m>]               Build + bundle into a distributable tarball");
    std::println("  mcpp emit xpkg [-V VER] [-o FILE]    Generate xpkg Lua entry");
    std::println("");
    std::println("Resource management:");
    std::println("  mcpp toolchain install|list|default  Manage mcpp's private toolchains");
    std::println("  mcpp cache list|prune|clean|info     Inspect/manage the global BMI cache");
    std::println("  mcpp index list|add|remove|update    Manage package registries");
    std::println("");
    std::println("About mcpp itself:");
    std::println("  mcpp self doctor                     Diagnose mcpp environment health");
    std::println("  mcpp self env                        Print mcpp paths and toolchain");
    std::println("  mcpp self version                    Show mcpp version");
    std::println("  mcpp self explain <CODE>             Show extended description for an error code");
    std::println("  mcpp --help / --version              Help / version");
    std::println("");
    std::println("Build options:");
    std::println("  --verbose, -v                        Verbose compiler output");
    std::println("  --quiet, -q                          Suppress status output");
    std::println("  --print-fingerprint                  Show toolchain fingerprint and 10 inputs");
    std::println("  --no-cache                           Force-clear target/ before building");
    std::println("  --no-color                           Disable colored output");
    std::println("");
    std::println("Docs: https://github.com/mcpp-community/mcpp/tree/main/docs");
}

// Locate mcpp.toml by walking upward from cwd.
std::optional<std::filesystem::path> find_manifest_root(std::filesystem::path start) {
    auto p = std::filesystem::absolute(start);
    while (true) {
        if (std::filesystem::exists(p / "mcpp.toml")) return p;
        auto parent = p.parent_path();
        if (parent == p) return std::nullopt;
        p = parent;
    }
}

std::filesystem::path target_dir(const mcpp::toolchain::Toolchain& tc,
                                 const mcpp::toolchain::Fingerprint& fp,
                                 const std::filesystem::path& root)
{
    auto triple = tc.targetTriple.empty() ? std::string{"unknown"} : tc.targetTriple;
    return root / "target" / triple / fp.hex;
}

// ─── Toolchain version-spec helpers ──────────────────────────────────
//
// Partial versions: `mcpp toolchain install gcc 15` must match
// the latest installed/available 15.x.y, `gcc 15.1` matches the latest
// 15.1.y, etc. Accept either `<comp> <ver>` (two positionals) or `<comp>@<ver>`
// (one positional with `@`) — both forms are normalised here.

// Split "X.Y.Z…" into integer components. A trailing "-musl" (or any other
// non-numeric tail) is dropped — the caller has already handled the libc
// flavour and we only care about the numeric prefix for matching.
std::vector<int> parse_version_components(std::string_view s) {
    std::vector<int> out;
    int cur = 0;
    bool any = false;
    for (char c : s) {
        if (c >= '0' && c <= '9') { cur = cur * 10 + (c - '0'); any = true; }
        else if (c == '.') {
            if (any) { out.push_back(cur); cur = 0; any = false; }
            else     { out.clear(); break; }
        } else {
            break; // non-numeric tail (e.g. "-musl")
        }
    }
    if (any) out.push_back(cur);
    return out;
}

// Pick the version from `available` that best matches `partial`:
//   ""       → highest version overall
//   "15"     → highest 15.X.Y
//   "15.1"   → highest 15.1.Y
//   "15.1.0" → exact match (or empty if not present)
// Empty result = no match.
std::optional<std::string>
resolve_version_match(std::string_view partial,
                      std::vector<std::string> available)
{
    if (available.empty()) return std::nullopt;
    auto want = parse_version_components(partial);
    auto matches = [&](const std::vector<int>& cand) {
        if (want.size() > cand.size()) return false;
        for (std::size_t i = 0; i < want.size(); ++i)
            if (cand[i] != want[i]) return false;
        return true;
    };
    std::optional<std::string>      best;
    std::vector<int>                bestVec;
    for (auto& v : available) {
        auto comps = parse_version_components(v);
        if (comps.empty()) continue;
        if (!matches(comps)) continue;
        if (!best || std::lexicographical_compare(
                bestVec.begin(), bestVec.end(), comps.begin(), comps.end()))
        {
            best    = v;
            bestVec = std::move(comps);
        }
    }
    return best;
}

// Enumerate installed `<pkgsDir>/xim-x-<name>/<version>/` subdirs.
std::vector<std::string>
list_installed_versions(const std::filesystem::path& pkgsDir,
                        std::string_view ximName)
{
    std::vector<std::string> out;
    auto root = pkgsDir / std::format("xim-x-{}", ximName);
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) return out;
    for (auto& v : std::filesystem::directory_iterator(root, ec)) {
        if (v.is_directory(ec)) out.push_back(v.path().filename().string());
    }
    return out;
}

// Look up available versions for `xim:<name>` from the locally synced index.
// Falls back to an empty list silently — the caller will then either error
// out with a clear message or just keep the partial as-is.
//
// Index layout in mcpp's sandbox is two-tier:
//   <reg>/data/xim-pkgindex/pkgs/<n[0]>/<name>.lua          — primary
//   <reg>/data/xim-index-repos/<sub-index>/pkgs/<n[0]>/<name>.lua
// We scan both so a package living in either tier resolves.
std::vector<std::string>
list_available_xpkg_versions(const mcpp::config::GlobalConfig& cfg,
                             std::string_view ximName)
{
    if (ximName.empty()) return {};
    std::string subdir(1, ximName[0]);
    std::string fname = std::string(ximName) + ".lua";

    auto try_load = [&](const std::filesystem::path& p)
        -> std::optional<std::vector<std::string>>
    {
        std::error_code ec;
        if (!std::filesystem::exists(p, ec)) return std::nullopt;
        std::ifstream is(p);
        std::string body((std::istreambuf_iterator<char>(is)), {});
        return mcpp::manifest::list_xpkg_versions(body, "linux");
    };

    auto data = cfg.xlingsHome() / "data";
    if (auto v = try_load(data / "xim-pkgindex" / "pkgs" / subdir / fname); v)
        return std::move(*v);

    std::error_code ec;
    auto repos = data / "xim-index-repos";
    if (std::filesystem::exists(repos, ec)) {
        for (auto& repo : std::filesystem::directory_iterator(repos, ec)) {
            auto cand = repo.path() / "pkgs" / subdir / fname;
            if (auto v = try_load(cand); v) return std::move(*v);
        }
    }
    return {};
}

// ─── Install-time progress display ───────────────────────────────────
//
// xlings emits NDJSON events on stdout via `xlings interface install_packages
// --args ...` (see fetcher.cppm). The events we care about for UX are:
//
//   {"kind":"data","dataKind":"download_progress","payload":{
//     "elapsedSec": 2.0,
//     "files": [{"name":"...", "downloadedBytes":..., "totalBytes":..., "finished":bool, ...}],
//     ...
//   }}
//
// We parse the first file in the `files` array (xlings serializes the
// currently-active download first) and feed (current, total) to a
// ui::ProgressBar so the user sees a "Downloading <pkg> [====   ]
// 45 MB / 110 MB" line.

struct InstallProgressFile {
    std::string name;
    double      downloaded = 0;
    double      total      = 0;
    bool        started    = false;
    bool        finished   = false;
};

namespace {

// Extract one `{ ... }` object starting at payload[*pos], moving *pos past
// the closing `}`. Returns the slice or empty when no object is here.
std::string_view scan_one_object(std::string_view payload, std::size_t* pos) {
    auto p = *pos;
    while (p < payload.size() && (payload[p] == ' ' || payload[p] == '\n')) ++p;
    if (p >= payload.size() || payload[p] != '{') { *pos = p; return {}; }
    auto start = p;
    int depth = 0;
    bool in_string = false;
    for (; p < payload.size(); ++p) {
        char c = payload[p];
        if (in_string) {
            if (c == '\\' && p + 1 < payload.size()) { ++p; continue; }
            if (c == '"') in_string = false;
            continue;
        }
        if (c == '"')      in_string = true;
        else if (c == '{') ++depth;
        else if (c == '}') { if (--depth == 0) { ++p; break; } }
    }
    *pos = p;
    return payload.substr(start, (p == payload.size() ? p : p) - start);
}

InstallProgressFile parse_one_install_file(std::string_view obj) {
    auto get_str = [&](std::string_view key) -> std::string {
        std::string n = std::format("\"{}\":\"", key);
        auto q = obj.find(n);
        if (q == std::string_view::npos) return "";
        q += n.size();
        std::string out;
        while (q < obj.size() && obj[q] != '"') {
            if (obj[q] == '\\' && q + 1 < obj.size()) { out.push_back(obj[q+1]); q += 2; continue; }
            out.push_back(obj[q++]);
        }
        return out;
    };
    auto get_num = [&](std::string_view key) -> double {
        std::string n = std::format("\"{}\":", key);
        auto q = obj.find(n);
        if (q == std::string_view::npos) return 0;
        q += n.size();
        auto e = q;
        while (e < obj.size()
            && (std::isdigit(static_cast<unsigned char>(obj[e]))
                || obj[e] == '.' || obj[e] == '-' || obj[e] == '+'
                || obj[e] == 'e' || obj[e] == 'E')) ++e;
        try { return std::stod(std::string(obj.substr(q, e - q))); }
        catch (...) { return 0; }
    };
    auto get_bool = [&](std::string_view key) -> bool {
        std::string n = std::format("\"{}\":", key);
        auto q = obj.find(n);
        if (q == std::string_view::npos) return false;
        q += n.size();
        return obj.size() - q >= 4 && obj.substr(q, 4) == "true";
    };

    InstallProgressFile f;
    f.name       = get_str("name");
    f.downloaded = get_num("downloadedBytes");
    f.total      = get_num("totalBytes");
    f.started    = get_bool("started");
    f.finished   = get_bool("finished");
    return f;
}

} // namespace

// Parse every entry in the payload's `files` array. xlings emits an
// array-of-files for download_progress events even when only one is
// active, and during multi-package installs (gcc → glibc / binutils /
// linux-headers / gcc-runtime / gcc) the order of entries shifts as
// each file starts and finishes. Reading just the first one would
// flicker between names and re-emit the static "Downloading <pkg>"
// line every time the first slot rotates.
std::vector<InstallProgressFile>
parse_all_install_files(std::string_view payload)
{
    std::vector<InstallProgressFile> out;
    constexpr std::string_view kKey{"\"files\":["};
    auto p = payload.find(kKey);
    if (p == std::string_view::npos) return out;
    p += kKey.size();
    while (p < payload.size()) {
        while (p < payload.size() && (payload[p] == ' ' || payload[p] == '\n'
                                      || payload[p] == ',')) ++p;
        if (p >= payload.size() || payload[p] == ']') break;
        if (payload[p] != '{') break;
        auto obj = scan_one_object(payload, &p);
        if (obj.empty()) break;
        auto f = parse_one_install_file(obj);
        if (!f.name.empty()) out.push_back(std::move(f));
    }
    return out;
}

// Pull a top-level numeric field out of a payload JSON string. Cheap;
// only used for `elapsedSec` which we trust to be a plain number.
double extract_payload_number(std::string_view payload, std::string_view key) {
    std::string n = std::format("\"{}\":", key);
    auto q = payload.find(n);
    if (q == std::string_view::npos) return 0;
    q += n.size();
    auto e = q;
    while (e < payload.size()
        && (std::isdigit(static_cast<unsigned char>(payload[e]))
            || payload[e] == '.' || payload[e] == '-' || payload[e] == '+'
            || payload[e] == 'e' || payload[e] == 'E')) ++e;
    try { return std::stod(std::string(payload.substr(q, e - q))); }
    catch (...) { return 0; }
}

// Build the PathContext used to shorten user-visible paths in status
// output. project_root may be empty (for verbs that don't need it).
mcpp::ui::PathContext make_path_ctx(const mcpp::config::GlobalConfig* cfg,
                                    std::filesystem::path project_root = {})
{
    mcpp::ui::PathContext ctx;
    ctx.project_root = std::move(project_root);
    if (cfg) ctx.mcpp_home = cfg->mcppHome;
    if (auto* h = std::getenv("HOME"); h && *h) ctx.home = h;
    return ctx;
}

// Toolchain frontend lookup. Different xim packages ship the C++ frontend
// under different binary names — `gcc` ships `g++`, `musl-gcc` only ships
// the triple-prefixed `x86_64-linux-musl-g++`, etc. Returns the path to
// the frontend, or empty if none of the known candidates exist.
std::filesystem::path
toolchain_frontend(const std::filesystem::path& binDir,
                   std::string_view compiler)
{
    std::vector<std::string> cands;
    if (compiler == "musl-gcc") {
        cands = {"x86_64-linux-musl-g++", "g++"};
    } else if (compiler == "gcc") {
        cands = {"g++"};
    } else if (compiler == "clang" || compiler == "llvm") {
        cands = {"clang++"};
    } else if (compiler == "msvc") {
        cands = {"cl.exe"};
    } else {
        // Unknown xpkg — try each in priority order.
        cands = {"g++", "clang++", "x86_64-linux-musl-g++", "cl.exe"};
    }
    for (auto& c : cands) {
        auto p = binDir / c;
        if (std::filesystem::exists(p)) return p;
    }
    return {};
}

// Stateless adapter from `mcpp::config::BootstrapProgress` (xlings
// download_progress event) to a sticky ProgressBar. Used by
// load_or_init() during the one-time sandbox bootstrap (xim:patchelf,
// xim:ninja, plus their transitive deps).
//
// Two xlings quirks the callback has to absorb:
//   1. Each file's `finished=true` event arrives twice in a row.
//   2. During multi-package installs the `files[]` array reshuffles
//      between events (the active download isn't always at slot 0).
// The fix mirrors CliInstallProgress: dedupe via a `finished_` set and
// always pick "active first if still in event, else first
// started+unfinished" rather than reading slot 0 blindly.
mcpp::config::BootstrapProgressCallback make_bootstrap_progress_callback() {
    auto bar      = std::make_shared<std::optional<mcpp::ui::ProgressBar>>();
    auto active   = std::make_shared<std::string>();
    auto finished = std::make_shared<std::unordered_set<std::string>>();
    return [bar, active, finished](const mcpp::config::BootstrapProgress& ev) {
        // Process newly-finished entries.
        for (auto& f : ev.files) {
            if (finished->contains(f.name)) continue;
            if (!f.finished) continue;
            if (*active == f.name) {
                if (*bar) (*bar)->finish();
                bar->reset();
                active->clear();
            }
            finished->insert(f.name);
        }

        // Pick what to display: prefer continuing with `*active` if it's
        // still in the array and not finished, otherwise the first
        // started+unfinished entry.
        const mcpp::config::BootstrapFile* current = nullptr;
        for (auto& f : ev.files) {
            if (f.name == *active && !f.finished
                && !finished->contains(f.name)) { current = &f; break; }
        }
        if (!current) {
            for (auto& f : ev.files) {
                if (finished->contains(f.name)) continue;
                if (f.started && !f.finished) { current = &f; break; }
            }
        }
        if (!current) return;

        if (current->name != *active) {
            if (*bar) (*bar)->finish();
            *active = current->name;
            bar->emplace("Downloading", current->name);
        }
        if (current->totalBytes > 0) {
            (*bar)->update_bytes(static_cast<std::size_t>(current->downloadedBytes),
                                 static_cast<std::size_t>(current->totalBytes),
                                 ev.elapsedSec);
        }
    };
}

struct CliInstallProgress : mcpp::fetcher::EventHandler {
    std::optional<mcpp::ui::ProgressBar> bar_;
    std::string                          active_;
    std::unordered_set<std::string>      finished_;

    void on_data(const mcpp::fetcher::DataEvent& d) override {
        if (d.dataKind != "download_progress") return;
        auto files = parse_all_install_files(d.payloadJson);
        if (files.empty()) return;

        // 1. Process any newly-finished entries. Each file is reported
        //    twice with finished=true (xlings quirk); the `finished_`
        //    set dedupes both that AND the rotation case where the
        //    same file shows up at a different array slot in a later
        //    event.
        for (auto& f : files) {
            if (finished_.contains(f.name)) continue;
            if (!f.finished) continue;
            if (active_ == f.name) {
                if (bar_) bar_->finish();
                bar_.reset();
                active_.clear();
            }
            finished_.insert(f.name);
        }

        // 2. Pick what to display. Prefer continuing with the current
        //    `active_` if it's still in the array and not finished —
        //    otherwise the first started+unfinished entry. This stops
        //    the bar from flickering between names when xlings reshuffles
        //    files[] across events during a multi-package install.
        const InstallProgressFile* current = nullptr;
        for (auto& f : files) {
            if (f.name == active_ && !f.finished
                && !finished_.contains(f.name)) { current = &f; break; }
        }
        if (!current) {
            for (auto& f : files) {
                if (finished_.contains(f.name)) continue;
                if (f.started && !f.finished) { current = &f; break; }
            }
        }
        if (!current) return;

        if (current->name != active_) {
            if (bar_) bar_->finish();
            active_ = current->name;
            bar_.emplace("Downloading", current->name);
        }
        if (current->total > 0) {
            double elapsed = extract_payload_number(d.payloadJson, "elapsedSec");
            bar_->update_bytes(static_cast<std::size_t>(current->downloaded),
                               static_cast<std::size_t>(current->total),
                               elapsed);
        }
    }

    ~CliInstallProgress() override { if (bar_) bar_->finish(); }
};

// Compose a stable canonical compile-flags string for fingerprinting.
std::string canonical_compile_flags(const mcpp::manifest::Manifest& m) {
    std::string s;
    s += "-std="; s += m.language.standard;
    s += " -fmodules";
    return s;
}

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
void patchelf_walk(const std::filesystem::path& dir,
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
        auto probe = std::format("'{}' --print-interpreter '{}' 2>/dev/null",
                                 patchelfBin.string(), path.string());
        std::FILE* fp = ::popen(probe.c_str(), "r");
        bool hasInterp = false;
        if (fp) {
            char buf[1024]{};
            hasInterp = (std::fread(buf, 1, sizeof(buf) - 1, fp) > 0);
            ::pclose(fp);
        }
        if (hasInterp) {
            (void)std::system(std::format(
                "'{}' --set-interpreter '{}' '{}' 2>/dev/null",
                patchelfBin.string(), loader.string(), path.string()).c_str());
        }
        // Always set RUNPATH (works on .so too — they need to find deps).
        if (!rpath.empty()) {
            (void)std::system(std::format(
                "'{}' --set-rpath '{}' '{}' 2>/dev/null",
                patchelfBin.string(), rpath, path.string()).c_str());
        }
    }
}

// xim's gcc tarball is built with an absolute hardcoded path
// `/home/xlings/.xlings_data/subos/linux/lib/...` baked into the gcc specs
// (both as `--dynamic-linker` and `-rpath`). xim's gcc-specs-config.lua
// tries to override these but only matches `/lib64/ld-linux-x86-64.so.2`,
// not the baked-in `/home/xlings/...`, so the override silently no-ops.
//
// TODO(xim-pkgindex-upstream): file an issue against xim-pkgindex's
// gcc-specs-config.lua to also match the `/home/xlings/...` baked-in
// path. Once fixed, this whole post-install fixup can be deleted.
//
// Result: gcc compiles user binaries with PT_INTERP pointing at
// `/home/xlings/.xlings_data/...` AND an rpath pointing at the same
// non-existent directory. `elfpatch.auto` (now functional thanks to the
// patchelf bootstrap) only fixes ELF binaries it scans; it doesn't touch
// the gcc specs text file.
//
// Mcpp does the post-install spec rewrite itself:
//   - The dynamic-linker path becomes <glibc_lib64>/ld-linux-x86-64.so.2.
//   - The rpath becomes <glibc_lib64>:<gcc_lib64> so user binaries can find
//     both libc.so.6 (glibc) and libstdc++.so.6 + libgcc_s.so.1 (gcc).
// Idempotent.
void fixup_gcc_specs(const std::filesystem::path& gccPkgRoot,
                     const std::filesystem::path& glibcLibDir,
                     const std::filesystem::path& gccLibDir)
{
    auto specsParent = gccPkgRoot / "lib" / "gcc" / "x86_64-linux-gnu";
    if (!std::filesystem::exists(specsParent)) return;

    constexpr std::string_view kBakedDir =
        "/home/xlings/.xlings_data/subos/linux/lib";
    auto kBakedLoader = std::format("{}/ld-linux-x86-64.so.2", kBakedDir);

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

        if (content.find(kBakedDir) == std::string::npos) continue;

        // Order matters: replace the full loader file path first so the
        // shorter dir pattern doesn't eat its prefix.
        replace_all(content, kBakedLoader, loaderReplacement);
        replace_all(content, kBakedDir,    rpathReplacement);

        std::ofstream os(specs);
        os << content;
    }
}

// SemVer resolution: a version spec is a "constraint" (vs. exact literal) if
// it starts with one of `^~><=` or contains a comma (multi-part), or is `*`
// or empty. Bare `1.2.3` is treated as exact for back-compat with pre-SemVer
// pinning workflows; users opt into resolution by writing `^1.2.3` etc.
// `is_version_constraint`, `kXpkgPlatform` and `resolve_semver` have moved
// to `mcpp.pm.resolver` (PR-R4 — see
// `.agents/docs/2026-05-08-pm-subsystem-architecture.md`). Call sites
// below reference the `mcpp::pm::` qualified names directly.

// --- Commands ---

int cmd_new(const mcpplibs::cmdline::ParsedArgs& parsed) {
    std::string name = parsed.positional(0);
    if (name.empty()) {
        std::println(stderr, "error: `mcpp new` requires a package name (e.g. `mcpp new hello`)");
        return 2;
    }

    std::filesystem::path root = std::filesystem::current_path() / name;
    if (std::filesystem::exists(root)) {
        std::println(stderr, "error: '{}' already exists", root.string());
        return 1;
    }
    std::error_code ec;
    std::filesystem::create_directories(root / "src", ec);
    if (ec) {
        std::println(stderr, "error: cannot create '{}': {}", root.string(), ec.message());
        return 1;
    }

    // mcpp.toml
    {
        std::ofstream os(root / "mcpp.toml");
        os << mcpp::manifest::default_template(name);
    }
    // src/main.cpp — template with PROJECT placeholder, replaced with `name`.
    {
        std::string body = R"(// PROJECT — generated by `mcpp new`
import std;

int main(int argc, char* argv[]) {
    std::println("Hello from PROJECT!");
    std::println("Built with import std + std::println on modular C++23.");
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) std::println("  arg[{}] = {}", i, argv[i]);
    }
    return 0;
}
)";
        std::size_t pos;
        while ((pos = body.find("PROJECT")) != std::string::npos) {
            body.replace(pos, 7, name);
        }
        std::ofstream os(root / "src" / "main.cpp");
        os << body;
    }
    // tests/test_smoke.cpp — bundled smoke test (`mcpp test` works out-of-the-box).
    {
        std::filesystem::create_directories(root / "tests", ec);
        std::ofstream os(root / "tests" / "test_smoke.cpp");
        os << R"(// Smoke test — verifies the project compiles + a binary runs.
// Add more tests as tests/test_*.cpp files; mcpp test discovers them
// automatically (one binary per file).
import std;

int main() {
    std::println("test_smoke: ok");
    return 0;
}
)";
    }
    // .gitignore
    {
        std::ofstream os(root / ".gitignore");
        os << "target/\n";
    }

    std::println("Created package '{}' at {}", name, root.string());
    std::println("Next: cd {} && mcpp build && mcpp run  (or `mcpp test`)", name);
    return 0;
}

struct BuildContext {
    mcpp::manifest::Manifest        manifest;
    mcpp::toolchain::Toolchain      tc;
    mcpp::toolchain::Fingerprint    fp;
    std::filesystem::path           projectRoot;
    std::filesystem::path           outputDir;
    std::filesystem::path           stdBmi;
    std::filesystem::path           stdObject;
    mcpp::build::BuildPlan          plan;

    // M3.2 BMI cache: deps that did NOT hit cache and therefore need
    // populate_from(...) AFTER backend.build succeeds.
    struct CacheTask {
        mcpp::bmi_cache::CacheKey       key;
        mcpp::bmi_cache::DepArtifacts   artifacts;
    };
    std::vector<CacheTask>          depsToPopulate;

    // Names of deps that DID hit cache (for ui status output).
    std::vector<std::string>        cachedDepLabels;     // "mcpplibs.cmdline v0.0.1"
};

// Command-level overrides (--target / --static).
// Empty defaults preserve pre-existing behaviour exactly.
struct BuildOverrides {
    std::string target_triple;   // empty = host triple, fall through to [toolchain]
    bool        force_static = false;  // --static (or implied by musl target)
};

// `prepare_build` builds the BuildContext for any verb that compiles.
//   includeDevDeps: when true, dev-dependencies are also fetched + scanned
//                   into the modgraph. mcpp test passes true; build/run pass false.
//   extraTargets:   additional Target entries (e.g. synthetic test targets)
//                   appended to the manifest before the modgraph runs.
//   overrides:      --target / --static.
std::expected<BuildContext, std::string>
prepare_build(bool print_fingerprint,
              bool includeDevDeps = false,
              std::vector<mcpp::manifest::Target> extraTargets = {},
              BuildOverrides overrides = {}) {
    auto root = find_manifest_root(std::filesystem::current_path());
    if (!root) {
        return std::unexpected("no mcpp.toml found in current directory or any parent");
    }

    auto m = mcpp::manifest::load(*root / "mcpp.toml");
    if (!m) return std::unexpected(m.error().format());

    // Inject synthetic targets (e.g. test binaries from `mcpp test`).
    for (auto& t : extraTargets) m->targets.push_back(t);

    // ─── Toolchain resolution (docs/21) ────────────────────────────────
    // Priority chain:
    //   1. mcpp.toml [toolchain].<platform>      → resolve_xpkg_path → abs path
    //   2. $MCPP_HOME/registry/subos/<active>/bin/g++  (xlings sandbox subos)
    //   3. $CXX env var
    //   4. PATH g++  (with warning)
    std::filesystem::path explicit_compiler;
    std::optional<mcpp::config::GlobalConfig> cfg_opt;
    auto get_cfg = [&]() -> std::expected<mcpp::config::GlobalConfig*, std::string> {
        if (!cfg_opt) {
            auto c = mcpp::config::load_or_init(/*quiet=*/false,
                make_bootstrap_progress_callback());
            if (!c) return std::unexpected(c.error().message);
            cfg_opt = std::move(*c);
        }
        return &*cfg_opt;
    };

    constexpr std::string_view kCurrentPlatform =
#if defined(__linux__)
        "linux";
#elif defined(__APPLE__)
        "macos";
#elif defined(_WIN32)
        "windows";
#else
        "unknown";
#endif

    // M5.5: toolchain resolution priority:
    //   0. --target X / --static, looked up in [target.<triple>]
    //   1. project mcpp.toml [toolchain].<platform> or .default
    //   2. global ~/.mcpp/config.toml [toolchain].default
    //   3. hard error (no system fallback)
    auto tcSpec = m->toolchain.for_platform(kCurrentPlatform);
    if (!tcSpec.has_value()) {
        auto cfg = get_cfg();
        if (cfg && !(*cfg)->defaultToolchain.empty()) {
            tcSpec = (*cfg)->defaultToolchain;
        }
    }

    // ─── --target / --static overrides ──────────────────────────────────
    // Look up [target.<triple>] from manifest; fall back to convention
    // (anything ending with "-musl" → gcc@<inherited-version>-musl + static).
    auto endswith = [](std::string_view s, std::string_view suf) {
        return s.size() >= suf.size()
            && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
    };
    if (!overrides.target_triple.empty()) {
        auto it = m->targetOverrides.find(overrides.target_triple);
        if (it != m->targetOverrides.end()) {
            if (!it->second.toolchain.empty()) tcSpec = it->second.toolchain;
            if (!it->second.linkage.empty())   m->buildConfig.linkage = it->second.linkage;
        }
        // Convention: "*-musl" target without an explicit `[target.X]`
        // override gets the canonical musl-gcc spec the rest of mcpp
        // uses internally. We can't just append "-musl" to the inherited
        // toolchain version because xim doesn't have a `musl-gcc@<host
        // gcc version>` for every gcc release — gcc 16.1 has no musl
        // variant yet, only 9.4 / 11.5 / 13.3 / 15.1 do. Picking 15.1.0
        // as the static default matches what mcpp itself uses for
        // `mcpp build --target x86_64-linux-musl` (see mcpp.toml).
        if (endswith(overrides.target_triple, "-musl")
            && (it == m->targetOverrides.end() || it->second.toolchain.empty()))
        {
            tcSpec = "gcc@15.1.0-musl";
        }
        if (endswith(overrides.target_triple, "-musl")
            && m->buildConfig.linkage.empty()) {
            m->buildConfig.linkage = "static";
        }
    }
    if (overrides.force_static) m->buildConfig.linkage = "static";

    if (tcSpec.has_value() && *tcSpec != "system") {
        // Parse "<pkg>@<version>" with an optional "-musl" libc suffix on
        // the version: "gcc@15.1.0" or "gcc@15.1.0-musl".
        auto at = tcSpec->find('@');
        if (at == std::string::npos) {
            return std::unexpected(std::format(
                "[toolchain].{} = '{}' is invalid; expected '<pkg>@<version>'",
                kCurrentPlatform, *tcSpec));
        }
        auto compiler = tcSpec->substr(0, at);
        auto version  = tcSpec->substr(at + 1);
        // Strip xim: prefix if user wrote it explicitly
        std::string indexedCompiler = compiler;
        if (auto colon = compiler.find(':'); colon != std::string::npos)
            indexedCompiler = compiler.substr(colon + 1);

        // libc suffix: `gcc@15.1.0-musl` → xim package `xim:musl-gcc@15.1.0`,
        // binary at `<root>/bin/x86_64-linux-musl-g++`.
        bool isMusl = endswith(version, "-musl");
        std::string ximName = indexedCompiler;
        std::string ximVersion = version;
        if (isMusl) {
            ximVersion = version.substr(0, version.size() - 5); // drop "-musl"
            ximName    = "musl-" + indexedCompiler;             // gcc → musl-gcc
        }

        std::string target = std::format("xim:{}@{}", ximName, ximVersion);

        auto cfg = get_cfg();
        if (!cfg) return std::unexpected(cfg.error());
        mcpp::fetcher::Fetcher fetcher(**cfg);

        mcpp::ui::info("Resolving", "toolchain");
        CliInstallProgress progress;
        auto payload = fetcher.resolve_xpkg_path(target, /*autoInstall=*/true, &progress);
        if (!payload) {
            return std::unexpected(std::format(
                "toolchain '{}': {}", *tcSpec, payload.error().message));
        }

        std::string binary_name = "g++";
        if      (indexedCompiler == "llvm" || indexedCompiler == "clang") binary_name = "clang++";
        else if (indexedCompiler == "msvc")                                binary_name = "cl.exe";
        if (isMusl) {
            // musl-gcc xpkg ships only the triple-prefixed frontends.
            binary_name = "x86_64-linux-musl-g++";
        }
        explicit_compiler = payload->binDir / binary_name;
        if (!std::filesystem::exists(explicit_compiler)) {
            return std::unexpected(std::format(
                "toolchain payload '{}' has no '{}' (looked at {})",
                target, binary_name, explicit_compiler.string()));
        }
        mcpp::ui::info("Resolved",
            std::format("{} → {}", *tcSpec,
                mcpp::ui::shorten_path(explicit_compiler,
                    make_path_ctx(&**get_cfg(), *root))));
    } else if (tcSpec.has_value() && *tcSpec == "system") {
        // Explicit user opt-in to system PATH compiler — kept as escape hatch.
    } else if (auto* opt = std::getenv("MCPP_NO_AUTO_INSTALL"); opt && *opt && *opt != '0') {
        // CI / offline / test opt-out: hard-error instead of silently
        // pulling ~800 MB of toolchain. Preserves the original M5.5
        // contract for environments that need it.
        return std::unexpected(
            "no toolchain configured.\n"
            "       run one of:\n"
            "         mcpp toolchain install gcc 15.1.0-musl\n"
            "         mcpp toolchain default gcc@15.1.0-musl\n"
            "       or unset MCPP_NO_AUTO_INSTALL to let mcpp auto-install.");
    } else {
        // First-run UX: no project-level [toolchain], no global default,
        // and the user just ran `mcpp build` (or similar). Auto-install
        // mcpp's canonical default — musl-gcc 15.1.0 — so the user gets
        // a portable static binary out of the box without any config. We
        // pin it as the global default so the next invocation is silent.
        // Users can switch any time via `mcpp toolchain default <spec>`.
        std::string defaultSpec = "gcc@15.1.0-musl";

        mcpp::ui::info("First run",
            std::format("no toolchain configured — installing {} (musl, static) as default",
                        defaultSpec));

        auto cfg = get_cfg();
        if (!cfg) return std::unexpected(cfg.error());
        mcpp::fetcher::Fetcher fetcher(**cfg);

        // gcc@15.1.0-musl → xim:musl-gcc@15.1.0; binary at
        // <root>/bin/x86_64-linux-musl-g++. (Same translation `cmd_toolchain`
        // and the build path apply.)
        std::string ximTarget    = "xim:musl-gcc@15.1.0";
        std::string ximBinaryRel = "x86_64-linux-musl-g++";

        CliInstallProgress progress;
        auto payload = fetcher.resolve_xpkg_path(ximTarget,
                            /*autoInstall=*/true, &progress);
        if (!payload) {
            return std::unexpected(std::format(
                "auto-installing default toolchain {} failed: {}\n"
                "       you can install it manually with:\n"
                "         mcpp toolchain install gcc 15.1.0-musl",
                defaultSpec, payload.error().message));
        }
        explicit_compiler = payload->binDir / ximBinaryRel;
        if (!std::filesystem::exists(explicit_compiler)) {
            return std::unexpected(std::format(
                "default toolchain payload {} has no {} at {}",
                ximTarget, ximBinaryRel, explicit_compiler.string()));
        }

        // Persist the default so we don't ask again next time.
        if (auto wr = mcpp::config::write_default_toolchain(**cfg, defaultSpec); wr) {
            (*cfg)->defaultToolchain = defaultSpec;
            mcpp::ui::status("Default", std::format("set to {}", defaultSpec));
        } // best-effort: a failed config write only loses the persistence,
          // not the running build.
        tcSpec = defaultSpec;
    }

    auto tc = mcpp::toolchain::detect(explicit_compiler);
    if (!tc) return std::unexpected(tc.error().message);

    // For musl-gcc the toolchain is fully self-contained
    // (`<root>/x86_64-linux-musl/{include,lib}` is its own sysroot), and
    // pointing it at mcpp's glibc subos breaks compilation. Skip the
    // sysroot injection in that case — musl-gcc's `-dumpmachine` reports
    // `x86_64-linux-musl`, which is also the marker we use elsewhere.
    bool isMuslTc = tc->targetTriple.find("-musl") != std::string::npos;

    // A musl toolchain only really makes sense with static linkage —
    // dynamic-musl binaries depend on a system /lib/ld-musl-x86_64.so.1
    // that most distros don't ship. Default linkage to "static" when
    // the resolved toolchain is musl, unless the user has already opted
    // out via [build].linkage / [target.<triple>].linkage.
    if (isMuslTc && m->buildConfig.linkage.empty()) {
        m->buildConfig.linkage = "static";
    }

    // M5.5: prefer mcpp's xlings-managed subos as sysroot — it has glibc
    // headers + libs in the conventional layout that GCC expects. The
    // -print-sysroot output from a freshly-built GCC often points at
    // some build-time path that doesn't exist on the user's machine.
    if (!isMuslTc) {
        if (auto cfg = get_cfg(); cfg) {
            auto mcppSubos = (*cfg)->xlingsHome() / "subos" / "default";
            if (std::filesystem::exists(mcppSubos / "usr" / "include")) {
                tc->sysroot = mcppSubos;
            }
        }
    }

    if (m->language.importStd && !tc->hasImportStd) {
        return std::unexpected(std::format(
            "manifest requires import_std but toolchain '{}' provides no std module source",
            tc->label()));
    }

    // Resolve dependencies: walk the **transitive** graph from the main
    // manifest, BFS-style. Each unique `(namespace, shortName)` is fetched
    // once, its `[build].include_dirs` are propagated to the main
    // manifest, and its own `[dependencies]` are queued for processing
    // (its `[dev-dependencies]` are NOT — those are private to the dep's
    // own test runs).
    //
    // Conflict policy: C++ modules require globally-unique module names
    // and ODR-respecting symbols, so the same `(ns, name)` resolved to
    // two different exact versions is an error — mcpp prints both
    // requesting parents and asks the user to align them.
    std::vector<mcpp::modgraph::PackageRoot> packages;
    packages.push_back({*root, *m});

    // dep_manifests is kept around purely so the build plan can move it
    // out at the end (PackageRoot stores a `Manifest` by value, so the
    // unique_ptr is not load-bearing for liveness — it's a leftover from
    // an earlier design and harmless).
    std::vector<std::unique_ptr<mcpp::manifest::Manifest>> dep_manifests;

    struct ResolvedKey {
        std::string ns;
        std::string shortName;
        auto operator<=>(const ResolvedKey&) const = default;
    };
    struct ResolvedRecord {
        std::string version;            // empty for path/git deps
        std::string constraint;         // AND-combined original constraints (version src only)
        std::string requestedBy;        // human-readable for error messages
        std::string source;             // "version" | "path" | "git" — for type-clash check
        std::size_t depIndex = 0;       // index into dep_manifests/packages-1 (for in-place re-fetch)
        std::vector<std::filesystem::path> includeDirsAdded;  // entries appended to m->buildConfig.includeDirs by this dep
    };
    std::map<ResolvedKey, ResolvedRecord> resolved;

    // Sentinel for "the consumer is the main package" (no dep_manifests entry).
    constexpr std::size_t kMainConsumer = static_cast<std::size_t>(-1);

    struct WorkItem {
        std::string                          name;                // dep map key as written
        mcpp::manifest::DependencySpec       spec;                // copy (we may mutate version)
        std::string                          requestedBy;         // who asked for it
        std::string                          originalConstraint;  // spec.version BEFORE pinning (for SemVer merge)
        std::size_t                          consumerDepIndex;    // dep_manifests slot of who pushed this child; kMainConsumer for main
    };
    std::deque<WorkItem> worklist;

    // SemVer constraint resolver, shared across the worklist so transitive
    // deps with caret/range constraints (`^1.0`) also get pinned to a
    // concrete version before fetch.
    auto resolveSemver = [&](mcpp::manifest::DependencySpec& s,
                              const std::string& depName)
        -> std::expected<void, std::string>
    {
        if (s.isPath() || s.isGit()) return {};
        if (!mcpp::pm::is_version_constraint(s.version)) return {};
        auto cfg = get_cfg();
        if (!cfg) return std::unexpected(cfg.error());
        mcpp::fetcher::Fetcher fetcher(**cfg);
        auto resolved = mcpp::pm::resolve_semver(depName, s.version, fetcher);
        if (!resolved) return std::unexpected(resolved.error());
        mcpp::ui::info("Resolved",
            std::format("{} {} → v{}", depName, s.version, *resolved));
        s.version = std::move(*resolved);
        return {};
    };

    // Acquire a version-source dep at a specific pinned version. Used both
    // by the first-time walk and by the SemVer merger when a re-fetch at a
    // different version is needed. Returns the dep's effective root (where
    // mcpp.toml lives) and a fully loaded manifest.
    using LoadedDep = std::pair<std::filesystem::path, mcpp::manifest::Manifest>;
    auto loadVersionDep = [&](const std::string& depName,
                              const std::string& version)
        -> std::expected<LoadedDep, std::string>
    {
        auto cfg = get_cfg();
        if (!cfg) return std::unexpected(cfg.error());
        mcpp::fetcher::Fetcher fetcher(**cfg);

        auto installed = fetcher.install_path(depName, version);
        if (!installed) {
            mcpp::ui::info("Downloading", std::format("{} v{}", depName, version));
            std::vector<std::string> targets{ std::format("{}@{}", depName, version) };
            CliInstallProgress progress;
            auto r = fetcher.install(targets, &progress);
            if (!r) return std::unexpected(std::format(
                "fetch '{}@{}': {}", depName, version, r.error().message));
            if (r->exitCode != 0) {
                std::string err = std::format(
                    "fetch '{}@{}' failed (exit {})", depName, version, r->exitCode);
                if (r->error) err += ": " + r->error->message;
                return std::unexpected(err);
            }
            installed = fetcher.install_path(depName, version);
            if (!installed) return std::unexpected(std::format(
                "package '{}@{}' install path missing after fetch", depName, version));
        }
        std::filesystem::path verRoot = *installed;

        auto luaContent = fetcher.read_xpkg_lua(depName);
        if (!luaContent) return std::unexpected(std::format(
            "dependency '{}': index entry not found in local clone", depName));
        auto field = mcpp::manifest::extract_mcpp_field(*luaContent);

        std::optional<mcpp::manifest::Manifest> manifest;
        std::filesystem::path effRoot = verRoot;
        auto loadFrom = [&](const std::filesystem::path& mcppToml)
            -> std::expected<void, std::string>
        {
            auto dm = mcpp::manifest::load(mcppToml);
            if (!dm) return std::unexpected(std::format(
                "dependency '{}' (at '{}'): {}",
                depName, mcppToml.string(), dm.error().format()));
            manifest = std::move(*dm);
            effRoot  = mcppToml.parent_path();
            return {};
        };
        if (field.kind == mcpp::manifest::McppField::StringPath) {
            auto matches = mcpp::modgraph::expand_glob(verRoot, field.value);
            if (matches.empty()) return std::unexpected(std::format(
                "dependency '{}': mcpp pointer '{}' did not match any "
                "file under '{}'", depName, field.value, verRoot.string()));
            if (matches.size() > 1) return std::unexpected(std::format(
                "dependency '{}': mcpp pointer '{}' matched {} files "
                "(expected exactly one)", depName, field.value, matches.size()));
            if (auto r = loadFrom(matches.front()); !r) return std::unexpected(r.error());
        } else if (field.kind == mcpp::manifest::McppField::TableBody) {
            auto dm = mcpp::manifest::synthesize_from_xpkg_lua(*luaContent, depName, version);
            if (!dm) return std::unexpected(std::format(
                "dependency '{}': {}", depName, dm.error().format()));
            manifest = std::move(*dm);
            // effRoot stays as verRoot
        } else {
            std::vector<std::filesystem::path> matches;
            for (auto pat : { "mcpp.toml", "*/mcpp.toml" }) {
                matches = mcpp::modgraph::expand_glob(verRoot, pat);
                if (!matches.empty()) break;
            }
            if (matches.empty()) return std::unexpected(std::format(
                "dependency '{}': index entry has no `mcpp = ...` field, "
                "and no mcpp.toml was found at <verdir>/mcpp.toml or "
                "<verdir>/*/mcpp.toml — add an explicit `mcpp = \"<path>\"` "
                "or `mcpp = {{ ... }}` block to the .lua descriptor.",
                depName));
            if (matches.size() > 1) return std::unexpected(std::format(
                "dependency '{}': default mcpp.toml lookup matched {} "
                "files; pin one with explicit `mcpp = \"<path>\"`.",
                depName, matches.size()));
            if (auto r = loadFrom(matches.front()); !r) return std::unexpected(r.error());
        }
        return std::pair{effRoot, std::move(*manifest)};
    };

    // Append a dep's [build].include_dirs onto the main manifest's, glob-
    // expanded against the dep's root. Returns the absolute paths actually
    // appended so the caller can later evict them on a SemVer-merge re-fetch.
    auto propagateIncludeDirs = [&](const std::filesystem::path& depRoot,
                                    const mcpp::manifest::Manifest& depManifest)
        -> std::vector<std::filesystem::path>
    {
        std::vector<std::filesystem::path> added;
        for (auto& inc : depManifest.buildConfig.includeDirs) {
            if (inc.is_absolute()) {
                m->buildConfig.includeDirs.push_back(inc);
                added.push_back(inc);
                continue;
            }
            auto matches = mcpp::modgraph::expand_dir_glob(depRoot, inc.generic_string());
            if (matches.empty()) continue;
            for (auto& d : matches) {
                m->buildConfig.includeDirs.push_back(d);
                added.push_back(d);
            }
        }
        return added;
    };

    // Drop earlier include_dirs that came from a now-superseded dep version.
    // Erases by value match — safe because the outer code only ever appends,
    // and on re-fetch we re-record the new entries afterwards.
    auto removeIncludeDirs = [&](const std::vector<std::filesystem::path>& paths) {
        auto& dirs = m->buildConfig.includeDirs;
        for (auto& p : paths) {
            auto pos = std::find(dirs.begin(), dirs.end(), p);
            if (pos != dirs.end()) dirs.erase(pos);
        }
    };

    // Stage a dep's source files into a fresh directory, rewriting their
    // module / import declarations against `rename`. Used by the multi-
    // version mangling fallback (Level 1) so two cross-major copies of
    // the same package can coexist with distinct module names.
    //
    // Headers (referenced via `[build].include_dirs`) are NOT staged —
    // those keep pointing at the original install dir via absolutized
    // include paths.
    auto stage_with_rewrite = [](const std::filesystem::path& srcRoot,
                                  const std::filesystem::path& dstRoot,
                                  const mcpp::manifest::Manifest& depManifest,
                                  const std::map<std::string, std::string>& rename)
        -> std::expected<void, std::string>
    {
        std::error_code ec;
        std::filesystem::create_directories(dstRoot, ec);
        if (ec) return std::unexpected(std::format(
            "stage: cannot create '{}': {}", dstRoot.string(), ec.message()));

        // Resolve the source globs against the original root, falling
        // back to the convention default if the manifest didn't set any.
        std::vector<std::string> globs = depManifest.modules.sources;
        if (globs.empty()) {
            globs = { "src/**/*.cppm", "src/**/*.cpp",
                      "src/**/*.cc",   "src/**/*.c" };
        }
        std::set<std::filesystem::path> sourceFiles;
        for (auto const& g : globs) {
            for (auto& p : mcpp::modgraph::expand_glob(srcRoot, g)) {
                sourceFiles.insert(p);
            }
        }
        if (sourceFiles.empty()) {
            return std::unexpected(std::format(
                "stage: no source files found under '{}' (globs={})",
                srcRoot.string(), globs.size()));
        }

        for (auto const& f : sourceFiles) {
            auto rel = std::filesystem::relative(f, srcRoot, ec);
            if (ec) return std::unexpected(std::format(
                "stage: cannot relativize '{}': {}", f.string(), ec.message()));
            auto dst = dstRoot / rel;
            std::filesystem::create_directories(dst.parent_path(), ec);

            std::ifstream is(f);
            if (!is) return std::unexpected(std::format(
                "stage: cannot read '{}'", f.string()));
            std::stringstream buf; buf << is.rdbuf();
            std::string content = buf.str();

            std::string out = mcpp::pm::rewrite_module_decls(content, rename);
            std::ofstream os(dst);
            if (!os) return std::unexpected(std::format(
                "stage: cannot write '{}'", dst.string()));
            os << out;
        }
        return {};
    };

    // Seed the worklist from the main manifest. Dev-deps only when the
    // caller wants them; they're never propagated transitively.
    const std::string mainPkgLabel = m->package.name;
    for (auto& [n, s] : m->dependencies) {
        worklist.push_back({n, s, mainPkgLabel, s.version, kMainConsumer});
    }
    if (includeDevDeps) {
        for (auto& [n, s] : m->devDependencies) {
            worklist.push_back({n, s, mainPkgLabel + " (dev-dep)",
                                s.version, kMainConsumer});
        }
    }

    while (!worklist.empty()) {
        auto item = std::move(worklist.front());
        worklist.pop_front();

        const auto& name = item.name;
        auto& spec = item.spec;

        // Pin SemVer constraint before dedup/fetch.
        if (auto r = resolveSemver(spec, name); !r) {
            return std::unexpected(r.error());
        }

        ResolvedKey key{
            spec.namespace_.empty()
                ? std::string{mcpp::manifest::kDefaultNamespace}
                : spec.namespace_,
            spec.shortName.empty() ? name : spec.shortName,
        };
        const std::string sourceKind =
            spec.isPath()    ? "path"
            : spec.isGit()    ? "git"
            : "version";

        if (auto it = resolved.find(key); it != resolved.end()) {
            // Conflict detection.
            if (it->second.source != sourceKind) {
                return std::unexpected(std::format(
                    "dependency '{}{}{}' is requested as both a {} dep "
                    "(by '{}') and a {} dep (by '{}'). Pick one.",
                    key.ns, key.ns.empty() ? "" : ".", key.shortName,
                    it->second.source, it->second.requestedBy,
                    sourceKind, item.requestedBy));
            }
            if (sourceKind == "version" && it->second.version != spec.version) {
                // SemVer merge attempt: AND-combine the two original
                // constraint strings and ask the index for a single version
                // satisfying both. Same-major caret/tilde/exact pairs that
                // overlap converge here; cross-major or otherwise
                // unsatisfiable pairs fall through to a hard error (a future
                // PR adds multi-version mangling as a Level-1 fallback).
                auto cfg = get_cfg();
                if (!cfg) return std::unexpected(cfg.error());
                mcpp::fetcher::Fetcher fetcher(**cfg);

                auto merged = mcpp::pm::try_merge_semver(
                    name,
                    it->second.constraint,
                    item.originalConstraint,
                    fetcher);
                if (!merged) {
                    // Level 1 fallback: multi-version mangling. Two
                    // versions can't be reconciled by SemVer, but they
                    // can coexist in the same build if we mangle the
                    // secondary copy's module name and rewrite the one
                    // consumer that asked for it. The primary keeps its
                    // authored module name so consumers that don't care
                    // about the secondary see no churn.
                    //
                    // MVP scope (these limits surface as clear errors):
                    //   * The conflicting consumer must be a dep, not
                    //     the main package — main-package mangling
                    //     would mean rewriting user-authored sources,
                    //     which is too surprising for a fallback path.
                    //   * The secondary version must be a leaf (no own
                    //     transitive deps) — recursive mangling is
                    //     deferred to a follow-up.
                    if (item.consumerDepIndex == kMainConsumer) {
                        return std::unexpected(std::format(
                            "dependency '{}{}{}' has irreconcilable versions:\n"
                            "  '{}' (constraint '{}') requested by '{}'\n"
                            "  '{}' (constraint '{}') requested by '{}'\n"
                            "SemVer merge: {}\n"
                            "Multi-version mangling can't help here — the conflict "
                            "involves the main package directly. Pin one version "
                            "explicitly in your mcpp.toml.",
                            key.ns, key.ns.empty() ? "" : ".", key.shortName,
                            it->second.version, it->second.constraint, it->second.requestedBy,
                            spec.version, item.originalConstraint, item.requestedBy,
                            merged.error()));
                    }

                    auto loaded = loadVersionDep(name, spec.version);
                    if (!loaded) return std::unexpected(loaded.error());
                    auto& [secondaryRoot, secondaryManifest] = *loaded;

                    if (!secondaryManifest.dependencies.empty()) {
                        return std::unexpected(std::format(
                            "dependency '{}{}{}' has irreconcilable versions:\n"
                            "  '{}' requested by '{}'\n"
                            "  '{}' requested by '{}'\n"
                            "Multi-version mangling fallback only handles leaf "
                            "secondaries in 0.0.3 — but the secondary v{} declares "
                            "its own dependencies, which would need recursive "
                            "mangling. Pin one version explicitly, or wait for "
                            "the recursive-mangling extension.",
                            key.ns, key.ns.empty() ? "" : ".", key.shortName,
                            it->second.version, it->second.requestedBy,
                            spec.version, item.requestedBy,
                            spec.version));
                    }

                    // Module names in the source files use the dep's full
                    // [package].name (e.g. "mcpplibs.cmdline"), not the
                    // namespaced-subtable shortName. Use that for the
                    // rename key so the rewriter actually matches what the
                    // .cppm sources declare.
                    const std::string moduleName = secondaryManifest.package.name;
                    std::string mangled =
                        mcpp::pm::mangle_name(moduleName, spec.version);

                    // Stage layout:
                    //   <root>/target/.mangled/<consumerPkg>/<dep>__<version>/    ← rewritten secondary source
                    //   <root>/target/.mangled/<consumerPkg>/__self__/             ← rewritten consumer source
                    auto& consumerManifest = *dep_manifests[item.consumerDepIndex];
                    auto consumerRoot      = packages[item.consumerDepIndex + 1].root;
                    auto stageBase         = *root / "target" / ".mangled"
                                             / consumerManifest.package.name;
                    auto secStage          = stageBase
                                             / std::format("{}__{}", moduleName, spec.version);
                    auto consumerStage     = stageBase / "__self__";

                    std::map<std::string, std::string> rename{ {moduleName, mangled} };
                    if (auto r = stage_with_rewrite(secondaryRoot, secStage,
                                                     secondaryManifest, rename); !r)
                        return std::unexpected(r.error());
                    if (auto r = stage_with_rewrite(consumerRoot, consumerStage,
                                                     consumerManifest, rename); !r)
                        return std::unexpected(r.error());

                    // Re-anchor the consumer's PackageRoot at its staged copy
                    // so the modgraph scanner picks up the rewritten imports.
                    packages[item.consumerDepIndex + 1].root = consumerStage;

                    // Record the staged secondary as a brand-new dep entry
                    // under its mangled name, so future encounters of this
                    // exact (ns, mangled) pair dedup cleanly. The original
                    // primary entry (it->second) is untouched.
                    auto stagedManifest = secondaryManifest;
                    // Update [package].name to the mangled module name so
                    // the modgraph validator (which checks "exported module
                    // must be prefixed by package name") accepts the
                    // rewritten sources.
                    stagedManifest.package.name = mangled;
                    // Absolutize secondary's include_dirs against its original
                    // install root so the staged copy still finds headers.
                    for (auto& inc : stagedManifest.buildConfig.includeDirs) {
                        if (inc.is_relative()) inc = secondaryRoot / inc;
                    }

                    dep_manifests.push_back(
                        std::make_unique<mcpp::manifest::Manifest>(std::move(stagedManifest)));
                    packages.push_back({secStage, *dep_manifests.back()});
                    auto added = propagateIncludeDirs(secStage, *dep_manifests.back());

                    ResolvedKey mangledKey{key.ns, mangled};
                    resolved[mangledKey] = ResolvedRecord{
                        .version           = spec.version,
                        .constraint        = item.originalConstraint,
                        .requestedBy       = item.requestedBy,
                        .source            = "version",
                        .depIndex          = dep_manifests.size() - 1,
                        .includeDirsAdded  = std::move(added),
                    };

                    mcpp::ui::info("Mangled",
                        std::format("{} v{} ↔ v{} → {} (cross-major fallback)",
                            moduleName, it->second.version, spec.version, mangled));
                    continue;
                }

                // Combine the constraint strings so future merges AND with
                // both. Empty originalConstraint means "any" — use "*".
                const std::string& addCstr =
                    item.originalConstraint.empty() ? std::string("*")
                                                    : item.originalConstraint;
                if (it->second.constraint.empty())
                    it->second.constraint = addCstr;
                else
                    it->second.constraint += "," + addCstr;

                if (*merged == it->second.version) {
                    // The existing pin already satisfies the new constraint —
                    // no re-fetch needed; just record this consumer.
                    continue;
                }

                // Merged version differs from the previously-pinned one.
                // Re-fetch the dep at the merged version and replace the
                // earlier slot in dep_manifests / packages so the build plan
                // sees only one version. Old include_dir entries are evicted
                // and the new manifest's entries are appended.
                mcpp::ui::info("Merged",
                    std::format("{}{}{} {} ⨯ {} → v{}",
                        key.ns, key.ns.empty() ? "" : ".", key.shortName,
                        it->second.version, spec.version, *merged));
                auto reloaded = loadVersionDep(name, *merged);
                if (!reloaded) return std::unexpected(reloaded.error());
                auto& [newRoot, newManifest] = *reloaded;

                // Name match against the re-loaded manifest.
                {
                    const std::string& expectedShort =
                        spec.shortName.empty() ? name : spec.shortName;
                    std::string expectedComposite;
                    if (!spec.namespace_.empty()
                        && spec.namespace_ != mcpp::manifest::kDefaultNamespace) {
                        expectedComposite = std::format("{}.{}",
                            spec.namespace_, expectedShort);
                    }
                    const bool nameOk =
                        newManifest.package.name == expectedShort
                        || (!expectedComposite.empty()
                            && newManifest.package.name == expectedComposite);
                    if (!nameOk) {
                        return std::unexpected(std::format(
                            "dependency '{}' (merged to v{}) resolved to "
                            "package '{}' (mismatch with declared name '{}')",
                            name, *merged, newManifest.package.name,
                            expectedShort));
                    }
                }

                removeIncludeDirs(it->second.includeDirsAdded);
                auto added = propagateIncludeDirs(newRoot, newManifest);

                // Replace in dep_manifests + packages. depIndex is the slot
                // in dep_manifests; packages = [main, dep_0, dep_1, …], so
                // packages[depIndex+1] is the same dep.
                *dep_manifests[it->second.depIndex] = std::move(newManifest);
                packages[it->second.depIndex + 1] =
                    {newRoot, *dep_manifests[it->second.depIndex]};

                it->second.version            = *merged;
                it->second.includeDirsAdded   = std::move(added);

                // Walk the *new* manifest's deps so their constraints feed
                // future merges. Already-resolved children dedup via the
                // resolved map.
                const std::string newLabel = std::format("{}{}{}@{}",
                    key.ns, key.ns.empty() ? "" : ".",
                    key.shortName, *merged);
                for (auto& [child_name, child_spec] :
                        dep_manifests[it->second.depIndex]->dependencies) {
                    worklist.push_back({child_name, child_spec, newLabel,
                                        child_spec.version,
                                        it->second.depIndex});
                }
                continue;
            }
            // Same key, same version (or compatible path/git) — already
            // processed; skip.
            continue;
        }

        std::filesystem::path dep_root;

        if (spec.isPath()) {
            // Path-based: resolve relative to project root.
            dep_root = spec.path;
            if (dep_root.is_relative()) dep_root = *root / dep_root;
            dep_root = std::filesystem::weakly_canonical(dep_root);
        } else if (spec.isGit()) {
            // Git-based (M4 #5): clone into ~/.mcpp/git/<hash>/<rev>/
            // and treat as a path dep from there.
            auto mcppHome = [] {
                if (auto* e = std::getenv("MCPP_HOME"); e && *e)
                    return std::filesystem::path(e);
                if (auto* e = std::getenv("HOME"); e && *e)
                    return std::filesystem::path(e) / ".mcpp";
                return std::filesystem::current_path() / ".mcpp";
            }();
            // Cache key: hash(url + refkind + ref). Avoids collisions across
            // different revs of the same repo.
            std::hash<std::string> H;
            auto urlHash = std::format("{:016x}",
                H(spec.git + "|" + spec.gitRefKind + "|" + spec.gitRev));
            auto gitRoot = mcppHome / "git" / urlHash;
            std::error_code ec;
            std::filesystem::create_directories(gitRoot.parent_path(), ec);
            if (!std::filesystem::exists(gitRoot / ".git")) {
                mcpp::ui::info("Cloning",
                    std::format("{} ({} = {})", spec.git, spec.gitRefKind, spec.gitRev));
                std::string cloneCmd;
                if (spec.gitRefKind == "branch") {
                    cloneCmd = std::format(
                        "git clone --depth 1 --branch '{}' '{}' '{}' 2>&1",
                        spec.gitRev, spec.git, gitRoot.string());
                } else {
                    // For tag/rev: full clone, then checkout (depth-1 may miss the rev).
                    cloneCmd = std::format(
                        "git clone '{}' '{}' && cd '{}' && git checkout --quiet '{}' 2>&1",
                        spec.git, gitRoot.string(),
                        gitRoot.string(), spec.gitRev);
                }
                std::string out;
                {
                    std::array<char, 8192> buf{};
                    std::FILE* fp = ::popen(cloneCmd.c_str(), "r");
                    if (!fp) return std::unexpected("popen failed for git clone");
                    while (std::fgets(buf.data(), buf.size(), fp)) out += buf.data();
                    int rc = ::pclose(fp);
                    if (rc != 0) {
                        std::filesystem::remove_all(gitRoot, ec);
                        return std::unexpected(std::format(
                            "git clone of '{}' failed:\n{}", spec.git, out));
                    }
                }
            }
            dep_root = gitRoot;
        }
        // (version-source: dep_root + manifest are loaded together via
        // loadVersionDep below since the index entry drives both.)

        // Manifest acquisition.
        //   - Path/git dep: dep_root is the source tree, mcpp.toml at root.
        //   - Version dep: delegate to loadVersionDep — the index entry's
        //     `mcpp` field decides where mcpp.toml lives (StringPath /
        //     TableBody / default lookup).
        std::optional<mcpp::manifest::Manifest> dep_manifest;
        if (spec.isPath() || spec.isGit()) {
            if (!std::filesystem::exists(dep_root / "mcpp.toml")) {
                return std::unexpected(std::format(
                    "{} dependency '{}' (at '{}') has no mcpp.toml",
                    spec.isGit() ? "git" : "path", name, dep_root.string()));
            }
            auto dm = mcpp::manifest::load(dep_root / "mcpp.toml");
            if (!dm) {
                return std::unexpected(std::format(
                    "dependency '{}' (at '{}'): {}",
                    name, dep_root.string(), dm.error().format()));
            }
            dep_manifest = std::move(*dm);
        } else {
            auto loaded = loadVersionDep(name, spec.version);
            if (!loaded) return std::unexpected(loaded.error());
            dep_root     = std::move(loaded->first);
            dep_manifest = std::move(loaded->second);
        }

        // Name match: prefer the dep's *short* name (the new xpkg-style
        // `[package].name = "<short>"` + separate `namespace` field), but
        // fall back to the legacy composite form `<ns>.<short>` so existing
        // index descriptors that still embed the namespace in the name
        // string (`name = "mcpplibs.cmdline"`) keep resolving until the
        // mcpp-index repo is migrated.
        const std::string& expectedShort =
            spec.shortName.empty() ? name : spec.shortName;
        std::string expectedComposite;
        if (!spec.namespace_.empty()
            && spec.namespace_ != mcpp::manifest::kDefaultNamespace) {
            expectedComposite = std::format("{}.{}", spec.namespace_, expectedShort);
        }
        const bool nameOk =
            dep_manifest->package.name == expectedShort
            || (!expectedComposite.empty()
                && dep_manifest->package.name == expectedComposite);
        if (!nameOk) {
            return std::unexpected(std::format(
                "dependency '{}' resolved to package '{}' (mismatch with declared name '{}')",
                name, dep_manifest->package.name, expectedShort));
        }

        // Propagate dep's [build].include_dirs to the main manifest. The
        // returned vector is what was actually appended (after glob
        // expansion against dep_root) — stash it so a SemVer merge can
        // evict these entries on a re-fetch.
        auto includeDirsAdded = propagateIncludeDirs(dep_root, *dep_manifest);

        // Move the manifest into stable storage so we can later look it up
        // by depIndex (the SemVer merger needs to overwrite the slot).
        dep_manifests.push_back(
            std::make_unique<mcpp::manifest::Manifest>(std::move(*dep_manifest)));
        packages.push_back({dep_root, *dep_manifests.back()});

        // Record this dep as resolved so future encounters of the same
        // (ns, name) hit the fast path (skip / merge / conflict).
        resolved[key] = ResolvedRecord{
            .version           = sourceKind == "version" ? spec.version : "",
            .constraint        = sourceKind == "version" ? item.originalConstraint : "",
            .requestedBy       = item.requestedBy,
            .source            = sourceKind,
            .depIndex          = dep_manifests.size() - 1,
            .includeDirsAdded  = std::move(includeDirsAdded),
        };

        // Recurse: the dep's own [dependencies] become new worklist items.
        // dev-dependencies are intentionally NOT walked — those are
        // private to the dep's test runs, not part of its public ABI.
        const std::string thisDepLabel = std::format(
            "{}{}{}@{}",
            key.ns,
            key.ns.empty() ? "" : ".",
            key.shortName,
            sourceKind == "version" ? spec.version : sourceKind);
        const std::size_t selfIdx = dep_manifests.size() - 1;
        for (auto& [child_name, child_spec] : dep_manifests.back()->dependencies) {
            worklist.push_back({child_name, child_spec, thisDepLabel,
                                child_spec.version, selfIdx});
        }
    }

    // Modgraph: regex scanner by default; opt-in to compiler-driven P1689
    // scanner via env var MCPP_SCANNER=p1689 (see docs/27).
    auto scan = [&] {
        const char* sel = std::getenv("MCPP_SCANNER");
        if (sel && std::string_view(sel) == "p1689") {
            auto tmp = std::filesystem::temp_directory_path()
                     / std::format("mcpp_p1689_{}", std::random_device{}());
            std::filesystem::create_directories(tmp);
            return mcpp::modgraph::scan_packages_p1689(packages, *tc, tmp);
        }
        return mcpp::modgraph::scan_packages(packages);
    }();
    if (!scan.errors.empty()) {
        std::string msg = "scanner errors:\n";
        for (auto& e : scan.errors) msg += "  " + e.format() + "\n";
        return std::unexpected(msg);
    }
    for (auto& w : scan.warnings) {
        std::println(stderr, "warning: {}", w.format());
    }

    auto report = mcpp::modgraph::validate(scan.graph, *m, *root);
    for (auto& w : report.warnings) {
        if (w.path.empty()) std::println(stderr, "warning: {}", w.message);
        else std::println(stderr, "warning: {}: {}", w.path.string(), w.message);
    }
    if (!report.ok()) {
        std::string msg = "validation errors:\n";
        for (auto& e : report.errors) {
            if (e.path.empty()) msg += "  " + e.message + "\n";
            else msg += "  " + e.path.string() + ": " + e.message + "\n";
        }
        return std::unexpected(msg);
    }

    // Compute fingerprint (no lockfile in M1 → empty hash)
    mcpp::toolchain::FingerprintInputs fpi;
    fpi.toolchain            = *tc;
    fpi.cppStandard         = m->language.standard;
    fpi.compileFlags        = canonical_compile_flags(*m);
    fpi.dependencyLockHash = "";    // M2
    fpi.stdBmiHash         = "";    // updated after stdmod build (chicken/egg ok for M1)
    auto fp = mcpp::toolchain::compute_fingerprint(fpi);

    // Pre-build std module
    auto sm = mcpp::toolchain::ensure_built(*tc, fp.hex);
    if (!sm) return std::unexpected(sm.error().message);

    if (print_fingerprint) {
        std::println("Toolchain: {}", tc->label());
        std::println("Fingerprint: {}", fp.hex);
        for (std::size_t i = 0; i < fp.parts.size(); ++i) {
            std::println("  [{}] {}", i + 1, fp.parts[i]);
        }
    }

    BuildContext ctx;
    ctx.manifest    = *m;
    ctx.tc          = *tc;
    ctx.fp          = fp;
    ctx.projectRoot= *root;
    ctx.outputDir  = target_dir(*tc, fp, *root);
    ctx.stdBmi     = sm->bmiPath;
    ctx.stdObject  = sm->objectPath;
    ctx.plan        = mcpp::build::make_plan(*m, *tc, fp, scan.graph, report.topoOrder,
                                             *root, ctx.outputDir, sm->bmiPath, sm->objectPath);

    // ─── M3.2: BMI cache stage / populate-task collection ─────────────
    // For each version-based dep package (i.e. fetched from a registry,
    // not a path dep), check the global BMI cache. If cached → stage into
    // the project's target dir so ninja sees those outputs as up-to-date
    // and skips them. If not → record a populate task for AFTER build.
    //
    // Path deps don't go through the cache: their sources can change at
    // any time outside fingerprint awareness.
    auto cfg2 = get_cfg();
    if (cfg2) {
        std::error_code mkEc;
        std::filesystem::create_directories(ctx.outputDir, mkEc);
        for (std::size_t i = 1; i < packages.size(); ++i) {  // skip [0] = main
            const auto& pkgRoot   = packages[i];
            const auto& depName   = pkgRoot.manifest.package.name;
            const auto& depVer    = pkgRoot.manifest.package.version;

            // Find this dep's spec from the consumer manifest to know
            // if it's path-based or version-based.
            auto specIt = m->dependencies.find(depName);
            // Path AND git deps bypass the BMI cache: their sources can
            // change outside the fingerprint's awareness.
            bool skipCache = (specIt != m->dependencies.end() &&
                              (specIt->second.isPath() || specIt->second.isGit()));
            if (specIt == m->dependencies.end()) {
                auto devIt = m->devDependencies.find(depName);
                if (devIt != m->devDependencies.end()) {
                    skipCache = devIt->second.isPath() || devIt->second.isGit();
                }
            }
            if (skipCache) continue;

            mcpp::bmi_cache::CacheKey key {
                .mcppHome    = (*cfg2)->mcppHome,
                .fingerprint = fp.hex,
                .indexName   = (*cfg2)->defaultIndex,
                .packageName = depName,
                .version     = depVer,
            };

            // Compute the artifacts list from the build plan: every
            // CompileUnit whose source lies under this dep's root contributes.
            mcpp::bmi_cache::DepArtifacts arts;
            for (auto& cu : ctx.plan.compileUnits) {
                std::error_code ec;
                auto rel = std::filesystem::relative(cu.source, pkgRoot.root, ec);
                if (ec || rel.empty()) continue;
                auto rels = rel.string();
                if (rels.starts_with("..")) continue;       // not under depRoot

                if (cu.providesModule) {
                    std::string gcm;
                    for (char c : *cu.providesModule)
                        gcm.push_back(c == ':' ? '-' : c);
                    gcm += ".gcm";
                    arts.gcmFiles.push_back(std::move(gcm));
                }
                arts.objFiles.push_back(cu.object.filename().string());
            }

            if (mcpp::bmi_cache::is_cached(key)) {
                auto staged = mcpp::bmi_cache::stage_into(key, ctx.outputDir);
                if (staged) {
                    ctx.cachedDepLabels.push_back(
                        std::format("{} v{}", depName, depVer));
                    continue;       // skip populate task; it's already cached
                }
                // stage failed — fall through to recompile + repopulate
            }
            ctx.depsToPopulate.push_back({ std::move(key), std::move(arts) });
        }
    }
    // ──────────────────────────────────────────────────────────────────

    // Write/update mcpp.lock for any version-based deps that succeeded.
    // Path deps are intentionally NOT locked — their source is local filesystem.
    {
        mcpp::lockfile::Lockfile lock;
        for (auto const& [name, spec] : m->dependencies) {
            if (spec.isPath()) continue;
            mcpp::lockfile::LockedPackage lp;
            lp.name    = name;
            lp.version = spec.version;
            lp.source  = std::format("mcpp-index+{}",
                                     "https://github.com/mcpp-community/mcpp-index.git");
            lp.hash    = "sha256:<from-xlings>";   // M3 will populate from install plan
            lock.packages.push_back(std::move(lp));
        }
        if (!lock.packages.empty()) {
            auto lockPath = *root / "mcpp.lock";
            (void)mcpp::lockfile::write(lock, lockPath);
        }
    }

    return ctx;
}

// Compile a prepared BuildContext. Shared between `mcpp build` and `mcpp run`
// so the latter doesn't call prepare_build twice (and re-print the toolchain
// resolution banner).
int run_build_plan(BuildContext& ctx, bool verbose, bool no_cache) {
    if (no_cache) {
        std::error_code ec;
        std::filesystem::remove_all(ctx.outputDir, ec);
    }

    auto be = mcpp::build::make_ninja_backend();

    // M5.0: print "Inferred" banner when defaults / target inference fired.
    for (auto& note : ctx.manifest.inferredNotes) {
        mcpp::ui::status("Inferred", note);
    }

    // Announce the package being built (and any deps).
    // Deps that hit the BMI cache get "Cached" instead of "Compiling".
    std::set<std::string> cachedNames;
    for (auto& label : ctx.cachedDepLabels) {
        auto sp = label.find(' ');
        cachedNames.insert(sp == std::string::npos ? label : label.substr(0, sp));
    }
    std::set<std::string> announced;
    announced.insert(ctx.manifest.package.name);
    mcpp::ui::status("Compiling",
        std::format("{} v{} (.)",
                    ctx.manifest.package.name, ctx.manifest.package.version));
    for (auto& [name, spec] : ctx.manifest.dependencies) {
        if (announced.contains(name)) continue;
        announced.insert(name);
        std::string ver = spec.isPath() ? "(path)" : std::string("v") + spec.version;
        const char* verb = cachedNames.contains(name) ? "Cached" : "Compiling";
        mcpp::ui::status(verb, std::format("{} {}", name, ver));
    }

    mcpp::build::BuildOptions opts;
    opts.verbose = verbose;
    auto r = be->build(ctx.plan, opts);
    if (!r) {
        mcpp::ui::error(r.error().message);
        return 1;
    }

    // M3.2: populate BMI cache for deps that did NOT hit cache.
    for (auto& task : ctx.depsToPopulate) {
        auto pr = mcpp::bmi_cache::populate_from(task.key, ctx.outputDir, task.artifacts);
        if (!pr) {
            mcpp::ui::warning(std::format(
                "bmi cache populate failed for {}@{}: {}",
                task.key.packageName, task.key.version, pr.error()));
        }
    }

    mcpp::ui::finished("release", r->elapsed);
    return 0;
}

int cmd_build(const mcpplibs::cmdline::ParsedArgs& parsed) {
    bool verbose  = parsed.is_flag_set("verbose");
    bool print_fp = parsed.is_flag_set("print-fingerprint");
    bool no_cache = parsed.is_flag_set("no-cache");

    BuildOverrides ov;
    if (auto t = parsed.value("target")) ov.target_triple = *t;
    ov.force_static = parsed.is_flag_set("static");

    auto ctx = prepare_build(print_fp, /*includeDevDeps=*/false, /*extraTargets=*/{}, ov);
    if (!ctx) { std::println(stderr, "error: {}", ctx.error()); return 2; }

    return run_build_plan(*ctx, verbose, no_cache);
}

int cmd_run(const mcpplibs::cmdline::ParsedArgs& parsed,
            std::span<const std::string> passthrough) {
    // The action lambda has already split argv at the first "--" and
    // passed post-args as `passthrough`. Pre-args were parsed by cmdline
    // into `parsed`; the only positional we declare is the optional
    // `target` name.
    std::optional<std::string> targetName;
    if (parsed.positional_count() > 0) targetName = parsed.positional(0);

    // Build first. Single prepare_build → drive build → reuse ctx to locate
    // the binary, so we don't re-resolve the toolchain or re-scan modgraph.
    auto ctx = prepare_build(/*print_fp=*/false);
    if (!ctx) { std::println(stderr, "error: {}", ctx.error()); return 2; }
    if (auto rc = run_build_plan(*ctx, /*verbose=*/false, /*no_cache=*/false); rc != 0)
        return rc;

    // Find binary target
    const mcpp::build::LinkUnit* chosen = nullptr;
    for (auto& lu : ctx->plan.linkUnits) {
        if (lu.kind != mcpp::build::LinkUnit::Binary) continue;
        if (targetName && lu.targetName != *targetName) continue;
        chosen = &lu;
        if (targetName) break;
    }
    if (!chosen) {
        std::println(stderr, "error: no binary target {}",
            targetName ? std::format("'{}' found", *targetName) : "in this package");
        return 2;
    }

    auto exe = ctx->outputDir / chosen->output;
    auto pathCtx = make_path_ctx(/*cfg=*/nullptr, ctx->projectRoot);
    mcpp::ui::status("Running",
        std::format("`{}`", mcpp::ui::shorten_path(exe, pathCtx)));
    std::println("");
    std::fflush(stdout);
    std::string cmd = std::format("'{}'", exe.string());
    for (auto& a : passthrough) cmd += std::format(" '{}'", a);
    return std::system(cmd.c_str()) == 0 ? 0 : 1;
}

int cmd_env(const mcpplibs::cmdline::ParsedArgs& /*parsed*/) {
    auto cfg = mcpp::config::load_or_init(/*quiet=*/false, make_bootstrap_progress_callback());
    if (!cfg) { mcpp::ui::error(cfg.error().message); return 4; }

    mcpp::config::print_env(*cfg);

    auto tc = mcpp::toolchain::detect();
    if (tc) {
        std::println("");
        std::println("Toolchain       = {}", tc->label());
        std::println("std module src  = {}", tc->stdModuleSource.string());
    } else {
        std::println("");
        std::println("Toolchain       = (not detected: {})", tc.error().message);
    }
    return 0;
}

int cmd_search(const mcpplibs::cmdline::ParsedArgs& parsed) {
    std::string keyword = parsed.positional(0);
    if (keyword.empty()) {
        std::println(stderr, "error: `mcpp search` requires a keyword");
        return 2;
    }

    auto cfg = mcpp::config::load_or_init(/*quiet=*/false, make_bootstrap_progress_callback());
    if (!cfg) { mcpp::ui::error(cfg.error().message); return 4; }

    mcpp::ui::info("Updating", std::format("registry index `{}`", cfg->defaultIndex));

    mcpp::fetcher::Fetcher f(*cfg);
    auto hits = f.search(keyword);
    if (!hits) { mcpp::ui::error(hits.error().message); return 1; }

    if (hits->empty()) {
        std::println("");
        std::println("No packages match `{}`.", keyword);
        return 0;
    }
    std::println("");
    for (auto& h : *hits) {
        std::println("  {:<20}  {}", h.name, h.description);
    }
    return 0;
}

// `mcpp index` is dispatched at the App level — the top-level App declares
// `index list / add / remove / update` as nested subcommands, each with
// its own action lambda invoking one of these helpers.

int cmd_index_list(const mcpplibs::cmdline::ParsedArgs& /*parsed*/) {
    auto cfg = mcpp::config::load_or_init(/*quiet=*/false, make_bootstrap_progress_callback());
    if (!cfg) { mcpp::ui::error(cfg.error().message); return 4; }
    mcpp::fetcher::Fetcher f(*cfg);

    auto repos = f.list_repos();
    if (!repos) { mcpp::ui::error(repos.error().message); return 1; }
    if (repos->empty()) {
        for (auto& r : cfg->indexRepos) {
            bool isDefault = (r.name == cfg->defaultIndex);
            std::println("  {:<15}  {}{}",
                         r.name, r.url, isDefault ? "  (default)" : "");
        }
        return 0;
    }
    for (auto& r : *repos) {
        std::println("  {:<15}  {}", r.name, r.url);
    }
    return 0;
}

int cmd_index_add(const mcpplibs::cmdline::ParsedArgs& parsed) {
    std::string name = parsed.positional(0);
    std::string url  = parsed.positional(1);
    if (name.empty() || url.empty()) {
        mcpp::ui::error("usage: mcpp index add <name> <url>");
        return 2;
    }
    auto cfg = mcpp::config::load_or_init(/*quiet=*/false, make_bootstrap_progress_callback());
    if (!cfg) { mcpp::ui::error(cfg.error().message); return 4; }
    mcpp::fetcher::Fetcher f(*cfg);
    auto r = f.add_repo(name, url);
    if (!r) { mcpp::ui::error(r.error().message); return 1; }
    mcpp::ui::status("Added", std::format("registry `{}` -> {}", name, url));
    return 0;
}

int cmd_index_remove(const mcpplibs::cmdline::ParsedArgs& parsed) {
    std::string name = parsed.positional(0);
    if (name.empty()) {
        mcpp::ui::error("usage: mcpp index remove <name>");
        return 2;
    }
    auto cfg = mcpp::config::load_or_init(/*quiet=*/false, make_bootstrap_progress_callback());
    if (!cfg) { mcpp::ui::error(cfg.error().message); return 4; }
    mcpp::fetcher::Fetcher f(*cfg);
    auto r = f.remove_repo(name);
    if (!r) { mcpp::ui::error(r.error().message); return 1; }
    mcpp::ui::status("Removed", std::format("registry `{}`", name));
    return 0;
}

int cmd_index_update(const mcpplibs::cmdline::ParsedArgs& /*parsed*/) {
    auto cfg = mcpp::config::load_or_init(/*quiet=*/false, make_bootstrap_progress_callback());
    if (!cfg) { mcpp::ui::error(cfg.error().message); return 4; }
    mcpp::ui::status("Updating", "all index repos");
    std::string cmd = std::format(
        "env -u XLINGS_PROJECT_DIR XLINGS_HOME='{}' '{}' update 2>&1",
        cfg->xlingsHome().string(),
        cfg->xlingsBinary.string());
    std::array<char, 4096> buf{};
    std::FILE* fp = ::popen(cmd.c_str(), "r");
    if (!fp) { mcpp::ui::error("failed to spawn index updater"); return 1; }
    while (std::fgets(buf.data(), buf.size(), fp)) {
        std::fputs(buf.data(), stdout);
    }
    int rc = ::pclose(fp);
    if (rc != 0) { mcpp::ui::error("index update failed"); return 1; }
    mcpp::ui::status("Updated", "index refresh complete");
    return 0;
}

// `cmd_add` has moved to `mcpp.pm.commands` (PR-R5).

int cmd_test(const mcpplibs::cmdline::ParsedArgs& /*parsed*/,
             std::span<const std::string> passthrough) {
    // The action lambda has already split argv at the first "--" and
    // passed post-args as `passthrough`. `mcpp test` itself takes no
    // pre-`--` flags or positionals.
    auto root = find_manifest_root(std::filesystem::current_path());
    if (!root) {
        mcpp::ui::error("no mcpp.toml found in current directory or any parent");
        return 2;
    }

    // 1. Discover test files.
    auto testFiles = mcpp::modgraph::expand_glob(*root, "tests/**/*.cpp");
    if (testFiles.empty()) {
        std::println("no tests found in tests/");
        return 0;
    }

    // 2. Synthesize a Target for each test file.
    //    Name = file stem; collisions → error.
    std::vector<mcpp::manifest::Target> testTargets;
    std::set<std::string> seenNames;
    for (auto& f : testFiles) {
        auto name = f.stem().string();
        if (!seenNames.insert(name).second) {
            mcpp::ui::error(std::format(
                "duplicate test name '{}' (two .cpp files share the same stem)", name));
            return 2;
        }
        mcpp::manifest::Target t;
        t.name = name;
        t.kind = mcpp::manifest::Target::TestBinary;
        // Store as path relative to project root for portability of error messages.
        t.main = std::filesystem::relative(f, *root).string();
        testTargets.push_back(std::move(t));
    }

    // 3. prepare_build with dev-deps enabled + synthetic targets.
    auto ctx = prepare_build(/*print_fp=*/false,
                             /*includeDevDeps=*/true,
                             std::move(testTargets));
    if (!ctx) { mcpp::ui::error(ctx.error()); return 2; }

    // 4. "Compiling test_X (test)" lines for the test binaries.
    std::set<std::string> cachedNames;
    for (auto& label : ctx->cachedDepLabels) {
        auto sp = label.find(' ');
        cachedNames.insert(sp == std::string::npos ? label : label.substr(0, sp));
    }
    std::set<std::string> announced;
    announced.insert(ctx->manifest.package.name);
    mcpp::ui::status("Compiling",
        std::format("{} v{} (.)",
                    ctx->manifest.package.name, ctx->manifest.package.version));
    for (auto& [name, spec] : ctx->manifest.dependencies) {
        if (announced.contains(name)) continue;
        announced.insert(name);
        std::string ver = spec.isPath() ? "(path)" : std::string("v") + spec.version;
        const char* verb = cachedNames.contains(name) ? "Cached" : "Compiling";
        mcpp::ui::status(verb, std::format("{} {}", name, ver));
    }
    for (auto& [name, spec] : ctx->manifest.devDependencies) {
        if (announced.contains(name)) continue;
        announced.insert(name);
        std::string ver = spec.isPath() ? "(path)" : std::string("v") + spec.version;
        const char* verb = cachedNames.contains(name) ? "Cached" : "Compiling";
        mcpp::ui::status(verb,
            std::format("{} {} (dev)", name, ver));
    }
    // List test binaries.
    for (auto& lu : ctx->plan.linkUnits) {
        if (lu.kind == mcpp::build::LinkUnit::TestBinary) {
            mcpp::ui::status("Compiling",
                std::format("{} (test)", lu.targetName));
        }
    }

    // 5. Build everything.
    auto backend = mcpp::build::make_ninja_backend();
    mcpp::build::BuildOptions opts;
    auto buildResult = backend->build(ctx->plan, opts);
    if (!buildResult) {
        mcpp::ui::error(buildResult.error().message);
        return 1;
    }

    // M3.2: populate BMI cache for deps that did NOT hit cache.
    for (auto& task : ctx->depsToPopulate) {
        auto pr = mcpp::bmi_cache::populate_from(task.key, ctx->outputDir, task.artifacts);
        if (!pr) {
            mcpp::ui::warning(std::format(
                "bmi cache populate failed for {}@{}: {}",
                task.key.packageName, task.key.version, pr.error()));
        }
    }

    mcpp::ui::finished("test", buildResult->elapsed);

    // 6. Run each test binary in sequence; collect pass/fail.
    auto t0 = std::chrono::steady_clock::now();
    int passed = 0;
    int failed = 0;
    std::vector<std::string> failures;
    for (auto& lu : ctx->plan.linkUnits) {
        if (lu.kind != mcpp::build::LinkUnit::TestBinary) continue;
        auto exe = ctx->outputDir / lu.output;
        mcpp::ui::status("Running", std::format("bin/{}", lu.targetName));

        std::string cmd = std::format("'{}'", exe.string());
        for (auto& a : passthrough) cmd += std::format(" '{}'", a);
        int rc = std::system(cmd.c_str());
        // std::system returns wait status — extract exit code.
        int exitCode = WIFEXITED(rc) ? WEXITSTATUS(rc) : 127;

        if (exitCode == 0) {
            std::println("{} ... ok", lu.targetName);
            ++passed;
        } else {
            std::println("{} ... FAIL (exit {})", lu.targetName, exitCode);
            ++failed;
            failures.push_back(lu.targetName);
        }
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);

    // 7. Summary.
    std::println("");
    if (failed == 0) {
        mcpp::ui::status("test result",
            std::format("ok. {} passed; 0 failed; finished in {:.2f}s",
                        passed, static_cast<double>(elapsed.count()) / 1000.0));
        return 0;
    }
    mcpp::ui::error(std::format(
        "test result: FAILED. {} passed; {} failed; finished in {:.2f}s",
        passed, failed, static_cast<double>(elapsed.count()) / 1000.0));
    std::println("");
    std::println("failures:");
    for (auto& n : failures) std::println("    {}", n);
    return 1;
}

// `mcpp emit xpkg ...` — only one subcommand defined, so the action sits
// directly on the `emit xpkg` nested subcommand and receives its
// ParsedArgs.
int cmd_emit_xpkg(const mcpplibs::cmdline::ParsedArgs& parsed) {
    std::string version = parsed.option_or_empty("version").value();
    std::filesystem::path output =
        parsed.option_or_empty("output").value();

    auto root = find_manifest_root(std::filesystem::current_path());
    if (!root) {
        std::println(stderr, "error: no mcpp.toml found");
        return 2;
    }
    auto m = mcpp::manifest::load(*root / "mcpp.toml");
    if (!m) { std::println(stderr, "error: {}", m.error().format()); return 2; }
    auto scan = mcpp::modgraph::scan_package(*root, *m);
    if (!scan.errors.empty()) {
        for (auto& e : scan.errors) std::println(stderr, "error: {}", e.format());
        return 2;
    }

    if (version.empty()) version = m->package.version;
    auto release = mcpp::publish::placeholder_release(version);
    auto lua = mcpp::publish::emit_xpkg(*m, scan.graph, release);

    if (output.empty()) {
        std::print("{}", lua);
    } else {
        std::ofstream os(output);
        if (!os) { std::println(stderr, "error: cannot write '{}'", output.string()); return 1; }
        os << lua;
        std::println("Wrote {}", output.string());
    }
    return 0;
}

int cmd_clean(const mcpplibs::cmdline::ParsedArgs& parsed) {
    bool wipe_bmi = parsed.is_flag_set("bmi-cache");

    auto root = find_manifest_root(std::filesystem::current_path());
    if (!root) { std::println(stderr, "error: not in an mcpp package"); return 2; }
    std::error_code ec;
    std::filesystem::remove_all(*root / "target", ec);
    if (ec) {
        std::println(stderr, "error: cannot remove target/: {}", ec.message());
        return 1;
    }
    std::println("Cleaned: {}", (*root / "target").string());

    if (wipe_bmi) {
        auto bmi = mcpp::toolchain::default_cache_root();
        std::filesystem::remove_all(bmi, ec);
        std::println("Cleaned BMI cache: {}", bmi.string());
    }
    return 0;
}

// ─── M4 #1: mcpp doctor ─────────────────────────────────────────────────
static std::uintmax_t dir_size(const std::filesystem::path& p) {
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) return 0;
    std::uintmax_t total = 0;
    for (auto& e : std::filesystem::recursive_directory_iterator(p, ec)) {
        if (ec) break;
        std::error_code ec2;
        if (e.is_regular_file(ec2) && !ec2) {
            total += e.file_size(ec2);
        }
    }
    return total;
}

static std::string human_bytes(std::uintmax_t n) {
    constexpr const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(n);
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    return std::format("{:.1f} {}", v, units[u]);
}

int cmd_doctor(const mcpplibs::cmdline::ParsedArgs& /*parsed*/) {
    int warns = 0, errors = 0;
    auto ok    = [](std::string_view m) { mcpp::ui::status("ok", m); };
    auto warn = [&](std::string_view m) { mcpp::ui::warning(m); ++warns; };
    auto err   = [&](std::string_view m) { mcpp::ui::error(m);   ++errors; };

    mcpp::ui::status("Checking", "toolchain");
    auto tc = mcpp::toolchain::detect();
    if (!tc) {
        err(std::format("toolchain detection failed: {}", tc.error().message));
    } else {
        ok(std::format("{} at {}", tc->label(), tc->binaryPath.string()));
    }

    mcpp::ui::status("Checking", "std module");
    if (tc) {
        auto cacheRoot = mcpp::toolchain::default_cache_root();
        auto fpDir = cacheRoot;
        std::error_code ec;
        if (std::filesystem::exists(cacheRoot, ec)) {
            bool found = false;
            for (auto& e : std::filesystem::directory_iterator(cacheRoot, ec)) {
                auto stdgcm = e.path() / "std.gcm";
                if (std::filesystem::exists(stdgcm)) {
                    ok(std::format("{}  ({})",
                        stdgcm.string(),
                        human_bytes(std::filesystem::file_size(stdgcm))));
                    found = true;
                    break;
                }
            }
            if (!found) warn("no std.gcm found yet (will be built on first `mcpp build`)");
        } else {
            warn(std::format("cache root '{}' missing (run `mcpp env` to init)",
                             cacheRoot.string()));
        }
    }

    mcpp::ui::status("Checking", "registry");
    auto cfg = mcpp::config::load_or_init(/*quiet=*/false, make_bootstrap_progress_callback());
    if (!cfg) {
        err(cfg.error().message);
    } else {
        if (std::filesystem::exists((*cfg).xlingsBinary)) {
            ok(std::format("xlings at {}", (*cfg).xlingsBinary.string()));
        } else {
            warn(std::format("xlings binary missing at '{}'",
                             (*cfg).xlingsBinary.string()));
        }
        ok(std::format("default index = '{}'", (*cfg).defaultIndex));
    }

    mcpp::ui::status("Checking", "cache health");
    auto bmiRoot = mcpp::toolchain::default_cache_root();
    auto sz = dir_size(bmiRoot);
    if (sz > std::uintmax_t(1) * 1024 * 1024 * 1024) {
        warn(std::format("BMI cache occupies {} (run `mcpp cache prune` to GC)",
                         human_bytes(sz)));
    } else {
        ok(std::format("BMI cache size = {}", human_bytes(sz)));
    }

    std::println("");
    if (errors)        std::println("Doctor result: {} errors, {} warnings", errors, warns);
    else if (warns)    std::println("Doctor result: {} warnings", warns);
    else               std::println("Doctor result: all checks passed");
    return errors ? 2 : (warns ? 1 : 0);
}

// ─── M4 #4: mcpp cache list / prune / clean / info ──────────────────────
struct CacheEntry {
    std::filesystem::path           dir;
    std::string                     fingerprint;
    std::string                     pkgAtVer;        // "<idx>/<pkg>@<ver>"
    std::uintmax_t                  size = 0;
    std::filesystem::file_time_type lastWrite{};
    std::size_t                     fileCount = 0;
};

static std::vector<CacheEntry> walk_cache_entries() {
    std::vector<CacheEntry> entries;
    auto bmi = mcpp::toolchain::default_cache_root();
    std::error_code ec;
    if (!std::filesystem::exists(bmi, ec)) return entries;

    for (auto& fpEntry : std::filesystem::directory_iterator(bmi, ec)) {
        auto fpDir = fpEntry.path();
        auto depsDir = fpDir / "deps";
        if (!std::filesystem::exists(depsDir, ec)) continue;
        for (auto& idxEntry : std::filesystem::directory_iterator(depsDir, ec)) {
            for (auto& pkgEntry : std::filesystem::directory_iterator(idxEntry.path(), ec)) {
                CacheEntry e;
                e.dir         = pkgEntry.path();
                e.fingerprint = fpDir.filename().string();
                e.pkgAtVer    = idxEntry.path().filename().string()
                              + "/" + pkgEntry.path().filename().string();
                e.size        = dir_size(e.dir);
                e.lastWrite   = std::filesystem::last_write_time(e.dir, ec);
                for (auto& _ : std::filesystem::recursive_directory_iterator(e.dir, ec)) {
                    if (!ec) ++e.fileCount;
                }
                entries.push_back(std::move(e));
            }
        }
    }
    return entries;
}

static std::string format_age(std::filesystem::file_time_type t) {
    auto now = std::chrono::file_clock::now();
    auto diff = std::chrono::duration_cast<std::chrono::seconds>(now - t).count();
    if (diff < 60)        return std::format("{}s ago", diff);
    if (diff < 3600)      return std::format("{}m ago", diff / 60);
    if (diff < 86400)     return std::format("{}h ago", diff / 3600);
    return std::format("{}d ago", diff / 86400);
}

// `mcpp cache` is dispatched at the App level — list / info / prune / clean
// each get their own action lambda invoking one of these helpers.

int cmd_cache_list(const mcpplibs::cmdline::ParsedArgs& /*parsed*/) {
    auto entries = walk_cache_entries();
    if (entries.empty()) {
        std::println("(BMI cache is empty)");
        return 0;
    }
    std::println("{:<18}  {:>10}  {:>14}  {}",
                 "fingerprint", "size", "last accessed", "package");
    for (auto& e : entries) {
        auto fp = e.fingerprint.size() > 16
                  ? e.fingerprint.substr(0, 16) : e.fingerprint;
        std::println("{:<18}  {:>10}  {:>14}  {}",
            fp, human_bytes(e.size), format_age(e.lastWrite), e.pkgAtVer);
    }
    return 0;
}

int cmd_cache_info(const mcpplibs::cmdline::ParsedArgs& parsed) {
    std::string needle = parsed.positional(0);
    if (needle.empty()) {
        mcpp::ui::error("usage: mcpp cache info <pkg>@<ver>");
        return 2;
    }
    auto entries = walk_cache_entries();
    for (auto& e : entries) {
        if (e.pkgAtVer.ends_with(needle)) {
            std::println("dir          = {}", e.dir.string());
            std::println("fingerprint  = {}", e.fingerprint);
            std::println("package      = {}", e.pkgAtVer);
            std::println("size         = {}", human_bytes(e.size));
            std::println("file count   = {}", e.fileCount);
            std::println("last write   = {}", format_age(e.lastWrite));
            return 0;
        }
    }
    std::println("no cache entry matching '{}'", needle);
    return 1;
}

int cmd_cache_prune(const mcpplibs::cmdline::ParsedArgs& parsed) {
    std::string v = parsed.option_or_empty("older-than").value();
    if (v.empty()) {
        mcpp::ui::error("`mcpp cache prune` requires --older-than <N>{s,m,h,d}");
        return 2;
    }
    char unit = v.back();
    long long n = 0;
    try { n = std::stoll(v.substr(0, v.size() - 1)); }
    catch (...) { mcpp::ui::error(std::format("bad --older-than value '{}'", v)); return 2; }
    std::chrono::seconds threshold{0};
    if      (unit == 's') threshold = std::chrono::seconds(n);
    else if (unit == 'm') threshold = std::chrono::seconds(n * 60);
    else if (unit == 'h') threshold = std::chrono::seconds(n * 3600);
    else if (unit == 'd') threshold = std::chrono::seconds(n * 86400);
    else { mcpp::ui::error(std::format("bad time unit '{}': use s/m/h/d", unit)); return 2; }
    auto cutoff = std::chrono::file_clock::now() - threshold;
    auto entries = walk_cache_entries();
    int removed = 0;
    std::uintmax_t freed = 0;
    for (auto& e : entries) {
        if (e.lastWrite < cutoff) {
            std::error_code ec;
            std::filesystem::remove_all(e.dir, ec);
            if (!ec) {
                ++removed;
                freed += e.size;
                mcpp::ui::status("Pruned",
                    std::format("{} ({})", e.pkgAtVer, human_bytes(e.size)));
            }
        }
    }
    std::println("");
    std::println("Pruned {} entries, freed {}", removed, human_bytes(freed));
    return 0;
}

int cmd_cache_clean(const mcpplibs::cmdline::ParsedArgs& /*parsed*/) {
    auto bmi = mcpp::toolchain::default_cache_root();
    std::error_code ec;
    std::filesystem::remove_all(bmi / "deps", ec);   // deps only; preserve std.gcm
    if (std::filesystem::exists(bmi)) {
        for (auto& f : std::filesystem::directory_iterator(bmi, ec)) {
            auto deps = f.path() / "deps";
            std::filesystem::remove_all(deps, ec);
        }
    }
    std::println("Cleaned all dep BMI cache entries (std.gcm preserved)");
    return 0;
}

// ─── M4 #3: mcpp remove / mcpp update ───────────────────────────────────
// `cmd_remove` and `cmd_update` have moved to `mcpp.pm.commands` (PR-R5).

// ─── M4 #2: mcpp publish ────────────────────────────────────────────────
// ─── M5.5 #toolchain: mcpp toolchain install/list/default/remove ────────
//
// `mcpp toolchain` is dispatched as a single nested-subcommand handler
// (the App-level `toolchain` action calls this with the toolchain
// ParsedArgs that includes a populated `subcommand_name_`). This
// preserves the shared setup (cfg load + xlings bootstrap progress)
// without forcing it through every subcommand action.
int cmd_toolchain(const mcpplibs::cmdline::ParsedArgs& parsed) {
    namespace cl = mcpplibs::cmdline;
    auto subname = parsed.subcommand_name();
    if (subname.empty()) {
        mcpp::ui::error("`mcpp toolchain` requires a subcommand: install / list / default / remove");
        return 2;
    }
    const cl::ParsedArgs& sub_parsed = *parsed.subcommand_matches;

    // Surface bootstrap (xim:patchelf, xim:ninja) as a progress bar — same
    // treatment any other download here gets, so the user isn't staring at
    // a quiet "Bootstrap…" line during a slow link.
    auto cfg = mcpp::config::load_or_init(/*quiet=*/false,
        make_bootstrap_progress_callback());
    if (!cfg) { mcpp::ui::error(cfg.error().message); return 4; }

    auto pkgsDir = cfg->xlingsHome() / "data" / "xpkgs";

    if (subname == "list") {
        // `gcc 15.1.0-musl` and `musl-gcc@15.1.0` describe the same xpkg
        // (xim-x-musl-gcc/15.1.0) — match either user-form against the
        // currently-configured default for the `*` marker.
        auto matches_default = [&](std::string_view compiler,
                                   std::string_view version) {
            const auto& d = cfg->defaultToolchain;
            if (d == std::format("{}@{}", compiler, version)) return true;
            if (compiler == "musl-gcc"
                && d == std::format("gcc@{}-musl", version)) return true;
            return false;
        };

        // User-facing label for an xpkg row: `gcc 15.1.0-musl` reads more
        // naturally than `musl-gcc 15.1.0` and matches the toolchain spec
        // the rest of the CLI accepts.
        auto display_label = [](std::string_view compiler,
                                std::string_view version) {
            if (compiler == "musl-gcc")
                return std::format("gcc {}-musl", version);
            return std::format("{} {}", compiler, version);
        };

        auto pathCtx = make_path_ctx(&*cfg);
        struct Row {
            std::string compiler;
            std::string version;
            std::filesystem::path bin;
            bool isDefault = false;
        };
        std::vector<Row> installed;

        std::error_code ec;
        if (std::filesystem::exists(pkgsDir, ec)) {
            for (auto& entry : std::filesystem::directory_iterator(pkgsDir, ec)) {
                auto name = entry.path().filename().string();
                std::string compiler = name;
                if (auto sep = name.find("-x-"); sep != std::string::npos)
                    compiler = name.substr(sep + 3);
                for (auto& vEntry : std::filesystem::directory_iterator(entry.path(), ec)) {
                    auto bin = toolchain_frontend(vEntry.path() / "bin", compiler);
                    if (bin.empty()) continue;
                    Row r;
                    r.compiler  = compiler;
                    r.version   = vEntry.path().filename().string();
                    r.bin       = bin;
                    r.isDefault = matches_default(r.compiler, r.version);
                    installed.push_back(std::move(r));
                }
            }
        }

        if (installed.empty()) {
            std::println("(no toolchains installed — run `mcpp toolchain install gcc 16.1.0` "
                         "or just `mcpp build` to auto-install the default)");
        } else {
            std::println("Installed:");
            std::println("  {:<3}{:<22}  {}", "", "TOOLCHAIN", "BINARY");
            for (auto& r : installed) {
                std::println("  {:<3}{:<22}  {}",
                    r.isDefault ? "*" : "",
                    display_label(r.compiler, r.version),
                    mcpp::ui::shorten_path(r.bin, pathCtx));
            }
        }

        // ─── Available section ──────────────────────────────────────────
        // List xim:gcc + xim:musl-gcc versions known to the local index
        // that aren't already installed. Helpful to discover what users
        // can install without going to a website.
        struct AvailRow { std::string compiler; std::string version; };
        std::vector<AvailRow> avail;
        auto add_avail = [&](std::string_view ximName, std::string_view compiler) {
            auto versions = list_available_xpkg_versions(*cfg, ximName);
            for (auto& v : versions) {
                // xim packages declare a `["latest"] = { ref = "X.Y.Z" }`
                // alias entry; it shows up here but isn't a version users
                // can `mcpp toolchain install` directly. Skip non-numeric
                // entries.
                if (v.empty() || !std::isdigit(static_cast<unsigned char>(v[0])))
                    continue;
                bool already = std::any_of(installed.begin(), installed.end(),
                    [&](const Row& r){ return r.compiler == compiler && r.version == v; });
                if (!already) avail.push_back({std::string(compiler), v});
            }
        };
        add_avail("gcc",      "gcc");
        add_avail("musl-gcc", "musl-gcc");

        if (!avail.empty()) {
            // Newest first. Lexicographic-on-strings is good enough for
            // semver-shaped versions — any "X.Y.Z" with single-digit
            // segments compares the way users expect.
            std::sort(avail.begin(), avail.end(),
                [](const AvailRow& a, const AvailRow& b) {
                    if (a.compiler != b.compiler) return a.compiler < b.compiler;
                    return b.version < a.version;
                });
            if (!installed.empty()) std::println("");
            std::println("Available (run `mcpp toolchain install <compiler> <version>`):");
            std::println("  {:<3}{}", "", "TOOLCHAIN");
            for (auto& a : avail) {
                std::println("  {:<3}{}", "", display_label(a.compiler, a.version));
            }
        }
        return 0;
    }

    if (subname == "install") {
        // Accept three input shapes — they all collapse to (compiler, version):
        //   mcpp toolchain install gcc 16.1.0      → ("gcc", "16.1.0")
        //   mcpp toolchain install gcc@16.1.0      → ("gcc", "16.1.0")
        //   mcpp toolchain install gcc 15          → ("gcc", "15")  partial
        std::string compiler = sub_parsed.positional(0);
        std::string version  = sub_parsed.positional(1);
        if (auto at = compiler.find('@'); at != std::string::npos) {
            if (version.empty()) version = compiler.substr(at + 1);
            compiler = compiler.substr(0, at);
        }
        if (compiler.empty()) {
            mcpp::ui::error("missing compiler name; e.g. `mcpp toolchain install gcc 16.1.0`");
            return 2;
        }

        // Accept both spelling forms for the musl flavor:
        //   mcpp toolchain install musl-gcc 15.1.0
        //   mcpp toolchain install gcc      15.1.0-musl
        // and translate either into the underlying xim package.
        bool isMusl = (compiler == "musl-gcc")
                   || (version.size() > 5
                       && version.compare(version.size() - 5, 5, "-musl") == 0);
        std::string ximName    = compiler;
        std::string ximVersion = version;
        if (isMusl) {
            if (compiler != "musl-gcc") ximName = "musl-" + compiler;   // gcc → musl-gcc
            if (ximVersion.size() > 5
                && ximVersion.compare(ximVersion.size() - 5, 5, "-musl") == 0)
                ximVersion.resize(ximVersion.size() - 5);
        }

        // Partial-version resolution: `gcc 15` → highest available 15.x.y in
        // the synced index. Empty version → latest of any major.
        if (auto picked = resolve_version_match(
                ximVersion, list_available_xpkg_versions(*cfg, ximName))) {
            if (*picked != ximVersion) {
                mcpp::ui::info("Resolved",
                    std::format("{}@{} → {}@{}", compiler, version, compiler, *picked));
            }
            ximVersion = *picked;
            // Reflect the concrete version back into the user-facing label
            // so the "Installed gcc@<ver>" line shows the real version.
            version = ximVersion + (isMusl ? "-musl" : "");
        }

        std::string spec = std::format("xim:{}@{}", ximName, ximVersion);

        mcpp::ui::info("Installing", std::format("{} {} via mcpp's xlings", compiler, version));
        mcpp::fetcher::Fetcher fetcher(*cfg);
        CliInstallProgress progress;
        auto payload = fetcher.resolve_xpkg_path(spec, /*autoInstall=*/true, &progress);
        if (!payload) {
            mcpp::ui::error(std::format("install failed: {}", payload.error().message));
            return 1;
        }

        std::string bn = "g++";
        if      (compiler == "llvm" || compiler == "clang") bn = "clang++";
        else if (compiler == "msvc")                         bn = "cl.exe";
        if (isMusl) bn = "x86_64-linux-musl-g++";
        auto bin = payload->binDir / bn;
        if (!std::filesystem::exists(bin)) {
            mcpp::ui::error(std::format(
                "installed package has no '{}' at '{}'", bn, bin.string()));
            return 1;
        }

        // For GNU gcc only: post-install ELF + specs fixup so the toolchain
        // is self-contained in the sandbox.
        //   1. patchelf walk: rewrites PT_INTERP of gcc/binutils binaries
        //      themselves (xim's elfpatch is supposed to but currently
        //      scans 0 files — see `patchelf_walk_set_interp` doc).
        //   2. specs fixup: rewrites the linker-spec dynamic-linker/rpath
        //      so binaries gcc *compiles* use sandbox-local paths.
        // musl-gcc ships its own self-contained sysroot at
        // `<root>/x86_64-linux-musl/{include,lib}` and doesn't link against
        // xim:glibc, so this fixup is both unnecessary and harmful for it.
        if (compiler == "gcc" && !isMusl) {
            auto glibcRoot = pkgsDir / "xim-x-glibc";
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
            auto gccLibDir = payload->root / "lib64";
            auto patchelfBin = pkgsDir / "xim-x-patchelf" / "0.18.0" / "bin" / "patchelf";

            if (!glibcLibDir.empty() && std::filesystem::exists(gccLibDir)
                && std::filesystem::exists(patchelfBin))
            {
                auto loader = glibcLibDir / "ld-linux-x86-64.so.2";
                auto rpath = std::format("{}:{}",
                    glibcLibDir.string(), gccLibDir.string());

                // (1) patchelf walk: rewrite PT_INTERP + RUNPATH for binutils
                //     and gcc xpkgs so they're self-contained in sandbox.
                auto binutilsRoot = pkgsDir / "xim-x-binutils";
                if (std::filesystem::exists(binutilsRoot)) {
                    for (auto& v : std::filesystem::directory_iterator(binutilsRoot))
                        patchelf_walk(v.path(), loader, rpath, patchelfBin);
                }
                patchelf_walk(payload->root, loader, rpath, patchelfBin);

                // (2) specs fixup.
                fixup_gcc_specs(payload->root, glibcLibDir, gccLibDir);
            } else {
                mcpp::ui::warning(
                    "could not locate sandbox glibc/gcc/patchelf paths; "
                    "gcc-built binaries may have unresolved PT_INTERP/RUNPATH");
            }
        }

        mcpp::ui::status("Installed", std::format("{}@{} → {}", compiler, version, bin.string()));
        if (cfg->defaultToolchain.empty()) {
            std::println("");
            std::println("Tip: `mcpp toolchain default {}@{}` to make this the default.",
                         compiler, version);
        }
        return 0;
    }

    if (subname == "default") {
        // Accept three input shapes (mirrors `install`):
        //   mcpp toolchain default gcc@16.1.0
        //   mcpp toolchain default gcc 16.1.0
        //   mcpp toolchain default gcc 15        ← partial; picks highest 15.x.y
        std::string compiler = sub_parsed.positional(0);
        std::string version  = sub_parsed.positional(1);
        if (auto at = compiler.find('@'); at != std::string::npos) {
            if (version.empty()) version = compiler.substr(at + 1);
            compiler = compiler.substr(0, at);
        }
        if (compiler.empty()) {
            mcpp::ui::error("missing spec; e.g. `mcpp toolchain default gcc@16.1.0`");
            return 2;
        }

        std::string ximName    = compiler;
        std::string ximVersion = version;
        bool isMusl = (compiler == "musl-gcc")
                   || (version.size() > 5
                       && version.compare(version.size() - 5, 5, "-musl") == 0);
        if (isMusl) {
            if (compiler != "musl-gcc") ximName = "musl-" + compiler;
            if (ximVersion.size() > 5
                && ximVersion.compare(ximVersion.size() - 5, 5, "-musl") == 0)
                ximVersion.resize(ximVersion.size() - 5);
        }

        // Partial-version resolution against installed payloads.
        if (auto picked = resolve_version_match(
                ximVersion, list_installed_versions(pkgsDir, ximName))) {
            ximVersion = *picked;
        }

        // Reconstruct the user-visible spec for messages / config.toml.
        std::string spec = isMusl
            ? std::format("{}@{}-musl",
                  compiler == "musl-gcc" ? "musl-gcc" : compiler, ximVersion)
            : std::format("{}@{}", compiler, ximVersion);

        auto installDir = pkgsDir / std::format("xim-x-{}", ximName) / ximVersion;
        if (!std::filesystem::exists(installDir)) {
            mcpp::ui::error(std::format(
                "{} is not installed. Run `mcpp toolchain install {} {}` first.",
                spec, compiler, version.empty() ? ximVersion : version));
            return 1;
        }
        auto wr = mcpp::config::write_default_toolchain(*cfg, spec);
        if (!wr) {
            mcpp::ui::error(wr.error().message);
            return 1;
        }
        mcpp::ui::status("Default", std::format(
            "set to {} (was: {})", spec,
            cfg->defaultToolchain.empty() ? "<none>" : cfg->defaultToolchain));
        return 0;
    }

    if (subname == "remove") {
        std::string spec = sub_parsed.positional(0);
        auto at = spec.find('@');
        if (at == std::string::npos) {
            mcpp::ui::error(std::format("invalid spec '{}'", spec));
            return 2;
        }
        std::string compiler = spec.substr(0, at);
        std::string version  = spec.substr(at + 1);
        auto installDir = pkgsDir / std::format("xim-x-{}", compiler) / version;
        std::error_code ec;
        if (!std::filesystem::exists(installDir, ec)) {
            mcpp::ui::error(std::format("{} is not installed", spec));
            return 1;
        }
        std::filesystem::remove_all(installDir, ec);
        if (ec) {
            mcpp::ui::error(std::format("remove failed: {}", ec.message()));
            return 1;
        }
        mcpp::ui::status("Removed", spec);
        if (cfg->defaultToolchain == spec) {
            mcpp::ui::warning(std::format(
                "default toolchain '{}' was just removed; consider `mcpp toolchain default <other>`",
                spec));
        }
        return 0;
    }

    mcpp::ui::error(std::format("unknown toolchain subcommand '{}'", subname));
    return 2;
}

int cmd_publish(const mcpplibs::cmdline::ParsedArgs& parsed) {
    bool dry_run    = parsed.is_flag_set("dry-run");
    bool allow_dirty= parsed.is_flag_set("allow-dirty");

    auto root = find_manifest_root(std::filesystem::current_path());
    if (!root) { mcpp::ui::error("no mcpp.toml in current dir or parents"); return 2; }

    // Sanity: working tree clean (best-effort via git status).
    if (!allow_dirty && std::filesystem::exists(*root / ".git")) {
        std::string out;
        std::FILE* fp = ::popen(
            std::format("git -C {} status --porcelain 2>&1", root->string()).c_str(), "r");
        if (fp) {
            std::array<char, 4096> buf{};
            while (std::fgets(buf.data(), buf.size(), fp)) out += buf.data();
            ::pclose(fp);
        }
        if (!out.empty()) {
            mcpp::ui::error("working tree has uncommitted changes; pass --allow-dirty to skip this check");
            std::println(stderr, "{}", out);
            return 1;
        }
    }

    auto m = mcpp::manifest::load(*root / "mcpp.toml");
    if (!m) {
        mcpp::ui::error(std::format("manifest parse: {}", m.error().format()));
        return 2;
    }
    auto scan = mcpp::modgraph::scan_package(*root, *m);
    if (!scan.errors.empty()) {
        for (auto& e : scan.errors) mcpp::ui::error(e.format());
        return 2;
    }

    auto& pkg = m->package;
    mcpp::ui::status("Packaging", std::format("{} v{}", pkg.name, pkg.version));

    // Output dir: target/dist/
    auto distDir = *root / "target" / "dist";
    std::error_code ec;
    std::filesystem::create_directories(distDir, ec);

    auto tarball = distDir / std::format("{}-{}.tar.gz", pkg.name, pkg.version);
    auto xpkgPath = distDir / std::format("{}.lua", pkg.name);

    // 1. Pack source via `git archive` (respects .gitignore).
    if (auto err = mcpp::publish::make_release_tarball(
            *root, pkg.name, pkg.version, tarball);
        !err.empty())
    {
        mcpp::ui::error(std::format("tarball: {}", err));
        return 1;
    }
    auto tarballSize = std::filesystem::file_size(tarball, ec);

    // 2. Compute SHA-256.
    auto sha = mcpp::publish::sha256_of_file(tarball);
    if (sha.empty()) {
        mcpp::ui::error("sha256: failed to hash tarball (is `sha256sum` installed?)");
        return 1;
    }

    // 3. Compute the convention-based GitHub Release URL from manifest.repo.
    auto url = mcpp::publish::release_tarball_url(
        pkg.repo, pkg.name, pkg.version);
    if (url.empty()) {
        mcpp::ui::error(std::format(
            "cannot derive tarball URL: [package].repo='{}' is empty or not "
            "a https URL. Set [package].repo in mcpp.toml.", pkg.repo));
        return 1;
    }

    // 4. Build release info + emit xpkg.lua.
    auto release = mcpp::publish::make_release_info(pkg.version, url, sha);
    auto lua = mcpp::publish::emit_xpkg(*m, scan.graph, release);

    {
        std::ofstream os(xpkgPath);
        os << lua;
        if (!os) {
            mcpp::ui::error(std::format(
                "cannot write '{}'", xpkgPath.string()));
            return 1;
        }
    }

    mcpp::ui::status("Tarball",
        std::format("{} ({} bytes)", tarball.string(), tarballSize));
    mcpp::ui::status("SHA-256", sha);
    mcpp::ui::status("Xpkg",    xpkgPath.string());

    if (dry_run) {
        std::println("");
        std::println("--- xpkg.lua content ---");
        std::print("{}", lua);
        std::println("--- end ---");
    }

    // 5. Print step-by-step PR instructions.
    char first = pkg.name.empty() ? '?' : pkg.name[0];
    std::println("");
    std::println("Next steps to publish to mcpp-index:");
    std::println("");
    std::println("  1. Tag this commit and push:");
    std::println("       git tag -a v{0} -m \"v{0}\"", pkg.version);
    std::println("       git push --tags");
    std::println("");
    std::println("  2. Upload the tarball to your repo's GitHub Release:");
    std::println("       URL: {}/releases/new?tag=v{}", pkg.repo, pkg.version);
    std::println("       Attach: {}", tarball.string());
    std::println("");
    std::println("  3. Open a PR to mcpp-index:");
    std::println("       Fork:  https://github.com/mcpp-community/mcpp-index");
    std::println("       Add:   pkgs/{}/{}.lua", first, pkg.name);
    std::println("       (file content is in {})", xpkgPath.string());
    std::println("");
    // TODO(post-v0.0.3): if `gh` CLI is on PATH and authenticated, offer
    //   `mcpp publish --auto` to:
    //     - gh release create v<v> <tarball>
    //     - fork mcpp-index, add pkg lua, gh pr create
    //   See docs/34-release-readiness.md §3.
    std::println("Tip: future versions of mcpp may automate steps 2-3 via the gh CLI.");
    return 0;
}

// ─── M5: mcpp pack — bundle build output into a self-contained tarball ───
//
// Three modes (see docs/35-pack-design.md):
//   static          full musl static, no dynamic deps
//   bundle-project  default; only bundle non-system .so
//   bundle-all      bundle every dynamic dep including libc / libstdc++
int cmd_pack(const mcpplibs::cmdline::ParsedArgs& parsed) {
    // ─── Resolve mode ────────────────────────────────────────────────
    mcpp::pack::Options opts;
    bool modeFromUser = false;
    if (auto v = parsed.value("mode")) {
        auto m = mcpp::pack::parse_mode(*v);
        if (!m) {
            mcpp::ui::error(std::format(
                "invalid --mode '{}'; expected static | bundle-project | bundle-all", *v));
            return 2;
        }
        opts.mode = *m;
        modeFromUser = true;
    }
    if (auto v = parsed.value("format")) {
        if (*v == "tar")      opts.format = mcpp::pack::Format::Tar;
        else if (*v == "dir") opts.format = mcpp::pack::Format::Dir;
        else {
            mcpp::ui::error(std::format(
                "invalid --format '{}'; expected tar | dir", *v));
            return 2;
        }
    }
    if (auto v = parsed.value("output")) opts.output = *v;
    if (auto v = parsed.value("target")) opts.targetTriple = *v;

    // `--target *-linux-musl` without an explicit `--mode` implies
    // `--mode static` — packaging a musl-static ELF as bundle-project
    // would feed patchelf a static binary and crash. The docs treat
    // this pair as equivalent; surface it in the code path too.
    if (!modeFromUser && opts.targetTriple.find("-musl") != std::string::npos) {
        opts.mode     = mcpp::pack::Mode::Static;
        modeFromUser  = true;   // user-equivalent intent — block manifest override
    }

    // ─── Build first (pack implies a fresh build) ────────────────────
    BuildOverrides ov;
    if (opts.mode == mcpp::pack::Mode::Static && opts.targetTriple.empty())
        ov.target_triple = "x86_64-linux-musl";
    else
        ov.target_triple = opts.targetTriple;

    auto ctx = prepare_build(/*print_fp=*/false, /*includeDevDeps=*/false,
                             /*extraTargets=*/{}, ov);
    if (!ctx) {
        mcpp::ui::error(ctx.error());
        return 2;
    }

    // Manifest may override mode only when neither --mode nor an
    // equivalent flag (--target *-musl → static) was given.
    if (!modeFromUser && !ctx->manifest.packConfig.defaultMode.empty()) {
        if (auto m = mcpp::pack::parse_mode(ctx->manifest.packConfig.defaultMode))
            opts.mode = *m;
    }

    // Re-derive target triple: if mode is Static we force the musl
    // triple even when the manifest's [pack].default_mode bumped us
    // here after `prepare_build` ran with the host toolchain.
    if (opts.mode == mcpp::pack::Mode::Static && ctx->tc.targetTriple.find("-musl") == std::string::npos) {
        // Need to re-prepare the build with the musl target.
        BuildOverrides ov2;
        ov2.target_triple = "x86_64-linux-musl";
        auto ctx2 = prepare_build(false, false, {}, ov2);
        if (!ctx2) { mcpp::ui::error(ctx2.error()); return 2; }
        ctx = std::move(ctx2);
    }

    auto be = mcpp::build::make_ninja_backend();
    mcpp::build::BuildOptions bo;
    auto br = be->build(ctx->plan, bo);
    if (!br) {
        mcpp::ui::error(br.error().message);
        return 1;
    }

    // ─── Pick the main binary target ─────────────────────────────────
    std::filesystem::path mainBinary;
    for (auto& lu : ctx->plan.linkUnits) {
        if (lu.kind == mcpp::build::LinkUnit::Binary
            && lu.targetName == ctx->manifest.package.name)
        {
            mainBinary = ctx->outputDir / lu.output;
            break;
        }
    }
    if (mainBinary.empty()) {
        // Fall back to the first binary target if package.name doesn't match.
        for (auto& lu : ctx->plan.linkUnits) {
            if (lu.kind == mcpp::build::LinkUnit::Binary) {
                mainBinary = ctx->outputDir / lu.output;
                break;
            }
        }
    }
    if (mainBinary.empty()) {
        mcpp::ui::error("no binary target to pack");
        return 1;
    }

    auto cfg = mcpp::config::load_or_init(/*quiet=*/false,
        make_bootstrap_progress_callback());
    if (!cfg) { mcpp::ui::error(cfg.error().message); return 4; }

    // ─── Build the plan + run ────────────────────────────────────────
    auto plan = mcpp::pack::make_plan(ctx->manifest, *cfg, opts,
        mainBinary, ctx->projectRoot, ctx->tc.targetTriple);
    if (!plan) { mcpp::ui::error(plan.error().message); return 1; }

    mcpp::ui::info("Packing", std::format("{} v{} ({})",
        plan->packageName, plan->packageVersion,
        mcpp::pack::mode_name(plan->opts.mode)));

    auto r = mcpp::pack::run(*plan, *cfg);
    if (!r) {
        mcpp::ui::error(r.error().message);
        return 1;
    }

    auto pathCtx = make_path_ctx(&*cfg, ctx->projectRoot);
    auto outPath = (opts.format == mcpp::pack::Format::Tar)
        ? plan->tarballPath : plan->stagingRoot;
    mcpp::ui::status("Packed", mcpp::ui::shorten_path(outPath, pathCtx));
    return 0;
}

// ─── M4 #8.2: mcpp --explain CODE ───────────────────────────────────────
int cmd_explain(std::string_view code) {
    struct Entry { std::string_view code, title, body; };
    static constexpr Entry table[] = {
        {"E0001", "dependency name mismatch",
         "The package located at the [dependencies.<key>] path declares a different\n"
         "name in its own [package].name. Either rename the [dependencies.<key>] in\n"
         "the consumer's mcpp.toml to match the producer, or fix the producer's\n"
         "[package].name."},
        {"E0002", "module imported but not provided",
         "A source file does `import X;` but no source file in the build graph\n"
         "exports `X`. Either add a dependency that provides X (mcpp add or\n"
         "[dependencies.X] path = \"...\") or fix the import."},
        {"E0003", "version constraint unsatisfiable",
         "No published version of the package matches the requested constraint.\n"
         "Run `mcpp search <pkg>` to list available versions, then loosen the\n"
         "constraint in mcpp.toml (e.g. ^1.2 instead of =1.2.3)."},
        {"E0004", "toolchain pin mismatch",
         "The [toolchain] pin in mcpp.toml does not match the detected toolchain.\n"
         "Either install the pinned toolchain (xlings install ...) or relax the\n"
         "pin (e.g. \"gcc@>=15\" instead of \"gcc@15.1.0\")."},
        {"E0005", "BMI cache corruption",
         "A cached BMI file referenced by manifest.txt is missing on disk. Run\n"
         "`mcpp cache prune --older-than 0d` to drop stale entries; the next build\n"
         "will repopulate."},
    };
    for (auto& e : table) {
        if (e.code == code) {
            std::println("{}: {}", e.code, e.title);
            std::println("");
            std::println("{}", e.body);
            return 0;
        }
    }
    std::println(stderr, "error: unknown error code '{}'", code);
    std::println(stderr, "       known codes: E0001..E0005");
    return 2;
}

// ─── M6.1: `mcpp self ...` — about mcpp itself ──────────────────────────
//
// `self` is declared as a parent subcommand on the top-level App with
// nested `doctor / env / version / explain` subcommands. Each nested
// subcommand has its own action; these helpers wrap the bodies so we
// can share `cmd_doctor` / `cmd_env` between top-level and `mcpp self`.

int cmd_self_version(const mcpplibs::cmdline::ParsedArgs& /*parsed*/) {
    std::println("mcpp {}", mcpp::toolchain::MCPP_VERSION);
    return 0;
}

// Used both by `mcpp explain <CODE>` (top-level) and `mcpp self explain
// <CODE>` (legacy alias).
int cmd_explain_action(const mcpplibs::cmdline::ParsedArgs& parsed) {
    std::string code = parsed.positional(0);
    if (code.empty()) {
        std::println(stderr, "error: explain requires an error code (e.g. E0001)");
        return 2;
    }
    return cmd_explain(code);
}

// Hidden subcommand: aggregate P1689 .ddi files into a Ninja dyndep file.
// Invoked by ninja during build (cxx_collect rule) under M3.3.b dyndep mode.
//   mcpp dyndep --output <build.ninja.dd> <ddi-1> <ddi-2> ...
int cmd_dyndep(const mcpplibs::cmdline::ParsedArgs& parsed) {
    std::filesystem::path outPath = parsed.option_or_empty("output").value();
    if (outPath.empty()) {
        std::println(stderr, "error: --output <path> required");
        return 2;
    }
    std::vector<std::filesystem::path> ddis;
    for (std::size_t i = 0; i < parsed.positional_count(); ++i)
        ddis.emplace_back(parsed.positional(i));
    auto body = mcpp::dyndep::emit_dyndep_from_files(ddis, /*stdImports=*/{});
    if (!body) {
        std::println(stderr, "error: {}", body.error());
        return 1;
    }
    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);
    std::ofstream os(outPath);
    os << *body;
    return os ? 0 : 1;
}

} // namespace mcpp::cli::detail

namespace mcpp::cli {

int run(int argc, char** argv) {
    using namespace mcpp::cli::detail;
    namespace cl = mcpplibs::cmdline;

    // ─── --quiet / --no-color: pre-scan ─────────────────────────────────
    // The cmdline lib propagates global options into nested subcommand
    // ParsedArgs, but we set ui:: state up-front so that *every* line
    // emitted from the action lambdas (including the very first
    // "Resolving toolchain" banner) honours the user's intent. This is
    // a side-channel only — the global options are still declared on
    // the App below so they show up in --help and pass schema checks.
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if      (a == "--quiet" || a == "-q") mcpp::ui::set_quiet(true);
        else if (a == "--no-color")           mcpp::ui::disable_color();
    }

    // ─── top-level --help / -h / --version intercept ────────────────────
    // cmdline auto-handles these but its formatter doesn't match the
    // mixed-case "Usage:" + per-command blurbs that documentation +
    // e2e tests pin. Print our canonical screen and `mcpp X.Y.Z`
    // version line up-front so the App never sees these tokens.
    if (argc >= 2) {
        std::string_view a = argv[1];
        if (a == "--help" || a == "-h") { print_usage(); return 0; }
        if (a == "--version" || a == "-V") {
            std::println("mcpp {}", mcpp::toolchain::MCPP_VERSION);
            return 0;
        }
    }

    // ─── action_rc plumbing ─────────────────────────────────────────────
    // cmdline's action callback returns void. Capture int return codes
    // via a shared local; `wrap_rc` adapts an `int(ParsedArgs&)` lambda
    // into the void-returning shape cmdline expects.
    int action_rc = 0;
    auto wrap_rc = [&action_rc](auto&& fn) {
        return [fn = std::forward<decltype(fn)>(fn), &action_rc]
               (const cl::ParsedArgs& args) {
            action_rc = fn(args);
        };
    };

    // ─── nested-subcommand dispatcher ───────────────────────────────────
    // cmdline's run() only dispatches one level — when a parent subcommand
    // (e.g. `self`, `cache`, `index`, `emit`) has its own children but no
    // parent action, the matched leaf never gets invoked. We give every
    // such parent an action that switches on the parsed child name and
    // forwards to the right cmd_* directly.
    using cmd_fn = int(*)(const cl::ParsedArgs&);
    auto dispatch_sub = [](std::string_view parent,
                           const cl::ParsedArgs& parsed,
                           std::initializer_list<std::pair<std::string_view, cmd_fn>> table) -> int {
        if (!parsed.has_subcommand()) {
            std::string usage = std::format("`mcpp {}` requires a subcommand: ", parent);
            bool first = true;
            for (auto& [n, _] : table) {
                if (!first) usage += " / ";
                usage += n;
                first = false;
            }
            mcpp::ui::error(usage);
            return 2;
        }
        auto name    = parsed.subcommand_name();
        auto sub_ref = parsed.subcommand();
        if (!sub_ref) return 2;
        for (auto& [n, fn] : table) {
            if (n == name) return fn(sub_ref->get());
        }
        mcpp::ui::error(std::format("unknown `mcpp {} {}` subcommand", parent, name));
        return 2;
    };

    // ─── `--` passthrough for `mcpp run` / `mcpp test` ──────────────────
    // cmdline natively recognises `--` and dumps everything after it
    // into `parsed.positionals` (with the bare `--` token dropped). For
    // `mcpp run [target] -- args...` we need to distinguish the
    // optional `target` (a real positional) from the passthrough
    // tokens (which must reach the executed binary verbatim, even when
    // they look like `-x` / `--foo`). We split ourselves at the first
    // `--` and present cmdline only the pre-`--` slice; post-args go
    // into `passthrough` and are handed to the action helper directly.
    std::vector<std::string> passthrough;
    std::vector<char*> trimmed_argv(argv, argv + argc);
    {
        for (std::size_t i = 1; i < trimmed_argv.size(); ++i) {
            if (std::string_view(trimmed_argv[i]) != "--") continue;
            for (std::size_t j = i + 1; j < trimmed_argv.size(); ++j)
                passthrough.emplace_back(trimmed_argv[j]);
            trimmed_argv.resize(i);  // drop `--` and everything after
            break;
        }
    }
    int      trimmed_argc = static_cast<int>(trimmed_argv.size());
    char**   trimmed_argp = trimmed_argv.empty() ? nullptr : trimmed_argv.data();

    // ─── Build the top-level App ────────────────────────────────────────
    auto app = cl::App("mcpp")
        .version(std::string{mcpp::toolchain::MCPP_VERSION})
        .description("modern C++ build tool")
        .option(cl::Option("quiet").short_name('q')
            .help("Suppress status output").global())
        .option(cl::Option("no-color")
            .help("Disable colored output").global())

        // ─── project commands ──────────────────────────────────────────
        .subcommand(cl::App("new")
            .description("Create a new mcpp package skeleton")
            .arg(cl::Arg("name").help("Package directory name").required())
            .action(wrap_rc(cmd_new)))
        .subcommand(cl::App("build")
            .description("Build the current package")
            .option(cl::Option("verbose").short_name('v')
                .help("Verbose compiler output"))
            .option(cl::Option("print-fingerprint")
                .help("Show toolchain fingerprint and 10 inputs"))
            .option(cl::Option("no-cache")
                .help("Force-clear target/ before building"))
            .option(cl::Option("target").takes_value().help(
                "Build for <triple> (e.g. x86_64-linux-musl); looks up [target.<triple>] in mcpp.toml"))
            .option(cl::Option("static").help(
                "Force static linking (-static). On Linux, prefer pairing with --target <arch>-linux-musl"))
            .action(wrap_rc(cmd_build)))
        .subcommand(cl::App("run")
            .description("Build + run a binary target (after `--`, args are passed to it)")
            .arg(cl::Arg("target").help("Binary target name (optional)"))
            .action(wrap_rc([&passthrough](const cl::ParsedArgs& p) {
                return cmd_run(p, std::span<const std::string>(passthrough));
            })))
        .subcommand(cl::App("test")
            .description("Build + run all tests/**/*.cpp (after `--`, args go to each test binary)")
            .action(wrap_rc([&passthrough](const cl::ParsedArgs& p) {
                return cmd_test(p, std::span<const std::string>(passthrough));
            })))
        .subcommand(cl::App("clean")
            .description("Remove target/ (and optionally the global BMI cache)")
            .option(cl::Option("bmi-cache").help("Also wipe the global BMI cache"))
            .action(wrap_rc(cmd_clean)))
        .subcommand(cl::App("add")
            .description("Add a dependency to mcpp.toml")
            .arg(cl::Arg("pkg").help("Package spec, e.g. foo@1.0.0").required())
            .action(wrap_rc(mcpp::pm::commands::cmd_add)))
        .subcommand(cl::App("remove")
            .description("Remove a dependency from mcpp.toml")
            .arg(cl::Arg("pkg").help("Package name").required())
            .action(wrap_rc(mcpp::pm::commands::cmd_remove)))
        .subcommand(cl::App("update")
            .description("Re-resolve dependencies and rewrite mcpp.lock")
            .arg(cl::Arg("pkg").help("If given, update only that package"))
            .action(wrap_rc(mcpp::pm::commands::cmd_update)))
        .subcommand(cl::App("search")
            .description("Search packages in configured registries")
            .arg(cl::Arg("keyword").help("Search keyword (substring match)").required())
            .action(wrap_rc(cmd_search)))
        .subcommand(cl::App("publish")
            .description("Publish package to default registry")
            .option(cl::Option("dry-run").help("Print xpkg.lua without uploading"))
            .option(cl::Option("allow-dirty").help("Allow uncommitted changes"))
            .action(wrap_rc(cmd_publish)))
        .subcommand(cl::App("pack")
            .description("Build + bundle into a self-contained tarball")
            .option(cl::Option("mode").takes_value()
                .help("static | bundle-project (default) | bundle-all"))
            .option(cl::Option("target").takes_value()
                .help("Triple, e.g. x86_64-linux-musl"))
            .option(cl::Option("format").takes_value()
                .help("tar (default) | dir"))
            .option(cl::Option("output").short_name('o').takes_value()
                .help("Override output path"))
            .action(wrap_rc(cmd_pack)))

        // ─── emit (one nested subcommand: xpkg) ────────────────────────
        .subcommand(cl::App("emit")
            .description("Generate package descriptor (xpkg)")
            .subcommand(cl::App("xpkg")
                .description("Generate xpkg Lua entry")
                .option(cl::Option("version").short_name('V').takes_value().value_name("VER")
                    .help("Override package version"))
                .option(cl::Option("output").short_name('o').takes_value().value_name("FILE")
                    .help("Write to file instead of stdout")))
            .action(wrap_rc([&dispatch_sub](const cl::ParsedArgs& p) {
                return dispatch_sub("emit", p, {{"xpkg", cmd_emit_xpkg}});
            })))

        // ─── resource management ───────────────────────────────────────
        .subcommand(cl::App("toolchain")
            .description("Install / list / select / remove C++ toolchains")
            .subcommand(cl::App("list").description("List installed toolchains"))
            .subcommand(cl::App("install")
                .description("Install a toolchain via mcpp's xlings")
                // Both `mcpp toolchain install gcc 16.1.0` and `mcpp toolchain
                // install gcc@16.1.0` are accepted, and the version may be
                // partial (`15`, `15.1`) — mcpp resolves to the highest match.
                .arg(cl::Arg("compiler").help("e.g. gcc, gcc@16.1.0, gcc@15-musl").required())
                .arg(cl::Arg("version").help("e.g. 16.1.0, 15, 15.1, 15.1.0-musl")))
            .subcommand(cl::App("default")
                .description("Set the default toolchain")
                // Same dual-form as `install`: `gcc@16.1.0` or `gcc 16.1.0`,
                // partial versions allowed.
                .arg(cl::Arg("spec").help("<compiler>[@<version>] (version may be partial)").required())
                .arg(cl::Arg("version").help("(optional, alternative to @-form)")))
            .subcommand(cl::App("remove")
                .description("Uninstall a toolchain")
                .arg(cl::Arg("spec").help("<compiler>@<version>").required()))
            .action(wrap_rc(cmd_toolchain)))
        .subcommand(cl::App("cache")
            .description("Inspect and manage the global BMI cache")
            .subcommand(cl::App("list")
                .description("List cache entries with size + last-access"))
            .subcommand(cl::App("info")
                .description("Show details for a cached package")
                .arg(cl::Arg("pkg").help("<pkg>@<ver>").required()))
            .subcommand(cl::App("prune")
                .description("Drop entries older than a threshold")
                .option(cl::Option("older-than").takes_value().value_name("N{s|m|h|d}")
                    .help("Age threshold (e.g. 30d)")))
            .subcommand(cl::App("clean")
                .description("Drop all dep cache entries (preserves std.gcm)"))
            .action(wrap_rc([&dispatch_sub](const cl::ParsedArgs& p) {
                return dispatch_sub("cache", p, {
                    {"list",  cmd_cache_list},
                    {"info",  cmd_cache_info},
                    {"prune", cmd_cache_prune},
                    {"clean", cmd_cache_clean},
                });
            })))
        .subcommand(cl::App("index")
            .description("Manage configured package registries")
            .subcommand(cl::App("list")
                .description("List configured registries"))
            .subcommand(cl::App("add")
                .description("Add a custom registry")
                .arg(cl::Arg("name").help("Registry name").required())
                .arg(cl::Arg("url").help("Registry URL").required()))
            .subcommand(cl::App("remove")
                .description("Remove a registry")
                .arg(cl::Arg("name").help("Registry name").required()))
            .subcommand(cl::App("update")
                .description("Refresh local registry clones"))
            .action(wrap_rc([&dispatch_sub](const cl::ParsedArgs& p) {
                return dispatch_sub("index", p, {
                    {"list",   cmd_index_list},
                    {"add",    cmd_index_add},
                    {"remove", cmd_index_remove},
                    {"update", cmd_index_update},
                });
            })))

        // ─── about mcpp itself ─────────────────────────────────────────
        .subcommand(cl::App("self")
            .description("Inspect and manage mcpp itself")
            .subcommand(cl::App("doctor")
                .description("Diagnose mcpp environment health"))
            .subcommand(cl::App("env")
                .description("Print mcpp paths and configuration"))
            .subcommand(cl::App("version")
                .description("Show mcpp version"))
            .subcommand(cl::App("explain")
                .description("Show extended description for an error code")
                .arg(cl::Arg("code").help("Error code such as E0001").required()))
            .action(wrap_rc([&dispatch_sub](const cl::ParsedArgs& p) {
                return dispatch_sub("self", p, {
                    {"doctor",  cmd_doctor},
                    {"env",     cmd_env},
                    {"version", cmd_self_version},
                    {"explain", cmd_explain_action},
                });
            })))

        // ─── top-level explain alias ──────────────────────────────────
        // Preserves `mcpp explain E0001` as a shortcut for
        // `mcpp self explain E0001`.
        .subcommand(cl::App("explain")
            .description("Show extended description for an error code")
            .arg(cl::Arg("code").help("Error code such as E0001").required())
            .action(wrap_rc(cmd_explain_action)))

        // ─── bareword `version` alias ─────────────────────────────────
        // cmdline natively handles `--help`/`--version`/`-h`; the bareword
        // `mcpp help` is intercepted by the pre-scan above (prints the
        // canonical usage screen). `mcpp version` is wired through the
        // App so it shows up in the auto-generated subcommand list.
        .subcommand(cl::App("version")
            .description("Show mcpp version")
            .action(wrap_rc(cmd_self_version)))

        // ─── hidden / internal ─────────────────────────────────────────
        .subcommand(cl::App("dyndep")
            .description("(internal: invoked by ninja) Emit ninja dyndep file from .ddi inputs")
            .option(cl::Option("output").short_name('o').takes_value().value_name("PATH")
                .help("Path to write dyndep file"))
            .action(wrap_rc(cmd_dyndep)))
    ;

    // The bareword `mcpp help` and `mcpp` (no args) both print the
    // canonical help screen.
    if (argc <= 1) {
        print_usage();
        return 0;
    }
    if (std::string_view(argv[1]) == "help") {
        print_usage();
        return 0;
    }

    // Legacy `--explain CODE` form (still tested by e2e #22). cmdline
    // wouldn't naturally accept this as a top-level option taking a
    // value (it'd require declaring it on the root App, but then it'd
    // also need to coexist with the `explain` subcommand, which is
    // the form documented going forward). Special-case here before
    // invoking the App.
    if (std::string_view(argv[1]) == "--explain") {
        if (argc < 3) {
            std::println(stderr, "error: --explain requires an error code (e.g. E0001)");
            return 2;
        }
        return cmd_explain(argv[2]);
    }

    // Unknown-command pre-check. cmdline doesn't error on an unknown
    // top-level word — it just treats it as a positional and returns
    // 0 silently (since the root App has no top-level action). The
    // pre-existing CLI returned 127 for "unknown command", and e2e
    // #01 asserts that exit code, so reproduce it here.
    {
        std::string_view first = argv[1];
        if (!first.starts_with('-')) {
            static constexpr std::array<std::string_view, 19> known = {
                "new", "build", "run", "test", "clean", "add", "remove",
                "update", "search", "publish", "pack", "emit",
                "toolchain", "cache", "index", "self", "explain",
                "version", "dyndep",
            };
            bool ok = false;
            for (auto k : known) if (k == first) { ok = true; break; }
            if (!ok) {
                std::println(stderr, "error: unknown command '{}'", first);
                print_usage();
                return 127;
            }
        }
    }

    auto app_rc = app.run(trimmed_argc, trimmed_argp);
    if (app_rc != 0) return app_rc;
    return action_rc;
}

} // namespace mcpp::cli

// mcpp.toolchain.lifecycle — toolchain lifecycle operations
// (list / install / set-default / remove) + version-spec matching.
// Bodies moved verbatim from the CLI layer. Zero behavior change.

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.toolchain.lifecycle;

import std;
import mcpp.config;
import mcpp.fetcher;
import mcpp.fetcher.progress;
import mcpp.manifest;
import mcpp.platform;
import mcpp.platform.axis;
import mcpp.toolchain.detect;
import mcpp.toolchain.msvc;
import mcpp.toolchain.registry;
import mcpp.toolchain.triple;
import mcpp.toolchain.post_install;
import mcpp.ui;
import mcpp.log;
import mcpp.platform.xlings;
import mcpp.platform.xlings.runtime_selection;
import mcpp.platform.runtime_binding;

namespace mcpp::toolchain {

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
        // The HOST axis, deliberately (#254): a toolchain payload runs on
        // this machine, so its version/asset table is the host's block —
        // unlike a library dependency, which is resolved for the target.
        // (Previously hardcoded "linux", which made the Available section
        // and partial-version resolution empty on Windows/macOS for any
        // package, e.g. mingw-gcc has no linux block.)
        return mcpp::manifest::list_xpkg_versions(
            body, mcpp::platform::HostPlatform::current());
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

// ─── M4 #3: mcpp remove / mcpp update ───────────────────────────────────
// `cmd_remove` and `cmd_update` have moved to `mcpp.pm.commands` (PR-R5).

// ─── M4 #2: mcpp publish ────────────────────────────────────────────────
// ─── M5.5 #toolchain: mcpp toolchain install/list/default/remove ────────
//
// `mcpp toolchain` is dispatched as a single nested-subcommand handler
// (the App-level `toolchain` action calls this with the toolchain
// ParsedArgs that includes a populated `subcommand_name_`). This
// preserves the shared setup (cfg load + xlings bootstrap progress)

// `mcpp toolchain list` — installed + available toolchains.
// The toolchain that `mcpp build`/`run` actually resolves from the current
// directory. A project mcpp.toml `[toolchain]` shadows the global config
// default (see prepare.cppm resolution order), so `mcpp toolchain default`
// and `mcpp toolchain list`'s `*` — which read only the global config — can
// otherwise disagree with what a build in this directory really uses.
// (`--target` overrides are intentionally not folded in: they only take
// effect when an explicit `--target` is passed.)
struct EffectiveDefault {
    std::string spec;
    bool        fromProject = false;
};

// ─── msvc@system: system-toolchain helpers ──────────────────────────────
//
// MSVC is located and identified, never installed/removed by mcpp. All four
// subcommands branch here before the xim-package path.

int msvc_wrong_host() {
    mcpp::ui::error("the msvc toolchain is only available on Windows hosts");
    return 1;
}

// `label` names the ORIGIN, because the two are not the same event.
// "Detected" is a claim about probing the machine, and saying it after
// unpacking a payload the caller named would describe the wrong thing —
// quietly, and in exactly the direction this whole change is about.
void msvc_print_detected(const mcpp::toolchain::msvc::MsvcInstallation& inst,
                         std::string_view label = "Detected") {
    mcpp::ui::status(label, std::format(
        "msvc {}{} (VC tools {})",
        inst.display_version(),
        inst.vsProduct.empty() ? "" : std::format(" (VS {})", inst.vsProduct),
        inst.toolsVersion));
    std::println("           cl: {}", inst.clPath.string());
    std::println("           import std: {}",
        inst.hasStdModules ? "available (std.ixx)" : "not available");
}

// The Windows SDK is the OTHER half of a usable MSVC, and a payload that
// unpacked a compiler without it is a half-installed state: cl.exe is right
// there, so everything reports success, and the build dies inside the ucrt
// headers much later. `has_usable_msvc()` exists for exactly this reason;
// this is the same judgement applied to the managed origin, where the SDK
// arrives as a package dependency and can therefore fail on its own.
// Delete a payload tree, coping with the two things that make a plain
// `remove_all` fail on Windows and on nothing else:
//
//   read-only files — payloads are unpacked from .vsix/.msi, and archive
//     entries carry the attribute through. POSIX only needs the DIRECTORY
//     writable to unlink a child, so this never shows up on Linux or macOS.
//   a lingering handle — a build with /Zi leaves mspdbsrv.exe running for a
//     few seconds after cl.exe exits, and it lives inside the payload.
//
// Both surface as the same "Access is denied", which is why this handles both
// rather than picking one: clear the attribute, then give a live process a
// bounded moment to go away. Retries are capped and short — `toolchain
// remove` should not hang because something holds the directory forever.
// Whether any FILE is left under `root`. Directories alone are not a
// toolchain: nothing resolves a compiler out of an empty tree.
bool any_regular_file(const std::filesystem::path& root) {
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator{}; it.increment(ec)) {
        if (it->is_regular_file(ec)) return true;
    }
    return false;
}

export bool remove_payload_tree(const std::filesystem::path& root,
                         std::error_code& ec) {
    std::filesystem::remove_all(root, ec);
    if (!ec) return true;

    // Second pass: make everything writable, then try again.
    std::error_code ignore;
    for (auto it = std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied,
             ignore);
         it != std::filesystem::recursive_directory_iterator{}; it.increment(ignore)) {
        std::filesystem::permissions(it->path(), std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::add, ignore);
    }
    for (int attempt = 0; attempt < 10; ++attempt) {
        ec.clear();
        std::filesystem::remove_all(root, ec);
        if (!ec) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds{300});
    }
    if (!std::filesystem::exists(root, ignore)) return true;

    // Still held. Move the held FILES out instead of waiting on a process we
    // do not own.
    //
    // ⚠️ NOT by renaming the payload directory. Windows lets you rename an
    // open FILE — that is how an updater replaces a running .exe — but it
    // does NOT let you rename a DIRECTORY that contains one. An earlier
    // version of this function renamed `root` and was wrong about exactly
    // that: the rename failed with the same "Access is denied", and remove
    // still reported failure. What follows is the shape that actually works:
    //
    //   move every surviving file to a sibling `.trash-*` directory  (allowed
    //   even while open, same volume, the handle follows the file)
    //   then delete the payload tree, which now holds only directories
    //
    // This is what makes `remove` mean something after a /Zi build with the
    // toolset being removed: mspdbsrv.exe lives INSIDE the payload and
    // outlives cl.exe by tens of seconds, so "wait for it" is a guess and a
    // CLI that hangs on a guess is worse than the failure. The bytes are
    // swept by the next lifecycle command, when nothing holds them.
    auto trash = root.parent_path() /
                 std::format(".trash-{}-{}", root.filename().string(),
                             std::chrono::steady_clock::now()
                                 .time_since_epoch().count());
    std::vector<std::filesystem::path> survivors;
    for (auto it = std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied,
             ignore);
         it != std::filesystem::recursive_directory_iterator{}; it.increment(ignore)) {
        if (it->is_regular_file(ignore)) survivors.push_back(it->path());
    }
    if (survivors.empty()) return false;         // nothing to move; genuinely stuck

    std::filesystem::create_directories(trash, ignore);
    // Flat, index-prefixed: two payload subdirectories can hold the same file
    // name, and this directory exists to be deleted, not to be read.
    std::size_t moved = 0;
    for (std::size_t i = 0; i < survivors.size(); ++i) {
        std::error_code ren;
        std::filesystem::rename(
            survivors[i],
            trash / std::format("{}-{}", i, survivors[i].filename().string()), ren);
        if (!ren) ++moved;
    }
    if (moved != survivors.size()) {
        // Some file could not even be moved. Leave the rest where they are —
        // a half-moved payload is worse than one that is still all there.
        std::filesystem::remove_all(trash, ignore);
        return false;
    }

    ec.clear();
    std::filesystem::remove_all(root, ec);       // directories only now
    std::filesystem::remove_all(trash, ignore);  // usually works; fine if not
    if (!ec) return true;

    // The files are gone and an empty directory skeleton would not die. That
    // is a sharing violation on a DIRECTORY, which on Windows means some
    // process has one of them as its current directory -- mspdbsrv.exe is
    // launched inside the payload, so this is the normal tail of a /Zi build.
    //
    // A toolchain with no files in it is not installed, which is what
    // `remove` promises. Reporting failure here would be reporting the
    // opposite of what happened, and the skeleton is swept by the next
    // lifecycle command once that process exits.
    ec.clear();
    return !any_regular_file(root);
}

// Delete `.trash-*` left behind by a removal that had to park a held payload.
// Takes the directory the payload lived IN (`<pkgs>/xim-x-<name>`), which is
// where park() puts them -- one place, one level, nothing to walk.
//
// Best-effort and run before a lifecycle operation rather than after one: by
// the next command the process that held the bytes is normally gone, so this
// is where they actually get freed.
export void sweep_parked_payloads(const std::filesystem::path& pkgRoot,
                           const std::filesystem::path& skeleton) {
    std::error_code ec;
    if (!std::filesystem::is_directory(pkgRoot, ec)) return;

    // `.trash-*` is a name only this code writes, so deleting any of them is
    // safe regardless of who else is running.
    for (auto& e : std::filesystem::directory_iterator(pkgRoot, ec)) {
        if (e.is_directory(ec)
            && e.path().filename().string().starts_with(".trash-"))
            std::filesystem::remove_all(e.path(), ec);
    }

    // The file-less skeleton, on the other hand, is swept for ONE named
    // version — the one this command is about — and never for whatever else
    // happens to be sitting in the family directory.
    //
    // "No files in it" is not a safe thing to conclude about someone else's
    // directory: an install populates a version directory over time (it is
    // why package_fetcher tracks completeness with a marker file rather than
    // by existence), so a concurrent `toolchain install` of a DIFFERENT
    // version is briefly indistinguishable from a skeleton, and sweeping the
    // whole family would delete it mid-extraction. Two mcpp processes against
    // one MCPP_HOME is ordinary on a shared or self-hosted runner.
    if (!skeleton.empty() && std::filesystem::is_directory(skeleton, ec)
        && !any_regular_file(skeleton))
        std::filesystem::remove_all(skeleton, ec);
}

// The first entry that is still there after a failed removal. The error code
// alone says "Access is denied" and not by whom or to what, which is the
// difference between a report someone can act on and one they cannot.
//
// It does not probe by deleting. `remove_all` has already removed everything
// it could, so whatever SURVIVED is exactly what blocked it -- reporting the
// first survivor needs no further destruction. (An earlier version did probe
// by deleting, which turns a diagnostic into a second act of damage on a
// payload the caller may well want to keep and retry.)
export std::optional<std::filesystem::path>
first_undeletable(const std::filesystem::path& root) {
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) return std::nullopt;
    for (auto it = std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator{}; it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec)) return it->path();
    }
    return root;
}

void msvc_warn_if_sdk_missing(const mcpp::toolchain::msvc::MsvcInstallation& inst) {
    // The SAME resolution the build will perform, not a second one shaped
    // like it. Install-time and build-time disagreeing about which SDK this
    // toolset uses is worse than not reporting at all: the line printed here
    // is what the user will believe.
    auto choice = mcpp::toolchain::msvc::resolve_sdk_for(inst.clPath);
    if (choice.sdk) {
        std::println("           windows sdk: {} ({})",
                     choice.sdk->version, choice.sdk->root.string());
        if (!choice.note.empty()) mcpp::ui::info("note", choice.note);
        return;
    }
    mcpp::ui::warning(
        "the toolset installed, but no Windows SDK was found next to it.\n"
        "         cl.exe cannot compile anything without the ucrt/um headers.\n"
        "         The toolset declares `xim:windows-sdk` as a dependency, so this\n"
        "         means that dependency did not install — check `xlings list`.");
}

EffectiveDefault effective_default_toolchain(const mcpp::config::GlobalConfig& cfg) {
    std::error_code ec;
    auto mpath = std::filesystem::current_path(ec) / "mcpp.toml";
    if (!ec && std::filesystem::exists(mpath, ec)) {
        if (auto m = mcpp::manifest::load(mpath)) {
            if (auto t = m->toolchain.for_platform(mcpp::platform::name);
                t && !t->empty())
                return { *t, true };
        }
    }
    return { cfg.defaultToolchain, false };
}

// Numeric semver comparison for display ordering (newest first). The old
// lexicographic sort put "9.4.0" above "15.1.0" — two-digit majors are the
// norm for GCC, so compare component-wise.
bool version_greater(const std::string& a, const std::string& b) {
    auto ca = parse_version_components(a);
    auto cb = parse_version_components(b);
    if (ca != cb) return cb < ca;
    return b < a;   // stable tie-break for non-numeric tails
}

export int toolchain_list(const mcpp::config::GlobalConfig& cfg) {
    auto pkgsDir = cfg.xlingsHome() / "data" / "xpkgs";
    auto effective = effective_default_toolchain(cfg);

    // The default is a PAIR: toolchain axis (family@version) + target axis.
    // Legacy default strings ("gcc@15.1.0-musl", "mingw-cross@16.1.0") carry
    // their target inside the spelling — compat normalization surfaces it.
    std::optional<mcpp::toolchain::ToolchainSpec> defSpec;
    if (auto s = mcpp::toolchain::parse_toolchain_spec(effective.spec); s)
        defSpec = *s;
    mcpp::toolchain::triple::Triple defTarget;
    if (defSpec) defTarget = defSpec->target;
    if (!cfg.defaultTarget.empty()) {
        if (auto t = mcpp::toolchain::triple::parse(cfg.defaultTarget))
            defTarget = *t;
    }
    auto hostT = mcpp::toolchain::triple::host_triple();

    // ── enumerate installed payloads → (identity, version, frontend) ────
    struct Payload {
        mcpp::toolchain::PayloadIdentity id;
        std::string version;
        std::filesystem::path bin;
    };
    std::vector<Payload> payloads;
    std::error_code ec;
    if (std::filesystem::exists(pkgsDir, ec)) {
        for (auto& entry : std::filesystem::directory_iterator(pkgsDir, ec)) {
            auto name = entry.path().filename().string();
            if (auto sep = name.find("-x-"); sep != std::string::npos)
                name = name.substr(sep + 3);
            auto id = mcpp::toolchain::identify_xim_payload(name);
            if (!id) continue;                       // ninja/glibc/… — not a toolchain
            for (auto& vEntry : std::filesystem::directory_iterator(entry.path(), ec)) {
                if (!vEntry.is_directory(ec)) continue;
                mcpp::toolchain::ToolchainSpec s;
                s.family  = id->family;
                s.version = vEntry.path().filename().string();
                s.target  = id->target;
                auto pkg = mcpp::toolchain::to_xim_package(s);
                // From the payload ROOT, not `root/bin`: msvc keeps cl.exe
                // four levels deeper, and asking for `root/bin` skipped every
                // installed toolset silently.
                auto bin = mcpp::toolchain::payload_frontend(vEntry.path(), pkg,
                                                             id->family);
                if (bin.empty()) continue;
                payloads.push_back({ *id, s.version, bin });
            }
        }
    }

    // ── Toolchains block: the family@version axis ────────────────────────
    struct FamVer { mcpp::toolchain::Family family; std::string version; };
    std::vector<FamVer> famvers;
    for (auto& p : payloads) {
        bool seen = std::any_of(famvers.begin(), famvers.end(), [&](const FamVer& f){
            return f.family == p.id.family && f.version == p.version; });
        if (!seen) famvers.push_back({ p.id.family, p.version });
    }
    std::sort(famvers.begin(), famvers.end(), [](const FamVer& a, const FamVer& b){
        if (a.family != b.family)
            return mcpp::toolchain::family_name(a.family) < mcpp::toolchain::family_name(b.family);
        return version_greater(a.version, b.version);
    });

    if (payloads.empty()) {
        std::println("(no toolchains installed — run `mcpp toolchain install gcc 16.1.0` "
                     "or just `mcpp build` to auto-install the default)");
    } else {
        std::println("Toolchains:");
        for (auto& f : famvers) {
            bool isDefault = defSpec
                && mcpp::toolchain::spec_matches_payload(*defSpec, { f.family, {} }, f.version);
            std::println("  {:<3}{:<22}{}",
                isDefault ? "*" : "",
                std::format("{} {}", mcpp::toolchain::family_name(f.family), f.version),
                isDefault ? "  (default)" : "");
        }
        if (effective.fromProject) {
            std::println("  (* = effective toolchain from project mcpp.toml "
                         "[toolchain]; global default is '{}')",
                         cfg.defaultToolchain.empty() ? "<none>"
                                                      : cfg.defaultToolchain);
        }
    }

    // ─── System section (Windows: detected MSVC) ────────────────────────
    // MSVC is never in xpkgs — it's located on the machine. Show it so
    // `toolchain list` reflects everything `toolchain default` accepts.
    if (mcpp::platform::is_windows) {
        if (auto inst = mcpp::toolchain::msvc::detect_installation()) {
            bool isDefault = defSpec
                && defSpec->family == mcpp::toolchain::Family::Msvc;
            std::println("");
            std::println("System:");
            std::println("  {:<3}{:<22}  {}",
                isDefault ? "*" : "",
                std::format("msvc {}", inst->display_version()),
                inst->clPath.string());
        } else {
            std::println("");
            std::println("  (msvc: not detected — run `mcpp toolchain default msvc` "
                         "for setup guidance)");
        }
    }

    // ── Targets block: the target axis (installed payload rows + the known-
    //    target vocabulary so available/planned targets are discoverable
    //    without reading CI configs) ───────────────────────────────────────
    struct TargetRow {
        std::string target;      // canonical triple
        std::string note;        // "host" / "static" / "PE" / "cross" tags
        std::string toolchain;   // "gcc 16.1.0" or "—"
        std::string status;      // installed | available | planned
        bool        isDefault = false;
        int         rank = 0;    // display order: installed < available < planned
    };
    std::vector<TargetRow> targetRows;

    auto note_for = [&](const mcpp::toolchain::triple::Triple& t) {
        std::vector<std::string> tags;
        if (t == hostT) tags.push_back("host");
        if (auto* info = mcpp::toolchain::triple::find_known_target(t)) {
            if (!info->note.empty()) tags.emplace_back(info->note);
            if (info->defaultStatic) tags.push_back("static");
        }
        if (t != hostT && (t.os != hostT.os || t.arch != hostT.arch))
            tags.push_back("cross");
        std::string out;
        for (auto& tag : tags) { if (!out.empty()) out += ", "; out += tag; }
        return out;
    };

    for (auto& p : payloads) {
        auto t = p.id.target.empty() ? hostT : p.id.target;
        std::string tcLabel = std::format("{} {}",
            mcpp::toolchain::family_name(p.id.family), p.version);
        bool dup = std::any_of(targetRows.begin(), targetRows.end(),
            [&](const TargetRow& r){ return r.target == t.str() && r.toolchain == tcLabel; });
        if (dup) continue;
        TargetRow r;
        r.target    = t.str();
        r.note      = note_for(t);
        r.toolchain = tcLabel;
        r.status    = "installed";
        r.rank      = 0;
        r.isDefault = defSpec
            && mcpp::toolchain::spec_matches_payload(*defSpec, p.id, p.version)
            && t == (defTarget.empty() ? hostT : defTarget);
        targetRows.push_back(std::move(r));
    }

    // Vocabulary rows not covered by an installed payload. Only list a
    // verified target as "available" when this host can actually install it.
    //
    // The predicate lives in registry.cppm next to the payload resolution it
    // must agree with — this used to be a second, independent derivation of
    // the same question and the two had already drifted apart.
    auto installable_here = [&](const mcpp::toolchain::triple::Triple& t) {
        return mcpp::toolchain::host_can_serve(t);
    };
    for (auto& info : mcpp::toolchain::triple::known_targets()) {
        bool covered = std::any_of(targetRows.begin(), targetRows.end(),
            [&](const TargetRow& r){ return r.target == info.canonical; });
        if (covered) continue;
        auto t = mcpp::toolchain::triple::parse(info.canonical);
        if (!t) continue;
        bool planned = info.tier == "planned";
        // ⭐⭐ A ROW NO PAYLOAD SERVES IS NOT THEREFORE UNBUILDABLE.
        //
        // `host_can_serve` answers "can this host serve the target FROM A
        // PAYLOAD". Dropping the row presents that as "can this host build for
        // the target", and one row separates the two. Measured 2026-08-25 on
        // Linux: `x86_64-windows-musl` was absent from this list while the same
        // machine produced a real artefact for it —
        //
        //     $ mcpp build --target x86_64-windows-musl
        //       c-abi  musl  (openkal-musl@0.3.5, graph)
        //     $ file …/winmusl.exe
        //       PE32+ executable (console) x86-64, for MS Windows
        //
        // — because its system came from the dependency graph, which is what
        // the row's own note in the target table says happens.
        //
        // ⚠️ AND NOT EVERY ABSENT ROW IS THAT. `x86_64-windows-msvc` and
        // `aarch64-macos` are absent on a Linux host CORRECTLY: MSVC and the
        // macOS SDK are host-only and no dependency substitutes for them.
        // The discriminator is already in the table and needs no new field —
        // a row that names a compiler THIS host can install is one whose only
        // missing piece is the system, and a graph can supply a system.
        bool graphCouldServe = false;
        if (!planned && !installable_here(*t) && !info.pin.empty()) {
            auto at = info.pin.find('@');
            auto fam = info.pin.substr(0, at == std::string_view::npos
                                              ? info.pin.size() : at);
            for (auto const& idx : mcpp::toolchain::available_toolchain_indexes())
                if (idx.ximName == fam) { graphCouldServe = true; break; }
        }
        if (!planned && !installable_here(*t) && !graphCouldServe) continue;
        TargetRow r;
        r.target    = std::string(info.canonical);
        r.note      = note_for(*t);
        // Render the pin in the same "family version" shape as installed rows.
        std::string pin(info.pin);
        if (auto at = pin.find('@'); at != std::string::npos) pin[at] = ' ';
        r.toolchain = pin.empty() ? "—" : pin;
        // Three answers, not two. "available" means a payload here produces it;
        // "via dependency graph" means the compiler is here and the system has
        // to come from packages — a different thing to do next, so a different
        // word.
        r.status    = planned          ? "planned"
                    : graphCouldServe  ? "via dependency graph"
                                       : "available";
        r.rank      = planned ? 2 : 1;
        targetRows.push_back(std::move(r));
    }
    std::sort(targetRows.begin(), targetRows.end(),
        [](const TargetRow& a, const TargetRow& b){
            if (a.rank != b.rank) return a.rank < b.rank;
            return a.target < b.target;
        });

    if (!targetRows.empty()) {
        std::println("");
        std::println("Targets:");
        std::println("  {:<3}{:<24}{:<22}{:<18}{}",
                     "", "TARGET", "NOTE", "TOOLCHAIN", "STATUS");
        for (auto& r : targetRows) {
            std::println("  {:<3}{:<24}{:<22}{:<18}{}",
                r.isDefault ? "*" : "",
                r.target, r.note, r.toolchain, r.status);
        }
    }

    // ── Available toolchains: versions known to the local index, per family,
    //    excluding family@version pairs already installed ──────────────────
    std::map<mcpp::toolchain::Family, std::vector<std::string>> avail;
    for (auto& idx : mcpp::toolchain::available_toolchain_indexes()) {
        for (auto& v : list_available_xpkg_versions(cfg, idx.ximName)) {
            // Skip the `["latest"] = { ref = … }` alias entries.
            if (v.empty() || !std::isdigit(static_cast<unsigned char>(v[0])))
                continue;
            bool installed = std::any_of(famvers.begin(), famvers.end(),
                [&](const FamVer& f){ return f.family == idx.family && f.version == v; });
            if (installed) continue;
            auto& vec = avail[idx.family];
            if (std::find(vec.begin(), vec.end(), v) == vec.end())
                vec.push_back(v);
        }
    }
    if (!avail.empty()) {
        std::println("");
        std::println("Available toolchains (run `mcpp toolchain install <family> <version>`):");
        for (auto& [family, versions] : avail) {
            std::sort(versions.begin(), versions.end(), version_greater);
            std::string joined;
            for (auto& v : versions) {
                if (!joined.empty()) joined += " / ";
                joined += v;
            }
            std::println("  {:<3}{} {}", "",
                mcpp::toolchain::family_name(family), joined);
        }
    }
    return 0;
}

// Parse-and-attach the optional `--target <triple>` axis onto a spec.
// Validates against the known-target vocabulary (with did-you-mean).
// Returns non-zero exit code on error, 0 on success.
int attach_target_arg(mcpp::toolchain::ToolchainSpec& spec,
                      const std::string& targetArg) {
    if (targetArg.empty()) return 0;
    auto t = mcpp::toolchain::triple::parse(targetArg);
    if (!t || !mcpp::toolchain::triple::is_known_target(*t)) {
        auto suggestion = mcpp::toolchain::triple::did_you_mean(targetArg);
        mcpp::ui::error(std::format(
            "unknown target '{}'{}\n       known targets: run `mcpp toolchain list`",
            targetArg,
            suggestion ? std::format(" — did you mean '{}'?", *suggestion) : ""));
        return 2;
    }
    spec.target = *t;
    return 0;
}

// `mcpp toolchain install <spec> [--target <triple>]` — install + fixups.
export int toolchain_install(const mcpp::config::GlobalConfig& cfg,
                             const std::string& pos0, const std::string& pos1,
                             const std::string& targetArg = {}) {
        // Accept three input shapes — they all collapse to (family, version):
        //   mcpp toolchain install gcc 16.1.0      → ("gcc", "16.1.0")
        //   mcpp toolchain install gcc@16.1.0      → ("gcc", "16.1.0")
        //   mcpp toolchain install gcc 15          → ("gcc", "15")  partial
        // plus the target axis:
        //   mcpp toolchain install gcc 16 --target x86_64-windows-gnu
        //   mcpp toolchain install --target x86_64-linux-musl   (family from pin)
        // (parsed before the bootstrap check: system toolchains need no
        // bootstrap, and an invalid spec should not report a bootstrap error)
        std::string pos0Eff = pos0;
        if (pos0Eff.empty() && !targetArg.empty()) {
            // Family omitted: take the target's convention pin (gcc@16.1.0
            // for musl / windows-gnu targets).
            if (auto t = mcpp::toolchain::triple::parse(targetArg)) {
                if (auto* info = mcpp::toolchain::triple::find_known_target(*t);
                    info && !info->pin.empty())
                    pos0Eff = std::string(info->pin);
            }
        }
        auto spec = mcpp::toolchain::parse_toolchain_spec(
            pos0Eff, pos1);
        if (!spec) {
            mcpp::ui::error(std::format(
                "{}; e.g. `mcpp toolchain install gcc 16.1.0`", spec.error()));
            return 2;
        }
        mcpp::toolchain::print_compat_hint(*spec);
        if (int rc = attach_target_arg(*spec, targetArg); rc != 0) return rc;

        // msvc@system: mcpp never installs the machine's Visual Studio —
        // report what's there, or point at both ways forward.
        // (msvc@<toolset> is NOT a system spec and falls through to the xim
        // package path below, like every other family.)
        if (mcpp::toolchain::is_system_toolchain(*spec)) {
            if (!mcpp::platform::is_windows) return msvc_wrong_host();
            if (auto inst = mcpp::toolchain::msvc::detect_installation()) {
                msvc_print_detected(*inst);
                std::println("");
                std::println("This is the machine's own Visual Studio — mcpp does not manage it.");
                std::println("Tip: `mcpp toolchain default msvc` to make it the default,");
                std::println("     or `mcpp toolchain install msvc <toolset>` for a pinned one");
                std::println("     that does not depend on what this machine has installed.");
                return 0;
            }
            mcpp::ui::error(mcpp::toolchain::msvc::install_guidance());
            return 1;
        }

        // Toolchain install needs patchelf (ELF fixup) and ninja (build).
        // Fail early if bootstrap is incomplete rather than producing a
        // broken toolchain with missing fixups.
        auto bsProblem = mcpp::config::check_base_init(cfg);
        if (!bsProblem.empty()) {
            mcpp::ui::error(std::format(
                "{}\n  hint: run `mcpp self init --force` to reset and re-initialize",
                bsProblem));
            return 1;
        }

        auto pkg = mcpp::toolchain::to_xim_package(*spec);

        // Partial-version resolution: `gcc 15` → highest available 15.x.y in
        // the synced index. Empty version → latest of any major.
        if (auto picked = resolve_version_match(
                pkg.ximVersion, list_available_xpkg_versions(cfg, pkg.ximName))) {
            if (*picked != pkg.ximVersion) {
                mcpp::ui::info("Resolved",
                    std::format("{} → {}@{}", spec->spec_str(),
                                mcpp::toolchain::family_name(spec->family), *picked));
            }
            spec = mcpp::toolchain::with_resolved_xim_version(*spec, *picked);
            pkg = mcpp::toolchain::to_xim_package(*spec);
        }

        mcpp::ui::info("Installing",
            std::format("{} via mcpp's xlings", spec->display()));
        mcpp::log::verbose("toolchain", std::format(
            "install: target='{}' xlingsHome='{}'", pkg.target(), cfg.xlingsHome().string()));
        mcpp::log::debug("toolchain", std::format(
            "  ximName='{}' needsGccFixup={} xlingsBinary='{}'",
            pkg.ximName, pkg.needsGccPostInstallFixup, cfg.xlingsBinary.string()));
        mcpp::fetcher::Fetcher fetcher(cfg);
        mcpp::fetcher::InstallProgressHandler progress;

        // Ensure sysroot dependencies (glibc, linux-headers) are installed:
        // the C library and kernel headers a glibc-targeting compile needs.
        // The rule itself lives in registry.cppm — prepare's first-run
        // install asks the same question and used to answer it in its own
        // words, with one term missing.
        if (mcpp::toolchain::needs_linux_sysroot_payloads(spec->target)) {
            for (auto dep : {"xim:glibc", "xim:linux-headers"}) {
                mcpp::log::verbose("toolchain", std::format("installing dep: {}", dep));
                auto depPayload = fetcher.resolve_xpkg_path(dep, /*autoInstall=*/true, &progress);
                mcpp::log::debug("toolchain", std::format("dep {} result: {}",
                    dep, depPayload ? "ok" : depPayload.error().message));
            }
        }

        mcpp::log::verbose("toolchain", std::format("installing main: {}", pkg.target()));
        // A previous `remove` may have parked a held payload beside this one;
        // by now whatever held it has exited, so free the bytes before adding
        // another few hundred MB.
        {
            auto vdir = mcpp::xlings::paths::xim_tool(
                mcpp::config::make_xlings_env(cfg), pkg.ximName, pkg.ximVersion);
            sweep_parked_payloads(vdir.parent_path(), vdir);
        }
        auto payload = fetcher.resolve_xpkg_path(pkg.target(), /*autoInstall=*/true, &progress);
        mcpp::log::verbose("toolchain", std::format("main install result: {}",
            payload ? ("ok → " + payload->root.string()) : payload.error().message));
        if (!payload) {
            mcpp::ui::error(std::format("install failed: {}", payload.error().message));
            return 1;
        }

        // A pinned MSVC toolset shares the ACQUISITION with gcc (the xim
        // install above) and the RESOLUTION with msvc@system (below) — the
        // two axes are independent, so neither gets a private copy of the
        // other's logic.
        //
        // What it does not share is the bin/-shaped frontend lookup and the
        // ELF post-install fixup: an msvc payload keeps cl.exe under
        // VC/Tools/MSVC/<ver>/bin/Hostx64/x64, and there is nothing to
        // patchelf on a PE toolchain.
        if (spec->family == mcpp::toolchain::Family::Msvc) {
            // Where the payload IS — not `payload->root`, which is the
            // fetcher's guess and guesses wrong here. That rule and its
            // reasons live in `resolve_managed_msvc`, because a build needs
            // exactly the same one and the two used to spell it out
            // separately. `resolve_xpkg_path` above is what INSTALLS; this is
            // what says where.
            auto inst = mcpp::toolchain::resolve_managed_msvc(
                mcpp::config::make_xlings_env(cfg), pkg);
            if (!inst) {
                mcpp::ui::error(std::format(
                    "{} — the payload is not what this version claims to be",
                    inst.error()));
                return 1;
            }
            msvc_print_detected(*inst, "Installed");
            msvc_warn_if_sdk_missing(*inst);
            mcpp::ui::status("Installed",
                std::format("{} → {}", pkg.display_spec(), inst->clPath.string()));
            if (cfg.defaultToolchain.empty()) {
                std::println("");
                std::println("Tip: `mcpp toolchain default {}` to make this the default.",
                             spec->spec_str());
            }
            return 0;
        }

        auto bin = mcpp::toolchain::toolchain_frontend(payload->binDir, pkg);
        if (!std::filesystem::exists(bin)) {
            mcpp::ui::error(std::format(
                "installed package has no known C++ frontend in '{}'",
                payload->binDir.string()));
            return 1;
        }

        // Post-install fixup (patchelf / specs / cfg regeneration) — ONE
        // pipeline shared by every toolchain install path, dispatched and
        // made idempotent inside ensure_post_install_fixup.
        //
        // ⚠️ RESOLVE THE RUNTIME BINDING HERE, do not let the fixup guess.
        // The fixup is a CONSUMER of RuntimeBinding (`prepare.cppm` says so
        // where it does the same thing) and it no longer derives an identity
        // of its own — so a caller that passes nothing gets no fixup at all.
        // `mcpp toolchain install` is the path that needs it MOST: without
        // the patchelf/specs wiring a fresh sandbox gcc cannot find the C
        // library. There is no project here, so the selection is mcpp's
        // default SubOS, which is exactly what `RuntimeSelection{}` means.
        std::string runtimeId;
        std::filesystem::path runtimeLibDir;
        if (auto rb = mcpp::platform::runtime::resolve_runtime_binding(
                mcpp::xlings::runtime::RuntimeSelection{}, {}, cfg)) {
            runtimeId = rb->runtimeId;
            if (!rb->libraryDirs.empty()) runtimeLibDir = rb->libraryDirs.front();
        }   // a binding that cannot be resolved degrades below, it does not stop
            // the install — same rule the build path follows.

        if (auto fixed = mcpp::toolchain::ensure_post_install_fixup(
                cfg, payload->root, pkg, runtimeId, runtimeLibDir); !fixed) {
            mcpp::ui::error(std::format(
                "post-install fixup failed: {}", fixed.error()));
            return 1;
        } else if (!fixed->skippedReason.empty()) {
            // The user asked for this install explicitly, so the degradation
            // is louder here than on a build — but it is still a warning, and
            // the install itself succeeded.
            mcpp::ui::warning(std::format(
                "installed, but not bound to a C runtime: {}.\n"
                "         The install itself succeeded. To complete the wiring, "
                "run `xlings self doctor --fix` from inside this SubOS, then "
                "re-run this command (openxlings/xlings#547).",
                fixed->skippedReason));
        }

        mcpp::ui::status("Installed",
            std::format("{} → {}", pkg.display_spec(), bin.string()));
        if (cfg.defaultToolchain.empty()) {
            std::println("");
            std::println("Tip: `mcpp toolchain default {}{}` to make this the default.",
                         spec->spec_str(),
                         spec->target.empty()
                             ? std::string{}
                             : std::format(" --target {}", spec->target.str()));
        }
        return 0;
    }

// `mcpp toolchain default <spec> [--target <triple>]` — persist the default
// (toolchain axis + target axis) pair.
export int toolchain_set_default(const mcpp::config::GlobalConfig& cfg,
                                 const std::string& pos0, const std::string& pos1,
                                 const std::string& targetArg = {}) {
    auto pkgsDir = cfg.xlingsHome() / "data" / "xpkgs";
    auto xlEnv = mcpp::config::make_xlings_env(cfg);
        // Accept three input shapes (mirrors `install`):
        //   mcpp toolchain default gcc@16.1.0
        //   mcpp toolchain default gcc 16.1.0
        //   mcpp toolchain default gcc 15        ← partial; picks highest 15.x.y
        auto spec = mcpp::toolchain::parse_toolchain_spec(
            pos0, pos1);
        if (!spec) {
            mcpp::ui::error(std::format(
                "{}; e.g. `mcpp toolchain default gcc@16.1.0`", spec.error()));
            return 2;
        }
        mcpp::toolchain::print_compat_hint(*spec);
        if (int rc = attach_target_arg(*spec, targetArg); rc != 0) return rc;

        // msvc@system: locate + identify the machine's own MSVC, persist the
        // stable spec (never a concrete version — config survives VS
        // updates). A versioned msvc spec is a payload and falls through.
        if (mcpp::toolchain::is_system_toolchain(*spec)) {
            if (!mcpp::platform::is_windows) return msvc_wrong_host();
            auto inst = mcpp::toolchain::msvc::detect_installation();
            if (!inst) {
                mcpp::ui::error(mcpp::toolchain::msvc::install_guidance());
                return 1;
            }
            msvc_print_detected(*inst);
            auto wr = mcpp::config::write_default_toolchain(cfg, "msvc@system");
            if (!wr) {
                mcpp::ui::error(wr.error().message);
                return 1;
            }
            // Clear the target axis too: a stale default_target (e.g.
            // x86_64-windows-gnu left by a previous mingw default) would
            // otherwise hijack the next build — the target's convention pin
            // overrides the toolchain, silently building with gcc instead
            // of the just-selected cl.exe. Caught by ci-windows e2e 99.
            if (auto wt = mcpp::config::write_default_target(cfg, ""); !wt) {
                mcpp::ui::error(wt.error().message);
                return 1;
            }
            mcpp::ui::status("Default", std::format(
                "set to msvc@system (was: {})",
                cfg.defaultToolchain.empty() ? "<none>" : cfg.defaultToolchain));

            return 0;
        }

        auto pkg = mcpp::toolchain::to_xim_package(*spec);

        // Partial-version resolution against installed payloads.
        if (auto picked = resolve_version_match(
                pkg.ximVersion, list_installed_versions(pkgsDir, pkg.ximName))) {
            spec = mcpp::toolchain::with_resolved_xim_version(*spec, *picked);
            pkg = mcpp::toolchain::to_xim_package(*spec);
        }

        auto installDir = mcpp::xlings::paths::xim_tool(xlEnv, pkg.ximName, pkg.ximVersion);
        // A RESOLVABLE COMPILER, not a directory that exists. A removal that
        // could not delete the last empty directories leaves a skeleton
        // behind (see remove_payload_tree), and `exists()` would call that
        // skeleton an installed toolchain and then hand it to a build.
        //
        // Same rule as everywhere else in this round: installed means usable,
        // not present.
        if (mcpp::toolchain::payload_frontend(installDir, pkg, spec->family).empty()) {
            // Before "not installed", check whether this is the retired
            // `msvc@<cl-version>` spelling — otherwise the advice is to
            // install a toolset that does not exist and never will.
            if (spec->family == mcpp::toolchain::Family::Msvc) {
                if (auto hint = mcpp::toolchain::msvc::cl_version_spelling_hint(
                        spec->version)) {
                    mcpp::ui::error(*hint);
                    return 1;
                }
            }
            mcpp::ui::error(std::format(
                "{} is not installed. Run `mcpp toolchain install {} {}{}` first.",
                spec->display(), mcpp::toolchain::family_name(spec->family),
                spec->version.empty() ? pkg.ximVersion : spec->version,
                spec->target.empty()
                    ? std::string{}
                    : std::format(" --target {}", spec->target.str())));
            return 1;
        }
        // Persist the pair: the toolchain axis in [toolchain].default, the
        // target axis in [toolchain].default_target (empty = host). Legacy
        // combined spellings in existing configs keep parsing via compat.
        auto wr = mcpp::config::write_default_toolchain(cfg, spec->spec_str());
        if (!wr) {
            mcpp::ui::error(wr.error().message);
            return 1;
        }
        if (auto wt = mcpp::config::write_default_target(cfg, spec->target.str()); !wt) {
            mcpp::ui::error(wt.error().message);
            return 1;
        }
        mcpp::ui::status("Default", std::format(
            "set to {} (was: {})", spec->display(),
            cfg.defaultToolchain.empty() ? "<none>" : cfg.defaultToolchain));
        return 0;
    }

// `mcpp toolchain remove <spec> [--target <triple>]` — uninstall a payload.
export int toolchain_remove(const mcpp::config::GlobalConfig& cfg,
                            const std::string& pos0,
                            const std::string& targetArg = {}) {
    auto xlEnv = mcpp::config::make_xlings_env(cfg);
        auto parsedSpec = mcpp::toolchain::parse_toolchain_spec(pos0);
        if (parsedSpec && mcpp::toolchain::is_system_toolchain(*parsedSpec)) {
            mcpp::ui::error(
                "msvc@system is the machine's own Visual Studio, managed by the "
                "Visual Studio Installer — mcpp cannot remove it.\n"
                "  A toolset mcpp installed can be removed by naming it:\n"
                "    mcpp toolchain remove msvc@<toolset>");
            return 1;
        }
        if (!parsedSpec || parsedSpec->version.empty()) {
            mcpp::ui::error(std::format("invalid spec '{}'", pos0));
            return 2;
        }
        mcpp::toolchain::print_compat_hint(*parsedSpec);
        if (int rc = attach_target_arg(*parsedSpec, targetArg); rc != 0) return rc;
        auto pkg = mcpp::toolchain::to_xim_package(*parsedSpec);
        auto spec = parsedSpec->display();
        auto installDir = mcpp::xlings::paths::xim_tool(xlEnv, pkg.ximName, pkg.ximVersion);
        std::error_code ec;
        if (!std::filesystem::exists(installDir, ec)) {
            mcpp::ui::error(std::format("{} is not installed", spec));
            return 1;
        }
        sweep_parked_payloads(installDir.parent_path(), installDir);
        if (!remove_payload_tree(installDir, ec)) {
            // Say that the payload is now BROKEN, not merely that removal
            // failed. `remove_all` deletes what it can before it stops, so a
            // failed remove is not a no-op: what is left is a toolchain with
            // holes in it, and someone who reads "remove failed" and moves on
            // will meet those holes as a build error instead.
            mcpp::ui::error(std::format(
                "remove failed: {}{}\n"
                "         The payload is now INCOMPLETE — files were deleted "
                "before this one\n"
                "         blocked the rest. Close whatever holds it and run "
                "the same command\n"
                "         again; do not build with this toolchain until it "
                "removes cleanly.",
                ec.message(),
                first_undeletable(installDir)
                    .transform([](const std::filesystem::path& p) {
                        return std::format("\n         stuck at: {}", p.string());
                    })
                    .value_or(std::string{})));
            return 1;
        }
        mcpp::ui::status("Removed", spec);
        // The persisted default may be the removed payload under either the
        // canonical or a legacy spelling — compare parsed identities.
        if (auto def = mcpp::toolchain::parse_toolchain_spec(cfg.defaultToolchain);
            def && def->family == parsedSpec->family
                && def->version == parsedSpec->version
                && def->target == parsedSpec->target) {
            mcpp::ui::warning(std::format(
                "default toolchain '{}' was just removed; consider `mcpp toolchain default <other>`",
                spec));
        }
        return 0;
    }

} // namespace mcpp::toolchain

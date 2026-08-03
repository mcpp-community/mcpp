// mcpp.toolchain.triple — the single source of truth for target identity.
//
// mcpp owns its target-triple language: canonical form is `arch-os[-env]`
// (three segments, no vendor — Zig-style). `x86_64-linux-musl` was already
// canonical before this module existed; this extends the same convention to
// every target. GNU/LLVM spellings (`x86_64-w64-mingw32`,
// `x86_64-unknown-linux-gnu`, `arm64-apple-darwin24`) are permanent input
// aliases, normalized here.
//
// Everything that previously parsed triples ad hoc (cfgpred::context_for,
// abi_profile, model.cppm's is_*_target, registry's musl signals) consumes
// this module now. Vocabulary: os ∈ {linux, macos, windows} (never "darwin"),
// arch is the GNU spelling ({x86_64, aarch64, riscv64, …} — never "arm64"),
// env ∈ {gnu, musl, msvc} (empty on macos). `static` is NOT part of a triple:
// it is a target's default linkage property, flipped via [build].
//
// The known-target table below is the closed vocabulary `--target` validates
// against (with an escape hatch for explicit [target.X] manifest sections)
// and the source the README platform table is drawn from. Adding a target =
// adding a row here (+ payload mapping in registry.cppm if a new payload
// shape is involved).
//
// See .agents/docs/2026-07-15-toolchain-target-naming-unification-design.md.

export module mcpp.toolchain.triple;

import std;
import mcpp.platform;

export namespace mcpp::toolchain::triple {

struct Triple {
    std::string arch;   // "x86_64" | "aarch64" | "riscv64" | ... (GNU spelling)
    std::string os;     // "linux" | "macos" | "windows"
    std::string env;    // "gnu" | "musl" | "msvc" | "" (always empty on macos)

    bool empty() const { return arch.empty() && os.empty(); }

    // Canonical rendering: "arch-os[-env]"; "" for an empty (= host) triple.
    std::string str() const {
        if (empty()) return {};
        std::string s = arch + "-" + os;
        if (!env.empty()) { s += "-"; s += env; }
        return s;
    }

    bool is_musl() const        { return env == "musl"; }
    bool is_msvc_env() const    { return env == "msvc"; }
    bool is_windows_gnu() const { return os == "windows" && env == "gnu"; }
    bool is_pe() const          { return os == "windows"; }

    // cfg() `family` dimension: unix | windows.
    std::string family() const {
        if (os == "windows") return "windows";
        if (os == "linux" || os == "macos") return "unix";
        return {};
    }

    // NASM `-f` output format for this target. NASM is x86-family only:
    // nullopt off x86, and the caller must hard-error (suggesting cfg-gated
    // sources) rather than pick a format.
    std::optional<std::string> nasm_format() const {
        bool x64 = arch == "x86_64";
        bool x32 = arch == "x86" || arch == "i386" || arch == "i486"
                || arch == "i586" || arch == "i686";
        if (!x64 && !x32) return std::nullopt;
        if (os == "windows") return x64 ? "win64" : "win32";
        if (os == "macos")   return x64 ? "macho64" : "macho32";
        if (os == "linux")   return x64 ? "elf64" : "elf32";
        return std::nullopt;
    }

    bool operator==(const Triple&) const = default;
};

// Lenient parse of any recognizable triple spelling into canonical fields.
// Handles mcpp-canonical ("x86_64-linux-musl"), GNU ("x86_64-w64-mingw32",
// "x86_64-pc-linux-gnu"), LLVM/Rust 4-segment ("x86_64-unknown-linux-musl",
// "x86_64-pc-windows-msvc") and Apple ("arm64-apple-darwin24.1.0",
// "arm64-apple-macosx15.0") forms. Returns nullopt when no OS is
// recognizable — the input is not a triple at all.
std::optional<Triple> parse(std::string_view s);

// ── Known-target registry (closed vocabulary; data, not code) ────────────────
//
// tier semantics (Rust-style):
//   verified  — CI builds AND executes the artifact end-to-end (qemu/wine count)
//   planned   — registered intent; payload or CI row not wired yet
struct TargetInfo {
    std::string_view canonical;   // "x86_64-linux-musl"
    std::string_view tier;        // "verified" | "planned"
    std::string_view note;        // display annotation: "static" / "PE" / ""
    // Convention toolchain pin for `--target <canonical>` with no explicit
    // [target.X] toolchain override. Empty = no convention (host default).
    std::string_view pin;
    bool defaultStatic;           // target's default linkage is static
};

// (note deliberately excludes "static" — the display layer derives that tag
// from defaultStatic, so listing it here would duplicate it.)
inline constexpr TargetInfo kKnownTargets[] = {
    // canonical               tier         note   pin           defaultStatic
    { "x86_64-linux-gnu",      "verified",  "",    "",           false },
    { "x86_64-linux-musl",     "verified",  "",    "gcc@16.1.0", true  },
    { "aarch64-linux-musl",    "verified",  "",    "gcc@16.1.0", true  },
    { "x86_64-windows-gnu",    "verified",  "PE",  "gcc@16.1.0", true  },
    { "x86_64-windows-msvc",   "verified",  "PE",  "",           false },
    { "aarch64-macos",         "verified",  "",    "",           false },
    { "riscv64-linux-musl",    "planned",   "",    "",           true  },
    { "aarch64-linux-gnu",     "planned",   "",    "",           false },
    { "x86_64-macos",          "planned",   "",    "",           false },
};

inline std::span<const TargetInfo> known_targets() { return kKnownTargets; }

inline const TargetInfo* find_known_target(const Triple& t) {
    auto s = t.str();
    for (auto& k : kKnownTargets)
        if (k.canonical == s) return &k;
    return nullptr;
}

inline bool is_known_target(const Triple& t) { return find_known_target(t) != nullptr; }

// Closest known-target canonical name for a mistyped `--target` (checked
// against canonical names AND common alias spellings). nullopt when nothing
// is plausibly close.
std::optional<std::string> did_you_mean(std::string_view input);

// Host coordinates as a canonical Triple (linux hosts report env=gnu — the
// user-facing host default, independent of how mcpp itself was linked).
inline Triple host_triple() {
    Triple t;
    t.arch = std::string(mcpp::platform::host_arch);
    t.os   = std::string(mcpp::platform::name);
    if (t.os == "linux")        t.env = "gnu";
    else if (t.os == "windows") t.env = "msvc";
    return t;
}

// ── Version pins (single site; §4.6 of the design doc) ───────────────────────
// Every default/convention toolchain version literal lives here. Help and
// error strings format these — never inline a pinned version elsewhere.
// Changing a pin: update this block, then sync docs/03-toolchains.md and the
// README platform table (drawn from kKnownTargets above).
namespace pins {
    // First-run auto-install defaults (prepare.cppm), per host platform/arch.
    //
    // macOS and Windows shared ONE pin until 2026.8.2.1. They must not:
    // Apple ships no GCC, so upstream LLVM with bundled libc++ is the only
    // self-contained choice there — but on Windows clang targets the MSVC
    // ABI (host triple env=msvc) and therefore uses the MSVC STL, which only
    // arrives with Visual Studio's "Desktop development with C++" workload.
    // A bare Windows box got a default it could never build with, and no
    // diagnostic. The Windows pin is now chosen by detection, not by
    // sharing macOS's answer.
    inline constexpr std::string_view kFirstRunMac          = "llvm@20.1.7";
    // Windows WITH a usable MSVC (STL + SDK, see msvc::has_usable_msvc()):
    // unchanged behavior. The MSVC ABI is what lets a project link vcpkg /
    // third-party .lib artifacts, so it stays the answer when it can work.
    inline constexpr std::string_view kFirstRunWinMsvc      = "llvm@20.1.7";
    // Windows WITHOUT one: winlibs GCC targeting PE/GNU. Fully self-contained
    // (static libstdc++/libgcc, its own UCRT), zero Visual Studio dependency,
    // `import std` works. Must stay equal to the x86_64-windows-gnu row's
    // `pin` in kKnownTargets above — test_windows_defaults.cpp enforces it.
    inline constexpr std::string_view kFirstRunWinGnu       = "gcc@16.1.0";
    inline constexpr std::string_view kFirstRunWinGnuTarget = "x86_64-windows-gnu";
    inline constexpr std::string_view kFirstRunLinuxX86_64  = "gcc@16.1.0";
    inline constexpr std::string_view kFirstRunLinuxOther   = "gcc@15.1.0-musl";
    // Suggested install spellings used by help / MCPP_NO_AUTO_INSTALL errors.
    inline constexpr std::string_view kSuggestLlvm          = "llvm 20.1.7";
    inline constexpr std::string_view kSuggestGccMusl       = "gcc 15.1.0-musl";
    inline constexpr std::string_view kSuggestGccMingw      = "gcc 16.1.0";
} // namespace pins

// ── Artifact naming conventions ──────────────────────────────────────────────
//
// How a built artifact is NAMED is a property of the TARGET, never of the
// machine doing the build. `mcpp::platform::{exe_suffix,lib_prefix,…}` answer a
// different question — "what does THIS machine call its own binaries" — and
// using them to name build outputs is wrong the moment host != target.
//
// It is a function of (os, env), not of os alone. The trap:
//
//   x86_64-windows-gnu   → libfoo.a    (GNU/mingw convention)
//   x86_64-windows-msvc  → foo.lib     (MSVC convention)
//
// A single `_WIN32` branch cannot express that, which is why building a static
// library with mingw ON a Windows host produces `foo.lib` today — a GNU archive
// wearing an MSVC name. That is a pre-existing defect, unrelated to cross
// compilation.
//
// See .agents/docs/2026-08-03-b3-target-aware-artifact-naming.md.
struct ArtifactNaming {
    std::string_view exeSuffix;     // ""      | ".exe"
    std::string_view libPrefix;     // "lib"   | ""
    std::string_view staticLibExt;  // ".a"    | ".lib"
    std::string_view sharedLibExt;  // ".so"   | ".dylib" | ".dll"
    // PE consumers link against an import library, not the .dll itself. mcpp
    // does not model import libraries yet, so this currently marks "shared
    // libraries are not supported for this target" rather than describing a
    // produced artifact. Shared libraries have never been verified end-to-end
    // on PE or Mach-O — every shared-library e2e declares `# requires: elf`.
    bool             sharedNeedsImportLib;
};

// Naming for an explicit target triple. An EMPTY triple means "build for this
// machine", and only then is the host answer the correct one — so the caller
// passes it in rather than this module reaching for mcpp::platform, which keeps
// the decision testable from any host (and keeps this module dependency-free).
inline ArtifactNaming artifact_naming(const Triple& t, const ArtifactNaming& hostNaming) {
    if (t.empty()) return hostNaming;

    if (t.os == "windows") {
        // PE. The static-library convention splits on env, not on os.
        const bool msvc = t.is_msvc_env();
        return ArtifactNaming{
            .exeSuffix            = ".exe",
            .libPrefix            = msvc ? "" : "lib",
            .staticLibExt         = msvc ? ".lib" : ".a",
            .sharedLibExt         = ".dll",
            .sharedNeedsImportLib = true,
        };
    }
    if (t.os == "macos") {
        return ArtifactNaming{
            .exeSuffix = "", .libPrefix = "lib",
            .staticLibExt = ".a", .sharedLibExt = ".dylib",
            .sharedNeedsImportLib = false,
        };
    }
    if (t.os == "linux") {
        return ArtifactNaming{
            .exeSuffix = "", .libPrefix = "lib",
            .staticLibExt = ".a", .sharedLibExt = ".so",
            .sharedNeedsImportLib = false,
        };
    }
    // Outside the triple language: fall back to the host answer rather than
    // guessing. A wrong guess here silently misnames every artifact.
    return hostNaming;
}

} // namespace mcpp::toolchain::triple

namespace mcpp::toolchain::triple {

namespace {

bool starts_with(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.substr(0, p.size()) == p;
}

std::string normalize_arch(std::string_view a) {
    if (a == "arm64") return "aarch64";   // Apple/xlings spelling → GNU
    if (a == "amd64") return "x86_64";
    return std::string(a);
}

// Levenshtein distance (small inputs only).
std::size_t edit_distance(std::string_view a, std::string_view b) {
    std::vector<std::size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) prev[j] = j;
    for (std::size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            std::size_t sub = prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
            cur[j] = std::min({ prev[j] + 1, cur[j - 1] + 1, sub });
        }
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

} // namespace

std::optional<Triple> parse(std::string_view s) {
    if (s.empty()) return std::nullopt;

    // Split on '-'.
    std::vector<std::string_view> tok;
    for (std::size_t b = 0; b <= s.size();) {
        auto d = s.find('-', b);
        if (d == std::string_view::npos) { tok.push_back(s.substr(b)); break; }
        tok.push_back(s.substr(b, d - b));
        b = d + 1;
    }
    if (tok.size() < 2 || tok[0].empty()) return std::nullopt;

    Triple t;
    t.arch = normalize_arch(tok[0]);

    bool sawOs = false;
    for (std::size_t i = 1; i < tok.size(); ++i) {
        std::string_view k = tok[i];
        if (k.empty()) return std::nullopt;
        // Vendor segments carry no information — skip. ("w64" is mingw-w64's
        // vendor; "apple" implies macOS when no OS token follows.)
        if (k == "unknown" || k == "pc" || k == "w64" || k == "none") continue;
        if (k == "apple") { if (!sawOs) { t.os = "macos"; sawOs = true; } continue; }

        if (k == "linux")                       { t.os = "linux";   sawOs = true; continue; }
        if (k == "windows")                     { t.os = "windows"; sawOs = true; continue; }
        if (starts_with(k, "darwin")
            || starts_with(k, "macosx")
            || starts_with(k, "macos"))         { t.os = "macos";   sawOs = true; t.env.clear(); continue; }
        // "mingw32" is the GNU os segment for ALL MinGW targets (64-bit
        // included — historical residue); it means windows + gnu env.
        if (starts_with(k, "mingw"))            { t.os = "windows"; sawOs = true; t.env = "gnu"; continue; }

        if (t.os != "macos") {
            if (k == "musl" || starts_with(k, "musleabi")) { t.env = "musl"; continue; }
            if (k == "gnu"  || starts_with(k, "gnueabi"))  { t.env = "gnu";  continue; }
            // starts_with: clang effective triples can carry a version suffix
            // on the env segment ("…-windows-msvc19.44.35211").
            if (starts_with(k, "msvc"))                    { t.env = "msvc"; continue; }
        }
        // Unrecognized segment (androideabi, wasi, …): not in mcpp's target
        // language — treat as unparseable rather than guessing.
        return std::nullopt;
    }

    if (!sawOs) return std::nullopt;
    if (t.os == "macos") t.env.clear();                 // macos carries no env segment
    if (t.os == "linux" && t.env.empty()) t.env = "gnu"; // "x86_64-linux" alias
    return t;
}

std::optional<std::string> did_you_mean(std::string_view input) {
    // Compare against canonical names and the common alias spellings a user
    // is likely to half-remember.
    static constexpr std::string_view kAliases[] = {
        "x86_64-w64-mingw32", "x86_64-unknown-linux-musl",
        "x86_64-unknown-linux-gnu", "x86_64-pc-windows-msvc",
        "x86_64-pc-windows-gnu", "aarch64-unknown-linux-musl",
    };
    std::optional<std::string> best;
    std::size_t bestDist = std::string_view::npos;
    auto consider = [&](std::string_view cand, std::string_view canonical) {
        auto d = edit_distance(input, cand);
        if (d < bestDist) { bestDist = d; best = std::string(canonical); }
    };
    for (auto& k : kKnownTargets) consider(k.canonical, k.canonical);
    for (auto& a : kAliases) {
        if (auto t = parse(a); t && is_known_target(*t)) consider(a, t->str());
    }
    // Only suggest when plausibly a typo: allow more slack for longer inputs.
    std::size_t budget = std::max<std::size_t>(2, input.size() / 4);
    if (best && bestDist <= budget) return best;
    return std::nullopt;
}

} // namespace mcpp::toolchain::triple

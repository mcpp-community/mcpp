// mcpp.toolchain.base — the target base: what the manifest key `sysroot`
// names, as data (SPEC-006 §3.5; design 2026-10-23, "target base and
// execution host").
//
// A SYSROOT IS A DELIVERY FORM, NOT A LAYER. It is the prebuilt supplier of a
// target's lower layers: it supplies `c-abi`, usually `kernel-abi`, and may
// supply `c++-abi`, `compiler-runtime` and platform libraries. The layers stay
// the closed five of SPEC-002; this module describes the form that supplies
// them, along four orthogonal attributes:
//
//   source    who stated it (override, project, machine default, engine
//             default), in #755's source classes
//   address   how a driver is told about it — one renderer per kind
//   layers    what it supplies, measured (§4.3) or declared (`provides`)
//   runtime   what a produced artifact does with it: binding, floor, none
//
// Pure data and pure functions. Locating, measuring and installing live in
// `src/toolchain/base_probe.cppm`; deciding which statement applies lives in
// prepare. Every one of the readers that used to answer "which sysroot" for
// itself reads the `Resolution` this module defines.

export module mcpp.toolchain.base;

import std;
import mcpp.toolchain.triple;

export namespace mcpp::toolchain::base {

// The spelling a statement used. `Default` is the absence of a statement.
enum class Form : std::uint8_t {
    Default  = 0,
    Package  = 1,   // `xim:glibc@2.39`, `acme:sysroot-rhel8@1.0`
    Locator  = 2,   // `glibc@system`, `iphoneos@18.5`, `msvc@14.44`
    Path     = 3,   // `{ path = "…" }`, `path:<dir>` in the environment
    Empty    = 4,   // `""`: no prebuilt C library
};

enum class Address : std::uint8_t {
    None,
    GnuSysroot,      // `--sysroot` for the driver and the linker
    AppleIsysroot,   // `-isysroot` and a deployment target
    MsvcRoots,       // the toolset and Windows SDK roots
    Multilib,        // `<root>/{include,lib}/<libdir>` of a bare-metal C library
    DriverInternal,  // an SDK toolchain that resolves its own system
};

enum class Runtime : std::uint8_t {
    None,      // nothing is loaded at run time (static, bare metal, wasm)
    Binding,   // PT_INTERP and RUNPATH name the base (a native build)
    Floor,     // the target's standard loader; the base is a lower bound
};

constexpr std::string_view form_name(Form f) {
    switch (f) {
        case Form::Default: return "default";
        case Form::Package: return "package";
        case Form::Locator: return "locator";
        case Form::Path:    return "path";
        case Form::Empty:   return "empty";
    }
    return "default";
}

constexpr std::string_view address_name(Address a) {
    switch (a) {
        case Address::None:           return "none";
        case Address::GnuSysroot:     return "gnu-sysroot";
        case Address::AppleIsysroot:  return "apple-isysroot";
        case Address::MsvcRoots:      return "msvc-roots";
        case Address::Multilib:       return "multilib";
        case Address::DriverInternal: return "driver-internal";
    }
    return "none";
}

constexpr std::string_view runtime_name(Runtime r) {
    switch (r) {
        case Runtime::None:    return "none";
        case Runtime::Binding: return "binding";
        case Runtime::Floor:   return "floor";
    }
    return "none";
}

// THE LOCATOR FAMILIES ARE THE INDUSTRY'S WORDS, NOT mcpp'S: the C library
// names a Linux row is built on, xcrun's SDK names, and the MSVC toolset. A
// family names a platform, so it must agree with the row it is written on.
inline constexpr std::string_view kLocatorFamilies[] = {
    "glibc", "musl", "macosx", "iphoneos", "iphonesimulator", "msvc",
};

constexpr bool is_locator_family(std::string_view f) {
    for (auto k : kLocatorFamilies) if (k == f) return true;
    return false;
}

constexpr bool is_apple_family(std::string_view f) {
    return f == "macosx" || f == "iphoneos" || f == "iphonesimulator";
}

// One statement of a base, wherever it was written.
//
// `text` is the canonical spelling every reader and every report uses:
// `xim:glibc@2.39`, `glibc@system`, `path:/abs/dir`, or empty for the empty
// form. A table `{ path = "…" }` arrives here already absolute, as
// `path:<dir>`, because a relative path is relative to the file that wrote it
// and only the reader of that file knows which file that was.
struct Statement {
    Form                     form = Form::Default;
    std::string              text;
    std::string              family;    // Locator: one of kLocatorFamilies
    std::string              version;   // Locator: "system" or a version
    std::filesystem::path    path;      // Path
    std::vector<std::string> provides;  // `mcpp:c-abi=<impl>` and the like
    // Who said so, in #755's vocabulary: `env`, `manifest`, `toolchain`,
    // `build-program`, `config`, `default`.
    std::string              originKind = "default";
    std::string              originFile;
    int                      originLine = 0;
    std::string              originKey;

    bool stated() const { return form != Form::Default; }
};

// The string grammar, shared by the manifest, `config.toml`, the environment
// and a toolchain phase. Errors name the accepted spellings.
//
//   ""                      Empty
//   path:<dir>              Path
//   <ns>:<name>[@<v>]       Package
//   <family>@system         Locator
//   glibc@<v> / musl@<v>    Package `xim:<family>@<v>` (SPEC-006 §2.3)
//   macosx|iphoneos|iphonesimulator|msvc@<v>   Locator with a version
inline std::expected<Statement, std::string> parse_text(std::string_view text) {
    Statement s;
    s.text = std::string(text);
    if (text.empty()) { s.form = Form::Empty; return s; }
    if (text.starts_with("path:")) {
        auto p = text.substr(5);
        if (p.empty())
            return std::unexpected(std::string("`path:` names no directory"));
        s.form = Form::Path;
        s.path = std::filesystem::path(std::string(p)).lexically_normal();
        s.text = "path:" + s.path.generic_string();
        return s;
    }
    const auto at = text.find('@');
    const auto colon = text.find(':');
    if (colon != std::string_view::npos && (at == std::string_view::npos || colon < at)) {
        // A package. Apple SDKs and the host's own C library are located, so
        // the package spelling of a locator names something that cannot exist.
        auto ns = text.substr(0, colon);
        auto rest = text.substr(colon + 1);
        auto name = rest.substr(0, rest.find('@'));
        if (ns.empty() || name.empty())
            return std::unexpected(std::format(
                "'{}' is not a package reference; expected `<namespace>:<name>[@<version>]`", text));
        if (is_apple_family(name))
            return std::unexpected(std::format(
                "'{}': an Apple SDK is located, not installed, so it has no package; "
                "write \"{}@system\", \"{}@<version>\" or {{ path = \"…\" }}", text, name, name));
        if (rest.ends_with("@system"))
            return std::unexpected(std::format(
                "'{}': `@system` is a locator and takes no namespace; write \"{}\"",
                text, rest));
        s.form = Form::Package;
        return s;
    }
    if (at != std::string_view::npos) {
        auto family = text.substr(0, at);
        auto version = text.substr(at + 1);
        if (is_locator_family(family) && !version.empty()) {
            if (version != "system" && (family == "glibc" || family == "musl")) {
                s.form = Form::Package;
                s.text = std::format("xim:{}", text);
                return s;
            }
            s.form = Form::Locator;
            s.family = std::string(family);
            s.version = std::string(version);
            return s;
        }
    }
    return std::unexpected(std::format(
        "'{}' is not a sysroot. A sysroot is a package (\"xim:glibc@2.39\"), a locator "
        "(\"glibc@system\", \"musl@system\", \"macosx@system\", \"iphoneos@18.5\", "
        "\"msvc@system\"), {{ path = \"<dir>\" }}, or \"\" for no prebuilt C library", text));
}

// ── The rows ────────────────────────────────────────────────────────────────
//
// WHICH SPELLINGS A ROW ACCEPTS IS DATA, keyed on the row's platform class.
// Every row of one class takes the same base; a table per canonical name
// would repeat one entry thirty times and miss the rows a manifest names
// without the target vocabulary knowing them.

constexpr std::uint8_t bit(Form f) { return static_cast<std::uint8_t>(1u << static_cast<unsigned>(f)); }

struct RowBase {
    Address          address = Address::None;
    std::uint8_t     forms   = 0;     // bit(Form) set
    std::string_view family;          // the row's locator family, or empty
    std::string_view accepted;        // the accepted spellings, for a refusal
};

inline RowBase row_base(const triple::Triple& t) {
    const auto pkg = bit(Form::Package), loc = bit(Form::Locator),
               path = bit(Form::Path), empty = bit(Form::Empty);
    if (t.has_own_sysroot())
        return {Address::DriverInternal, static_cast<std::uint8_t>(pkg | empty), {},
                "nothing new: the SDK toolchain carries its own system"};
    if (t.is_freestanding())
        return {Address::Multilib, static_cast<std::uint8_t>(pkg | path | empty), {},
                "a package (\"xim:picolibc-riscv@1.8.12\"), { path = \"<dir>\" } or \"\""};
    // The Apple rows took a package and "" before they took a locator, and
    // those spellings keep their meaning (§9).
    if (t.os == "macos")
        return {Address::AppleIsysroot, static_cast<std::uint8_t>(loc | path | pkg | empty), "macosx",
                "\"macosx@system\", \"macosx@<version>\" or { path = \"<dir>\" }"};
    if (t.is_ios_simulator())
        return {Address::AppleIsysroot, static_cast<std::uint8_t>(loc | path | pkg | empty), "iphonesimulator",
                "\"iphonesimulator@system\", \"iphonesimulator@<version>\" or { path = \"<dir>\" }"};
    if (t.is_ios())
        return {Address::AppleIsysroot, static_cast<std::uint8_t>(loc | path | pkg | empty), "iphoneos",
                "\"iphoneos@system\", \"iphoneos@<version>\" or { path = \"<dir>\" }"};
    if (t.os == "windows" && t.is_msvc_env())
        return {Address::MsvcRoots, static_cast<std::uint8_t>(pkg | loc | path), "msvc",
                "\"msvc@system\", \"msvc@<toolset>\", \"xim:msvc@<toolset>\" or, for clang, "
                "{ path = \"<dir>\" }"};
    if (t.os == "windows")
        return {Address::DriverInternal, static_cast<std::uint8_t>(pkg | empty), {},
                "a package or \"\": the MinGW toolchain carries its own system"};
    if (t.os == "linux" && t.is_musl())
        return {Address::GnuSysroot, static_cast<std::uint8_t>(pkg | loc | path | empty), "musl",
                "a package, \"musl@system\", { path = \"<dir>\" } or \"\""};
    if (t.os == "linux")
        return {Address::GnuSysroot, static_cast<std::uint8_t>(pkg | loc | path | empty), "glibc",
                "a package (\"xim:glibc@2.39\"), \"glibc@system\", { path = \"<dir>\" } or \"\""};
    return {Address::GnuSysroot, static_cast<std::uint8_t>(pkg | path | empty), {},
            "a package, { path = \"<dir>\" } or \"\""};
}

// The refusal for a statement this row cannot take, or nullopt.
inline std::optional<std::string> refusal(const triple::Triple& t, const Statement& s) {
    if (!s.stated()) return std::nullopt;
    const auto row = row_base(t);
    const auto shown = s.form == Form::Path
        ? std::format("{{ path = \"{}\" }}", s.path.generic_string())
        : std::format("\"{}\"", s.text);
    if ((row.forms & bit(s.form)) == 0)
        return std::format("[target.{}].sysroot = {}: this row takes {}",
                           t.str(), shown, row.accepted);
    if (s.form == Form::Locator && s.family != row.family)
        return std::format(
            "[target.{}].sysroot = {}: `{}` describes another platform; this row takes {}",
            t.str(), shown, s.family, row.accepted);
    if (row.address == Address::MsvcRoots && s.form == Form::Package
        && !s.text.starts_with("xim:msvc@"))
        return std::format("[target.{}].sysroot = {}: this row takes {}",
                           t.str(), shown, row.accepted);
    return std::nullopt;
}

// `{ path = "/" }` IS THE HOST'S C LIBRARY, AND IT HAS A NAME. Normalised to the
// row's `<family>@system` so that the two spellings of one statement cannot
// resolve differently.
inline Statement normalise(const triple::Triple& t, Statement s) {
    if (s.form != Form::Path || s.path != std::filesystem::path("/")) return s;
    const auto row = row_base(t);
    if (row.family != "glibc" && row.family != "musl") return s;
    s.form = Form::Locator;
    s.family = std::string(row.family);
    s.version = "system";
    s.text = s.family + "@system";
    s.path.clear();
    return s;
}

// ── Measurement data (§4.3) ────────────────────────────────────────────────
//
// THE IMPLEMENTATION AND ITS VERSION ARE ANSWERED BY THE PREPROCESSOR, never by
// a directory layout: a C library defines its own identity macros, and the
// driver that will compile against it is the one that reads them.

struct LibcFact {
    std::string impl;      // "glibc", "musl", "bionic", "picolibc", "newlib", "emscripten"
    std::string version;   // "2.31", or empty when the library does not say
    std::string factName;  // the key a report and a floor use
};

inline std::optional<LibcFact>
classify_macros(const std::map<std::string, std::string, std::less<>>& m) {
    auto get = [&](std::string_view k) -> std::string {
        auto it = m.find(k);
        return it == m.end() ? std::string{} : it->second;
    };
    if (m.contains("__GLIBC__"))
        return LibcFact{"glibc", std::format("{}.{}", get("__GLIBC__"), get("__GLIBC_MINOR__")), "glibc"};
    if (m.contains("__BIONIC__"))
        return LibcFact{"bionic", get("__ANDROID_API__"), "android.api"};
    if (m.contains("__PICOLIBC__")) {
        auto v = get("__PICOLIBC_VERSION__");
        if (v.size() >= 2 && v.front() == '"') v = v.substr(1, v.size() - 2);
        return LibcFact{"picolibc", v, "picolibc"};
    }
    if (m.contains("_NEWLIB_VERSION")) {
        auto v = get("_NEWLIB_VERSION");
        if (v.size() >= 2 && v.front() == '"') v = v.substr(1, v.size() - 2);
        return LibcFact{"newlib", v, "newlib"};
    }
    if (m.contains("__EMSCRIPTEN__"))
        return LibcFact{"emscripten",
                        std::format("{}.{}.{}", get("__EMSCRIPTEN_major__"),
                                    get("__EMSCRIPTEN_minor__"), get("__EMSCRIPTEN_tiny__")),
                        "emscripten"};
    return std::nullopt;
}

// `#define NAME VALUE` lines, as `-E -dM` prints them.
inline std::map<std::string, std::string, std::less<>> parse_defines(std::string_view out) {
    std::map<std::string, std::string, std::less<>> m;
    std::size_t pos = 0;
    while (pos < out.size()) {
        auto end = out.find('\n', pos);
        auto line = out.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
        pos = end == std::string_view::npos ? out.size() : end + 1;
        constexpr std::string_view kDef = "#define ";
        if (!line.starts_with(kDef)) continue;
        line.remove_prefix(kDef.size());
        auto sp = line.find(' ');
        auto name = line.substr(0, sp);
        if (name.find('(') != std::string_view::npos) continue;   // function-like
        auto value = sp == std::string_view::npos ? std::string_view{} : line.substr(sp + 1);
        while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) value.remove_suffix(1);
        m.emplace(std::string(name), std::string(value));
    }
    return m;
}

// THE DEBIAN MULTIARCH TUPLE, from Debian's MultiarchTuples table. This is the
// one piece of layout knowledge the engine carries (D10): it is specified, and
// a gcc built without multiarch support cannot find `crt1.o` in a Debian or
// Ubuntu root without it (measured, scenario J). Empty for an unknown arch.
inline std::string multiarch_tuple(const triple::Triple& t) {
    const bool musl = t.is_musl();
    struct Row { std::string_view arch, gnu, musl; };
    static constexpr Row kRows[] = {
        {"x86_64",      "x86_64-linux-gnu",      "x86_64-linux-musl"},
        {"aarch64",     "aarch64-linux-gnu",     "aarch64-linux-musl"},
        {"i686",        "i386-linux-gnu",        "i386-linux-musl"},
        {"i386",        "i386-linux-gnu",        "i386-linux-musl"},
        {"riscv64",     "riscv64-linux-gnu",     "riscv64-linux-musl"},
        {"loongarch64", "loongarch64-linux-gnu", "loongarch64-linux-musl"},
        {"powerpc64le", "powerpc64le-linux-gnu", "powerpc64le-linux-musl"},
        {"s390x",       "s390x-linux-gnu",       "s390x-linux-musl"},
        {"armv7",       "arm-linux-gnueabihf",   "arm-linux-musleabihf"},
    };
    for (auto const& r : kRows)
        if (t.arch == r.arch) return std::string(musl ? r.musl : r.gnu);
    return {};
}

// Dotted numeric versions, compared numerically: "2.9" < "2.31". Anything
// that is not a number compares as 0, so a version that cannot be read never
// produces a refusal on its own.
inline int compare_versions(std::string_view a, std::string_view b) {
    auto next = [](std::string_view& s) -> unsigned {
        unsigned v = 0;
        std::size_t i = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') { v = v * 10 + unsigned(s[i] - '0'); ++i; }
        while (i < s.size() && s[i] != '.') ++i;
        s.remove_prefix(i < s.size() ? i + 1 : i);
        return v;
    };
    while (!a.empty() || !b.empty()) {
        auto x = next(a), y = next(b);
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

// The highest `GLIBC_x.y` a list of version needs names, or empty.
inline std::string highest_glibc(const std::vector<std::string>& needs) {
    std::string best;
    for (auto const& n : needs) {
        if (!n.starts_with("GLIBC_")) continue;
        auto v = n.substr(6);
        if (v.empty() || !(v[0] >= '0' && v[0] <= '9')) continue;
        if (best.empty() || compare_versions(v, best) > 0) best = std::move(v);
    }
    return best;
}

// ── The resolution ────────────────────────────────────────────────────────

struct MsvcRoots {
    std::filesystem::path toolsDir;         // <vs>/VC/Tools/MSVC/<v>
    std::string           toolsVersion;     // "14.44.35207"
    std::string           compilerVersion;  // cl.exe's file version (mcpp#746)
    std::string           origin;           // "system" | "managed" | "path"
    std::string           product;          // for the one printed line
    std::filesystem::path sdkRoot;          // <kits>/10 or the payload
    std::string           sdkVersion;       // "10.0.26100.0"
    std::filesystem::path redistDir;        // the toolset's redistributable CRT
};

struct AppleSdk {
    std::filesystem::path root;         // the located SDK
    std::string           name;         // "iphoneos"
    std::string           version;      // "18.5", from SDKSettings.json or xcrun
    bool                  cxxHeaders = false;   // the C++ headers are the SDK's
};

// A toolchain-supplied glibc in the ecosystem's payload layout: headers and
// libraries in separate payloads, assembled onto the command line by hand.
struct PayloadPaths {
    std::filesystem::path glibcInclude;     // glibc headers (features.h, bits/)
    std::filesystem::path glibcLib;          // glibc runtime (libc.so, crt*.o, ld-linux)
    std::filesystem::path linuxInclude;      // linux kernel headers (linux/, asm/)
};

// THE ONE ANSWER. Every reader of "which sysroot" reads a field of this — the
// compile and link renderers, the std module, the scanner, the artifact's
// runtime, closure validation, hermeticity, the fingerprint, reports, pack and
// the runner. Defaults describe the ecosystem's base, so a build that states
// nothing renders exactly what it rendered before this existed.
struct Resolution {
    Form                   form = Form::Default;
    Address                address = Address::None;
    Runtime                runtime = Runtime::None;
    std::string            text;          // the statement's canonical spelling
    // The directory a gnu-sysroot address hands the driver: the stated root,
    // or the root a toolchain was built with (`-print-sysroot`).
    std::filesystem::path  root;
    // The ecosystem's glibc in payload layout (the default base on Linux).
    std::optional<PayloadPaths> payload;
    // A bare-metal C library in multilib layout.
    std::filesystem::path  multilibRoot;
    std::filesystem::path  multilibInclude;
    std::filesystem::path  multilibLib;
    std::string            package;       // its package name (`picolibc-riscv`)
    AppleSdk               apple;
    MsvcRoots              msvc;
    // Measured (§4.3).
    std::string            libc;          // "glibc", "musl", …
    std::string            libcVersion;   // "2.31"
    bool                   multiarch = false;
    std::string            multiarchTuple;
    // What a binding artifact loads: the base's dynamic loader and the
    // directories its RUNPATH names.
    std::filesystem::path  loader;
    std::vector<std::filesystem::path> runtimeDirs;
    // Kernel headers the base lacks, supplied by the engine (§2.4, additive).
    std::vector<std::filesystem::path> addedIncludes;
    // Content identity: enters the runtime contract and so the fingerprint.
    // Empty for the ecosystem default, whose identity is the contract itself.
    std::string            identity;

    bool stated() const { return form != Form::Default; }
};

} // namespace mcpp::toolchain::base

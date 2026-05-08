// mcpp.manifest — load and validate mcpp.toml

export module mcpp.manifest;

import std;
import mcpp.libs.toml;

export namespace mcpp::manifest {

struct Package {
    std::string                 name;
    std::string                 version;
    std::string                 standard   = "c++23";   // C++ standard (M5.0: moved from [language])
    std::string                 description;
    std::string                 license;
    std::vector<std::string>    authors;
    std::string                 repo;
};

struct Language {
    std::string                 standard   = "c++23";
    bool                        modules    = true;
    bool                        importStd = true;
};

struct Modules {
    std::vector<std::string>    sources;        // glob patterns
    std::vector<std::string>    exports_;       // declared module names (optional)
    bool                        strict = false;
};

struct Target {
    std::string                 name;
    enum Kind { Library, Binary, SharedLibrary, TestBinary } kind;
    std::string                 main;           // for binary / test
};

// One declared dependency. Path-based deps refer to a sibling mcpp package
// on disk; version-based deps (M2 future) come from a registry.
struct DependencySpec {
    std::string                 version;        // "0.0.1" / "^1.2" / "" (req string)
    std::string                 path;           // filesystem path, or empty
    std::string                 git;            // "https://..." or empty
    std::string                 gitRev;         // commit / tag / branch (any one)
    std::string                 gitRefKind;     // "rev" / "tag" / "branch" (for clarity)
    bool isPath()    const { return !path.empty(); }
    bool isGit()     const { return !git.empty(); }
    bool isVersion() const { return !isPath() && !isGit() && !version.empty(); }
};

// `[toolchain]` section per docs/21-toolchain-and-tools.md
//   linux   = "gcc@15.1.0"
//   macos   = "llvm@20"
//   windows = "msvc@system"
//   default = "gcc@15.1.0"   (used when current platform isn't listed)
struct Toolchain {
    std::map<std::string, std::string> byPlatform;   // platform -> "pkg@ver"

    // Returns the toolchain spec for a platform, falling back to "default".
    std::optional<std::string> for_platform(std::string_view platform) const {
        if (auto it = byPlatform.find(std::string(platform)); it != byPlatform.end()) {
            return it->second;
        }
        if (auto it = byPlatform.find("default"); it != byPlatform.end()) {
            return it->second;
        }
        return std::nullopt;
    }
};

// `[build]` section — tunables for the build backend.
//
// M5.0: now also carries `sources` (moved from [modules]) and `include_dirs`
// (new). Defaults are injected by load() after parse if these are empty.
struct BuildConfig {
    std::vector<std::string>           sources;        // glob patterns
    std::vector<std::filesystem::path> includeDirs;    // relative to package root
    bool                                staticStdlib = true;
    // "" (default = dynamic), "static", "dynamic" — chosen at resolve
    // time from --static / --target / [target.<triple>].linkage. Wired
    // through to ninja backend as the `-static` link flag.
    std::string                         linkage;
    // M5.x C-language support. `cflags` / `cxxflags` are appended verbatim
    // to the per-rule baseline (see `ninja_backend` cflags / cxxflags).
    // `cStandard` controls -std= for the C compile rule (.c files).
    // Empty cStandard → backend default ("c11" today).
    std::vector<std::string>           cflags;
    std::vector<std::string>           cxxflags;
    std::string                         cStandard;
};

// `[target.<triple>]` — per-target overrides.
// Picked up when caller passes --target <triple> to build/run/test.
struct TargetEntry {
    std::string                         toolchain;     // e.g. "gcc@15.1.0-musl"; empty = inherit [toolchain]
    std::string                         linkage;       // "static" | "dynamic" | "" (= auto by libc)
};

// `[pack]` — `mcpp pack` configuration. See docs/35-pack-design.md.
//
// `default_mode` picks the bundling strategy when the user runs bare
// `mcpp pack` (no `--mode` flag):
//   "static"          — full musl static, no PT_INTERP / RUNPATH
//   "bundle-project"  — bundle only project's third-party .so (default)
//   "bundle-all"      — bundle every dynamic dep including libc / libstdc++
struct PackConfig {
    std::string                         defaultMode;   // empty → "bundle-project"
    std::vector<std::string>            include;       // extra files/globs to ship
    std::vector<std::string>            exclude;       // patterns to drop from include
    // Mode C overrides — let the user expand or contract the PEP 600
    // skip list when their target distros differ from the default
    // assumption ("modern desktop Linux").
    std::vector<std::string>            alsoSkip;      // libs to ALSO skip on top of PEP 600
    std::vector<std::string>            forceBundle;   // libs to bundle even if PEP 600 says skip
};

struct Manifest {
    std::filesystem::path       sourcePath;    // mcpp.toml's filesystem path

    Package                     package;
    Language                    language;
    Modules                     modules;
    std::vector<Target>         targets;

    // version-string keyed dependencies (M2 short form only).
    std::map<std::string, DependencySpec> dependencies;
    std::map<std::string, DependencySpec> devDependencies;
    std::map<std::string, DependencySpec> buildDependencies;   // host-side tools (M5+ behavior)

    Toolchain                   toolchain;     // optional; empty == fallback
    BuildConfig                 buildConfig;

    // [target.<triple>] tables — empty if user didn't declare any.
    // Triple keys are accepted in either GCC form (x86_64-linux-musl)
    // or Rust form (x86_64-unknown-linux-musl); both are normalised by
    // stripping `-unknown-` on read.
    std::map<std::string, TargetEntry> targetOverrides;

    // [pack] — `mcpp pack` config (see docs/35-pack-design.md).
    PackConfig                         packConfig;

    // M5.0: post-parse computed/inferred state
    bool                        usesModules    = true;   // refined by scanner
    bool                        usesImportStd  = true;   // refined by scanner
    std::vector<std::string>    inferredNotes;           // for `Inferred ...` banner
};

struct ManifestError {
    std::string                 message;
    std::filesystem::path       file;
    std::size_t                 line   = 0;
    std::size_t                 column = 0;

    std::string format() const {
        if (line)
            return std::format("{}:{}:{}: error: {}", file.string(), line, column, message);
        return std::format("{}: error: {}", file.string(), message);
    }
};

std::expected<Manifest, ManifestError> parse_string(std::string_view content,
                                                    const std::filesystem::path& origin = "mcpp.toml");
std::expected<Manifest, ManifestError> load(const std::filesystem::path& path);

// For `mcpp new` scaffolding.
std::string default_template(std::string_view packageName);

// M6.x: `mcpp` field in xpkg.lua may be either:
//   - a string (path to mcpp.toml inside the extracted tarball, glob-able)
//   - a table (inline Form B descriptor)
// extract_mcpp_field discriminates and returns the right kind.
struct McppField {
    enum Kind { Absent, StringPath, TableBody } kind = Absent;
    std::string                 value;   // glob path (StringPath) or table body (TableBody)
};
McppField extract_mcpp_field(std::string_view luaContent);

// Extract the list of available versions for `platform` (e.g. "linux", "macosx",
// "windows") from an xpkg .lua's xpm.<platform> = { ["X.Y.Z"] = {...}, ... }.
// Returns an empty vector if the platform table is missing or has no entries.
std::vector<std::string>
list_xpkg_versions(std::string_view luaContent, std::string_view platform);

// Synthesize a Manifest from an xpkg .lua file's `mcpp = {}` segment.
// Used when a fetched dep has no source/mcpp.toml — the index entry's
// `mcpp = {}` workaround block carries the missing build info.
//
// The resulting Manifest is in-memory only; sourcePath is set to the
// supplied package name + version so error messages can refer to it.
std::expected<Manifest, ManifestError>
synthesize_from_xpkg_lua(std::string_view luaContent,
                         std::string_view packageName,
                         std::string_view packageVersion);

} // namespace mcpp::manifest

// =====================================================================
// Implementation
// =====================================================================

namespace mcpp::manifest {

namespace t = mcpp::libs::toml;

namespace {

ManifestError error(const std::filesystem::path& origin,
                    const std::string& msg,
                    t::Position pos = {0, 0}) {
    return ManifestError{msg, origin, pos.line, pos.column};
}

} // namespace

std::expected<Manifest, ManifestError> parse_string(std::string_view content,
                                                    const std::filesystem::path& origin) {
    auto doc = t::parse(content);
    if (!doc) {
        return std::unexpected(error(origin, doc.error().message, doc.error().where));
    }

    Manifest m;
    m.sourcePath = origin;

    // [package]
    auto* pkg_t = doc->get_table("package");
    if (!pkg_t) return std::unexpected(error(origin, "missing required [package] section"));

    auto name = doc->get_string("package.name");
    if (!name) return std::unexpected(error(origin, "missing required field 'package.name'"));
    m.package.name = *name;

    auto version = doc->get_string("package.version");
    if (!version) return std::unexpected(error(origin, "missing required field 'package.version'"));
    m.package.version = *version;

    if (auto v = doc->get_string("package.description")) m.package.description = *v;
    if (auto v = doc->get_string("package.license"))     m.package.license     = *v;
    if (auto v = doc->get_string("package.repo"))        m.package.repo        = *v;
    if (auto v = doc->get_string_array("package.authors")) m.package.authors  = *v;

    // [package].standard (M5.0 new home)
    if (auto v = doc->get_string("package.standard"))    m.package.standard    = *v;

    // [language] (M5.0: deprecated, kept for backward compat — drop in M6)
    // Reads to old fields AND mirrors to new package.standard if [package].standard not set.
    bool had_language_section = (doc->get_table("language") != nullptr);
    if (auto v = doc->get_string("language.standard")) {
        m.language.standard = *v;
        // mirror to new home only if [package].standard wasn't explicitly set
        if (!doc->get_string("package.standard")) m.package.standard = *v;
    } else {
        m.language.standard = m.package.standard;   // keep old field consistent with new
    }
    if (auto v = doc->get_bool("language.modules"))      m.language.modules    = *v;
    if (auto v = doc->get_bool("language.import_std"))   m.language.importStd = *v;

    // Validation on the unified standard
    if (m.package.standard != "c++23" && m.package.standard != "c++latest") {
        return std::unexpected(error(origin,
            std::format("MVP only supports c++23; got '{}'", m.package.standard)));
    }
    if (had_language_section && !m.language.modules) {
        return std::unexpected(error(origin,
            "language.modules must be true (mcpp is modules-only)"));
    }

    // [build].sources (M5.0 new home) + [modules].sources (deprecated, compat)
    if (auto v = doc->get_string_array("build.sources"))   m.buildConfig.sources = *v;
    if (auto v = doc->get_string_array("modules.sources")) {
        m.modules.sources = *v;
        // If [build].sources wasn't set, mirror legacy field into new field.
        if (m.buildConfig.sources.empty()) m.buildConfig.sources = *v;
    }
    // Mirror new → legacy so existing code reading manifest.modules.sources keeps working.
    if (m.modules.sources.empty()) m.modules.sources = m.buildConfig.sources;

    if (auto v = doc->get_string_array("modules.exports")) m.modules.exports_ = *v;
    if (auto v = doc->get_bool("modules.strict"))          m.modules.strict   = *v;

    // [build].include_dirs (M5.0 new field)
    if (auto v = doc->get_string_array("build.include_dirs")) {
        for (auto& s : *v) m.buildConfig.includeDirs.emplace_back(s);
    }

    // [targets.*] — M5.0: now optional. If absent, defer to auto-inference (in load()).
    auto* targets_table = doc->get_table("targets");
    if (targets_table && !targets_table->empty()) {
    for (auto& [tname, tval] : *targets_table) {
        if (!tval.is_table()) {
            return std::unexpected(error(origin,
                std::format("[targets.{}] must be a table", tname)));
        }
        Target t;
        t.name = tname;
        auto& tt = tval.as_table();

        auto kit = tt.find("kind");
        if (kit == tt.end() || !kit->second.is_string()) {
            return std::unexpected(error(origin,
                std::format("targets.{}.kind missing or not a string", tname)));
        }
        const auto& kind_s = kit->second.as_string();
        if      (kind_s == "lib"    || kind_s == "library")  t.kind = Target::Library;
        else if (kind_s == "bin"    || kind_s == "binary")   t.kind = Target::Binary;
        else if (kind_s == "shared" || kind_s == "dylib"
              || kind_s == "so"     || kind_s == "shlib")    t.kind = Target::SharedLibrary;
        else return std::unexpected(error(origin,
            std::format("targets.{}.kind must be 'bin', 'lib' or 'shared'; got '{}'", tname, kind_s)));

        if (t.kind == Target::Binary) {
            auto mit = tt.find("main");
            if (mit == tt.end() || !mit->second.is_string()) {
                return std::unexpected(error(origin,
                    std::format("targets.{} (kind=bin) requires 'main' field", tname)));
            }
            t.main = mit->second.as_string();
        }
        m.targets.push_back(std::move(t));
    }
    } // close `if (targets_table && !targets_table->empty())`

    // [dependencies] / [dev-dependencies]
    auto load_deps = [&](std::string_view section, std::map<std::string, DependencySpec>& out)
        -> std::expected<void, ManifestError>
    {
        auto* tt = doc->get_table(section);
        if (!tt) return {};
        for (auto& [k, v] : *tt) {
            DependencySpec spec;
            if (v.is_string()) {
                spec.version = v.as_string();
            } else if (v.is_table()) {
                auto& sub = v.as_table();
                if (auto it = sub.find("path");    it != sub.end() && it->second.is_string()) spec.path    = it->second.as_string();
                if (auto it = sub.find("version"); it != sub.end() && it->second.is_string()) spec.version = it->second.as_string();
                if (auto it = sub.find("git");     it != sub.end() && it->second.is_string()) spec.git     = it->second.as_string();
                if (auto it = sub.find("rev");     it != sub.end() && it->second.is_string()) {
                    spec.gitRev     = it->second.as_string();
                    spec.gitRefKind = "rev";
                } else if (auto it = sub.find("tag");    it != sub.end() && it->second.is_string()) {
                    spec.gitRev     = it->second.as_string();
                    spec.gitRefKind = "tag";
                } else if (auto it = sub.find("branch"); it != sub.end() && it->second.is_string()) {
                    spec.gitRev     = it->second.as_string();
                    spec.gitRefKind = "branch";
                }
                if (spec.path.empty() && spec.version.empty() && spec.git.empty()) {
                    return std::unexpected(error(origin,
                        std::format("[{}.\"{}\"] must specify 'path', 'version', or 'git'", section, k)));
                }
                if (!spec.git.empty() && spec.gitRev.empty()) {
                    return std::unexpected(error(origin,
                        std::format("[{}.\"{}\"] git dep requires one of: rev / tag / branch", section, k)));
                }
            } else {
                return std::unexpected(error(origin,
                    std::format("[{}].{} must be a string (version) or table (path/version)", section, k)));
            }
            out[k] = std::move(spec);
        }
        return {};
    };
    if (auto r = load_deps("dependencies",       m.dependencies);       !r) return std::unexpected(r.error());
    if (auto r = load_deps("dev-dependencies",   m.devDependencies);    !r) return std::unexpected(r.error());
    if (auto r = load_deps("build-dependencies", m.buildDependencies);  !r) return std::unexpected(r.error());

    // [toolchain] — platform → "pkg@version" map (docs/21)
    if (auto* tt = doc->get_table("toolchain")) {
        for (auto& [platform, val] : *tt) {
            if (!val.is_string()) {
                return std::unexpected(error(origin,
                    std::format("[toolchain].{} must be a string like \"gcc@15.1.0\"", platform)));
            }
            m.toolchain.byPlatform[platform] = val.as_string();
        }
    }

    // [build] — backend tunables
    if (auto v = doc->get_bool("build.static_stdlib")) m.buildConfig.staticStdlib = *v;
    if (auto v = doc->get_string_array("build.cflags"))   m.buildConfig.cflags   = *v;
    if (auto v = doc->get_string_array("build.cxxflags")) m.buildConfig.cxxflags = *v;
    if (auto v = doc->get_string("build.c_standard"))     m.buildConfig.cStandard = *v;

    // [pack] — `mcpp pack` configuration. See docs/35-pack-design.md.
    if (auto v = doc->get_string("pack.default_mode")) {
        const auto& s = *v;
        if (s != "static" && s != "bundle-project" && s != "bundle-all") {
            return std::unexpected(error(origin, std::format(
                "[pack].default_mode = '{}' invalid; expected "
                "'static' | 'bundle-project' | 'bundle-all'", s)));
        }
        m.packConfig.defaultMode = s;
    }
    if (auto v = doc->get_string_array("pack.include"))
        m.packConfig.include = *v;
    if (auto v = doc->get_string_array("pack.exclude"))
        m.packConfig.exclude = *v;
    // [pack.bundle-project] sub-table for fine-grained PEP 600 overrides.
    if (auto v = doc->get_string_array("pack.bundle-project.also_skip"))
        m.packConfig.alsoSkip = *v;
    if (auto v = doc->get_string_array("pack.bundle-project.force_bundle"))
        m.packConfig.forceBundle = *v;

    // [target.<triple>] — per-target overrides. We accept both GCC
    // (x86_64-linux-musl) and Rust-style (x86_64-unknown-linux-musl)
    // triple forms; the latter is canonicalised by stripping the
    // `-unknown-` segment so both keys map to the same entry.
    auto canon_triple = [](std::string s) {
        constexpr std::string_view kUnknown = "-unknown-";
        if (auto p = s.find(kUnknown); p != std::string::npos)
            s.replace(p, kUnknown.size(), "-");
        return s;
    };
    if (auto* tt = doc->get_table("target")) {
        for (auto& [triple, val] : *tt) {
            if (!val.is_table()) continue;
            auto& body = val.as_table();
            TargetEntry e;
            if (auto it = body.find("toolchain"); it != body.end() && it->second.is_string())
                e.toolchain = it->second.as_string();
            if (auto it = body.find("linkage"); it != body.end() && it->second.is_string()) {
                e.linkage = it->second.as_string();
                if (e.linkage != "static" && e.linkage != "dynamic") {
                    return std::unexpected(error(origin, std::format(
                        "[target.{}].linkage = '{}' is invalid; expected 'static' or 'dynamic'",
                        triple, e.linkage)));
                }
            }
            m.targetOverrides[canon_triple(triple)] = std::move(e);
        }
    }

    return m;
}

// M5.0: inject defaults and auto-infer targets when fields are absent.
// Mutates manifest in-place; called from load() with the project root.
namespace {

void apply_defaults_and_infer(Manifest& m, const std::filesystem::path& root) {
    // Default sources glob (covers .cppm/.cpp/.cc/.c under src/).
    if (m.buildConfig.sources.empty()) {
        m.buildConfig.sources = {
            "src/**/*.cppm",
            "src/**/*.cpp",
            "src/**/*.cc",
            "src/**/*.c",
        };
        m.modules.sources = m.buildConfig.sources;   // legacy mirror
        m.inferredNotes.push_back("sources [src/**/*.{cppm,cpp,cc,c}]");
    }

    // Default include_dirs: ["include"] iff <root>/include/ exists.
    if (m.buildConfig.includeDirs.empty()) {
        std::error_code ec;
        if (std::filesystem::is_directory(root / "include", ec)) {
            m.buildConfig.includeDirs.push_back("include");
            m.inferredNotes.push_back("include_dirs [include]");
        }
    }

    // Auto-target inference (only when no [targets] declared).
    if (m.targets.empty()) {
        std::error_code ec;
        auto mainCpp = root / "src" / "main.cpp";
        bool hasMain   = std::filesystem::exists(mainCpp, ec);

        bool hasCppm = false;
        if (std::filesystem::is_directory(root / "src", ec)) {
            for (auto& e : std::filesystem::recursive_directory_iterator(root / "src", ec)) {
                if (ec) break;
                if (e.is_regular_file(ec) && !ec
                    && e.path().extension() == ".cppm") {
                    hasCppm = true; break;
                }
            }
        }

        if (hasMain) {
            Target t;
            t.name = m.package.name;
            t.kind = Target::Binary;
            t.main = "src/main.cpp";
            m.targets.push_back(std::move(t));
            m.inferredNotes.push_back(
                std::format("target {} (bin from src/main.cpp)", m.package.name));
        } else if (hasCppm) {
            Target t;
            t.name = m.package.name;
            t.kind = Target::Library;
            m.targets.push_back(std::move(t));
            m.inferredNotes.push_back(
                std::format("target {} (lib from .cppm in src/)", m.package.name));
        }
        // If neither, no auto-target — caller will error if it needs one.
    }
}

} // namespace

std::expected<Manifest, ManifestError> load(const std::filesystem::path& path) {
    std::ifstream is(path);
    if (!is) {
        return std::unexpected(ManifestError{
            std::format("cannot open '{}'", path.string()),
            path, 0, 0});
    }
    std::stringstream ss;
    ss << is.rdbuf();
    auto m = parse_string(ss.str(), path);
    if (!m) return m;

    // M5.0: defaults + target inference (uses filesystem context relative to mcpp.toml).
    apply_defaults_and_infer(*m, path.parent_path());
    return m;
}

// =====================================================================
//  synthesize_from_xpkg_lua — parse mcpp = {} segment from an xpkg .lua
// =====================================================================
//
//  Scope: tiny Lua-subset reader specialised for our `mcpp = { ... }`
//  workaround block. We don't run real Lua; we just locate the mcpp
//  table and read a short list of typed fields out of it.

namespace {

struct LuaCursor {
    std::string_view text;
    std::size_t      pos = 0;

    bool eof() const { return pos >= text.size(); }
    char peek() const { return pos < text.size() ? text[pos] : '\0'; }

    void skip_ws_and_comments() {
        while (!eof()) {
            char c = peek();
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',' || c == ';') {
                ++pos;
            } else if (c == '-' && pos + 1 < text.size() && text[pos + 1] == '-') {
                while (!eof() && peek() != '\n') ++pos;
            } else {
                break;
            }
        }
    }

    bool consume(char c) {
        skip_ws_and_comments();
        if (peek() == c) { ++pos; return true; }
        return false;
    }

    std::string read_string() {
        skip_ws_and_comments();
        if (peek() != '"' && peek() != '\'') return {};
        char q = text[pos++];
        std::string out;
        while (!eof() && peek() != q) {
            if (peek() == '\\' && pos + 1 < text.size()) {
                ++pos;
                char e = text[pos++];
                switch (e) {
                    case 'n':  out.push_back('\n'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'r':  out.push_back('\r'); break;
                    case '"':  out.push_back('"');  break;
                    case '\'': out.push_back('\''); break;
                    case '\\': out.push_back('\\'); break;
                    default:   out.push_back(e);
                }
            } else {
                out.push_back(text[pos++]);
            }
        }
        if (!eof()) ++pos;     // closing quote
        return out;
    }

    std::string read_ident() {
        skip_ws_and_comments();
        std::string out;
        while (!eof() && (std::isalnum(static_cast<unsigned char>(peek())) ||
                          peek() == '_'))
        {
            out.push_back(text[pos++]);
        }
        return out;
    }

    // Read either a bare ident or `["string"]`.
    std::string read_key() {
        skip_ws_and_comments();
        if (peek() == '[') {
            ++pos;
            auto s = read_string();
            skip_ws_and_comments();
            if (peek() == ']') ++pos;
            return s;
        }
        return read_ident();
    }

    // Read a Lua barewordy value: number/true/false/nil up to delimiter.
    std::string read_bareword() {
        skip_ws_and_comments();
        std::string out;
        while (!eof() && !std::isspace(static_cast<unsigned char>(peek())) &&
               peek() != ',' && peek() != '}' && peek() != ';')
        {
            out.push_back(text[pos++]);
        }
        return out;
    }

    // Skip an entire balanced { ... } block, string-aware.
    void skip_table() {
        if (!consume('{')) return;
        int depth = 1;
        while (!eof() && depth > 0) {
            char c = peek();
            if (c == '"' || c == '\'') {
                read_string();
                continue;
            } else if (c == '-' && pos + 1 < text.size() && text[pos+1] == '-') {
                while (!eof() && peek() != '\n') ++pos;
                continue;
            } else if (c == '{') { ++depth; ++pos; }
            else if (c == '}') { --depth; ++pos; }
            else { ++pos; }
        }
    }
};

// Strip Lua line comments (`-- ...\n`) and string contents from text,
// replacing them with spaces of the same length so positions are
// preserved. This is a simple-but-correct way to make the scanner
// in extract_mcpp_segment_body() ignore comments and strings without
// re-implementing a full Lua tokenizer.
std::string strip_lua_comments_and_strings(std::string_view text) {
    std::string out(text.size(), ' ');
    std::size_t i = 0;
    while (i < text.size()) {
        char c = text[i];
        // Line comment
        if (c == '-' && i + 1 < text.size() && text[i+1] == '-') {
            while (i < text.size() && text[i] != '\n') {
                // keep newlines for line-number fidelity
                out[i] = (text[i] == '\n') ? '\n' : ' ';
                ++i;
            }
            continue;
        }
        // String literal
        if (c == '"' || c == '\'') {
            char q = c;
            out[i] = c;        // keep opening quote so structure-aware search still sees it
            ++i;
            while (i < text.size() && text[i] != q) {
                if (text[i] == '\\' && i + 1 < text.size()) {
                    out[i]   = ' ';
                    out[i+1] = ' ';
                    i += 2;
                    continue;
                }
                out[i] = (text[i] == '\n') ? '\n' : ' ';
                ++i;
            }
            if (i < text.size()) {
                out[i] = q;     // closing quote
                ++i;
            }
            continue;
        }
        out[i] = c;
        ++i;
    }
    return out;
}

// Locate the body of `mcpp = { ... }` and return the inner content (no
// surrounding braces). Returns empty string if not found.
// M6.x: locate the `mcpp = ...` field at top level of an xpkg.lua and
// classify it as either a table body or a string path. Operates on a
// comment-/string-stripped copy so literal "mcpp = ..." inside Lua
// comments doesn't false-match.
McppField extract_mcpp_field_impl(std::string_view raw_text) {
    auto sanitized = strip_lua_comments_and_strings(raw_text);
    std::string_view text { sanitized };

    std::size_t p = 0;
    while ((p = text.find("mcpp", p)) != std::string_view::npos) {
        bool word_start = (p == 0 || (!std::isalnum(static_cast<unsigned char>(text[p-1]))
                                       && text[p-1] != '_'));
        if (!word_start) { ++p; continue; }
        std::size_t q = p + 4;
        if (q < text.size() && (std::isalnum(static_cast<unsigned char>(text[q])) ||
                                text[q] == '_')) { ++p; continue; }
        while (q < text.size() && (text[q] == ' ' || text[q] == '\t')) ++q;
        if (q >= text.size() || text[q] != '=') { ++p; continue; }
        ++q;
        while (q < text.size() && (text[q] == ' ' || text[q] == '\t' ||
                                    text[q] == '\n' || text[q] == '\r')) ++q;
        if (q >= text.size()) { ++p; continue; }

        // Discriminate: { → table body, " → string path
        if (text[q] == '{') {
            ++q;
            std::size_t body_start = q;
            int depth = 1;
            while (q < text.size() && depth > 0) {
                char c = text[q];
                if (c == '{') ++depth;
                else if (c == '}') {
                    --depth;
                    if (depth == 0) {
                        return McppField{
                            McppField::TableBody,
                            std::string(raw_text.substr(body_start, q - body_start))};
                    }
                }
                ++q;
            }
            return {};
        }
        if (text[q] == '"') {
            // string literal — but the sanitizer blanks string contents, so
            // re-locate the same `"..."` in raw_text and take its body.
            // Find the opening `"` at offset q in raw_text (offsets align
            // because sanitizer keeps positions).
            std::size_t s = q;
            if (s >= raw_text.size() || raw_text[s] != '"') { ++p; continue; }
            ++s;
            std::string val;
            while (s < raw_text.size() && raw_text[s] != '"') {
                if (raw_text[s] == '\\' && s + 1 < raw_text.size()) {
                    char nc = raw_text[s + 1];
                    switch (nc) {
                        case 'n': val.push_back('\n'); break;
                        case 't': val.push_back('\t'); break;
                        case '"': val.push_back('"');  break;
                        case '\\': val.push_back('\\'); break;
                        default: val.push_back(nc);
                    }
                    s += 2;
                } else {
                    val.push_back(raw_text[s++]);
                }
            }
            return McppField{ McppField::StringPath, std::move(val) };
        }
        ++p;
    }
    return {};
}

// Backward-compat: old API; prefer extract_mcpp_field for new callers.
std::string extract_mcpp_segment_body(std::string_view raw_text) {
    auto f = extract_mcpp_field_impl(raw_text);
    return f.kind == McppField::TableBody ? std::move(f.value) : std::string{};
}

} // namespace

McppField extract_mcpp_field(std::string_view luaContent) {
    return extract_mcpp_field_impl(luaContent);
}

std::vector<std::string>
list_xpkg_versions(std::string_view luaContent, std::string_view platform) {
    // Locate `xpm = { ... <platform> = { ["X.Y.Z"] = {...}, ... } ... }`.
    // We work on a sanitized copy so quoted version keys remain locatable
    // by their offsets in the original text.
    auto sanitized = strip_lua_comments_and_strings(luaContent);
    std::string_view text { sanitized };
    std::vector<std::string> versions;

    auto find_word_at_lhs = [&](std::string_view name, std::size_t from)
        -> std::size_t
    {
        std::size_t p = from;
        while ((p = text.find(name, p)) != std::string_view::npos) {
            bool word_start = (p == 0 ||
                (!std::isalnum(static_cast<unsigned char>(text[p-1])) && text[p-1] != '_'));
            std::size_t after = p + name.size();
            bool word_end = (after >= text.size() ||
                (!std::isalnum(static_cast<unsigned char>(text[after])) && text[after] != '_'));
            if (!word_start || !word_end) { ++p; continue; }
            std::size_t q = after;
            while (q < text.size() && (text[q] == ' ' || text[q] == '\t' ||
                                       text[q] == '\n' || text[q] == '\r')) ++q;
            if (q < text.size() && text[q] == '=') return p;
            ++p;
        }
        return std::string_view::npos;
    };

    auto skip_to_open_brace = [&](std::size_t from) -> std::size_t {
        std::size_t q = from;
        while (q < text.size() && text[q] != '{') ++q;
        return q < text.size() ? q : std::string_view::npos;
    };

    // Match braces to find table extent.
    auto find_table_end = [&](std::size_t open) -> std::size_t {
        int depth = 1;
        std::size_t q = open + 1;
        while (q < text.size() && depth > 0) {
            char c = text[q];
            if (c == '{') ++depth;
            else if (c == '}') {
                --depth;
                if (depth == 0) return q;
            }
            ++q;
        }
        return std::string_view::npos;
    };

    auto xpm_pos = find_word_at_lhs("xpm", 0);
    if (xpm_pos == std::string_view::npos) return versions;
    auto xpm_open = skip_to_open_brace(xpm_pos);
    if (xpm_open == std::string_view::npos) return versions;
    auto xpm_end = find_table_end(xpm_open);
    if (xpm_end == std::string_view::npos) return versions;

    auto plat_pos = find_word_at_lhs(platform, xpm_open + 1);
    if (plat_pos == std::string_view::npos || plat_pos >= xpm_end) return versions;
    auto plat_open = skip_to_open_brace(plat_pos);
    if (plat_open == std::string_view::npos || plat_open >= xpm_end) return versions;
    auto plat_end = find_table_end(plat_open);
    if (plat_end == std::string_view::npos) return versions;

    // Inside platform table: scan for ["X.Y.Z"] = { ... }
    std::size_t q = plat_open + 1;
    while (q < plat_end) {
        if (text[q] == '[') {
            std::size_t r = q + 1;
            while (r < plat_end && (text[r] == ' ' || text[r] == '\t')) ++r;
            if (r < plat_end && text[r] == '"') {
                ++r;
                std::size_t key_start = r;
                while (r < plat_end && text[r] != '"' && text[r] != '\n') ++r;
                if (r < plat_end && text[r] == '"') {
                    versions.emplace_back(luaContent.substr(key_start, r - key_start));
                }
            }
        }
        ++q;
    }
    return versions;
}

std::expected<Manifest, ManifestError>
synthesize_from_xpkg_lua(std::string_view luaContent,
                         std::string_view packageName,
                         std::string_view packageVersion)
{
    auto body = extract_mcpp_segment_body(luaContent);
    if (body.empty()) {
        return std::unexpected(ManifestError{
            std::format(
                "package '{}' has no `mcpp = {{}}` segment in its index entry "
                "and the source has no mcpp.toml — cannot derive a manifest.",
                packageName),
            std::format("xpkg-lua of {}@{}", packageName, packageVersion),
            0, 0});
    }

    Manifest m;
    m.sourcePath  = std::format("xpkg-lua://{}@{}", packageName, packageVersion);
    m.package.name    = std::string(packageName);
    m.package.version = std::string(packageVersion);
    m.language.standard   = "c++23";
    m.language.modules    = true;
    m.language.importStd  = true;

    LuaCursor cur { body };
    cur.skip_ws_and_comments();

    while (!cur.eof()) {
        cur.skip_ws_and_comments();
        if (cur.eof()) break;
        auto key = cur.read_key();
        if (key.empty()) {
            cur.skip_ws_and_comments();
            if (cur.eof()) break;
            ++cur.pos;            // unknown char — advance and retry
            continue;
        }
        cur.skip_ws_and_comments();
        if (!cur.consume('=')) {
            return std::unexpected(ManifestError{
                std::format("malformed mcpp segment near key '{}'", key),
                m.sourcePath, 0, 0});
        }
        cur.skip_ws_and_comments();

        if      (key == "language") {
            auto v = cur.read_string();
            if (!v.empty()) m.language.standard = v;
        }
        else if (key == "import_std") {
            auto v = cur.read_bareword();
            m.language.importStd = (v == "true");
        }
        else if (key == "modules") {
            // `{ "a", "b", ... }`
            if (!cur.consume('{')) {
                return std::unexpected(ManifestError{
                    "expected '{' after `modules =`", m.sourcePath, 0, 0});
            }
            cur.skip_ws_and_comments();
            while (!cur.eof() && cur.peek() != '}') {
                auto s = cur.read_string();
                if (!s.empty()) m.modules.exports_.push_back(std::move(s));
                cur.skip_ws_and_comments();
            }
            cur.consume('}');
        }
        else if (key == "sources") {
            if (!cur.consume('{')) {
                return std::unexpected(ManifestError{
                    "expected '{' after `sources =`", m.sourcePath, 0, 0});
            }
            cur.skip_ws_and_comments();
            while (!cur.eof() && cur.peek() != '}') {
                auto s = cur.read_string();
                if (!s.empty()) {
                    m.modules.sources.push_back(s);
                    m.buildConfig.sources.push_back(std::move(s));   // M5.0 mirror
                }
                cur.skip_ws_and_comments();
            }
            cur.consume('}');
        }
        else if (key == "include_dirs") {
            // M5.0: shipped headers exposed to dependents AND used by this
            // package's own compile (mcpp's symmetric include_dirs semantics).
            if (!cur.consume('{')) {
                return std::unexpected(ManifestError{
                    "expected '{' after `include_dirs =`", m.sourcePath, 0, 0});
            }
            cur.skip_ws_and_comments();
            while (!cur.eof() && cur.peek() != '}') {
                auto s = cur.read_string();
                if (!s.empty()) m.buildConfig.includeDirs.emplace_back(s);
                cur.skip_ws_and_comments();
            }
            cur.consume('}');
        }
        else if (key == "targets") {
            // `{ ["name"] = { kind = "lib" }, ... }`
            if (!cur.consume('{')) {
                return std::unexpected(ManifestError{
                    "expected '{' after `targets =`", m.sourcePath, 0, 0});
            }
            cur.skip_ws_and_comments();
            while (!cur.eof() && cur.peek() != '}') {
                auto tname = cur.read_key();
                if (tname.empty()) { cur.skip_ws_and_comments(); break; }
                cur.skip_ws_and_comments();
                if (!cur.consume('=')) break;
                cur.skip_ws_and_comments();
                if (!cur.consume('{')) break;

                Target t;
                t.name = tname;
                t.kind = Target::Library;     // default
                cur.skip_ws_and_comments();
                while (!cur.eof() && cur.peek() != '}') {
                    auto sub = cur.read_key();
                    cur.skip_ws_and_comments();
                    if (!cur.consume('=')) break;
                    cur.skip_ws_and_comments();
                    if (sub == "kind") {
                        auto k = cur.read_string();
                        if      (k == "lib" || k == "library")     t.kind = Target::Library;
                        else if (k == "bin" || k == "binary")      t.kind = Target::Binary;
                        else if (k == "shared" || k == "dylib"
                              || k == "so" || k == "shlib")        t.kind = Target::SharedLibrary;
                    } else if (sub == "main") {
                        t.main = cur.read_string();
                    } else {
                        // unknown subfield — skip its value
                        cur.skip_ws_and_comments();
                        if (cur.peek() == '{') cur.skip_table();
                        else (void)cur.read_bareword();
                    }
                    cur.skip_ws_and_comments();
                }
                cur.consume('}');
                m.targets.push_back(std::move(t));
                cur.skip_ws_and_comments();
            }
            cur.consume('}');
        }
        else if (key == "deps") {
            // `{ ["name"] = "version", ... }`
            if (!cur.consume('{')) {
                return std::unexpected(ManifestError{
                    "expected '{' after `deps =`", m.sourcePath, 0, 0});
            }
            cur.skip_ws_and_comments();
            while (!cur.eof() && cur.peek() != '}') {
                auto dname = cur.read_key();
                if (dname.empty()) break;
                cur.skip_ws_and_comments();
                if (!cur.consume('=')) break;
                cur.skip_ws_and_comments();
                auto dver = cur.read_string();
                if (!dname.empty()) {
                    DependencySpec spec;
                    spec.version = dver;
                    m.dependencies[dname] = std::move(spec);
                }
                cur.skip_ws_and_comments();
            }
            cur.consume('}');
        }
        else if (key == "cflags" || key == "cxxflags") {
            // `{ "-Dfoo", "-Wall", ... }` — appended to the per-rule baseline
            // by ninja_backend. cflags goes to the C rule (.c files), cxxflags
            // to C++ rule (.cpp/.cc/.cxx/.cppm).
            if (!cur.consume('{')) {
                return std::unexpected(ManifestError{
                    std::format("expected '{{' after `{} =`", key),
                    m.sourcePath, 0, 0});
            }
            cur.skip_ws_and_comments();
            auto& target = (key == "cflags")
                ? m.buildConfig.cflags : m.buildConfig.cxxflags;
            while (!cur.eof() && cur.peek() != '}') {
                auto s = cur.read_string();
                if (!s.empty()) target.push_back(std::move(s));
                cur.skip_ws_and_comments();
            }
            cur.consume('}');
        }
        else if (key == "c_standard") {
            auto v = cur.read_string();
            if (!v.empty()) m.buildConfig.cStandard = v;
        }
        else {
            // Unknown key — skip the value (string / bareword / table).
            cur.skip_ws_and_comments();
            if      (cur.peek() == '"' || cur.peek() == '\'') (void)cur.read_string();
            else if (cur.peek() == '{') cur.skip_table();
            else                        (void)cur.read_bareword();
        }
    }

    // Validate minimum
    if (m.modules.sources.empty()) {
        return std::unexpected(ManifestError{
            "synthesised manifest missing sources (mcpp segment must declare `sources = { ... }`)",
            m.sourcePath, 0, 0});
    }
    if (m.targets.empty()) {
        // Default to a library target with the same name as the package.
        Target t;
        t.name = m.package.name;
        // For dotted names like mcpplibs.cmdline, take the last segment.
        auto dot = t.name.find_last_of('.');
        if (dot != std::string::npos) t.name = t.name.substr(dot + 1);
        t.kind = Target::Library;
        m.targets.push_back(std::move(t));
    }

    return m;
}

std::string default_template(std::string_view packageName) {
    // M5.0: minimal mcpp.toml — convention over configuration.
    // sources / target / standard are all auto-inferred. Users add fields as
    // they grow out of the defaults.
    return std::format(R"([package]
name        = "{}"
version     = "0.1.0"
description = "A modular C++23 package"
license     = "Apache-2.0"
)", packageName);
}

} // namespace mcpp::manifest

// mcpp.build.engine_package — the engine interface (`mcpp`, `mcpp.core`) as a
// package of the build program's graph (mcpp#797).
//
// A build program imports `mcpp.core` (spelled `mcpp` too, SPEC-007 R9.1).
// The interface is the engine's own text: it follows this mcpp's directive
// protocol, so it is generated from the binary rather than shipped as a file
// that could disagree with it. It is written once per mcpp version, as an
// ordinary package with a manifest, into
//
//     $MCPP_HOME/engine/<mcpp version>/modules/
//
// and the build program's graph depends on it like on any library. That is
// what lets the interface be compiled by the same plan, with the same flags,
// as every unit importing it, and what lets an editor read it: the directory
// is stated by `mcpp env` (`engineModules`) and by the build database's
// `mcpp:core` collection (SPEC-005).
//
// THE DIRECTORY IS IMMUTABLE once written, like a payload in the package
// store: its content is a function of the mcpp version alone. The dependency
// cache therefore keeps its compiled entry globally, under the address it has
// always had (`pkg/_engine/mcpp-build-module@<version>`), and `mcpp cache`
// lists, verifies and collects it as before.

export module mcpp.build.engine_package;

import std;
import mcpp.build.directives;   // kProtocolVersion -- the announced value has ONE source
import mcpp.build.hostprogram;  // the interface text
import mcpp.home;
import mcpp.version;

export namespace mcpp::build::engine {

// The package's identity in a manifest, and the address its compiled entry
// has in the dependency cache.
inline constexpr std::string_view kNamespace    = "mcpp";
inline constexpr std::string_view kName         = "core";
inline constexpr std::string_view kCacheIndex   = "_engine";
inline constexpr std::string_view kCachePackage = "mcpp-build-module";

// `$MCPP_HOME/engine`: every version's package lives below it.
std::filesystem::path root();
// The package directory of this mcpp version.
std::filesystem::path package_dir();
// Whether `dir` is an engine package directory (of any version).
bool is_engine_package(const std::filesystem::path& dir);

// The interface and its alias, as they are written: `mcpp.cppm` and
// `mcpp_core.cppm`.
std::pair<std::string, std::string> module_texts();

// Writes the package of this version when it is absent or differs, atomically
// (a sibling directory renamed into place), and returns its directory.
std::expected<std::filesystem::path, std::string> ensure_package();

}  // namespace mcpp::build::engine

namespace mcpp::build::engine {

namespace fs = std::filesystem;

namespace {

std::string substituted(std::string_view text, std::string_view placeholder,
                        std::string_view value) {
    std::string out(text);
    if (auto p = out.find(placeholder); p != std::string::npos)
        out.replace(p, placeholder.size(), value);
    return out;
}

std::string manifest_text() {
    return std::format(
        "# Written by mcpp {0}: the engine interface a build program imports.\n"
        "# Do not edit; it is replaced when it differs from what mcpp {0} writes.\n"
        "[package]\n"
        "namespace = \"{1}\"\n"
        "name      = \"{2}\"\n"
        "version   = \"{0}\"\n"
        "\n"
        "[language]\n"
        "import_std = false\n"
        "\n"
        "[lib]\n"
        "path = \"mcpp.cppm\"\n"
        "\n"
        "[build]\n"
        "sources = [\"mcpp.cppm\", \"mcpp_core.cppm\"]\n"
        "\n"
        "[targets.core]\n"
        "kind = \"lib\"\n",
        mcpp::MCPP_VERSION, kNamespace, kName);
}

std::string read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return {std::istreambuf_iterator<char>(in), {}};
}

}  // namespace

fs::path root() { return mcpp::home::root() / "engine"; }

fs::path package_dir() { return root() / std::string(mcpp::MCPP_VERSION) / "modules"; }

bool is_engine_package(const fs::path& dir) {
    auto inside = [](const fs::path& a, const fs::path& b) {
        auto rel = a.lexically_normal().lexically_relative(b.lexically_normal());
        return !rel.empty() && *rel.begin() != "..";
    };
    if (inside(dir, root())) return true;
    // One directory has more than one spelling: `/var` is `/private/var` on
    // macOS, and Windows has HOME and USERPROFILE, drive-letter case, 8.3 names.
    std::error_code e1, e2;
    const auto cd = fs::weakly_canonical(dir, e1);
    const auto cr = fs::weakly_canonical(root(), e2);
    return !e1 && !e2 && inside(cd, cr);
}

std::pair<std::string, std::string> module_texts() {
    auto mod = substituted(kMcppModuleSource, "@MODULE@", "export module");
    // Substituted rather than hardcoded so the announced version can never
    // drift from the one the engine checks against.
    mod = substituted(mod, "@PROTOCOL@",
                      std::to_string(mcpp::build::directives::kProtocolVersion));
    auto alias = substituted(kMcppCoreAliasSource, "@MODULE@", "export module");
    alias = substituted(alias, "@EXPORT@", "export");
    return {std::move(mod), std::move(alias)};
}

std::expected<fs::path, std::string> ensure_package() {
    const auto dir = package_dir();
    const auto [mod, alias] = module_texts();
    const auto manifest = manifest_text();
    if (read_all(dir / "mcpp.toml") == manifest && read_all(dir / "mcpp.cppm") == mod
        && read_all(dir / "mcpp_core.cppm") == alias)
        return dir;

    std::error_code ec;
    fs::create_directories(dir.parent_path(), ec);
    static std::atomic<unsigned> serial{0};
    const auto scratch = dir.parent_path() / std::format(
        ".modules.{}.{}", std::chrono::steady_clock::now().time_since_epoch().count(),
        serial.fetch_add(1));
    fs::create_directories(scratch, ec);
    if (ec)
        return std::unexpected(std::format("cannot create {}: {}", scratch.string(), ec.message()));
    for (auto const& [name, text] : {std::pair{"mcpp.toml", manifest},
                                     std::pair{"mcpp.cppm", mod},
                                     std::pair{"mcpp_core.cppm", alias}}) {
        std::ofstream out(scratch / name, std::ios::binary | std::ios::trunc);
        out << text;
        out.close();
        if (!out)
            return std::unexpected(std::format("cannot write {}", (scratch / name).string()));
    }
    // Another process may have written the same content meanwhile; either
    // copy is this version's package.
    fs::remove_all(dir, ec);
    fs::rename(scratch, dir, ec);
    if (ec) {
        fs::remove_all(scratch, ec);
        if (read_all(dir / "mcpp.cppm") != mod)
            return std::unexpected(std::format("cannot place the engine interface at {}",
                                               dir.string()));
    }
    return dir;
}

}  // namespace mcpp::build::engine

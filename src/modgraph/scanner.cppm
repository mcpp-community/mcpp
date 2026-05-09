// mcpp.modgraph.scanner — regex-based scan of .cppm/.cpp for module statements.
//
// Hard constraints (per docs/01 §4.2):
//   ✗ no #if/#ifdef-guarded import
//   ✗ no header units (import "h" / import <h>)
//   ✗ files outside [modules].sources glob
//
// Returns a Graph or a list of detailed errors.

export module mcpp.modgraph.scanner;

import std;
import mcpp.manifest;
import mcpp.modgraph.graph;
import mcpp.modgraph.p1689;
import mcpp.toolchain.detect;

export namespace mcpp::modgraph {

struct ScanError {
    std::filesystem::path path;
    std::size_t           line   = 0;
    std::string           message;

    std::string format() const {
        if (line)
            return std::format("{}:{}: {}", path.string(), line, message);
        return std::format("{}: {}", path.string(), message);
    }
};

// Expand a glob like "src/**/*.cppm" into a list of matching paths anchored at root.
std::vector<std::filesystem::path> expand_glob(const std::filesystem::path& root,
                                               std::string_view glob);

// M6.x: same as expand_glob but matches DIRECTORIES (used for include_dirs
// like "*/include"). Always returns absolute paths under `root`.
std::vector<std::filesystem::path> expand_dir_glob(const std::filesystem::path& root,
                                                   std::string_view glob);

// Scan a single source file.
std::expected<SourceUnit, ScanError> scan_file(const std::filesystem::path& file,
                                               const std::string&           packageName);

// Scan the entire package: collects all sources via manifest globs and returns a Graph.
struct ScanResult {
    Graph                       graph;
    std::vector<ScanError>      errors;
    std::vector<ScanError>      warnings;
};
ScanResult scan_package(const std::filesystem::path& root,
                        const mcpp::manifest::Manifest& manifest);

// Scan multiple packages (primary + path-based deps) into one combined Graph.
// Each SourceUnit retains its own packageName, so validate() applies the
// correct naming rules per-package.
struct PackageRoot {
    std::filesystem::path           root;
    mcpp::manifest::Manifest        manifest;
};
ScanResult scan_packages(const std::vector<PackageRoot>& packages);

// Drop-in replacement that delegates per-file scanning to GCC's P1689r5
// (.ddi) output instead of regex parsing. Same ScanResult shape — used by
// cli when MCPP_SCANNER=p1689 (see docs/27).
ScanResult scan_packages_p1689(const std::vector<PackageRoot>&     packages,
                               const mcpp::toolchain::Toolchain&   tc,
                               const std::filesystem::path&        tmpDir);

} // namespace mcpp::modgraph

namespace mcpp::modgraph {

namespace {

bool path_matches_glob(const std::filesystem::path& candidate,
                       const std::filesystem::path& root,
                       std::string_view             glob)
{
    // Supports "**" (any number of dirs) and "*" (within one segment).
    // Matches relative-path of candidate against glob.
    auto rel = std::filesystem::relative(candidate, root).generic_string();

    auto match = [](std::string_view s, std::string_view p) -> bool {
        // Simple recursive matcher.
        std::function<bool(std::size_t, std::size_t)> rec =
            [&](std::size_t si, std::size_t pi) -> bool {
            while (pi < p.size()) {
                if (p[pi] == '*' && pi + 1 < p.size() && p[pi + 1] == '*') {
                    // ** : skip zero or more chars/segments
                    pi += 2;
                    if (pi < p.size() && p[pi] == '/') ++pi;
                    if (pi >= p.size()) return true;
                    while (si <= s.size()) {
                        if (rec(si, pi)) return true;
                        ++si;
                    }
                    return false;
                } else if (p[pi] == '*') {
                    // * : skip zero or more chars within segment (not /)
                    ++pi;
                    if (pi >= p.size()) {
                        return s.find('/', si) == std::string_view::npos;
                    }
                    while (si <= s.size()) {
                        if (rec(si, pi)) return true;
                        if (si < s.size() && s[si] == '/') break;
                        ++si;
                    }
                    return false;
                } else {
                    if (si >= s.size() || s[si] != p[pi]) return false;
                    ++si; ++pi;
                }
            }
            return si == s.size();
        };
        return rec(0, 0);
    };
    return match(rel, glob);
}

// Trim leading/trailing whitespace.
std::string_view trim(std::string_view s) {
    std::size_t i = 0, j = s.size();
    while (i < j && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    while (j > i && std::isspace(static_cast<unsigned char>(s[j-1]))) --j;
    return s.substr(i, j - i);
}

// Strip a trailing line comment ("//...").
std::string_view strip_line_comment(std::string_view s) {
    auto p = s.find("//");
    if (p == std::string_view::npos) return s;
    return s.substr(0, p);
}

bool is_module_name_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == ':';
}

} // namespace

std::vector<std::filesystem::path> expand_glob(const std::filesystem::path& root,
                                               std::string_view glob)
{
    std::vector<std::filesystem::path> out;
    if (!std::filesystem::exists(root)) return out;
    for (auto& e : std::filesystem::recursive_directory_iterator(root)) {
        if (!e.is_regular_file()) continue;
        if (path_matches_glob(e.path(), root, glob)) out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::filesystem::path> expand_dir_glob(const std::filesystem::path& root,
                                                   std::string_view glob)
{
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) return out;

    // Fast path: glob with no wildcards → literal path under root.
    if (glob.find('*') == std::string_view::npos) {
        auto p = root / std::filesystem::path(glob);
        if (std::filesystem::is_directory(p, ec)) out.push_back(p);
        return out;
    }
    // Walk all directories under root, match each against the glob.
    out.push_back(root);   // root itself eligible if glob is "" (rare)
    for (auto& e : std::filesystem::recursive_directory_iterator(root, ec)) {
        if (ec) break;
        if (!e.is_directory(ec) || ec) continue;
        if (path_matches_glob(e.path(), root, glob)) out.push_back(e.path());
    }
    out.erase(out.begin());   // drop root sentinel
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::expected<SourceUnit, ScanError> scan_file(const std::filesystem::path& file,
                                               const std::string&           packageName)
{
    std::ifstream is(file);
    if (!is) return std::unexpected(ScanError{file, 0, "cannot open"});

    SourceUnit u;
    u.path         = file;
    u.packageName = packageName;

    // .c files are pure C: they cannot legally contain `module` / `import`
    // declarations, and we route them to the C-language compile rule (no
    // P1689 scan, no BMI lookups). Skip the line-by-line module scan to
    // avoid any chance of a benign C identifier (`import_foo`, `module_t`,
    // ...) being misparsed.
    if (file.extension() == ".c") {
        return u;
    }

    int          if_depth        = 0;     // #if/#ifdef nesting
    std::size_t  lineno          = 0;
    std::string  line;
    while (std::getline(is, line)) {
        ++lineno;
        std::string_view sv = strip_line_comment(line);
        sv = trim(sv);
        if (sv.empty()) continue;

        // Track preprocessor depth (we only need to know if we're inside #if).
        if (sv.size() > 0 && sv[0] == '#') {
            std::string_view rest = trim(sv.substr(1));
            if (rest.starts_with("if") || rest.starts_with("ifdef") || rest.starts_with("ifndef")) {
                ++if_depth;
            } else if (rest.starts_with("endif")) {
                if (if_depth > 0) --if_depth;
            }
            continue;
        }

        // Strip leading `export ` so we can handle uniformly:
        //   `export module foo;`
        //   `module foo;`
        //   `export import :part;`   ← new: re-exported partitions / modules
        //   `import foo;`
        std::string_view r = sv;
        bool is_export = false;
        if (r.starts_with("export") &&
            (r.size() == 6 || r[6] == ' ' || r[6] == '\t')) {
            is_export = true;
            r = trim(r.substr(6));
        }

        // module name [: partition] ;
        if (r.starts_with("module") &&
            (r.size() == 6 || r[6] == ' ' || r[6] == '\t' || r[6] == ';')) {
            r = trim(r.substr(6));
            if (r.empty() || r == ";") {
                continue;  // global module fragment marker (`module;`)
            }
            std::string name;
            std::size_t i = 0;
            while (i < r.size() && is_module_name_char(r[i])) {
                name.push_back(r[i]);
                ++i;
            }
            if (is_export) {
                if (u.provides) {
                    return std::unexpected(ScanError{file, lineno,
                        std::format("file already exports module '{}'; cannot export '{}'",
                                    u.provides->logicalName, name)});
                }
                u.provides = ModuleId{name};
                u.isModuleInterface = true;
            } else {
                // implementation unit (`module foo;`) — non-exporting.
                // Don't claim ownership of `foo` (partition would be foo:part);
                // record import dep on the module's interface.
                if (!u.provides) {
                    u.requires_.push_back(ModuleId{name});
                }
            }
            continue;
        }

        // import [name | "h" | <h>] ;
        if (r.starts_with("import") &&
            (r.size() == 6 || r[6] == ' ' || r[6] == '\t' ||
             r[6] == '<' || r[6] == '"' || r[6] == ':'))
        {
            if (if_depth > 0) {
                return std::unexpected(ScanError{file, lineno,
                    "import statement inside conditional preprocessor block (forbidden in M1)"});
            }
            r = trim(r.substr(6));
            if (r.empty()) continue;
            if (r[0] == '<' || r[0] == '"') {
                return std::unexpected(ScanError{file, lineno,
                    "header units (import \"h\" / import <h>) are forbidden in M1"});
            }
            std::string name;
            std::size_t i = 0;
            while (i < r.size() && is_module_name_char(r[i])) {
                name.push_back(r[i]);
                ++i;
            }
            if (name.empty()) continue;
            // Partition import within the same module: prepend the *base*
            // module name. If the current TU itself owns a partition (e.g.
            // its `export module foo:http;`), `u.provides->logicalName`
            // already includes that suffix — concatenating naively would
            // produce `foo:http:tls` instead of the intended `foo:tls`.
            // Strip our own `:partition` first.
            if (name.starts_with(":") && u.provides) {
                std::string base = u.provides->logicalName;
                if (auto p = base.find(':'); p != std::string::npos) {
                    base.resize(p);
                }
                name = base + name;
            }
            u.requires_.push_back(ModuleId{name});
            continue;
        }
    }

    // Classify implementation .cpp (no provides + not a partition)
    if (!u.provides && file.extension() == ".cpp") {
        u.isImplementation = true;
    }

    return u;
}

namespace {

// Phase 1: scan a single package, append units to result.graph.units;
// errors go straight into result.errors. producerOf/edges are NOT built
// here — the caller does that after all packages are scanned.
void scan_one_into(ScanResult& result,
                   const std::filesystem::path& root,
                   const mcpp::manifest::Manifest& manifest)
{
    // Glob exclusion: patterns starting with `!` remove files from the
    // include set (like .gitignore).
    //   sources = ["src/**/*.cpp", "!src/**/*_test.cpp"]
    // All positive patterns are expanded first, then all `!`-prefixed
    // patterns are expanded and the resulting paths are removed.
    std::set<std::filesystem::path> all_files;
    std::set<std::filesystem::path> excluded;
    for (auto const& g : manifest.modules.sources) {
        if (!g.empty() && g[0] == '!') {
            for (auto& p : expand_glob(root, g.substr(1))) {
                excluded.insert(p);
            }
        } else {
            for (auto& p : expand_glob(root, g)) {
                all_files.insert(p);
            }
        }
    }
    for (auto& p : excluded) all_files.erase(p);

    for (auto const& f : all_files) {
        auto r = scan_file(f, manifest.package.name);
        if (!r) {
            result.errors.push_back(r.error());
            continue;
        }
        result.graph.units.push_back(std::move(*r));
    }
}

// Phase 2: producerOf + edges over already-collected units.
void resolve_graph(ScanResult& result) {
    auto& g = result.graph;
    for (std::size_t i = 0; i < g.units.size(); ++i) {
        auto& u = g.units[i];
        if (u.provides) {
            auto [it, inserted] = g.producerOf.emplace(u.provides->logicalName, i);
            if (!inserted) {
                result.errors.push_back(ScanError{
                    u.path, 0,
                    std::format("module '{}' already provided by {}",
                                u.provides->logicalName,
                                g.units[it->second].path.string())});
            }
        }
    }
    for (std::size_t i = 0; i < g.units.size(); ++i) {
        auto& u = g.units[i];
        for (auto const& req : u.requires_) {
            auto it = g.producerOf.find(req.logicalName);
            if (it == g.producerOf.end()) {
                if (req.logicalName == "std" || req.logicalName == "std.compat") continue;
                result.warnings.push_back(ScanError{
                    u.path, 0,
                    std::format("module '{}' imported but not provided in this build",
                                req.logicalName)});
                continue;
            }
            g.edges.emplace_back(i, it->second);
        }
    }
}

} // namespace

ScanResult scan_package(const std::filesystem::path& root,
                        const mcpp::manifest::Manifest& manifest)
{
    ScanResult result;
    scan_one_into(result, root, manifest);
    resolve_graph(result);
    return result;
}

ScanResult scan_packages(const std::vector<PackageRoot>& packages) {
    ScanResult result;
    for (auto const& p : packages) {
        scan_one_into(result, p.root, p.manifest);
    }
    resolve_graph(result);
    return result;
}

ScanResult scan_packages_p1689(const std::vector<PackageRoot>&     packages,
                               const mcpp::toolchain::Toolchain&   tc,
                               const std::filesystem::path&        tmpDir)
{
    ScanResult result;
    for (auto const& p : packages) {
        std::set<std::filesystem::path> all_files;
        for (auto const& g : p.manifest.modules.sources) {
            for (auto& f : expand_glob(p.root, g)) all_files.insert(f);
        }
        for (auto const& f : all_files) {
            auto r = mcpp::modgraph::p1689::scan_file(
                f, p.manifest.package.name, tc, tmpDir);
            if (!r) {
                result.errors.push_back(ScanError{ f, 0, r.error() });
                continue;
            }
            result.graph.units.push_back(std::move(*r));
        }
    }
    resolve_graph(result);
    return result;
}

} // namespace mcpp::modgraph

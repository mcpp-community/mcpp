// mcpp.modgraph.p1689 — compiler-driven module dep scan via P1689r5 (.ddi).
//
// Replaces (under MCPP_SCANNER=p1689) the regex-based scanner. Each source
// file gets fed to:
//
//   g++ <std-flag> -fmodules -fdeps-format=p1689r5 \
//       -fdeps-file=<tmp>/x.ddi -fdeps-target=<tmp>/x.o \
//       -M -MM -MF <tmp>/x.dep -E <source> -o <tmp>/x.o
//
// The resulting .ddi JSON describes the file's `provides` / `requires`
// directly from the compiler's frontend, removing any drift between
// our regex heuristic and what GCC actually sees.
//
// We hand-parse the .ddi (no nlohmann::json dep — same justification
// as fetcher.cppm).
//
// Spec: P1689R5, the dependency format ninja consumes.

module;

export module mcpp.modgraph.p1689;

import std;
import mcpp.modgraph.graph;
import mcpp.run;
import mcpp.source_kind;
import mcpp.toolchain.model;
import mcpp.toolchain.linkmodel;

export namespace mcpp::modgraph::p1689 {

struct DdiProvide {
    std::string logicalName;
    // Absent, not false, when the scanner did not say. P1689 makes the key
    // optional and mcpp cannot invent an answer: `export module M:api;` and
    // `module M:impl;` differ only in the keyword, and getting that backwards
    // decides whether a closed-source partition's SOURCE is published.
    std::optional<bool> isInterface;
};

struct DdiRule {
    std::string             primaryOutput;
    std::vector<DdiProvide> provides;
    std::vector<std::string> requires_;
};

// Parse a .ddi JSON body into the first rule's info.
// Tolerates the slightly unstable GCC formatting (variable whitespace).
std::expected<DdiRule, std::string> parse_ddi(std::string_view body);

// THE DEPENDENCY SCAN OF ONE UNIT, ITS WORDS IN ORDER, CONSTRUCTED IN ONE
// PLACE for the two times it runs: the build's scan edge, whose words are
// ninja expressions (`$in`, `$out`, `$cxxflags`) joined into its command, and
// the plan, which asks the compiler for a unit whose imports stand inside a
// preprocessor condition and hands the words to the program as its arguments.
// No shell stands between: a shell's quoting is its host's (cmd.exe reads no
// single quotes, measured: Windows error 123 for the quoted program), and an
// argument vector has none. GCC scans with the compiler itself; Clang with
// clang-scan-deps, given the compile.
struct ScanWords {
    std::string prefix;                // the edge's launcher, with its space; not an argument
    std::string driver;                // the compiler
    std::string scanDeps;              // clang-scan-deps; empty: the compiler scans
    std::vector<std::string> flags;    // the unit's compile words
    std::vector<std::string> lang;     // the language flag of a module interface; may be empty
    std::string in;                    // the source
    std::string out;                   // the P1689 record written
    std::string depsTarget;            // GCC: the target the record names
    std::string depFile;               // GCC: the make-style list of the files the scan read
    std::string scratch;               // GCC: the `-o` the driver creates, which nothing reads
    std::string compileTarget;         // Clang: the object the compile it is given would write
};
std::vector<std::string> scan_words(const ScanWords& w);
// The words joined after the prefix: the build's scan edge.
std::string scan_command(const ScanWords& w);

// Run the compiler's scan on `source` and return the populated SourceUnit.
//   tmpDir   : per-build scratch dir for .ddi/.dep/.o byproducts.
//   stdWords : the standard and the graph's dialect words, one argument each
//              (empty: -std=c++23)
//   flags    : the unit's own compile words, after the standard and the
//              include directories (its package's, the graph's dialect words)
//   scanDeps : clang-scan-deps for a Clang toolchain; empty for GCC
std::expected<SourceUnit, std::string>
scan_file(const std::filesystem::path&        source,
          const std::string&                  packageName,
          const mcpp::toolchain::Toolchain&   tc,
          const std::filesystem::path&        tmpDir,
          const std::vector<std::filesystem::path>& includeDirs,
          std::span<const std::string>        stdWords,
          const mcpp::ExtensionTable&         extTable,
          const std::vector<std::string>&     flags = {},
          const std::filesystem::path&        scanDeps = {});

} // namespace mcpp::modgraph::p1689

namespace mcpp::modgraph::p1689 {

namespace {

// --- Tiny JSON helpers (subset). ----------------------------------------

void skip_ws(std::string_view s, std::size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' ||
                            s[i] == '\n' || s[i] == '\r'))
        ++i;
}

// Parse a JSON string starting at s[i] which must point at '"'.
// On success returns the unescaped contents and advances i past the closing '"'.
std::expected<std::string, std::string>
parse_string(std::string_view s, std::size_t& i) {
    if (i >= s.size() || s[i] != '"') {
        return std::unexpected("expected '\"'");
    }
    ++i;
    std::string out;
    while (i < s.size()) {
        char c = s[i++];
        if (c == '"') return out;
        if (c == '\\') {
            if (i >= s.size()) break;
            char nc = s[i++];
            switch (nc) {
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case '"': out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/'); break;
                default:   out.push_back(nc);
            }
        } else {
            out.push_back(c);
        }
    }
    return std::unexpected("unterminated string");
}

// Find a key at the *current depth* of the object starting at s[start]
// (which must be at '{'). Returns the index just after the matching ':'
// for the requested key, or npos.
std::size_t find_key_in_object(std::string_view s, std::size_t start,
                               std::string_view key)
{
    if (start >= s.size() || s[start] != '{') return std::string_view::npos;
    std::size_t i = start + 1;
    int depth = 0;
    while (i < s.size()) {
        skip_ws(s, i);
        if (i >= s.size()) break;
        char c = s[i];
        if (depth == 0 && c == '}') return std::string_view::npos;
        if (c == '"') {
            // String key candidate
            std::size_t kStart = i;
            std::string parsed;
            auto pr = parse_string(s, i);
            if (!pr) return std::string_view::npos;
            parsed = *pr;
            skip_ws(s, i);
            if (i < s.size() && s[i] == ':') {
                ++i;
                if (depth == 0 && parsed == key) {
                    skip_ws(s, i);
                    return i;
                }
                // Skip the value at this depth.
                skip_ws(s, i);
                // Skip a value: string / number / bool / null / object / array
                if (i < s.size() && s[i] == '"') {
                    auto _ = parse_string(s, i);
                } else if (i < s.size() && (s[i] == '{' || s[i] == '[')) {
                    char open = s[i];
                    char close = (open == '{') ? '}' : ']';
                    int d2 = 0;
                    bool in_str = false;
                    while (i < s.size()) {
                        char cc = s[i];
                        if (in_str) {
                            if (cc == '\\' && i + 1 < s.size()) { i += 2; continue; }
                            if (cc == '"') in_str = false;
                            ++i; continue;
                        }
                        if (cc == '"') { in_str = true; ++i; continue; }
                        if (cc == open) ++d2;
                        else if (cc == close) { --d2; if (d2 == 0) { ++i; break; } }
                        ++i;
                    }
                } else {
                    // bare token
                    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']'
                           && s[i] != ' ' && s[i] != '\n' && s[i] != '\r' && s[i] != '\t')
                        ++i;
                }
                skip_ws(s, i);
                if (i < s.size() && s[i] == ',') { ++i; }
                continue;
            }
            (void)kStart;
            continue;
        }
        // Stray separator
        if (c == ',') { ++i; continue; }
        ++i;
        // Defensive infinite-loop guard
        (void)depth;
    }
    return std::string_view::npos;
}

} // namespace

std::expected<DdiRule, std::string> parse_ddi(std::string_view body) {
    // Locate top-level "rules":[...]
    std::size_t i = 0;
    skip_ws(body, i);
    if (i >= body.size() || body[i] != '{')
        return std::unexpected("ddi: expected top-level object");

    auto rulesAt = find_key_in_object(body, i, "rules");
    if (rulesAt == std::string_view::npos)
        return std::unexpected("ddi: missing 'rules'");

    skip_ws(body, rulesAt);
    if (rulesAt >= body.size() || body[rulesAt] != '[')
        return std::unexpected("ddi: 'rules' is not an array");

    // Step into the first rule object inside the array.
    std::size_t j = rulesAt + 1;
    skip_ws(body, j);
    if (j >= body.size() || body[j] != '{')
        return std::unexpected("ddi: 'rules' array empty or malformed");

    DdiRule rule;

    // primary-output
    auto poAt = find_key_in_object(body, j, "primary-output");
    if (poAt != std::string_view::npos) {
        skip_ws(body, poAt);
        if (poAt < body.size() && body[poAt] == '"') {
            auto v = parse_string(body, poAt);
            if (v) rule.primaryOutput = *v;
        }
    }

    // provides: array of objects
    auto provAt = find_key_in_object(body, j, "provides");
    if (provAt != std::string_view::npos) {
        skip_ws(body, provAt);
        if (provAt < body.size() && body[provAt] == '[') {
            std::size_t k = provAt + 1;
            while (k < body.size()) {
                skip_ws(body, k);
                if (k >= body.size() || body[k] == ']') break;
                if (body[k] == '{') {
                    DdiProvide p;
                    auto lnAt = find_key_in_object(body, k, "logical-name");
                    if (lnAt != std::string_view::npos) {
                        skip_ws(body, lnAt);
                        if (lnAt < body.size() && body[lnAt] == '"') {
                            auto v = parse_string(body, lnAt);
                            if (v) p.logicalName = *v;
                        }
                    }
                    auto isAt = find_key_in_object(body, k, "is-interface");
                    if (isAt != std::string_view::npos) {
                        skip_ws(body, isAt);
                        if (body.substr(isAt, 4) == "true")  p.isInterface = true;
                        if (body.substr(isAt, 5) == "false") p.isInterface = false;
                    }
                    rule.provides.push_back(std::move(p));
                }
                // Skip past this object then comma.
                int d2 = 0; bool in_str = false;
                while (k < body.size()) {
                    char cc = body[k];
                    if (in_str) {
                        if (cc == '\\' && k + 1 < body.size()) { k += 2; continue; }
                        if (cc == '"') in_str = false;
                        ++k; continue;
                    }
                    if (cc == '"') { in_str = true; ++k; continue; }
                    if (cc == '{') ++d2;
                    else if (cc == '}') { --d2; ++k; if (d2 == 0) break; continue; }
                    ++k;
                }
                skip_ws(body, k);
                if (k < body.size() && body[k] == ',') { ++k; continue; }
                if (k < body.size() && body[k] == ']') break;
            }
        }
    }

    // requires: array of objects with logical-name
    auto reqAt = find_key_in_object(body, j, "requires");
    if (reqAt != std::string_view::npos) {
        skip_ws(body, reqAt);
        if (reqAt < body.size() && body[reqAt] == '[') {
            std::size_t k = reqAt + 1;
            while (k < body.size()) {
                skip_ws(body, k);
                if (k >= body.size() || body[k] == ']') break;
                if (body[k] == '{') {
                    auto lnAt = find_key_in_object(body, k, "logical-name");
                    if (lnAt != std::string_view::npos) {
                        skip_ws(body, lnAt);
                        if (lnAt < body.size() && body[lnAt] == '"') {
                            auto v = parse_string(body, lnAt);
                            if (v) rule.requires_.push_back(*v);
                        }
                    }
                }
                int d2 = 0; bool in_str = false;
                while (k < body.size()) {
                    char cc = body[k];
                    if (in_str) {
                        if (cc == '\\' && k + 1 < body.size()) { k += 2; continue; }
                        if (cc == '"') in_str = false;
                        ++k; continue;
                    }
                    if (cc == '"') { in_str = true; ++k; continue; }
                    if (cc == '{') ++d2;
                    else if (cc == '}') { --d2; ++k; if (d2 == 0) break; continue; }
                    ++k;
                }
                skip_ws(body, k);
                if (k < body.size() && body[k] == ',') { ++k; continue; }
                if (k < body.size() && body[k] == ']') break;
            }
        }
    }

    return rule;
}

namespace {

// A path as an argument of the scan: the spelling the old command line gave it,
// without the quoting a shell would have read.
std::string argument(const std::filesystem::path& p) { return p.string(); }

} // namespace

std::vector<std::string> scan_words(const ScanWords& w) {
    std::vector<std::string> out;
    auto add = [&](std::string_view word) {
        if (!word.empty()) out.emplace_back(word);
    };
    auto add_all = [&](const std::vector<std::string>& words) {
        for (auto const& word : words) add(word);
    };
    if (w.scanDeps.empty()) {
        add(w.driver);
        add_all(w.flags);
        add("-fmodules");
        add("-fdeps-format=p1689r5");
        add("-fdeps-file=" + w.out);
        add("-fdeps-target=" + w.depsTarget);
        add("-M");
        add("-MM");
        add("-MF");
        add(w.depFile);
        add_all(w.lang);
        add("-E");
        add(w.in);
        add("-o");
        add(w.scratch);
        return out;
    }
    add(w.scanDeps);
    add("-format=p1689");
    add("-o");
    add(w.out);
    add("--");
    add(w.driver);
    add_all(w.flags);
    add_all(w.lang);
    add("-c");
    add(w.in);
    add("-o");
    add(w.compileTarget);
    return out;
}

std::string scan_command(const ScanWords& w) {
    std::string out = w.prefix;
    const auto words = scan_words(w);
    for (std::size_t i = 0; i < words.size(); ++i) {
        if (i > 0) out += ' ';
        out += words[i];
    }
    return out;
}

std::expected<SourceUnit, std::string>
scan_file(const std::filesystem::path&        source,
          const std::string&                  packageName,
          const mcpp::toolchain::Toolchain&   tc,
          const std::filesystem::path&        tmpDir,
          const std::vector<std::filesystem::path>& includeDirs,
          std::span<const std::string>        stdWords,
          const mcpp::ExtensionTable&         extTable,
          const std::vector<std::string>&     flags,
          const std::filesystem::path&        scanDeps)
{
    std::error_code ec;
    std::filesystem::create_directories(tmpDir, ec);

    // One unique name per source — flat layout in tmpDir is fine since
    // the caller gives us a fresh dir per build.
    // `source` is always a glob-filtered path. path_matches_glob drops any
    // candidate try_narrow() cannot spell, so a name this code page has no
    // spelling for never reaches a scan — and a PROJECT ROOT that cannot be
    // spelled makes every candidate fail that same filter, so the build stops
    // at "no sources" well before here. What this feeds is a compiler command
    // line anyway: a path that cannot be narrowed cannot be handed to a
    // compiler at all. (mcpp#516 audit)
    auto stem = source.filename().string();  // NARROW-OK: glob-filtered, see above
    auto base = tmpDir / std::format("{}_{}",
        std::hash<std::string>{}(source.string()) % 1000000, stem);
    auto ddi = base; ddi += ".ddi";
    auto dep = base; dep += ".dep";
    auto obj = base; obj += ".o";

    std::vector<std::string> words;
    if (stdWords.empty()) words.emplace_back("-std=c++23");
    else words.assign(stdWords.begin(), stdWords.end());
    if (tc.base.stated()) {
        // A stated target base: the compile side the link model renders for
        // it (its sysroot, multiarch, or payload headers), as every compile
        // of this build is given.
        for (auto& word : mcpp::toolchain::resolve_link_model(tc).compile_tokens(
                 [](const std::filesystem::path& p) { return argument(p); }))
            words.push_back(std::move(word));
    } else if (!tc.base.root.empty()) {
        words.push_back("--sysroot=" + argument(tc.base.root));
    }
    for (auto const& dir : includeDirs) words.push_back("-I" + argument(dir));
    words.insert(words.end(), flags.begin(), flags.end());
    // Same suffix-recognition problem the ninja scan edge has: a driver that
    // does not know this source's extension hands it to the linker and exits 0
    // with no .ddi. Told explicitly for module interfaces, and only for them —
    // an implementation unit must not be scanned as an interface.
    std::vector<std::string> lang;
    if (mcpp::produces_bmi(mcpp::classify(source, extTable)))
        for (auto word : std::string_view(mcpp::toolchain::bmi_traits(tc).moduleInterfaceLangFlag)
                             | std::views::split(' '))
            if (!word.empty()) lang.emplace_back(word.begin(), word.end());
    auto scratch = base; scratch += ".scan";
    const auto argv = scan_words({
        .driver = argument(tc.binaryPath),
        .scanDeps = scanDeps.empty() ? std::string{} : argument(scanDeps),
        .flags = std::move(words),
        .lang = std::move(lang),
        .in = argument(source),
        .out = argument(ddi),
        .depsTarget = argument(obj),
        .depFile = argument(dep),
        .scratch = argument(scratch),
        .compileTarget = argument(obj),
    });

    int spawnError = 0;
    auto run = mcpp::run::capture_exec(argv, {}, {}, &spawnError);
    if (spawnError != 0 || run.exit_code != 0 || !std::filesystem::exists(ddi)) {
        return std::unexpected(std::format(
            "p1689 scan failed for '{}':\n{}", source.string(), run.output));
    }

    std::ifstream is(ddi);
    std::string body((std::istreambuf_iterator<char>(is)), {});
    auto rule = parse_ddi(body);
    if (!rule) {
        return std::unexpected(std::format(
            "p1689 .ddi parse failed for '{}': {}", source.string(), rule.error()));
    }

    SourceUnit u;
    u.path        = source;
    u.packageName = packageName;
    u.kind        = mcpp::classify(source, extTable);
    if (!rule->provides.empty()) {
        u.provides = ModuleId{ rule->provides.front().logicalName };
        // Carried through, including its absence: the compiler is the one thing
        // here that has actually parsed the declaration, so when it answers,
        // its answer beats any inference; when it stays quiet, so does mcpp.
        u.providesInterface = rule->provides.front().isInterface;
    }
    for (auto& r : rule->requires_) {
        u.requires_.push_back(ModuleId{ r });
    }
    // The declaration form, only as far as the record states it. A provided
    // module with `is-interface` names its form; a unit that requires modules
    // and provides none is either `module M;` or an importer, and P1689 does not
    // say which; a unit with neither has no module declaration.
    if (u.provides && u.providesInterface) {
        const bool partition =
            u.provides->logicalName.find(':') != std::string::npos;
        u.declaration = *u.providesInterface
            ? (partition ? ModuleDeclaration::InterfacePartition
                         : ModuleDeclaration::Interface)
            : (partition ? ModuleDeclaration::ImplementationPartition
                         : ModuleDeclaration::Unknown);
    } else if (u.provides || !u.requires_.empty()) {
        u.declaration = ModuleDeclaration::Unknown;
    }
    return u;
}

} // namespace mcpp::modgraph::p1689

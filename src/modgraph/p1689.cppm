// mcpp.modgraph.p1689 — compiler-driven module dep scan via P1689r5 (.ddi).
//
// Replaces (under MCPP_SCANNER=p1689) the regex-based scanner. Each source
// file gets fed to:
//
//   g++ -std=c++23 -fmodules -fdeps-format=p1689r5 \
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
// Spec: docs/27-p1689-dyndep.md.

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.modgraph.p1689;

import std;
import mcpp.modgraph.graph;
import mcpp.toolchain.detect;

export namespace mcpp::modgraph::p1689 {

struct DdiProvide {
    std::string logicalName;
    bool        isInterface = false;
};

struct DdiRule {
    std::string             primaryOutput;
    std::vector<DdiProvide> provides;
    std::vector<std::string> requires_;
};

// Parse a .ddi JSON body into the first rule's info.
// Tolerates the slightly unstable GCC formatting (variable whitespace).
std::expected<DdiRule, std::string> parse_ddi(std::string_view body);

// Run g++ on `source` and return the populated SourceUnit.
//   tmpDir : per-build scratch dir for .ddi/.dep/.o byproducts.
std::expected<SourceUnit, std::string>
scan_file(const std::filesystem::path&        source,
          const std::string&                  packageName,
          const mcpp::toolchain::Toolchain&   tc,
          const std::filesystem::path&        tmpDir);

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

bool run_capturing(const std::string& cmd, std::string& out) {
    std::array<char, 8192> buf{};
    out.clear();
    std::FILE* fp = ::popen(cmd.c_str(), "r");
    if (!fp) return false;
    while (std::fgets(buf.data(), buf.size(), fp)) out += buf.data();
    return ::pclose(fp) == 0;
}

std::string shell_escape(const std::filesystem::path& p) {
    std::string s = p.string();
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out += "'";
    return out;
}

} // namespace

std::expected<SourceUnit, std::string>
scan_file(const std::filesystem::path&        source,
          const std::string&                  packageName,
          const mcpp::toolchain::Toolchain&   tc,
          const std::filesystem::path&        tmpDir)
{
    std::error_code ec;
    std::filesystem::create_directories(tmpDir, ec);

    // One unique name per source — flat layout in tmpDir is fine since
    // the caller gives us a fresh dir per build.
    auto stem = source.filename().string();
    auto base = tmpDir / std::format("{}_{}",
        std::hash<std::string>{}(source.string()) % 1000000, stem);
    auto ddi = base; ddi += ".ddi";
    auto dep = base; dep += ".dep";
    auto obj = base; obj += ".o";

    std::string sysroot_flag;
    if (!tc.sysroot.empty()) {
        sysroot_flag = std::format(" --sysroot={}", shell_escape(tc.sysroot));
    }
    std::string cmd = std::format(
        "{} -std=c++23 -fmodules{}"
        " -fdeps-format=p1689r5"
        " -fdeps-file={}"
        " -fdeps-target={}"
        " -M -MM -MF {}"
        " -E {} -o {} 2>&1",
        shell_escape(tc.binaryPath),
        sysroot_flag,
        shell_escape(ddi),
        shell_escape(obj),
        shell_escape(dep),
        shell_escape(source),
        shell_escape(obj));

    std::string output;
    bool ok = run_capturing(cmd, output);
    if (!ok || !std::filesystem::exists(ddi)) {
        return std::unexpected(std::format(
            "p1689 scan failed for '{}':\n{}", source.string(), output));
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
    if (!rule->provides.empty()) {
        u.provides           = ModuleId{ rule->provides.front().logicalName };
        u.isModuleInterface = rule->provides.front().isInterface
                                || source.extension() == ".cppm";
    } else if (source.extension() == ".cpp" || source.extension() == ".cxx") {
        u.isImplementation = true;
    }
    for (auto& r : rule->requires_) {
        u.requires_.push_back(ModuleId{ r });
    }
    return u;
}

} // namespace mcpp::modgraph::p1689

// mcpp.dyndep — turn a list of P1689 .ddi files into a Ninja dyndep file
// (see docs/27 §四 for design and ninja-build.org/manual.html#_dyndep).
//
// Input layout (one .ddi per primary translation unit, produced by GCC's
// -fdeps-format=p1689r5):
//
//   .ddi.rules[i].primary-output         → the compile edge's main output
//   .ddi.rules[i].provides[*]            → BMI files this TU produces
//   .ddi.rules[i].requires[*]            → BMI files this TU consumes
//
// Output: a Ninja dyndep file mapping logical-name → BMI path
// (`gcm.cache/<sanitized>.gcm`, with ':' replaced by '-' to keep
// filename-safe).

module;
#include <cstdio>

export module mcpp.dyndep;

import std;

export namespace mcpp::dyndep {

// One scanned source: derived from a single .ddi.
struct UnitInfo {
    std::filesystem::path        primaryOutput;   // e.g. "obj/foo.m.o"
    std::vector<std::string>     provides;        // logical names
    std::vector<std::string>     requires_;       // logical names
};

// Stable convention: gcm.cache/<sanitized>.gcm, where ':' → '-'.
// Centralized here + ninja_backend so the two stay in sync.
std::string bmi_basename(std::string_view logicalName);

// Parse a single .ddi JSON body to a UnitInfo. Returns unexpected on JSON error.
std::expected<UnitInfo, std::string> parse_ddi(std::string_view body);

// Emit a complete Ninja dyndep file body for the given units.
// Each compile edge gets one `build` line declaring its provided BMIs as
// implicit outputs and its required BMIs as implicit inputs.
//
// `stdImports` is the set of logical names that are pre-staged into the
// project's gcm.cache/ (typically just "std" / "std.compat") — they are
// resolved to gcm.cache/<name>.gcm regardless of whether any unit
// "provides" them.
std::string emit_dyndep(const std::vector<UnitInfo>&     units,
                        const std::set<std::string>&     stdImports);

// Convenience: read .ddi files from disk, parse them, and emit a dyndep
// file string. Returns unexpected if any .ddi fails to parse.
std::expected<std::string, std::string>
emit_dyndep_from_files(const std::vector<std::filesystem::path>& ddiPaths,
                       const std::set<std::string>& stdImports);

} // namespace mcpp::dyndep

namespace mcpp::dyndep {

namespace {

void skip_ws(std::string_view s, std::size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' ||
                            s[i] == '\n' || s[i] == '\r'))
        ++i;
}

std::expected<std::string, std::string>
parse_string(std::string_view s, std::size_t& i) {
    if (i >= s.size() || s[i] != '"')
        return std::unexpected("expected '\"'");
    ++i;
    std::string out;
    while (i < s.size()) {
        char c = s[i++];
        if (c == '"') return out;
        if (c == '\\' && i < s.size()) {
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

void skip_value(std::string_view s, std::size_t& i) {
    skip_ws(s, i);
    if (i >= s.size()) return;
    char c = s[i];
    if (c == '"') { auto _ = parse_string(s, i); return; }
    if (c == '{' || c == '[') {
        char open = c, close = (c == '{') ? '}' : ']';
        int d = 0; bool in_str = false;
        while (i < s.size()) {
            char cc = s[i];
            if (in_str) {
                if (cc == '\\' && i + 1 < s.size()) { i += 2; continue; }
                if (cc == '"') in_str = false;
                ++i; continue;
            }
            if (cc == '"') { in_str = true; ++i; continue; }
            if (cc == open) ++d;
            else if (cc == close) { --d; ++i; if (d == 0) return; continue; }
            ++i;
        }
        return;
    }
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']'
           && s[i] != ' ' && s[i] != '\n' && s[i] != '\r' && s[i] != '\t')
        ++i;
}

// At s[start] = '{', find the value of <key> at THIS object's depth. Returns
// index just after ':' on hit, npos otherwise.
std::size_t find_key(std::string_view s, std::size_t start, std::string_view key) {
    if (start >= s.size() || s[start] != '{') return std::string_view::npos;
    std::size_t i = start + 1;
    while (i < s.size()) {
        skip_ws(s, i);
        if (i >= s.size() || s[i] == '}') return std::string_view::npos;
        if (s[i] == ',') { ++i; continue; }
        if (s[i] != '"') { ++i; continue; }
        auto k = parse_string(s, i);
        if (!k) return std::string_view::npos;
        skip_ws(s, i);
        if (i >= s.size() || s[i] != ':') continue;
        ++i;
        skip_ws(s, i);
        if (*k == key) return i;
        skip_value(s, i);
        skip_ws(s, i);
        if (i < s.size() && s[i] == ',') ++i;
    }
    return std::string_view::npos;
}

} // namespace

std::string bmi_basename(std::string_view logicalName) {
    std::string out;
    out.reserve(logicalName.size() + 4);
    for (char c : logicalName) out.push_back(c == ':' ? '-' : c);
    out += ".gcm";
    return out;
}

std::expected<UnitInfo, std::string> parse_ddi(std::string_view body) {
    std::size_t i = 0;
    skip_ws(body, i);
    if (i >= body.size() || body[i] != '{')
        return std::unexpected("ddi: expected top-level object");

    auto rulesAt = find_key(body, i, "rules");
    if (rulesAt == std::string_view::npos)
        return std::unexpected("ddi: missing 'rules'");
    skip_ws(body, rulesAt);
    if (rulesAt >= body.size() || body[rulesAt] != '[')
        return std::unexpected("ddi: 'rules' is not an array");

    std::size_t j = rulesAt + 1;
    skip_ws(body, j);
    if (j >= body.size() || body[j] != '{')
        return std::unexpected("ddi: 'rules' is empty");

    UnitInfo info;

    if (auto poAt = find_key(body, j, "primary-output");
        poAt != std::string_view::npos)
    {
        skip_ws(body, poAt);
        if (poAt < body.size() && body[poAt] == '"') {
            auto v = parse_string(body, poAt);
            if (v) info.primaryOutput = *v;
        }
    }

    auto pull_logical_names = [&](std::string_view key, std::vector<std::string>& dst) {
        auto at = find_key(body, j, key);
        if (at == std::string_view::npos) return;
        skip_ws(body, at);
        if (at >= body.size() || body[at] != '[') return;
        std::size_t k = at + 1;
        while (k < body.size()) {
            skip_ws(body, k);
            if (k >= body.size() || body[k] == ']') break;
            if (body[k] == '{') {
                auto lnAt = find_key(body, k, "logical-name");
                if (lnAt != std::string_view::npos) {
                    skip_ws(body, lnAt);
                    if (lnAt < body.size() && body[lnAt] == '"') {
                        auto v = parse_string(body, lnAt);
                        if (v) dst.push_back(*v);
                    }
                }
            }
            // Step past this object.
            int d = 0; bool in_str = false;
            while (k < body.size()) {
                char cc = body[k];
                if (in_str) {
                    if (cc == '\\' && k + 1 < body.size()) { k += 2; continue; }
                    if (cc == '"') in_str = false;
                    ++k; continue;
                }
                if (cc == '"') { in_str = true; ++k; continue; }
                if (cc == '{') ++d;
                else if (cc == '}') { --d; ++k; if (d == 0) break; continue; }
                ++k;
            }
            skip_ws(body, k);
            if (k < body.size() && body[k] == ',') { ++k; continue; }
            if (k < body.size() && body[k] == ']') break;
        }
    };
    pull_logical_names("provides",  info.provides);
    pull_logical_names("requires",  info.requires_);

    return info;
}

std::string emit_dyndep(const std::vector<UnitInfo>&     units,
                        const std::set<std::string>&     stdImports)
{
    // Per ninja's dyndep contract: the dyndep file's `build <out>: dyndep`
    // line augments the main build edge with **additional implicit inputs**
    // (and may declare new implicit outputs). We declare implicit outputs
    // statically in build.ninja (mcpp knows them at plan time from the
    // BuildPlan), so the dyndep file here adds inputs only.
    std::string out = "ninja_dyndep_version = 1\n";

    for (auto& u : units) {
        if (u.primaryOutput.empty()) continue;

        std::string line = "build " + u.primaryOutput.string() + ": dyndep";

        bool firstImplicit = true;
        auto add_implicit = [&](const std::string& path) {
            if (firstImplicit) { line += " |"; firstImplicit = false; }
            line += " " + path;
        };
        for (auto& r : u.requires_) {
            bool selfProvides = false;
            for (auto& p : u.provides) if (p == r) { selfProvides = true; break; }
            if (selfProvides) continue;
            add_implicit("gcm.cache/" + bmi_basename(r));
        }
        line += "\n  restat = 1\n";
        out += line;
        (void)stdImports;
    }

    return out;
}

std::expected<std::string, std::string>
emit_dyndep_from_files(const std::vector<std::filesystem::path>& ddiPaths,
                       const std::set<std::string>& stdImports)
{
    std::vector<UnitInfo> units;
    units.reserve(ddiPaths.size());
    for (auto& p : ddiPaths) {
        std::ifstream is(p);
        if (!is) {
            return std::unexpected(std::format("cannot read '{}'", p.string()));
        }
        std::string body((std::istreambuf_iterator<char>(is)), {});
        auto u = parse_ddi(body);
        if (!u) {
            return std::unexpected(std::format("{}: {}", p.string(), u.error()));
        }
        units.push_back(std::move(*u));
    }
    return emit_dyndep(units, stdImports);
}

} // namespace mcpp::dyndep

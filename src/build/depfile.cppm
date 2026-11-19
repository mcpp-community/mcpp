// mcpp.build.depfile — the first record of a GNU depfile.
//
// GCC's `-fmodules` adds records to any `-MMD`/`-MF` depfile of a unit that
// imports or provides a module: the BMI "having its own inputs", a phony
// `<name>.c++-module` target, and a `CXX_IMPORTS +=` line. ninja's depfile
// loader rejects the first of those, because the BMI is also a declared output
// of the same edge (ninja_backend.cppm, the note above `gnuDepfile`). The
// textual `#include` graph, which is all header tracking needs, is the FIRST
// record: the target line and its indented continuation lines.
//
// On POSIX an awk program keeps it (`NR==1{print;next} /^[^ ]/{exit}
// {print}`), chained after the compile in the rule's shell command. Windows has
// no shell to chain it in, so `mcpp depfile-filter` runs the compile and
// applies this function; the two are the same rule, stated once each for their
// host, and `tests/unit/test_depfile.cpp` holds them to the same output.

export module mcpp.build.depfile;

import std;
import nlohmann.json;

export namespace mcpp::build::depfile {

// The first line, then every following line up to the first one that starts
// with a character other than a space. An empty line does not end the record,
// as it does not for the awk program.
inline std::string first_record(std::string_view raw) {
    std::string out;
    std::size_t at = 0;
    bool first = true;
    while (at < raw.size()) {
        auto nl = raw.find('\n', at);
        const auto end = nl == std::string_view::npos ? raw.size() : nl + 1;
        const auto line = raw.substr(at, end - at);
        if (!first && !line.empty() && line.front() != ' ' && line.front() != '\n'
            && line.front() != '\r')
            break;
        out.append(line);
        first = false;
        at = end;
    }
    return out;
}

// THE FILES cl.exe'S RECORD NAMES (mcpp#970, design 2026-11-19 §20 T1).
//
// cl.exe states the files a compile read in the JSON record
// `/sourceDependencies <file>` writes, in UTF-8 and the same in every language
// and console code page (measured on windows-2022, probe mcpp#971). The
// executor gives them to ninja as lines of its own output, each `kInputsPrefix`
// and a path, which the edge reads as `deps = msvc` with that prefix: ninja
// takes the whole rest of such a line, where its reader of a Make rule ends a
// name at `'`, `&`, `;`, `^` or a backquote, which a Windows path may hold
// (measured with ninja 1.12.1). The paths:
//   * a header under one of `toolchainDirs` (the INCLUDE and EXTERNAL_INCLUDE
//     of the toolchain's environment) is left out, as -MMD leaves out a system
//     header;
//   * a header under `buildDir`, the directory the compile ran in, is named as
//     `buildDir` and then each name below it as the file system spells it
//     (`spelled`): the record spells every path in lower case, and a generated
//     header is another edge's output, whose node keeps its case;
//   * every other header is named as the record spells it, which the file
//     system of a cl.exe host opens without regard to case.
// Paths are compared in the generic spelling (`/`), without ASCII case when
// `foldCase`; a path that differs from `buildDir` in a letter beyond ASCII is
// compared again with its names as the file system spells them. Not inline: an
// inline function of an imported module that iterates a string fails to
// compile in the importer under GCC 16.1.
// The prefix ninja is given (`msvc_deps_prefix`): it skips the spaces after it,
// so a line is the prefix, a space and the path.
inline constexpr std::string_view kInputsPrefix = "mcpp.inputs:";

struct SourceRecord {
    std::string_view         record;          // what /sourceDependencies wrote
    std::string              buildDir;        // generic spelling
    std::vector<std::string> toolchainDirs;   // generic spellings
    bool                     foldCase = true;
    // The entry of `dir` that `name` names, as the file system spells it.
    std::function<std::string(const std::string& dir, const std::string& name)> spelled;
};

namespace detail {

std::string generic(std::string_view path) {
    std::string out(path);
    std::ranges::replace(out, '\\', '/');
    while (out.size() > 1 && out.back() == '/' && out[out.size() - 2] != ':') out.pop_back();
    return out;
}

bool same_text(std::string_view a, std::string_view b, bool fold) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (fold && x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (fold && y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

// The part of `path` below `dir`, when `path` is beneath it.
std::optional<std::string_view> below(std::string_view path, std::string_view dir, bool fold) {
    if (dir.empty() || path.size() <= dir.size() + 1) return std::nullopt;
    if (!same_text(path.substr(0, dir.size()), dir, fold) || path[dir.size()] != '/') return std::nullopt;
    return path.substr(dir.size() + 1);
}

// `path` with its first `count` names after the root as the file system spells
// them, and the rest as written; empty when it has fewer.
std::string respelled_prefix(std::string_view path, std::size_t count, const SourceRecord& in) {
    const auto root = path.find('/');
    if (root == std::string_view::npos) return {};
    std::string out(path.substr(0, root + 1));
    std::size_t at = root + 1;
    for (std::size_t n = 0; n < count; ++n) {
        if (at >= path.size()) return {};
        const auto end = std::min(path.find('/', at), path.size());
        const std::string name(path.substr(at, end - at));
        out += (n == 0 ? "" : "/") + in.spelled(out, name);
        at = end + 1;
    }
    return at < path.size() ? out + '/' + std::string(path.substr(at)) : out;
}

} // namespace detail

std::expected<std::vector<std::string>, std::string> inputs_from_source_dependencies(const SourceRecord& in) {
    const auto doc = nlohmann::json::parse(in.record, nullptr, false);
    if (doc.is_discarded() || !doc.is_object())
        return std::unexpected(std::string("the record is not a JSON object"));
    const auto data = doc.find("Data");
    if (data == doc.end() || !data->is_object() || !data->contains("Includes") || !(*data)["Includes"].is_array())
        return std::unexpected(std::string("the record has no Data.Includes list"));
    const std::string buildDir = detail::generic(in.buildDir);
    const auto buildDepth = static_cast<std::size_t>(std::ranges::count(buildDir, '/'));
    std::vector<std::string> system;
    for (auto const& d : in.toolchainDirs)
        if (!d.empty()) system.push_back(detail::generic(d));
    std::vector<std::string> out;
    for (auto const& entry : (*data)["Includes"]) {
        if (!entry.is_string()) return std::unexpected(std::string("an entry of Data.Includes is not a path"));
        std::string path = detail::generic(entry.get<std::string>());
        if (std::ranges::any_of(system, [&](const std::string& d) { return detail::below(path, d, in.foldCase).has_value(); }))
            continue;
        auto rest = detail::below(path, buildDir, in.foldCase);
        if (!rest && in.foldCase && in.spelled && std::ranges::any_of(path, [](char c) { return (c & 0x80) != 0; }))
            if (auto spelled = detail::respelled_prefix(path, buildDepth, in);
                !spelled.empty() && detail::below(spelled, buildDir, true)) {
                path = std::move(spelled);
                rest = detail::below(path, buildDir, true);
            }
        std::string named = path;
        if (rest && in.spelled) {
            named = buildDir;
            for (auto part : std::views::split(*rest, '/')) {
                const std::string name(std::string_view(part.begin(), part.end()));
                named += '/' + in.spelled(named, name);
            }
        }
        out.push_back(std::move(named));
    }
    return out;
}

} // namespace mcpp::build::depfile

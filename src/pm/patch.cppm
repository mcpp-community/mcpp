// A dependency's patches, applied (design 2026-11-02, part 3, §14.1).
//
// A PATCHED LIBRARY IS ANOTHER LIBRARY. A dependency edge may carry `patches`,
// unified diffs that are applied in order to the verified source of the
// package; the result is a VARIANT, whose identity is the package, its version
// and the patch set, and which has a source tree of its own under the mcpp
// home (`cache/src/<package>/<version>+p<digest>`), so that its build, its
// BMIs and its cache never meet the unpatched package's.
//
// THE APPLIER IS EXACT AND IS mcpp's OWN. A hunk applies where its header says
// and only if every line of context and every line it removes is there, byte
// for byte; there is no search for a nearby place and no fuzz, so a patch
// gives the same tree on every machine or none. It reads what `diff -u` and
// `git format-patch` write, removes the one leading directory of each path
// (`a/` and `b/`), and transfers bytes as they are: a line ending is part of
// its line, and nothing is converted. No `patch` or `git` program is used.
export module mcpp.pm.patch;

import std;
import mcpp.pack.distribution;   // sha256

export namespace mcpp::pm::patch {

// The SHA-256 of one patch file's bytes, or nothing when it cannot be read.
std::optional<std::string> file_sha256(const std::filesystem::path& patch);

// The set's name: the SHA-256 over each patch's SHA-256 and a newline, in
// order. Empty for no patches. The paths are not part of it: one set of
// patches carried by two packages is one variant.
std::expected<std::string, std::string> set_digest(const std::vector<std::string>& patches);

// Applies one patch's text to the tree at `root`. `name` is how a failure
// names the patch.
std::expected<void, std::string> apply(const std::filesystem::path& root,
                                       std::string_view text, std::string_view name);

// The variant's tree: `source` copied to `variant` and the patches applied in
// order, unless `variant` already holds the result for this digest. Written
// beside `variant` and renamed into place, so a tree that exists is whole.
std::expected<void, std::string> materialize(const std::filesystem::path& source,
                                             const std::vector<std::string>& patches,
                                             std::string_view digest,
                                             const std::filesystem::path& variant);

// The directory name of a variant of `version`: `<version>+p<first 12 of the
// digest>`. Short, because Windows paths have a length.
std::string variant_dir_name(std::string_view version, std::string_view digest);

}  // namespace mcpp::pm::patch

namespace mcpp::pm::patch {

namespace {

namespace fs = std::filesystem;

std::optional<std::string> read_bytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return std::move(ss).str();
}

bool write_bytes(const fs::path& p, std::string_view bytes) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    // A link is replaced by a file rather than written through.
    if (fs::is_symlink(fs::symlink_status(p, ec))) fs::remove(p, ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

// A text as lines, each without its '\n' and with a flag saying whether one
// ended it (only the last line may lack it). A '\r' stays in its line.
struct line { std::string text; bool newline = true; };

std::vector<line> split_lines(std::string_view s) {
    std::vector<line> out;
    std::size_t at = 0;
    while (at < s.size()) {
        const auto nl = s.find('\n', at);
        if (nl == std::string_view::npos) { out.push_back({std::string(s.substr(at)), false}); break; }
        out.push_back({std::string(s.substr(at, nl - at)), true});
        at = nl + 1;
    }
    return out;
}

std::string join_lines(const std::vector<line>& ls) {
    std::string out;
    for (auto const& l : ls) { out += l.text; if (l.newline) out += '\n'; }
    return out;
}

struct hunk {
    std::size_t oldStart = 0, oldCount = 0, newStart = 0, newCount = 0;
    std::string header;
    std::vector<line> oldLines, newLines;
};

struct file_patch {
    std::string oldPath, newPath;   // after the leading directory is removed; "" for /dev/null
    std::vector<hunk> hunks;
};

// "--- a/x/y.c\t2026-..." -> "x/y.c"; "/dev/null" -> "".
std::expected<std::string, std::string> header_path(std::string_view rest) {
    if (auto tab = rest.find('\t'); tab != std::string_view::npos) rest = rest.substr(0, tab);
    while (!rest.empty() && (rest.back() == '\r' || rest.back() == ' ')) rest.remove_suffix(1);
    if (rest == "/dev/null") return std::string{};
    if (rest.size() >= 2 && rest.front() == '"')
        return std::unexpected(std::string("a quoted path is not supported"));
    const auto slash = rest.find('/');
    if (slash == std::string_view::npos)
        return std::unexpected(std::format("'{}' carries no leading directory to remove", rest));
    std::string path(rest.substr(slash + 1));
    if (path.empty() || path.find("..") != std::string::npos || path.front() == '/')
        return std::unexpected(std::format("'{}' does not name a file of the package", rest));
    return path;
}

bool parse_range(std::string_view s, std::size_t& start, std::size_t& count) {
    count = 1;
    const auto comma = s.find(',');
    auto num = [](std::string_view t, std::size_t& v) {
        if (t.empty()) return false;
        v = 0;
        for (char c : t) { if (c < '0' || c > '9') return false; v = v * 10 + static_cast<std::size_t>(c - '0'); }
        return true;
    };
    if (comma == std::string_view::npos) return num(s, start);
    return num(s.substr(0, comma), start) && num(s.substr(comma + 1), count);
}

std::expected<std::vector<file_patch>, std::string> parse(std::string_view text) {
    const auto lines = split_lines(text);
    std::vector<file_patch> files;
    std::size_t i = 0;
    while (i < lines.size()) {
        std::string_view l = lines[i].text;
        if (l.starts_with("GIT binary patch") || l.starts_with("Binary files "))
            return std::unexpected(std::string("a binary change is not supported"));
        if (l.starts_with("rename from ") || l.starts_with("copy from "))
            return std::unexpected(std::string("a rename or a copy is not supported; state it as a removal and an addition"));
        if (!l.starts_with("--- ") || i + 1 >= lines.size() || !std::string_view(lines[i + 1].text).starts_with("+++ ")) {
            ++i;
            continue;
        }
        file_patch fp;
        auto oldPath = header_path(l.substr(4));
        if (!oldPath) return std::unexpected(oldPath.error());
        auto newPath = header_path(std::string_view(lines[i + 1].text).substr(4));
        if (!newPath) return std::unexpected(newPath.error());
        fp.oldPath = *oldPath;
        fp.newPath = *newPath;
        if (fp.oldPath.empty() && fp.newPath.empty())
            return std::unexpected(std::string("a file both absent and absent"));
        if (!fp.oldPath.empty() && !fp.newPath.empty() && fp.oldPath != fp.newPath)
            return std::unexpected(std::format("'{}' and '{}' name two files; a rename is not supported",
                                               fp.oldPath, fp.newPath));
        i += 2;
        while (i < lines.size() && std::string_view(lines[i].text).starts_with("@@ ")) {
            hunk h;
            h.header = lines[i].text;
            std::string_view hd = lines[i].text;
            const auto end = hd.find(" @@", 3);
            if (end == std::string_view::npos)
                return std::unexpected(std::format("'{}' is not a hunk header", hd));
            std::string_view ranges = hd.substr(3, end - 3);
            const auto space = ranges.find(' ');
            if (space == std::string_view::npos || ranges[0] != '-' || ranges[space + 1] != '+'
                || !parse_range(ranges.substr(1, space - 1), h.oldStart, h.oldCount)
                || !parse_range(ranges.substr(space + 2), h.newStart, h.newCount))
                return std::unexpected(std::format("'{}' is not a hunk header", hd));
            ++i;
            std::size_t seenOld = 0, seenNew = 0;
            char last = 0;
            while (i < lines.size() && (seenOld < h.oldCount || seenNew < h.newCount
                                        || std::string_view(lines[i].text).starts_with("\\"))) {
                std::string_view body = lines[i].text;
                if (body.starts_with("\\")) {
                    // "\ No newline at end of file": the line before has none.
                    if (last == '-' || last == ' ') { if (!h.oldLines.empty()) h.oldLines.back().newline = false; }
                    if (last == '+' || last == ' ') { if (!h.newLines.empty()) h.newLines.back().newline = false; }
                    ++i;
                    continue;
                }
                const char kind = body.empty() ? ' ' : body[0];
                const std::string rest(body.empty() ? std::string_view{} : body.substr(1));
                if (kind == ' ') { h.oldLines.push_back({rest, true}); h.newLines.push_back({rest, true}); ++seenOld; ++seenNew; }
                else if (kind == '-') { h.oldLines.push_back({rest, true}); ++seenOld; }
                else if (kind == '+') { h.newLines.push_back({rest, true}); ++seenNew; }
                else return std::unexpected(std::format("'{}' ends before its {} and {} lines", h.header, h.oldCount, h.newCount));
                last = kind;
                ++i;
            }
            if (seenOld != h.oldCount || seenNew != h.newCount)
                return std::unexpected(std::format("'{}' ends before its {} and {} lines", h.header, h.oldCount, h.newCount));
            fp.hunks.push_back(std::move(h));
        }
        if (fp.hunks.empty())
            return std::unexpected(std::format("the change to '{}' has no hunk",
                                               fp.newPath.empty() ? fp.oldPath : fp.newPath));
        files.push_back(std::move(fp));
    }
    if (files.empty()) return std::unexpected(std::string("no change is described"));
    return files;
}

std::string shown(const line& l) {
    std::string s = l.text;
    if (s.size() > 60) s = s.substr(0, 57) + "...";
    return s;
}

}  // namespace

std::optional<std::string> file_sha256(const fs::path& patch) {
    auto bytes = read_bytes(patch);
    if (!bytes) return std::nullopt;
    return mcpp::pack::distribution::sha256(*bytes);
}

std::expected<std::string, std::string> set_digest(const std::vector<std::string>& patches) {
    if (patches.empty()) return std::string{};
    std::string acc;
    for (auto const& p : patches) {
        auto one = file_sha256(p);
        if (!one) return std::unexpected(std::format("the patch '{}' cannot be read", p));
        acc += *one;
        acc += '\n';
    }
    return mcpp::pack::distribution::sha256(acc);
}

std::string variant_dir_name(std::string_view version, std::string_view digest) {
    return std::format("{}+p{}", version, digest.substr(0, std::min<std::size_t>(12, digest.size())));
}

std::expected<void, std::string> apply(const fs::path& root, std::string_view text, std::string_view name) {
    auto parsed = parse(text);
    if (!parsed) return std::unexpected(std::format("patch '{}': {}", name, parsed.error()));
    for (auto const& fp : *parsed) {
        const std::string rel = fp.newPath.empty() ? fp.oldPath : fp.newPath;
        const fs::path file = root / fs::path(rel);
        std::vector<line> original;
        if (!fp.oldPath.empty()) {
            auto bytes = read_bytes(file);
            if (!bytes) return std::unexpected(std::format("patch '{}': '{}' is not in the package", name, rel));
            original = split_lines(*bytes);
        } else {
            std::error_code ec;
            if (fs::exists(file, ec))
                return std::unexpected(std::format("patch '{}': '{}' is created and already exists", name, rel));
        }
        std::vector<line> result;
        std::size_t cursor = 0;   // the next original line not yet copied
        std::size_t index = 0;
        for (auto const& h : fp.hunks) {
            ++index;
            // Where the old lines begin, 0-based. A hunk that removes nothing
            // states the line AFTER which it adds; one that does states the
            // first line it reads.
            const std::size_t at = h.oldCount == 0 ? h.oldStart : h.oldStart - 1;
            if (h.oldCount > 0 && h.oldStart == 0)
                return std::unexpected(std::format("patch '{}': '{}' hunk {} ({}) starts at line 0", name, rel, index, h.header));
            if (at < cursor)
                return std::unexpected(std::format("patch '{}': '{}' hunk {} ({}) overlaps the hunk before it", name, rel, index, h.header));
            if (at + h.oldLines.size() > original.size())
                return std::unexpected(std::format(
                    "patch '{}': '{}' hunk {} ({}) does not apply: the file has {} lines", name, rel, index, h.header, original.size()));
            for (std::size_t k = 0; k < h.oldLines.size(); ++k) {
                auto const& want = h.oldLines[k];
                auto const& have = original[at + k];
                if (want.text != have.text || want.newline != have.newline)
                    return std::unexpected(std::format(
                        "patch '{}': '{}' hunk {} ({}) does not apply at line {}: it expects '{}'{} and the file has '{}'{}",
                        name, rel, index, h.header, at + k + 1, shown(want), want.newline ? "" : " (no newline)",
                        shown(have), have.newline ? "" : " (no newline)"));
            }
            for (std::size_t k = cursor; k < at; ++k) result.push_back(original[k]);
            for (auto const& l : h.newLines) result.push_back(l);
            cursor = at + h.oldLines.size();
        }
        for (std::size_t k = cursor; k < original.size(); ++k) result.push_back(original[k]);
        std::error_code ec;
        if (fp.newPath.empty()) {
            if (!result.empty())
                return std::unexpected(std::format("patch '{}': '{}' is removed and keeps lines the patch does not state", name, rel));
            fs::remove(file, ec);   // a link is removed, never what it names
            if (ec) return std::unexpected(std::format("patch '{}': '{}' cannot be removed: {}", name, rel, ec.message()));
            continue;
        }
        if (!write_bytes(file, join_lines(result)))
            return std::unexpected(std::format("patch '{}': '{}' cannot be written", name, rel));
    }
    return {};
}

std::expected<void, std::string> materialize(const fs::path& source,
                                             const std::vector<std::string>& patches,
                                             std::string_view digest,
                                             const fs::path& variant) {
    const fs::path marker = variant / ".mcpp-variant";
    std::error_code ec;
    if (auto have = read_bytes(marker); have && *have == std::string(digest) + "\n") {
        // Used now: the cache's collection by last use reads this time.
        fs::last_write_time(marker, fs::file_time_type::clock::now(), ec);
        return {};
    }

    // Written beside its place and renamed into it, so that a variant that
    // exists is whole: a build interrupted while patching leaves a directory
    // nothing reads.
    const fs::path work = variant.parent_path() / (variant.filename().string() + ".partial");
    fs::remove_all(work, ec);
    fs::create_directories(work.parent_path(), ec);
    // THE COPY IS OF FILES, NOT OF LINKS. A store may hold a package's version
    // directory, or files in it, as links into another store; a link copied as
    // a link would have the patch written through it into the source every
    // other build reads. Links are followed, so the variant's tree is its own.
    const auto real = fs::weakly_canonical(source, ec);
    if (ec) return std::unexpected(std::format("the source '{}' cannot be resolved: {}",
                                               source.string(), ec.message()));
    fs::copy(real, work, fs::copy_options::recursive, ec);
    if (ec) return std::unexpected(std::format("the source '{}' cannot be copied for its variant: {}",
                                               source.string(), ec.message()));
    for (auto const& p : patches) {
        auto text = read_bytes(p);
        if (!text) { fs::remove_all(work, ec); return std::unexpected(std::format("the patch '{}' cannot be read", p)); }
        if (auto r = apply(work, *text, fs::path(p).filename().string()); !r) {
            fs::remove_all(work, ec);
            return std::unexpected(std::format("{} (from {})", r.error(), p));
        }
    }
    if (!write_bytes(work / ".mcpp-variant", std::string(digest) + "\n")) {
        fs::remove_all(work, ec);
        return std::unexpected(std::format("the variant '{}' cannot be recorded", variant.string()));
    }
    fs::remove_all(variant, ec);
    fs::rename(work, variant, ec);
    if (ec) {
        // Another build made the same variant meanwhile: its tree is the same.
        if (auto have = read_bytes(marker); have && *have == std::string(digest) + "\n") {
            fs::remove_all(work, ec);
            return {};
        }
        return std::unexpected(std::format("the variant '{}' cannot be placed: {}", variant.string(), ec.message()));
    }
    return {};
}

}  // namespace mcpp::pm::patch

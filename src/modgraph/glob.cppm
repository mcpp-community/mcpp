// mcpp.modgraph.glob — the ONE path-glob matcher.
//
// It lived in scanner.cppm's anonymous namespace, which was fine while the
// scanner was its only user. #359 gives `build.mcpp` a glob-shaped INPUT
// (`rerun-if-changed-glob`), whose fingerprint must select exactly the files a
// `sources = [...]` glob would. Two matchers that "should" agree about `**` is
// the shape this codebase keeps paying for, so there is one.

export module mcpp.modgraph.glob;

import std;

export namespace mcpp::modgraph {

// Does `candidate` match `glob`, interpreted relative to `root`?
//
// Supports "**" (any number of directory levels) and "*" (within one segment).
// LEXICAL relative: fs::relative() canonicalizes, which would resolve a path
// reached through a directory symlink back to its real location and break the
// match — a glob is about where a file appears in the tree, not where its bits
// live.
bool path_matches_glob(const std::filesystem::path& candidate,
                       const std::filesystem::path& root,
                       std::string_view             glob)
{
    std::string rel;
    try {
        rel = candidate.lexically_relative(root).generic_string();
    } catch (const std::exception&) {
        // MSVC's narrow conversion throws std::system_error when the native
        // (wide) name has no spelling in the ANSI codepage (e.g. a CJK
        // filename on an en-US host — mcpp#230 hit this on an issue template
        // inside a walked index tree). Such a name can never be spelled in a
        // glob or a compile command either: not a match, and never a reason to
        // tear down the whole build.
        return false;
    }

    auto match = [](std::string_view s, std::string_view p) -> bool {
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

} // namespace mcpp::modgraph

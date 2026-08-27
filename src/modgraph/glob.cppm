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

// Convert a manifest-style path or glob prefix (always spelled with the
// generic `/` separator) to the platform's native spelling.
//
// MSVC's std::filesystem::path preserves the separators of the string it
// was constructed from instead of normalizing them, so wrapping a raw
// `generated/modules` in a path and joining it with `root / p` yields the
// MIXED `C:\...\generated/modules` — and the directory-walk children built
// on top of that stay mixed. `.string()` then carries the mixed form into
// `compile_commands.json` (its `file` / `-c` fields), which CLion refuses
// to parse. Ninja never notices because it renders everything via
// generic_string(); the CDB is the first `.string()` consumer.
//
// POSIX is untouched (`make_preferred()` is a no-op there, and it is also
// safe for already-native Windows input, which never contains `/`).
std::filesystem::path native_path_from_generic(std::string_view s) {
    std::filesystem::path p(s);
    p.make_preferred();
    return p;
}

// ─── narrowing a walk-derived path ────────────────────────────────────────
//
// THE ONE PLACE a path that came out of a directory walk becomes a narrow
// string. Everything else in the tree calls this; nothing calls
// `.string()` / `.generic_string()` on such a path directly.
//
// Why it needs to exist at all: on Windows `path::string()` converts the
// native (wide) name through the process's ANSI code page and THROWS
// `std::system_error` when a character has no spelling there —
// "No mapping for the Unicode character exists in the target multi-byte code
// page". Off Windows the same call is a copy that cannot fail, so this whole
// hazard is invisible on Linux and macOS — including to their tests.
//
// It has cost two incidents, wearing a different mask each time:
//
//   #230  a walked index tree held `bug-report---问题反馈.md`; the throw
//         escaped to std::terminate → `__fastfail(0xC0000409)` → git-bash
//         reported a bare **exit 127**, which reads as "command not found".
//   #516  cpp-httplib ships `test/www/日本語Dir/`, and the `include_dirs =
//         { "*" }` convention walks the whole extracted tarball; the throw
//         escaped to main()'s catch as `internal: unhandled exception`,
//         which reads as an **extraction/encoding bug in the downloader**.
//
// #231 hardened three sites against it and missed a fourth
// (`is_excluded_walk_dir`, which runs one line EARLIER in the same walk) —
// which is why this is now a single function with a CI gate behind it
// (tools/check-narrow-conversions.sh) rather than a fourth try/catch.
//
// nullopt means: this path cannot be named in any string we hand to a
// compiler, a build file, or a glob. Skip it — and record it, because
// "silently not built" is exactly where this class of bug hides.
std::optional<std::string> try_narrow(const std::filesystem::path& p) {
    try {
        return p.generic_string();
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// Record that something had to be skipped because `try_narrow` could not name
// it. Deduplicated to the nearest ANCESTOR that CAN be named: one unreadable
// subtree produces one record, not one per file.
//
// What is stored is that ancestor — never the offending name, which by
// definition cannot be put into a message without throwing the very exception
// this module exists to avoid. (Diagnostic code walking into its own trap is
// the most likely way this regresses.)
void note_unnarrowable_path(const std::filesystem::path& p);

// Take and clear this run's records.
//
// modgraph is a leaf layer — no module under `src/modgraph/` or
// `src/manifest/` imports `mcpp.ui` or `mcpp.diag` — so it RECORDS and the
// CLI reports. Drained in exactly one place (`cli::run`'s scope guard), which
// is what keeps "recorded but never shown" from becoming the next silent
// failure. See docs & AGENTS.md.
std::vector<std::string> take_unnarrowable_paths();

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
    // lexically_relative is pure path arithmetic and cannot throw; the
    // narrowing is the part that can, so it is the part that goes through
    // try_narrow.
    auto rel = try_narrow(candidate.lexically_relative(root));
    if (!rel) {
        // A name the code page cannot spell can never match a glob (a glob is
        // a narrow string) and could never reach a compile command either.
        // Not a match — and not a reason to tear down the whole build.
        note_unnarrowable_path(candidate);
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
    return match(*rel, glob);
}

} // namespace mcpp::modgraph

// ── implementation ──────────────────────────────────────────────────────────

namespace mcpp::modgraph {
namespace {

// A run's worth of unnarrowable subtrees, keyed by their nearest spellable
// ancestor. `std::set` so the report comes out in a stable order regardless of
// directory-enumeration order, which the standard leaves unspecified.
std::mutex             g_unnarrowableMu;
std::set<std::string>  g_unnarrowable;

}  // namespace

void note_unnarrowable_path(const std::filesystem::path& p) {
    // Climb to the first ancestor this code page CAN spell. `p` itself fails
    // by construction; usually exactly one component is at fault, so the
    // parent already succeeds.
    std::string anchor;
    for (auto dir = p.parent_path();; dir = dir.parent_path()) {
        if (auto s = try_narrow(dir)) { anchor = std::move(*s); break; }
        if (dir.parent_path() == dir) break;   // reached the root, still unspellable
    }
    // Every component was unspellable (or `p` was a bare relative name). Say so
    // rather than reporting an empty path, which reads as a bug in the report.
    if (anchor.empty()) anchor = "(a path this code page cannot spell)";

    std::lock_guard lk(g_unnarrowableMu);
    g_unnarrowable.insert(std::move(anchor));
}

std::vector<std::string> take_unnarrowable_paths() {
    std::lock_guard lk(g_unnarrowableMu);
    std::vector<std::string> out(g_unnarrowable.begin(), g_unnarrowable.end());
    g_unnarrowable.clear();
    return out;
}

}  // namespace mcpp::modgraph

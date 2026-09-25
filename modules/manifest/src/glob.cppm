// mcpp.modgraph.glob — the ONE path-glob matcher.
//
// It lived in scanner.cppm's anonymous namespace, which was fine while the
// scanner was its only user. #359 gives `build.mcpp` a glob-shaped INPUT
// (`rerun-if-changed-glob`), whose fingerprint must select exactly the files a
// `sources = [...]` glob would. Two matchers that "should" agree about `**` is
// the shape this codebase keeps paying for, so there is one.

export module mcpp.modgraph.glob;

import std;
import mcpp.platform.common;    // is_windows
import mcpp.platform.windows;   // active_code_page

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

// Whether `s` is well-formed UTF-8: no stray continuation byte, no truncated or
// overlong sequence, no surrogate, nothing above U+10FFFF.
bool is_valid_utf8(std::string_view s);

// A printable spelling of a path that has no UTF-8 spelling, for a diagnostic
// that must name it. Well-formed UTF-8 (POSIX) and well-formed UTF-16
// (Windows) pass through; any other byte appears as `\xNN` and an unpaired
// surrogate as `\u{NNNN}`. The result is UTF-8 whatever the input.
std::string escaped_spelling(const std::filesystem::path& p);

// Why a path can have no UTF-8 spelling on this host, as one sentence. The
// three places that refuse or skip such a path give the same reason.
std::string no_utf8_spelling_reason();

// ─── narrowing a walk-derived path ────────────────────────────────────────
//
// THE ONE PLACE a path that came out of a directory walk becomes a narrow
// string. A direct `.string()` / `.generic_string()` on such a path needs a
// written reason (`// NARROW-OK: …`) and there is exactly one of those today,
// in p1689.cppm. This is a convention with a gate behind it, not a guarantee
// the compiler enforces — see the gate's header for what it does and does not
// cover.
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
// (.github/tools/check_narrow_conversions.sh) rather than a fourth try/catch.
//
// nullopt means: this path cannot be named in any string we hand to a
// compiler, a build file, or a glob. Skip it — and record it, because
// "silently not built" is exactly where this class of bug hides.
//
// A NAME THAT IS NOT UTF-8 CANNOT BE NAMED EITHER (#693). mcpp's strings are
// UTF-8 on every platform, and the documents it writes are UTF-8 by definition
// (JSON) or by declaration (Ninja on Windows). Two inputs used to pass the
// narrowing and fail later, inside a JSON writer, as `internal: unhandled
// exception: [json.exception.type_error.316]`: a POSIX file name made of bytes
// that are not UTF-8 (measured on Linux), and, on a Windows host that ignores
// the UTF-8 code page mcpp declares, any non-ASCII name the ANSI code page can
// spell (measured on cp1252 before the manifest). Both are now skipped and
// reported here, at the point of entry.
std::optional<std::string> try_narrow(const std::filesystem::path& p) {
    try {
        auto s = p.generic_string();
        if (!is_valid_utf8(s)) return std::nullopt;
        return s;
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
//
// The stored spelling is GENERIC (`/`), because that is what try_narrow
// produces and there is no second narrowing here to disagree with it. On
// Windows the reported path therefore reads `C:/pkg/test/www`, not
// `C:\pkg\test\www`; docs/04-mcpp-toml.md shows it that way too.
void note_unnarrowable_path(const std::filesystem::path& p);

// Take and clear this run's records.
//
// modgraph is a leaf layer — no module under `src/modgraph/` or
// `src/manifest/` imports `mcpp.ui` or `mcpp.diag` — so it RECORDS and the
// CLI reports. Drained in exactly one place (`cli::run`'s scope guard), which
// is what keeps "recorded but never shown" from becoming the next silent
// failure. The rule is written up in .agents/skills/mcpp-contributing/SKILL.md
// ("路径窄化不变式") and the user-facing behaviour in docs/04-mcpp-toml.md.
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

// The length of the well-formed UTF-8 sequence starting at `s[i]`, or 0 when
// the bytes there are not one.
std::size_t utf8_sequence_length(std::string_view s, std::size_t i) {
    const auto c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) return 1;
    std::size_t len = 0;
    std::uint32_t cp = 0;
    if      ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1Fu; }
    else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0Fu; }
    else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07u; }
    else return 0;
    if (i + len > s.size()) return 0;
    for (std::size_t k = 1; k < len; ++k) {
        const auto cc = static_cast<unsigned char>(s[i + k]);
        if ((cc & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (cc & 0x3Fu);
    }
    if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800)
        || (len == 4 && cp < 0x10000) || cp > 0x10FFFF
        || (cp >= 0xD800 && cp <= 0xDFFF))
        return 0;
    return len;
}

void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// POSIX: a path is bytes. Well-formed sequences are kept, other bytes escaped.
[[maybe_unused]] std::string escape_native(const std::string& s) {
    std::string out;
    std::size_t i = 0;
    while (i < s.size()) {
        if (const std::size_t len = utf8_sequence_length(s, i)) {
            out.append(s, i, len);
            i += len;
        } else {
            out += std::format("\\x{:02X}", static_cast<unsigned char>(s[i]));
            ++i;
        }
    }
    return out;
}

// Windows: a path is UTF-16 code units. Pairs are combined, and a surrogate
// without its partner is escaped.
[[maybe_unused]] std::string escape_native(const std::wstring& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        std::uint32_t u = static_cast<std::uint16_t>(s[i]);
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < s.size()) {
            const std::uint32_t v = static_cast<std::uint16_t>(s[i + 1]);
            if (v >= 0xDC00 && v <= 0xDFFF) {
                append_utf8(out, 0x10000 + ((u - 0xD800) << 10) + (v - 0xDC00));
                ++i;
                continue;
            }
        }
        if (u >= 0xD800 && u <= 0xDFFF) out += std::format("\\u{{{:04X}}}", u);
        else append_utf8(out, u);
    }
    return out;
}

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

bool is_valid_utf8(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const std::size_t len = utf8_sequence_length(s, i);
        if (len == 0) return false;
        i += len;
    }
    return true;
}

std::string escaped_spelling(const std::filesystem::path& p) {
    return escape_native(p.native());
}

std::string no_utf8_spelling_reason() {
    if constexpr (mcpp::platform::is_windows) {
        const unsigned acp = mcpp::platform::windows::active_code_page();
        if (acp != 65001)
            return std::format(
                "This process runs in the ANSI code page {} rather than UTF-8: "
                "mcpp.exe declares UTF-8, which Windows 10 version 1903 and "
                "later honour, and `chcp` changes neither.", acp);
        return "On Windows only a name that is not valid Unicode (an unpaired "
               "surrogate) has none.";
    } else {
        return "The name's bytes are not UTF-8, and build.ninja and "
               "compile_commands.json hold UTF-8 text.";
    }
}

std::vector<std::string> take_unnarrowable_paths() {
    std::lock_guard lk(g_unnarrowableMu);
    std::vector<std::string> out(g_unnarrowable.begin(), g_unnarrowable.end());
    g_unnarrowable.clear();
    return out;
}

}  // namespace mcpp::modgraph

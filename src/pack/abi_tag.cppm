// mcpp.pack.abi_tag — the readable compatibility tag a prebuilt artifact
// publishes, and the check a consumer runs against it.
//
// WHY A TAG AND NOT THE BUILD FINGERPRINT
//
// There are two compatibility questions and they have different answerers:
//
//   "can this binary be linked into your build?"   ← the TAG
//   "can this BMI be reused verbatim?"             ← cache_key::key_hex
//
// Only the first is publishable. A build key folds in the Merkle closure of
// the package's own dependencies, so it is a function of the CONSUMER's
// resolution — a producer would have to enumerate one key per possible
// consumer graph, which is not a finite job. The tag is a projection of facts
// the producer knows alone, which is exactly why it can be written into an
// index before any consumer exists.
//
// THE SHAPE IS THE SURFACE
//
// A tag carries the dimensions the artifact actually constrains, and nothing
// more. A library whose whole interface is `extern "C"` constrains the libc
// ABI and not the C++ one, so it publishes a triple and stops:
//
//     x86_64-linux-gnu                                  ← C surface
//     x86_64-linux-gnu-gcc16-libstdcxx16-c++23          ← C++ surface
//
// `tag_check` then compares only the dimensions the published tag names. That
// is the same don't-care rule `mcpp.toolchain.abi::abi_check` has used since
// the glfw conflation was fixed, and it means the C case needs no flag, no
// mode and no escape hatch: a shorter tag IS the statement. Tag counts for C
// libraries stay at one-per-triple instead of one-per-triple-per-compiler.
//
// THE TRIPLE IS THE CANONICAL ONE
//
// `arch-os-env` comes from mcpp.toolchain.triple, never from the compiler's
// own `-dumpmachine` answer. Those disagree: gcc reports `x86_64-w64-mingw32`
// where mcpp's target vocabulary — and therefore every `[target.'<triple>']`
// key a package can be selected by — says `x86_64-windows-gnu`. Publishing the
// compiler's spelling would give one decision two spellings, and the halves
// would be compared by string.
//
// Design: .agents/docs/2026-08-17-library-distribution-design.md §1.2.

export module mcpp.pack.abi_tag;

import std;
import mcpp.toolchain.model;
import mcpp.toolchain.triple;

export namespace mcpp::pack {

// A parsed compatibility tag. An EMPTY dimension means "not constrained",
// never "unknown": the producer decides what to name, and a consumer must not
// invent a constraint the artifact did not declare.
struct AbiTag {
    std::string triple;     // canonical, e.g. "x86_64-linux-gnu" (never empty)
    std::string compiler;   // "gcc16"       — empty on a C-surface tag
    std::string stdlib;     // "libstdcxx16" — empty on a C-surface tag
    std::string standard;   // "c++23"       — empty on a C-surface tag

    bool c_surface() const { return compiler.empty() && stdlib.empty() && standard.empty(); }

    std::string str() const {
        std::string s = triple;
        for (auto const* seg : { &compiler, &stdlib, &standard })
            if (!seg->empty()) { s += '-'; s += *seg; }
        return s;
    }

    bool operator==(const AbiTag&) const = default;
};

// The major version segment of "16.1.0" → "16". Empty input yields "0" rather
// than an empty segment, so a tag never contains a bare "gcc-" that would
// re-split differently on parse.
std::string major_of(std::string_view version);

// "libstdc++" → "libstdcxx". The tag is joined and split on '-', and a '+'
// inside a segment is fine, but the spelling is also a filename component in
// `target/dist/`, where '+' is best avoided.
std::string stdlib_token(std::string_view stdlibId);

// The C surface: the triple alone.
AbiTag c_surface_tag(std::string_view canonicalTriple);

// The C++ surface: every dimension the artifact constrains.
//
// `canonicalTriple` is passed rather than read from `tc.targetTriple` on
// purpose — the caller has already resolved which target is being packed, and
// the toolchain's own triple is the compiler's spelling of it.
AbiTag cxx_surface_tag(const mcpp::toolchain::Toolchain& tc,
                       std::string_view canonicalTriple,
                       int cppLevel);

// Read a published tag back. The optional C++ half is exactly three segments
// and the last of them starts with "c++", so parsing runs from the END: that
// is the only split that is unambiguous when the triple itself contains
// dashes (and it has a variable number of them — `aarch64-macos` has one,
// `x86_64-linux-gnu` has two).
//
// Returns nullopt for a tag with no triple at all, or for a 3-segment tail
// whose last segment is not a `c++NN`.
std::optional<AbiTag> parse_abi_tag(std::string_view s);

// One dimension on which a published tag refuses the current toolchain.
struct TagMismatch {
    std::string dimension;   // "triple" | "compiler" | "stdlib" | "standard"
    std::string need;        // what the artifact was built for
    std::string got;         // what this build resolved
};

// Does `published` accept `current`? Empty dimensions in `published` are
// don't-care (see the header). Returns every mismatch so the diagnostic can
// name all of them at once rather than one per rebuild.
//
// `standard` is the one asymmetric dimension: a consumer building at a HIGHER
// level than the artifact was compiled at is fine (the interface it compiles
// is the artifact's own source, and a newer level accepts it), while a lower
// one is not (the interface may use syntax the consumer's level lacks). So it
// is compared as a floor, not for equality.
std::vector<TagMismatch> tag_check(const AbiTag& published, const AbiTag& current);

// The `c++NN` segment as its numeric level, or 0 when unparseable.
int standard_level(std::string_view standardSegment);

} // namespace mcpp::pack

namespace mcpp::pack {

std::string major_of(std::string_view version) {
    auto dot = version.find('.');
    auto head = dot == std::string_view::npos ? version : version.substr(0, dot);
    // Keep only leading digits: "16.1.0" → "16", "22.1.8" → "22", and a
    // vendor string like "19.44.35207" → "19".
    std::size_t n = 0;
    while (n < head.size() && std::isdigit(static_cast<unsigned char>(head[n]))) ++n;
    if (n == 0) return "0";
    return std::string(head.substr(0, n));
}

std::string stdlib_token(std::string_view stdlibId) {
    std::string out;
    out.reserve(stdlibId.size());
    for (char c : stdlibId) {
        if (c == '+') { out += "x"; continue; }
        if (c == '-' || c == ' ') continue;      // "msvc-stl" / "MSVC STL"
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out.empty() ? std::string("unknownstl") : out;
}

AbiTag c_surface_tag(std::string_view canonicalTriple) {
    AbiTag t;
    t.triple = std::string(canonicalTriple);
    return t;
}

AbiTag cxx_surface_tag(const mcpp::toolchain::Toolchain& tc,
                       std::string_view canonicalTriple,
                       int cppLevel)
{
    AbiTag t;
    t.triple   = std::string(canonicalTriple);
    t.compiler = std::string(tc.compiler_name()) + major_of(tc.version);
    t.stdlib   = stdlib_token(tc.stdlibId) + major_of(tc.stdlibVersion);
    t.standard = std::format("c++{}", cppLevel);
    return t;
}

int standard_level(std::string_view seg) {
    if (!seg.starts_with("c++")) return 0;
    int level = 0;
    for (char c : seg.substr(3)) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return 0;
        level = level * 10 + (c - '0');
    }
    return level;
}

std::optional<AbiTag> parse_abi_tag(std::string_view s) {
    if (s.empty()) return std::nullopt;

    std::vector<std::string_view> seg;
    for (std::size_t i = 0; i <= s.size(); ) {
        auto j = s.find('-', i);
        if (j == std::string_view::npos) { seg.push_back(s.substr(i)); break; }
        seg.push_back(s.substr(i, j - i));
        i = j + 1;
    }
    if (seg.empty()) return std::nullopt;

    AbiTag t;
    // Parse from the end: the C++ half is exactly three segments and its last
    // one is `c++NN`. Anything else is a triple-only (C-surface) tag.
    if (seg.size() >= 4 && standard_level(seg.back()) != 0) {
        t.standard = std::string(seg[seg.size() - 1]);
        t.stdlib   = std::string(seg[seg.size() - 2]);
        t.compiler = std::string(seg[seg.size() - 3]);
        seg.resize(seg.size() - 3);
    }
    std::string triple;
    for (std::size_t i = 0; i < seg.size(); ++i) {
        if (i) triple += '-';
        triple += seg[i];
    }
    if (triple.empty()) return std::nullopt;
    t.triple = std::move(triple);
    return t;
}

std::vector<TagMismatch> tag_check(const AbiTag& published, const AbiTag& current) {
    std::vector<TagMismatch> out;
    auto exact = [&](std::string_view dim, const std::string& need, const std::string& got) {
        if (need.empty()) return;                 // not constrained
        if (need != got) out.push_back({ std::string(dim), need, got });
    };
    exact("triple",   published.triple,   current.triple);
    exact("compiler", published.compiler, current.compiler);
    exact("stdlib",   published.stdlib,   current.stdlib);

    // Floor, not equality — see the declaration.
    if (!published.standard.empty()) {
        int need = standard_level(published.standard);
        int got  = standard_level(current.standard);
        if (need != 0 && got != 0 && got < need)
            out.push_back({ "standard", published.standard, current.standard });
    }
    return out;
}

} // namespace mcpp::pack

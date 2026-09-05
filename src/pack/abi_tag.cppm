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
// WHAT DEVICE CODE AN ARTIFACT CARRIES, for one backend.
//
// A PARALLEL FIELD RATHER THAN A TAG SEGMENT, on purpose. The tag is `-`
// joined and parsed from the end, and an architecture list is a SET: joining
// it in would put separators inside a segment of a dash-delimited string whose
// triple already contains a variable number of dashes. A file name is the
// wrong place to carry a set. `tag_check` still compares it, so there is one
// comparator and two storage locations rather than a second comparator.
struct AccelSet {
    std::string backend;                // "cuda" | "rocm" | … — never empty
    std::string version;                // toolkit version; empty = unconstrained
    std::vector<std::string> archs;     // real device code actually emitted
    // The virtual architecture whose portable form is embedded, if any. NVIDIA
    // embeds PTX so later hardware can JIT; AMD has no equivalent and obtains
    // the same reach through family targets on the `archs` side instead. An
    // empty floor must therefore widen nothing.
    // The floor of the artifact's embedded PORTABLE form, below which it no
    // longer reaches. Named for CUDA's PTX because that is where the idea and
    // the published spelling come from; the field itself is backend-neutral and
    // `floor>=` is its neutral spelling on the wire. A backend with no portable
    // form leaves it empty, which widens nothing -- AMD is that case.
    std::string ptxFloor;
};

struct AbiTag {
    std::string triple;     // canonical, e.g. "x86_64-linux-gnu" (never empty)
    std::string compiler;   // "gcc16"       — empty on a C-surface tag
    std::string stdlib;     // "libstdcxx16" — empty on a C-surface tag
    std::string standard;   // "c++23"       — empty on a C-surface tag
    // Empty means the artifact carries no device code, which is why a CPU-only
    // library is usable by every build: the same don't-care rule as the four
    // above, reaching one dimension further.
    std::vector<AccelSet> accel;

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

// The wire and diagnostic form of the device dimension:
//
//     cuda12.8+{sm_80,sm_90f} ptx>=90, rocm6.4+{gfx942}
//
// ONE form for both, so what a descriptor stores is what a refusal prints and
// a reader never has to hold two spellings of the same fact in their head.
// The backend is the leading run of letters and the version is what follows,
// which parses because every backend name is alphabetic and every version
// starts with a digit.
std::string accel_str(std::span<const AccelSet> sets);

// The inverse. Unparseable input yields an empty vector, which the comparison
// reads as "carries no device code" — the same answer as a descriptor that
// never mentioned the dimension, and the safe one.
std::vector<AccelSet> parse_accel(std::string_view s);

// The `c++NN` segment as its numeric level, or 0 when unparseable.
int standard_level(std::string_view standardSegment);

// The numeric level of a device architecture, or 0 when it is not of the
// numbered `sm_`/`compute_` shape. AMD's `gfx942` answers 0 on purpose: it
// carries no ordering that means anything here, so it compares by equality.
int accel_arch_level(std::string_view arch);

// Does the device code `published` names cover `wantedArch`?
bool accel_arch_covers(std::string_view published, std::string_view wantedArch);

// Does `published` satisfy every backend and architecture `wanted` asks for?
// An empty `published` is unconstrained; an empty `wanted` is satisfied.
bool accel_accepts(std::span<const AccelSet> published,
                   std::span<const AccelSet> wanted);

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

std::string accel_str(std::span<const AccelSet> sets) {
    if (sets.empty()) return "(none)";
    std::string out;
    for (auto const& a : sets) {
        if (!out.empty()) out += ", ";
        out += a.backend;
        out += a.version;
        out += "+{";
        for (std::size_t i = 0; i < a.archs.size(); ++i) {
            if (i) out += ',';
            out += a.archs[i];
        }
        out += '}';
        if (!a.ptxFloor.empty()) { out += " ptx>="; out += a.ptxFloor; }
    }
    return out;
}

namespace {

std::string_view trim_sv(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t')) s.remove_suffix(1);
    return s;
}

} // namespace

std::vector<AccelSet> parse_accel(std::string_view s) {
    std::vector<AccelSet> out;
    for (std::size_t i = 0; i <= s.size(); ) {
        auto comma = s.find(',', i);
        // A comma inside `{...}` separates architectures, not backends.
        auto open  = s.find('{', i);
        auto close = s.find('}', i);
        if (open != std::string_view::npos && close != std::string_view::npos
            && comma != std::string_view::npos && comma > open && comma < close)
            comma = s.find(',', close);
        auto chunk = trim_sv(comma == std::string_view::npos
                                 ? s.substr(i)
                                 : s.substr(i, comma - i));
        i = comma == std::string_view::npos ? s.size() + 1 : comma + 1;
        if (chunk.empty() || chunk == "(none)") continue;

        AccelSet a;
        auto plus = chunk.find('+');
        auto head = trim_sv(plus == std::string_view::npos ? chunk : chunk.substr(0, plus));
        std::size_t n = 0;
        while (n < head.size() && std::isalpha(static_cast<unsigned char>(head[n]))) ++n;
        if (n == 0) continue;                        // no backend: not our form
        a.backend = std::string(head.substr(0, n));
        a.version = std::string(head.substr(n));

        if (plus != std::string_view::npos) {
            auto tail = chunk.substr(plus + 1);
            if (auto o = tail.find('{'); o != std::string_view::npos) {
                auto c = tail.find('}', o);
                auto archs = tail.substr(o + 1,
                    c == std::string_view::npos ? std::string_view::npos : c - o - 1);
                for (std::size_t j = 0; j <= archs.size(); ) {
                    auto k = archs.find(',', j);
                    auto one = trim_sv(k == std::string_view::npos
                                           ? archs.substr(j) : archs.substr(j, k - j));
                    if (!one.empty()) a.archs.emplace_back(one);
                    if (k == std::string_view::npos) break;
                    j = k + 1;
                }
            }
            // The floor below which an artifact's embedded portable form no
            // longer reaches. `ptx>=` is CUDA's spelling of it and the one
            // already published; `floor>=` is the backend-neutral one, so a
            // backend whose portable form is not PTX -- SPIR-V, say -- does not
            // have to borrow NVIDIA's word for it. One field, two spellings,
            // and the value means the same thing to the comparison either way.
            if (auto pf = tail.find("floor>="); pf != std::string_view::npos)
                a.ptxFloor = std::string(trim_sv(tail.substr(pf + 7)));
            else if (auto pf2 = tail.find("ptx>="); pf2 != std::string_view::npos)
                a.ptxFloor = std::string(trim_sv(tail.substr(pf2 + 5)));
        }
        out.push_back(std::move(a));
    }
    return out;
}

int accel_arch_level(std::string_view arch) {
    auto digits = arch;
    if      (digits.starts_with("sm_"))      digits.remove_prefix(3);
    else if (digits.starts_with("compute_")) digits.remove_prefix(8);
    int level = 0, n = 0;
    for (char c : digits) {
        if (!std::isdigit(static_cast<unsigned char>(c))) break;
        level = level * 10 + (c - '0');
        ++n;
    }
    return n == 0 ? 0 : level;
}

namespace {

// The trailing letter of a qualified target, or '\0'.
//
//   sm_90    baseline      that compute capability
//   sm_90f   family        same major, equal-or-higher minor
//   sm_90a   architecture  that compute capability and no other
char accel_arch_suffix(std::string_view arch) {
    if (arch.empty()) return '\0';
    char last = arch.back();
    return (last == 'f' || last == 'a') ? last : '\0';
}

} // namespace

bool accel_arch_covers(std::string_view published, std::string_view wantedArch) {
    if (published == wantedArch) return true;
    int have = accel_arch_level(published);
    int want = accel_arch_level(wantedArch);
    // Not the numbered shape on either side: equality was the only question
    // available, and it has been answered.
    if (have == 0 || want == 0) return false;
    if (accel_arch_suffix(published) == 'f')
        return have / 10 == want / 10 && want % 10 >= have % 10;
    // Baseline and architecture-specific targets cover exactly their own
    // compute capability. Forward reach for those comes from embedded portable
    // code, which is ptxFloor's question rather than this one.
    return have == want;
}

bool accel_accepts(std::span<const AccelSet> published,
                   std::span<const AccelSet> wanted)
{
    if (published.empty()) return true;      // carries no device code
    for (auto const& want : wanted) {
        const AccelSet* have = nullptr;
        for (auto const& p : published)
            if (p.backend == want.backend) { have = &p; break; }
        if (!have) return false;
        // Minor version compatibility is real inside one major release family
        // and absent across families, so the major is compared and the minor
        // is not.
        if (!have->version.empty() && !want.version.empty()
            && major_of(have->version) != major_of(want.version))
            return false;
        int floor = have->ptxFloor.empty() ? 0 : accel_arch_level(have->ptxFloor);
        for (auto const& a : want.archs) {
            bool ok = false;
            for (auto const& p : have->archs)
                if (accel_arch_covers(p, a)) { ok = true; break; }
            if (!ok && floor != 0) {
                int lvl = accel_arch_level(a);
                ok = lvl != 0 && lvl >= floor;
            }
            if (!ok) return false;
        }
    }
    return true;
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

    // The device dimension. Reported as ONE mismatch rather than one per
    // backend: a consumer cannot act on "cuda disagrees and rocm disagrees"
    // any differently than on "this artifact's device code does not cover the
    // target", and the closest-refusal selection in prebuilt.cppm ranks
    // candidates by how many dimensions disagree.
    if (!accel_accepts(published.accel, current.accel))
        out.push_back({ "accel",
                        accel_str(published.accel),
                        accel_str(current.accel) });
    return out;
}

} // namespace mcpp::pack

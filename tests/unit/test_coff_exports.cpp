#include <gtest/gtest.h>

import std;
import mcpp.build.coff_exports;

using mcpp::build::coff::Export;
using mcpp::build::coff::read_exports;
using mcpp::build::coff::write_def;
using mcpp::build::coff::kMaxExports;
using mcpp::build::coff::declares_exports;

// The `.def` generator decides what a DLL's public surface is. Getting the
// filter wrong does not fail loudly: too few exports and the consumer sees
// unresolved externals for symbols that are visibly in the objects; too many
// and the DLL's surface depends on which translation unit instantiated what.
//
// So both halves are tested here, on every host:
//
//   * SYNTHETIC objects, built byte by byte, for the filter rules — storage
//     class, section number, type, section characteristics, the skip list.
//     Nothing else can vary a symbol's storage class on demand.
//   * A REAL object (tests/fixtures/coff/probe-amd64.obj, from mingw-cross
//     GCC 16.1), because a synthetic file only proves the reader agrees with
//     the test's idea of COFF. macOS and Windows CI cannot produce one, which
//     is precisely why it is committed rather than generated.

namespace {

// ── a minimal COFF writer, so a test can state its input exactly ──────────
struct Sym {
    std::string   name;         // ≤ 8 chars: written inline
    std::int16_t  section = 1;
    std::uint16_t type    = 0x20;   // function
    std::uint8_t  storage = 2;      // IMAGE_SYM_CLASS_EXTERNAL
    std::uint8_t  aux     = 0;
};

void put16(std::vector<std::byte>& b, std::size_t off, std::uint16_t v) {
    b[off]     = std::byte(v & 0xff);
    b[off + 1] = std::byte((v >> 8) & 0xff);
}
void put32(std::vector<std::byte>& b, std::size_t off, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[off + i] = std::byte((v >> (8 * i)) & 0xff);
}

// `sectionFlags` is 1-based to match how symbols address sections.
std::vector<std::byte> make_obj(std::vector<Sym> syms,
                                std::vector<std::uint32_t> sectionFlags = { 0x60000020u },
                                std::uint16_t machine = 0x8664)
{
    constexpr std::size_t kHdr = 20, kSec = 40, kSym = 18;
    const std::size_t nsec = sectionFlags.size();
    const std::size_t secOff = kHdr;
    const std::size_t symOff = secOff + nsec * kSec;

    std::vector<std::byte> b(symOff + syms.size() * kSym + 4, std::byte{0});
    put16(b, 0, machine);
    put16(b, 2, static_cast<std::uint16_t>(nsec));
    put32(b, 8, static_cast<std::uint32_t>(symOff));
    put32(b, 12, static_cast<std::uint32_t>(syms.size()));
    put16(b, 16, 0);                        // no optional header

    for (std::size_t i = 0; i < nsec; ++i)
        put32(b, secOff + i * kSec + 36, sectionFlags[i]);

    for (std::size_t i = 0; i < syms.size(); ++i) {
        const auto rec = symOff + i * kSym;
        for (std::size_t c = 0; c < syms[i].name.size() && c < 8; ++c)
            b[rec + c] = std::byte(syms[i].name[c]);
        put16(b, rec + 12, static_cast<std::uint16_t>(syms[i].section));
        put16(b, rec + 14, syms[i].type);
        b[rec + 16] = std::byte(syms[i].storage);
        b[rec + 17] = std::byte(syms[i].aux);
    }
    put32(b, b.size() - 4, 4);              // empty string table
    return b;
}

constexpr std::uint32_t kText  = 0x60000020u;  // CODE | MEM_EXECUTE | MEM_READ
constexpr std::uint32_t kData  = 0xC0000040u;  // INITIALIZED | MEM_READ | MEM_WRITE
constexpr std::uint32_t kRdata = 0x40000040u;  // INITIALIZED | MEM_READ

std::vector<std::string> names(const std::vector<Export>& v) {
    std::vector<std::string> out;
    for (auto const& e : v) out.push_back(e.name);
    std::ranges::sort(out);
    return out;
}

}  // namespace

// ─── what is exported ──────────────────────────────────────────────────────

TEST(CoffExports, TakesExternalDefinedSymbols) {
    auto obj = make_obj({ Sym{ .name = "f" } });
    auto r = read_exports(obj);
    ASSERT_TRUE(r) << r.error();
    EXPECT_EQ(names(*r), (std::vector<std::string>{ "f" }));
}

TEST(CoffExports, SkipsStaticSymbols) {
    // IMAGE_SYM_CLASS_STATIC (3). Exporting a TU-local symbol would put a name
    // in the DLL's surface that the author never made public.
    auto obj = make_obj({ Sym{ .name = "s", .storage = 3 } });
    auto r = read_exports(obj);
    ASSERT_TRUE(r);
    EXPECT_TRUE(r->empty());
}

TEST(CoffExports, SkipsUndefinedSymbols) {
    // SectionNumber 0 is UNDEFINED — a symbol this object REFERENCES. Exporting
    // it would have the DLL claim to provide what it is asking someone else for.
    auto obj = make_obj({ Sym{ .name = "u", .section = 0 } });
    auto r = read_exports(obj);
    ASSERT_TRUE(r);
    EXPECT_TRUE(r->empty());
}

TEST(CoffExports, SkipsAbsoluteAndDebugSections) {
    // -1 IMAGE_SYM_ABSOLUTE, -2 IMAGE_SYM_DEBUG: neither has an address to
    // export.
    auto obj = make_obj({ Sym{ .name = "a", .section = -1 },
                          Sym{ .name = "d", .section = -2 } });
    auto r = read_exports(obj);
    ASSERT_TRUE(r);
    EXPECT_TRUE(r->empty());
}

// ─── DATA, which is the half that fails silently ───────────────────────────

TEST(CoffExports, AVariableIsMarkedDATA) {
    // Without `DATA` the linker generates a call thunk for a variable and the
    // consumer reads the thunk instead of the value.
    auto obj = make_obj({ Sym{ .name = "v", .section = 2, .type = 0 } },
                        { kText, kData });
    auto r = read_exports(obj);
    ASSERT_TRUE(r);
    ASSERT_EQ(r->size(), 1u);
    EXPECT_TRUE((*r)[0].data);
}

TEST(CoffExports, AConstInAReadOnlySectionIsStillDATA) {
    // `.rdata` is not writable, so a rule keyed on MEM_WRITE alone calls this a
    // function. It is a variable, and consumers read it as one.
    auto obj = make_obj({ Sym{ .name = "c", .section = 2, .type = 0 } },
                        { kText, kRdata });
    auto r = read_exports(obj);
    ASSERT_TRUE(r);
    ASSERT_EQ(r->size(), 1u);
    EXPECT_TRUE((*r)[0].data);
}

TEST(CoffExports, AFunctionIsNotDATA) {
    auto obj = make_obj({ Sym{ .name = "f", .section = 1, .type = 0x20 } }, { kText });
    auto r = read_exports(obj);
    ASSERT_TRUE(r);
    ASSERT_EQ(r->size(), 1u);
    EXPECT_FALSE((*r)[0].data);
}

// ─── the skip list, each entry for its own reason ──────────────────────────

TEST(CoffExports, SkipsDestructorThunksAndManagedArtefacts) {
    auto obj = make_obj({
        Sym{ .name = "??_Gx" },      // scalar-deleting destructor
        Sym{ .name = "??_Ex" },      // vector-deleting destructor
        Sym{ .name = "a.b" },        // managed: '.' cannot appear in a C++ mangling
        Sym{ .name = "keep" },
    });
    auto r = read_exports(obj);
    ASSERT_TRUE(r);
    EXPECT_EQ(names(*r), (std::vector<std::string>{ "keep" }));
}

TEST(CoffExports, SkipsAuxiliaryRecords) {
    // An aux record follows its symbol and is NOT a symbol. Reading one as a
    // symbol yields a name made of raw section data — which is how a `.def`
    // ends up with entries no linker can resolve.
    auto obj = make_obj({ Sym{ .name = "fn", .aux = 1 },
                          Sym{ .name = "\x01\x02\x03" },   // the aux record
                          Sym{ .name = "after" } });
    auto r = read_exports(obj);
    ASSERT_TRUE(r);
    EXPECT_EQ(names(*r), (std::vector<std::string>{ "after", "fn" }));
}

TEST(CoffExports, StripsTheLeadingUnderscoreOnI386Only) {
    auto i386 = make_obj({ Sym{ .name = "_f" } }, { kText }, 0x014c);
    auto r32 = read_exports(i386);
    ASSERT_TRUE(r32);
    EXPECT_EQ(names(*r32), (std::vector<std::string>{ "f" }));

    // On amd64 a leading underscore is part of the name.
    auto amd64 = make_obj({ Sym{ .name = "_f" } }, { kText }, 0x8664);
    auto r64 = read_exports(amd64);
    ASSERT_TRUE(r64);
    EXPECT_EQ(names(*r64), (std::vector<std::string>{ "_f" }));
}

// ─── refusals, because a partial answer here ships a broken DLL ────────────

TEST(CoffExports, RefusesAnUnsupportedMachine) {
    auto obj = make_obj({ Sym{ .name = "f" } }, { kText }, 0x5032 /* RISC-V */);
    auto r = read_exports(obj);
    ASSERT_FALSE(r);
    EXPECT_NE(r.error().find("machine"), std::string::npos);
}

TEST(CoffExports, RefusesATruncatedSymbolTable) {
    auto obj = make_obj({ Sym{ .name = "f" }, Sym{ .name = "g" } });
    obj.resize(obj.size() - 20);          // cut into the symbol table
    auto r = read_exports(obj);
    ASSERT_FALSE(r) << "a truncated table must not read as a short one";
    EXPECT_NE(r.error().find("past the end"), std::string::npos);
}

TEST(CoffExports, RefusesSomethingThatIsNotAnObject) {
    std::vector<std::byte> tiny(4, std::byte{0});
    EXPECT_FALSE(read_exports(tiny));
}

// ─── the .def text ─────────────────────────────────────────────────────────

TEST(CoffExports, DefIsSortedDeduplicatedAndMarksData) {
    auto def = write_def("mathkit.dll", {
        { "zeta", false }, { "alpha", true }, { "zeta", false } });
    EXPECT_EQ(def,
        "LIBRARY mathkit.dll\n"
        "EXPORTS\n"
        "    alpha DATA\n"
        "    zeta\n");
}

TEST(CoffExports, TheExportCeilingIsPesNotOurs) {
    // Stated as a constant a reader can check against the format, not as a
    // number chosen here: PE addresses exports by 16-bit ordinal.
    EXPECT_EQ(kMaxExports, 65535u);
}

// ─── a real object, from a real compiler ───────────────────────────────────

TEST(CoffExports, ReadsARealMingwObject) {
    // Committed fixture: macOS and Windows CI cannot produce a COFF object, and
    // a byte-level reader that is only ever fed its own test's output is exactly
    // the kind that agrees with itself and with nothing else.
    for (auto const& base : { "tests/fixtures/coff/probe-amd64.obj",
                              "../tests/fixtures/coff/probe-amd64.obj",
                              "../../tests/fixtures/coff/probe-amd64.obj" }) {
        std::ifstream in(base, std::ios::binary);
        if (!in) continue;
        std::vector<std::byte> bytes;
        for (char c; in.get(c); ) bytes.push_back(static_cast<std::byte>(c));

        auto r = read_exports(bytes);
        ASSERT_TRUE(r) << r.error();
        auto got = names(*r);

        // From `x86_64-w64-mingw32-nm --defined-only --extern-only` on the same
        // object; see tests/fixtures/coff/README.md.
        EXPECT_NE(std::ranges::find(got, "exported_fn"), got.end());
        EXPECT_NE(std::ranges::find(got, "exported_data"), got.end());
        EXPECT_NE(std::ranges::find(got, "exported_const"), got.end());
        EXPECT_NE(std::ranges::find(got, "_ZN2ns10mangled_fnEi"), got.end());
        // `static` at namespace scope is internal linkage: not in the surface.
        EXPECT_EQ(std::ranges::find(got, "_ZL9hidden_fni"), got.end());

        for (auto const& e : *r) {
            if (e.name == "exported_data" || e.name == "exported_const")
                EXPECT_TRUE(e.data) << e.name << " is a variable";
            if (e.name == "exported_fn") EXPECT_FALSE(e.data);
        }
        return;
    }
    GTEST_SKIP() << "fixture not reachable from this working directory";
}

// ─── annotation wins ───────────────────────────────────────────────────────

TEST(CoffExports, RecognisesAnObjectThatAlreadyDeclaresExports) {
    // `__declspec(dllexport)` makes the compiler write `/EXPORT:` directives
    // into `.drectve`. Generating a `.def` on top of that would export the same
    // names twice (LNK4197) AND export everything else besides, replacing a
    // chosen public surface with all of it.
    //
    // Tested against a real object because the whole point is recognising what
    // a COMPILER emits; a hand-built `.drectve` would only prove this agrees
    // with the test.
    for (auto const& base : { "tests/fixtures/coff/annotated-amd64.obj",
                              "../tests/fixtures/coff/annotated-amd64.obj",
                              "../../tests/fixtures/coff/annotated-amd64.obj" }) {
        std::ifstream in(base, std::ios::binary);
        if (!in) continue;
        std::vector<std::byte> bytes;
        for (char c; in.get(c); ) bytes.push_back(static_cast<std::byte>(c));
        EXPECT_TRUE(declares_exports(bytes));
        return;
    }
    GTEST_SKIP() << "fixture not reachable from this working directory";
}

TEST(CoffExports, AnObjectWithNoDirectivesDeclaresNothing) {
    // The negative half: without it, a `declares_exports` that always said true
    // would pass the test above and silently disable auto-export everywhere.
    for (auto const& base : { "tests/fixtures/coff/probe-amd64.obj",
                              "../tests/fixtures/coff/probe-amd64.obj",
                              "../../tests/fixtures/coff/probe-amd64.obj" }) {
        std::ifstream in(base, std::ios::binary);
        if (!in) continue;
        std::vector<std::byte> bytes;
        for (char c; in.get(c); ) bytes.push_back(static_cast<std::byte>(c));
        EXPECT_FALSE(declares_exports(bytes));
        return;
    }
    GTEST_SKIP() << "fixture not reachable from this working directory";
}

TEST(CoffExports, SyntheticObjectsHaveNoDirectiveSection) {
    EXPECT_FALSE(declares_exports(make_obj({ Sym{ .name = "f" } })));
}


import std;
import mcpplibs.mtest;
import mcpp.build.coff_exports;

using namespace mcpplibs::mtest;

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

UTest takes_external_defined_symbols{"CoffExports/TakesExternalDefinedSymbols", [] {
    auto obj = make_obj({ Sym{ .name = "f" } });
    auto r = read_exports(obj);
    if (auto&& failed = expect(fatal(bool(r))); !failed) failed << r.error();
    expect(eq(names(*r), (std::vector<std::string>{ "f" })));
}};

UTest skips_static_symbols{"CoffExports/SkipsStaticSymbols", [] {
    // IMAGE_SYM_CLASS_STATIC (3). Exporting a TU-local symbol would put a name
    // in the DLL's surface that the author never made public.
    auto obj = make_obj({ Sym{ .name = "s", .storage = 3 } });
    auto r = read_exports(obj);
    expect(fatal(bool(r)));
    expect(r->empty());
}};

UTest skips_undefined_symbols{"CoffExports/SkipsUndefinedSymbols", [] {
    // SectionNumber 0 is UNDEFINED — a symbol this object REFERENCES. Exporting
    // it would have the DLL claim to provide what it is asking someone else for.
    auto obj = make_obj({ Sym{ .name = "u", .section = 0 } });
    auto r = read_exports(obj);
    expect(fatal(bool(r)));
    expect(r->empty());
}};

UTest skips_absolute_and_debug_sections{"CoffExports/SkipsAbsoluteAndDebugSections", [] {
    // -1 IMAGE_SYM_ABSOLUTE, -2 IMAGE_SYM_DEBUG: neither has an address to
    // export.
    auto obj = make_obj({ Sym{ .name = "a", .section = -1 },
                          Sym{ .name = "d", .section = -2 } });
    auto r = read_exports(obj);
    expect(fatal(bool(r)));
    expect(r->empty());
}};

// ─── DATA, which is the half that fails silently ───────────────────────────

UTest a_variable_is_marked_data{"CoffExports/AVariableIsMarkedDATA", [] {
    // Without `DATA` the linker generates a call thunk for a variable and the
    // consumer reads the thunk instead of the value.
    auto obj = make_obj({ Sym{ .name = "v", .section = 2, .type = 0 } },
                        { kText, kData });
    auto r = read_exports(obj);
    expect(fatal(bool(r)));
    expect(fatal(eq(r->size(), 1u)));
    expect(bool((*r)[0].data));
}};

UTest a_const_in_a_read_only_section_is_still_data{"CoffExports/AConstInAReadOnlySectionIsStillDATA", [] {
    // `.rdata` is not writable, so a rule keyed on MEM_WRITE alone calls this a
    // function. It is a variable, and consumers read it as one.
    auto obj = make_obj({ Sym{ .name = "c", .section = 2, .type = 0 } },
                        { kText, kRdata });
    auto r = read_exports(obj);
    expect(fatal(bool(r)));
    expect(fatal(eq(r->size(), 1u)));
    expect(bool((*r)[0].data));
}};

UTest a_function_is_not_data{"CoffExports/AFunctionIsNotDATA", [] {
    auto obj = make_obj({ Sym{ .name = "f", .section = 1, .type = 0x20 } }, { kText });
    auto r = read_exports(obj);
    expect(fatal(bool(r)));
    expect(fatal(eq(r->size(), 1u)));
    expect(!((*r)[0].data));
}};

// ─── the skip list, each entry for its own reason ──────────────────────────

UTest skips_destructor_thunks_and_managed_artefacts{"CoffExports/SkipsDestructorThunksAndManagedArtefacts", [] {
    auto obj = make_obj({
        Sym{ .name = "??_Gx" },      // scalar-deleting destructor
        Sym{ .name = "??_Ex" },      // vector-deleting destructor
        Sym{ .name = "a.b" },        // managed: '.' cannot appear in a C++ mangling
        Sym{ .name = "keep" },
    });
    auto r = read_exports(obj);
    expect(fatal(bool(r)));
    expect(eq(names(*r), (std::vector<std::string>{ "keep" })));
}};

UTest skips_auxiliary_records{"CoffExports/SkipsAuxiliaryRecords", [] {
    // An aux record follows its symbol and is NOT a symbol. Reading one as a
    // symbol yields a name made of raw section data — which is how a `.def`
    // ends up with entries no linker can resolve.
    auto obj = make_obj({ Sym{ .name = "fn", .aux = 1 },
                          Sym{ .name = "\x01\x02\x03" },   // the aux record
                          Sym{ .name = "after" } });
    auto r = read_exports(obj);
    expect(fatal(bool(r)));
    expect(eq(names(*r), (std::vector<std::string>{ "after", "fn" })));
}};

UTest strips_the_leading_underscore_on_i386_only{"CoffExports/StripsTheLeadingUnderscoreOnI386Only", [] {
    auto i386 = make_obj({ Sym{ .name = "_f" } }, { kText }, 0x014c);
    auto r32 = read_exports(i386);
    expect(fatal(bool(r32)));
    expect(eq(names(*r32), (std::vector<std::string>{ "f" })));

    // On amd64 a leading underscore is part of the name.
    auto amd64 = make_obj({ Sym{ .name = "_f" } }, { kText }, 0x8664);
    auto r64 = read_exports(amd64);
    expect(fatal(bool(r64)));
    expect(eq(names(*r64), (std::vector<std::string>{ "_f" })));
}};

// ─── refusals, because a partial answer here ships a broken DLL ────────────

UTest refuses_an_unsupported_machine{"CoffExports/RefusesAnUnsupportedMachine", [] {
    auto obj = make_obj({ Sym{ .name = "f" } }, { kText }, 0x5032 /* RISC-V */);
    auto r = read_exports(obj);
    expect(fatal(!r));
    expect(ne(r.error().find("machine"), std::string::npos));
}};

UTest refuses_a_truncated_symbol_table{"CoffExports/RefusesATruncatedSymbolTable", [] {
    auto obj = make_obj({ Sym{ .name = "f" }, Sym{ .name = "g" } });
    obj.resize(obj.size() - 20);          // cut into the symbol table
    auto r = read_exports(obj);
    expect(fatal(!r)) << "a truncated table must not read as a short one";
    expect(ne(r.error().find("past the end"), std::string::npos));
}};

// ─── anonymous objects (2026.10.5.2) ────────────────────────────────────────
//
// Machine 0 and 0xFFFF open three containers, told apart by a version and a
// class GUID. The headers below are the ones cl 19.51 writes (measured on
// windows-latest, 2026-10-05).

namespace {

constexpr std::array<std::uint8_t, 16> kBigObj{
    0xc7, 0xa1, 0xba, 0xd1, 0xee, 0xba, 0xa9, 0x4b,
    0xaf, 0x20, 0xfa, 0xf6, 0x6a, 0xa4, 0xdc, 0xb8};
constexpr std::array<std::uint8_t, 16> kClGl{
    0x38, 0xfe, 0xb3, 0x0c, 0xa5, 0xd9, 0xab, 0x4d,
    0xac, 0x9b, 0xd6, 0xb6, 0x22, 0x26, 0x53, 0xc2};

std::vector<std::byte> anonymous(std::uint16_t version,
                                 const std::array<std::uint8_t, 16>& classId,
                                 std::size_t size = 64) {
    std::vector<std::byte> b(size, std::byte{0});
    put16(b, 2, 0xFFFF);
    put16(b, 4, version);
    put16(b, 6, 0x8664);
    for (std::size_t i = 0; i < 16; ++i) b[12 + i] = std::byte(classId[i]);
    return b;
}

// The `/bigobj` form of `make_obj`: a 56-byte header, 32-bit counts and
// section numbers, 20-byte symbol records.
std::vector<std::byte> make_bigobj(std::vector<Sym> syms,
                                   std::vector<std::uint32_t> sectionFlags = { 0x60000020u }) {
    constexpr std::size_t kHdr = 56, kSec = 40, kSym = 20;
    const std::size_t nsec = sectionFlags.size();
    const std::size_t symOff = kHdr + nsec * kSec;
    auto b = anonymous(2, kBigObj, symOff + syms.size() * kSym + 4);
    put32(b, 44, static_cast<std::uint32_t>(nsec));
    put32(b, 48, static_cast<std::uint32_t>(symOff));
    put32(b, 52, static_cast<std::uint32_t>(syms.size()));
    for (std::size_t i = 0; i < nsec; ++i)
        put32(b, kHdr + i * kSec + 36, sectionFlags[i]);
    for (std::size_t i = 0; i < syms.size(); ++i) {
        const auto rec = symOff + i * kSym;
        for (std::size_t c = 0; c < syms[i].name.size() && c < 8; ++c)
            b[rec + c] = std::byte(syms[i].name[c]);
        put32(b, rec + 12, static_cast<std::uint32_t>(static_cast<std::int32_t>(syms[i].section)));
        put16(b, rec + 16, syms[i].type);
        b[rec + 18] = std::byte(syms[i].storage);
        b[rec + 19] = std::byte(syms[i].aux);
    }
    put32(b, b.size() - 4, 4);
    return b;
}

}  // namespace

UTest reads_a_bigobj_object_as_the_ordinary_one_with_the_same_symbols{"CoffExports/ReadsABigobjObjectAsTheOrdinaryOneWithTheSameSymbols", [] {
    const std::vector<Sym> syms = {
        Sym{ .name = "f" }, Sym{ .name = "v", .section = 2, .type = 0 },
        Sym{ .name = "s", .storage = 3 }, Sym{ .name = "u", .section = 0 },
        Sym{ .name = "g", .aux = 1 }, Sym{ .name = "aux" }, Sym{ .name = "h" },
    };
    auto small = read_exports(make_obj(syms, { kText, kData }));
    auto big   = read_exports(make_bigobj(syms, { kText, kData }));
    if (auto&& failed = expect(fatal(bool(small))); !failed) failed << small.error();
    if (auto&& failed = expect(fatal(bool(big))); !failed) failed << big.error();
    expect(eq(*big, *small));
    expect(eq(names(*big), (std::vector<std::string>{ "f", "g", "h", "v" })));
}};

UTest names_a_gl_object_and_says_what_to_do{"CoffExports/NamesAGlObjectAndSaysWhatToDo", [] {
    auto r = read_exports(anonymous(1, kClGl));
    expect(fatal(!r));
    if (auto&& failed = expect(ne(r.error().find("/GL"), std::string::npos)); !failed) failed << r.error();
    if (auto&& failed = expect(eq(r.error().find("bigobj"), std::string::npos)); !failed) failed << r.error();
    expect(ne(r.error().find("windows_auto_export = false"), std::string::npos));
    expect(ne(r.error().find("dllexport"), std::string::npos));
    expect(!(declares_exports(anonymous(1, kClGl))));
}};

UTest names_an_import_object_and_an_unknown_anonymous_object{"CoffExports/NamesAnImportObjectAndAnUnknownAnonymousObject", [] {
    auto imp = read_exports(anonymous(0, {}));
    expect(fatal(!imp));
    if (auto&& failed = expect(ne(imp.error().find("import object"), std::string::npos)); !failed) failed << imp.error();
    auto other = read_exports(anonymous(3, {}));
    expect(fatal(!other));
    if (auto&& failed = expect(ne(other.error().find("anonymous"), std::string::npos)); !failed) failed << other.error();
}};

UTest refuses_a_bigobj_object_whose_counts_overrun_it{"CoffExports/RefusesABigobjObjectWhoseCountsOverrunIt", [] {
    auto obj = make_bigobj({ Sym{ .name = "f" } });
    put32(obj, 44, 0x00ffffffu);   // sections no file of this size can hold
    auto r = read_exports(obj);
    expect(fatal(!r));
    if (auto&& failed = expect(ne(r.error().find("past the end"), std::string::npos)); !failed) failed << r.error();
}};

UTest refuses_something_that_is_not_an_object{"CoffExports/RefusesSomethingThatIsNotAnObject", [] {
    std::vector<std::byte> tiny(4, std::byte{0});
    expect(!read_exports(tiny));
}};

// ─── the .def text ─────────────────────────────────────────────────────────

UTest def_is_sorted_deduplicated_and_marks_data{"CoffExports/DefIsSortedDeduplicatedAndMarksData", [] {
    auto def = write_def("mathkit.dll", {
        { "zeta", false }, { "alpha", true }, { "zeta", false } });
    expect(eq(def, "LIBRARY mathkit.dll\n"
        "EXPORTS\n"
        "    alpha DATA\n"
        "    zeta\n"));
}};

UTest the_export_ceiling_is_pes_not_ours{"CoffExports/TheExportCeilingIsPesNotOurs", [] {
    // Stated as a constant a reader can check against the format, not as a
    // number chosen here: PE addresses exports by 16-bit ordinal.
    expect(eq(kMaxExports, 65535u));
}};

// ─── a real object, from a real compiler ───────────────────────────────────

UTest reads_a_real_mingw_object{"CoffExports/ReadsARealMingwObject", [] {
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
        if (auto&& failed = expect(fatal(bool(r))); !failed) failed << r.error();
        auto got = names(*r);

        // From `x86_64-w64-mingw32-nm --defined-only --extern-only` on the same
        // object; see tests/fixtures/coff/README.md.
        expect(ne(std::ranges::find(got, "exported_fn"), got.end()));
        expect(ne(std::ranges::find(got, "exported_data"), got.end()));
        expect(ne(std::ranges::find(got, "exported_const"), got.end()));
        expect(ne(std::ranges::find(got, "_ZN2ns10mangled_fnEi"), got.end()));
        // `static` at namespace scope is internal linkage: not in the surface.
        expect(eq(std::ranges::find(got, "_ZL9hidden_fni"), got.end()));

        for (auto const& e : *r) {
            if (e.name == "exported_data" || e.name == "exported_const")
                expect(bool(e.data)) << e.name << " is a variable";
            if (e.name == "exported_fn") expect(!e.data);
        }
        return;
    }
    skip("fixture not reachable from this working directory");
}};

// ─── annotation wins ───────────────────────────────────────────────────────

UTest recognises_an_object_that_already_declares_exports{"CoffExports/RecognisesAnObjectThatAlreadyDeclaresExports", [] {
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
        expect(bool(declares_exports(bytes)));
        return;
    }
    skip("fixture not reachable from this working directory");
}};

UTest an_object_with_no_directives_declares_nothing{"CoffExports/AnObjectWithNoDirectivesDeclaresNothing", [] {
    // The negative half: without it, a `declares_exports` that always said true
    // would pass the test above and silently disable auto-export everywhere.
    for (auto const& base : { "tests/fixtures/coff/probe-amd64.obj",
                              "../tests/fixtures/coff/probe-amd64.obj",
                              "../../tests/fixtures/coff/probe-amd64.obj" }) {
        std::ifstream in(base, std::ios::binary);
        if (!in) continue;
        std::vector<std::byte> bytes;
        for (char c; in.get(c); ) bytes.push_back(static_cast<std::byte>(c));
        expect(!declares_exports(bytes));
        return;
    }
    skip("fixture not reachable from this working directory");
}};

UTest synthetic_objects_have_no_directive_section{"CoffExports/SyntheticObjectsHaveNoDirectiveSection", [] {
    expect(!(declares_exports(make_obj({ Sym{ .name = "f" } }))));
}};

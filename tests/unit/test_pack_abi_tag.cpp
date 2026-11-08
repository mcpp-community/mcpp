
import std;
import mcpplibs.mtest;
import mcpp.pack.abi_tag;
import mcpp.toolchain.model;

using namespace mcpplibs::mtest;

using namespace mcpp::pack;
using mcpp::toolchain::CompilerId;
using mcpp::toolchain::Toolchain;

namespace {

Toolchain tc(CompilerId cc, std::string ver, std::string stdlib, std::string stdlibVer) {
    Toolchain t;
    t.compiler      = cc;
    t.version       = std::move(ver);
    t.stdlibId      = std::move(stdlib);
    t.stdlibVersion = std::move(stdlibVer);
    // Deliberately the COMPILER's spelling, which must not reach the tag.
    t.targetTriple  = "x86_64-w64-mingw32";
    return t;
}

}  // namespace

// ─── the tag is a projection, and of the CANONICAL triple ──────────────────

UTest cxx_surface_names_every_dimension{"AbiTag/CxxSurfaceNamesEveryDimension", [] {
    auto t = cxx_surface_tag(tc(CompilerId::GCC, "16.1.0", "libstdc++", "16.1.0"),
                             "x86_64-linux-gnu", 23);
    expect(eq(t.str(), "x86_64-linux-gnu-gcc16-libstdcxx16-c++23"));
    expect(!t.c_surface());
}};

UTest uses_the_canonical_triple_not_the_compiler_reported_one{"AbiTag/UsesTheCanonicalTripleNotTheCompilerReportedOne", [] {
    // The toolchain above reports `x86_64-w64-mingw32`; mcpp's target
    // vocabulary — and every `[target.'<triple>']` key a package can be
    // selected by — says `x86_64-windows-gnu`. Publishing the compiler's
    // spelling would give one decision two spellings.
    auto t = cxx_surface_tag(tc(CompilerId::GCC, "16.1.0", "libstdc++", "16.1.0"),
                             "x86_64-windows-gnu", 23);
    expect(eq(t.triple, "x86_64-windows-gnu"));
    expect(eq(t.str().find("mingw"), std::string::npos));
}};

UTest libcxx_and_msvc_stl_tokenize{"AbiTag/LibcxxAndMsvcStlTokenize", [] {
    expect(eq(stdlib_token("libstdc++"), "libstdcxx"));
    expect(eq(stdlib_token("libc++"), "libcxx"));
    expect(eq(stdlib_token("msvc-stl"), "msvcstl"));
    expect(eq(stdlib_token(""), "unknownstl"));
}};

UTest major_is_leading_digits_only{"AbiTag/MajorIsLeadingDigitsOnly", [] {
    expect(eq(major_of("16.1.0"), "16"));
    expect(eq(major_of("19.44.35207"), "19"));
    expect(eq(major_of("22"), "22"));
    expect(eq(major_of(""), "0"));
}};

// ─── the SHAPE is the surface: a C library publishes a shorter tag ─────────

UTest c_surface_is_triple_only{"AbiTag/CSurfaceIsTripleOnly", [] {
    auto t = c_surface_tag("x86_64-linux-gnu");
    expect(eq(t.str(), "x86_64-linux-gnu"));
    expect(bool(t.c_surface()));
}};

UTest c_surface_accepts_any_compiler_and_stdlib{"AbiTag/CSurfaceAcceptsAnyCompilerAndStdlib", [] {
    auto published = c_surface_tag("x86_64-linux-gnu");
    auto current   = cxx_surface_tag(tc(CompilerId::Clang, "22.1.8", "libc++", "22.1.8"),
                                     "x86_64-linux-gnu", 26);
    // An extern "C" library constrains the libc ABI and nothing else, so an
    // unspecified dimension is don't-care — the same rule abi_check uses.
    expect(tag_check(published, current).empty());
}};

UTest c_surface_still_refuses_a_foreign_triple{"AbiTag/CSurfaceStillRefusesAForeignTriple", [] {
    auto published = c_surface_tag("x86_64-linux-gnu");
    auto current   = c_surface_tag("aarch64-linux-gnu");
    auto bad = tag_check(published, current);
    expect(fatal(eq(bad.size(), 1u)));
    expect(eq(bad[0].dimension, "triple"));
}};

// ─── round-trip: parsing runs from the END, because triples vary in length ──

UTest parses_full_tag_back_with_a_two_dash_triple{"AbiTag/ParsesFullTagBackWithATwoDashTriple", [] {
    auto t = parse_abi_tag("x86_64-linux-gnu-gcc16-libstdcxx16-c++23");
    expect(fatal(t.has_value()));
    expect(eq(t->triple, "x86_64-linux-gnu"));
    expect(eq(t->compiler, "gcc16"));
    expect(eq(t->stdlib, "libstdcxx16"));
    expect(eq(t->standard, "c++23"));
}};

UTest parses_full_tag_back_with_a_one_dash_triple{"AbiTag/ParsesFullTagBackWithAOneDashTriple", [] {
    // `aarch64-macos` has one dash, `x86_64-linux-gnu` has two. Splitting from
    // the front cannot tell where the triple ends; splitting from the back can,
    // because the C++ half is exactly three segments ending in c++NN.
    auto t = parse_abi_tag("aarch64-macos-llvm22-libcxx22-c++23");
    expect(fatal(t.has_value()));
    expect(eq(t->triple, "aarch64-macos"));
    expect(eq(t->compiler, "llvm22"));
    expect(eq(t->standard, "c++23"));
}};

UTest parses_a_triple_only_tag{"AbiTag/ParsesATripleOnlyTag", [] {
    auto t = parse_abi_tag("x86_64-linux-gnu");
    expect(fatal(t.has_value()));
    expect(bool(t->c_surface()));
    expect(eq(t->triple, "x86_64-linux-gnu"));
}};

UTest round_trips_every_shape{"AbiTag/RoundTripsEveryShape", [] {
    for (auto s : { "x86_64-linux-gnu",
                    "x86_64-linux-musl-gcc16-libstdcxx16-c++23",
                    "aarch64-macos-llvm22-libcxx22-c++26",
                    "x86_64-windows-msvc-msvc19-msvcstl19-c++23" }) {
        auto t = parse_abi_tag(s);
        expect(fatal(t.has_value())) << s;
        expect(eq(t->str(), s));
    }
}};

UTest rejects_empty_and_non_tag_input{"AbiTag/RejectsEmptyAndNonTagInput", [] {
    expect(!(parse_abi_tag("").has_value()));
    // Three trailing segments that do not end in c++NN are part of the triple,
    // not a C++ half — this must not be silently mis-split.
    auto t = parse_abi_tag("a-b-c-d");
    expect(fatal(t.has_value()));
    expect(eq(t->triple, "a-b-c-d"));
    expect(bool(t->c_surface()));
}};

// ─── the gate ──────────────────────────────────────────────────────────────

UTest refuses_a_different_compiler_major{"AbiTag/RefusesADifferentCompilerMajor", [] {
    auto published = *parse_abi_tag("x86_64-linux-gnu-gcc15-libstdcxx15-c++23");
    auto current   = *parse_abi_tag("x86_64-linux-gnu-gcc16-libstdcxx16-c++23");
    auto bad = tag_check(published, current);
    expect(fatal(eq(bad.size(), 2u)));
    expect(eq(bad[0].dimension, "compiler"));
    expect(eq(bad[0].need, "gcc15"));
    expect(eq(bad[0].got, "gcc16"));
    expect(eq(bad[1].dimension, "stdlib"));
}};

UTest standard_is_a_floor_not_an_equality{"AbiTag/StandardIsAFloorNotAnEquality", [] {
    auto published = *parse_abi_tag("x86_64-linux-gnu-gcc16-libstdcxx16-c++23");
    // Building at a HIGHER level is fine: the interface being compiled is the
    // artifact's own source and a newer level accepts it.
    auto higher = *parse_abi_tag("x86_64-linux-gnu-gcc16-libstdcxx16-c++26");
    expect(tag_check(published, higher).empty());
    // Lower is not: the interface may use syntax this level lacks.
    auto lower = *parse_abi_tag("x86_64-linux-gnu-gcc16-libstdcxx16-c++20");
    auto bad = tag_check(published, lower);
    expect(fatal(eq(bad.size(), 1u)));
    expect(eq(bad[0].dimension, "standard"));
}};

UTest reports_every_mismatch_at_once{"AbiTag/ReportsEveryMismatchAtOnce", [] {
    // One diagnostic, not one per rebuild.
    auto published = *parse_abi_tag("aarch64-linux-gnu-gcc15-libcxx15-c++26");
    auto current   = *parse_abi_tag("x86_64-linux-gnu-gcc16-libstdcxx16-c++23");
    expect(eq(tag_check(published, current).size(), 4u));
}};

// The link-line slot order.
//
// This file exists because the order it asserts was, until now, an emergent
// property of three `+=` in two files — and the composition was wrong in a way
// every individual site looked right. A GLFW application linked against the
// libX11 mcpp had just built and then LOADED the one xlings had installed,
// because the SubOS farm reached DT_RPATH ahead of `$ORIGIN`.
//
// So these assertions are about the ORDER itself, not about "the flags look
// roughly right". Each one names the failure it prevents.

#include <gtest/gtest.h>

import std;
import mcpp.build.link_line;

namespace ll = mcpp::build::link_line;

namespace {

// Where does `needle` start in `haystack`? npos-safe ordering helper: gtest's
// output for a raw `EXPECT_LT(a.find(x), a.find(y))` on an absent needle is
// two huge numbers and no clue which one was missing.
std::size_t pos_of(std::string_view haystack, std::string_view needle) {
    auto p = haystack.find(needle);
    EXPECT_NE(p, std::string_view::npos) << "missing '" << needle << "' in: " << haystack;
    return p;
}

ll::UnitTail full_tail() {
    ll::UnitTail t;
    t.dependencies    = " -Lbin -Wl,-rpath,'$ORIGIN' -lX11";
    t.cxxRuntime      = " -static-libstdc++";
    t.runtimeFallback = " -Wl,-rpath,/home/u/.mcpp/registry/subos/default/lib";
    t.loaderTag       = "-Wl,--disable-new-dtags";
    return t;
}

}  // namespace

// THE invariant. The artifact's own directory holds the exact files this link
// resolved against; the farm is a mutable view that may hold another build of
// the same SONAME. Farm-first is how an already-linked artifact silently
// starts loading a different library.
TEST(LinkLine, ArtifactDirectoryPrecedesTheRuntimeFallback) {
    auto line = full_tail().render();
    EXPECT_LT(pos_of(line, "$ORIGIN"), pos_of(line, "subos/default/lib")) << line;
}

// ld honours the LAST `--enable/--disable-new-dtags` it sees, and both gcc
// specs and clang config files supply the opposite of what mcpp wants. A slot
// appended after this one silently changes RPATH into RUNPATH (or back), which
// changes whether the tag applies to transitive dependencies at all.
TEST(LinkLine, LoaderTagIsLiterallyLast) {
    auto line = full_tail().render();
    EXPECT_TRUE(line.ends_with("-Wl,--disable-new-dtags"))
        << "something was emitted after the loader tag: " << line;
    EXPECT_GT(pos_of(line, "--disable-new-dtags"),
              pos_of(line, "subos/default/lib")) << line;
}

// The C++ runtime archive is processed after the dependencies it might
// otherwise pre-empt: an archive member is pulled only for symbols still
// undefined when the archive is reached.
TEST(LinkLine, CxxRuntimeFollowsDependenciesAndPrecedesTheFallback) {
    auto line = full_tail().render();
    EXPECT_LT(pos_of(line, "-lX11"),            pos_of(line, "-static-libstdc++")) << line;
    EXPECT_LT(pos_of(line, "-static-libstdc++"), pos_of(line, "subos/default/lib")) << line;
}

// PE has no rpath and no loader tag; Mach-O has no farm. A format that fills
// only some slots must not acquire stray separators — the rendered line is
// compared byte-for-byte against build.ninja by other tests.
TEST(LinkLine, EmptySlotsProduceNoStraySeparators) {
    ll::UnitTail t;
    t.cxxRuntime = " -static";
    EXPECT_EQ(t.render(), " -static");

    ll::UnitTail only_deps;
    only_deps.dependencies = " target/bin/foo.lib";
    EXPECT_EQ(only_deps.render(), " target/bin/foo.lib");

    EXPECT_EQ(ll::UnitTail{}.render(), "");
    EXPECT_TRUE(ll::UnitTail{}.empty());
    EXPECT_FALSE(full_tail().empty());
}

// A producer that forgets the leading space must not weld two flags together.
// Every producer supplies it today; this keeps that from being load-bearing.
TEST(LinkLine, SlotWithoutLeadingSpaceGetsASeparator) {
    ll::UnitTail t;
    t.dependencies = " -lfoo";
    t.cxxRuntime   = "-static-libstdc++";   // no leading space
    EXPECT_EQ(t.render(), " -lfoo -static-libstdc++");
}

// Order is a property of the type, not of the caller's assignment order.
TEST(LinkLine, RenderOrderIsIndependentOfAssignmentOrder) {
    ll::UnitTail t;
    t.loaderTag       = "-Wl,--disable-new-dtags";
    t.runtimeFallback = " -Wl,-rpath,/farm";
    t.cxxRuntime      = " -static-libstdc++";
    t.dependencies    = " -Wl,-rpath,'$ORIGIN'";
    EXPECT_EQ(t.render(),
              " -Wl,-rpath,'$ORIGIN' -static-libstdc++ -Wl,-rpath,/farm"
              " -Wl,--disable-new-dtags");
}

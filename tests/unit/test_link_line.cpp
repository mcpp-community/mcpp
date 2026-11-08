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


import std;
import mcpplibs.mtest;
import mcpp.build.link_line;

using namespace mcpplibs::mtest;

namespace ll = mcpp::build::link_line;

namespace {

// Where does `needle` start in `haystack`? npos-safe ordering helper: gtest's
// output for a raw `EXPECT_LT(a.find(x), a.find(y))` on an absent needle is
// two huge numbers and no clue which one was missing.
std::size_t pos_of(std::string_view haystack, std::string_view needle) {
    auto p = haystack.find(needle);
    expect(ne(p, std::string_view::npos)) << "missing '" << needle << "' in: " << haystack;
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
UTest artifact_directory_precedes_the_runtime_fallback{"LinkLine/ArtifactDirectoryPrecedesTheRuntimeFallback", [] {
    auto line = full_tail().render();
    expect(lt(pos_of(line, "$ORIGIN"), pos_of(line, "subos/default/lib"))) << line;
}};

// ld honours the LAST `--enable/--disable-new-dtags` it sees, and both gcc
// specs and clang config files supply the opposite of what mcpp wants. A slot
// appended after this one silently changes RPATH into RUNPATH (or back), which
// changes whether the tag applies to transitive dependencies at all.
UTest loader_tag_is_literally_last{"LinkLine/LoaderTagIsLiterallyLast", [] {
    auto line = full_tail().render();
    expect(line.ends_with("-Wl,--disable-new-dtags"))
        << "something was emitted after the loader tag: " << line;
    expect(gt(pos_of(line, "--disable-new-dtags"), pos_of(line, "subos/default/lib"))) << line;
}};

// The C++ runtime archive is processed after the dependencies it might
// otherwise pre-empt: an archive member is pulled only for symbols still
// undefined when the archive is reached.
UTest cxx_runtime_follows_dependencies_and_precedes_the_fallback{"LinkLine/CxxRuntimeFollowsDependenciesAndPrecedesTheFallback", [] {
    auto line = full_tail().render();
    expect(lt(pos_of(line, "-lX11"), pos_of(line, "-static-libstdc++"))) << line;
    expect(lt(pos_of(line, "-static-libstdc++"), pos_of(line, "subos/default/lib"))) << line;
}};

// PE has no rpath and no loader tag; Mach-O has no farm. A format that fills
// only some slots must not acquire stray separators — the rendered line is
// compared byte-for-byte against build.ninja by other tests.
UTest empty_slots_produce_no_stray_separators{"LinkLine/EmptySlotsProduceNoStraySeparators", [] {
    ll::UnitTail t;
    t.cxxRuntime = " -static";
    expect(eq(t.render(), " -static"));

    ll::UnitTail only_deps;
    only_deps.dependencies = " target/bin/foo.lib";
    expect(eq(only_deps.render(), " target/bin/foo.lib"));

    expect(eq(ll::UnitTail{}.render(), ""));
    expect(ll::UnitTail{}.empty());
    expect(!(full_tail().empty()));
}};

// A producer that forgets the leading space must not weld two flags together.
// Every producer supplies it today; this keeps that from being load-bearing.
UTest slot_without_leading_space_gets_a_separator{"LinkLine/SlotWithoutLeadingSpaceGetsASeparator", [] {
    ll::UnitTail t;
    t.dependencies = " -lfoo";
    t.cxxRuntime   = "-static-libstdc++";   // no leading space
    expect(eq(t.render(), " -lfoo -static-libstdc++"));
}};

// Order is a property of the type, not of the caller's assignment order.
UTest render_order_is_independent_of_assignment_order{"LinkLine/RenderOrderIsIndependentOfAssignmentOrder", [] {
    ll::UnitTail t;
    t.loaderTag       = "-Wl,--disable-new-dtags";
    t.runtimeFallback = " -Wl,-rpath,/farm";
    t.cxxRuntime      = " -static-libstdc++";
    t.dependencies    = " -Wl,-rpath,'$ORIGIN'";
    expect(eq(t.render(), " -Wl,-rpath,'$ORIGIN' -static-libstdc++ -Wl,-rpath,/farm"
              " -Wl,--disable-new-dtags"));
}};

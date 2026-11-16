
import std;
import mcpplibs.mtest;
import mcpp.build.flags;
import mcpp.manifest;

using namespace mcpplibs::mtest;

using mcpp::build::LinkIntentFlavor;
using mcpp::build::render_link_intent_flags;

// A distribution package states each leg's link line TWICE: once as `ldflags`
// (`-L…`, `-l…`), which is all an older mcpp reads, and once as the neutral
// `[target.<pred>.runtime]` pair. The neutral half exists for exactly one
// reader — a consumer driven by native `cl.exe`, which rejects `-L` — and this
// is where "neutral" is turned back into a command line.
//
// THE FLAVOUR IS A QUESTION ABOUT THE DRIVER, NOT ABOUT THE TARGET, and that
// is the opposite of the rule three other flags in this area follow. `-fPIC`,
// `--out-implib` and `/DEF:` reach the LINKER (through `-Wl,`), so the target
// ABI decides their spelling. These reach whatever mcpp INVOKES: with the MSVC
// dialect that is `link.exe` directly (`LinkStyle::SeparateLinker`), which takes
// `/LIBPATH:`; everything else is a compiler driver that takes `-L`. Clang
// targeting the MSVC ABI is the case that separates the two questions — it
// takes `-L` while producing MSVC-ABI objects, and it must not be handed
// `/LIBPATH:`.

namespace {

mcpp::manifest::LinkIntent leg_intent() {
    // What `manifest_emit` writes for one leg.
    mcpp::manifest::LinkIntent i;
    i.linkLibraryDirs.emplace_back("lib/x86_64-windows-msvc");
    i.libraries.push_back("mathkit");
    return i;
}

}  // namespace

UTest msvc_gets_libpath_and_a_dot_lib{"LinkIntentSpelling/MsvcGetsLibpathAndADotLib", [] {
    auto s = render_link_intent_flags(leg_intent(), LinkIntentFlavor::PeMsvc);
    // `/LIBPATH` without the colon: this is NINJA text, not a shell command
    // line, and `:` is escaped to `$:` because ninja reads a bare colon as the
    // outputs/inputs separator. Asserting the raw `/LIBPATH:` fails against a
    // perfectly correct renderer — which is what the first version of this test
    // did.
    expect(ne(s.find("/LIBPATH"), std::string::npos)) << s;
    expect(ne(s.find("mathkit.lib"), std::string::npos)) << s;
    // The GNU spelling must be absent, not merely outnumbered: `cl` stops at
    // the first `-L` it does not recognise.
    expect(eq(s.find("-L"), std::string::npos)) << s;
    expect(eq(s.find("-lmathkit"), std::string::npos)) << s;
}};

UTest every_driver_flavour_gets_the_gnu_spelling{"LinkIntentSpelling/EveryDriverFlavourGetsTheGnuSpelling", [] {
    // PeGnu is not only MinGW: clang targeting the MSVC ABI lands here too,
    // because it is a compiler driver and takes `-L`. Handing it `/LIBPATH:`
    // because its OUTPUT is MSVC-ABI would be the mistake this test exists to
    // prevent.
    for (auto flavour : { LinkIntentFlavor::Elf, LinkIntentFlavor::MachO,
                          LinkIntentFlavor::PeGnu }) {
        auto s = render_link_intent_flags(leg_intent(), flavour);
        expect(ne(s.find("-L"), std::string::npos)) << s;
        expect(ne(s.find("-lmathkit"), std::string::npos)) << s;
        expect(eq(s.find("/LIBPATH:"), std::string::npos)) << s;
    }
}};

UTest an_explicit_token_is_passed_through_untouched{"LinkIntentSpelling/AnExplicitTokenIsPassedThroughUntouched", [] {
    // A library named with a path or an extension is already a file, not a
    // name to decorate. Decorating it would produce `lib/x.lib.lib`.
    mcpp::manifest::LinkIntent i;
    i.libraries.push_back("lib/x86_64-windows-msvc/mathkit.lib");
    auto s = render_link_intent_flags(i, LinkIntentFlavor::PeMsvc);
    expect(ne(s.find("mathkit.lib"), std::string::npos)) << s;
    expect(eq(s.find("mathkit.lib.lib"), std::string::npos)) << s;
}};

UTest an_empty_intent_renders_nothing{"LinkIntentSpelling/AnEmptyIntentRendersNothing", [] {
    // The generated manifest omits the block for legs that do not need it, and
    // an empty intent must not put a stray flag on the line.
    expect(render_link_intent_flags({}, LinkIntentFlavor::PeMsvc).empty());
    expect(render_link_intent_flags({}, LinkIntentFlavor::Elf).empty());
}};

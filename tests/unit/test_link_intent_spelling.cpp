#include <gtest/gtest.h>

import std;
import mcpp.build.flags;
import mcpp.manifest;

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

TEST(LinkIntentSpelling, MsvcGetsLibpathAndADotLib) {
    auto s = render_link_intent_flags(leg_intent(), LinkIntentFlavor::PeMsvc);
    // `/LIBPATH` without the colon: this is NINJA text, not a shell command
    // line, and `:` is escaped to `$:` because ninja reads a bare colon as the
    // outputs/inputs separator. Asserting the raw `/LIBPATH:` fails against a
    // perfectly correct renderer — which is what the first version of this test
    // did.
    EXPECT_NE(s.find("/LIBPATH"), std::string::npos) << s;
    EXPECT_NE(s.find("mathkit.lib"), std::string::npos) << s;
    // The GNU spelling must be absent, not merely outnumbered: `cl` stops at
    // the first `-L` it does not recognise.
    EXPECT_EQ(s.find("-L"), std::string::npos) << s;
    EXPECT_EQ(s.find("-lmathkit"), std::string::npos) << s;
}

TEST(LinkIntentSpelling, EveryDriverFlavourGetsTheGnuSpelling) {
    // PeGnu is not only MinGW: clang targeting the MSVC ABI lands here too,
    // because it is a compiler driver and takes `-L`. Handing it `/LIBPATH:`
    // because its OUTPUT is MSVC-ABI would be the mistake this test exists to
    // prevent.
    for (auto flavour : { LinkIntentFlavor::Elf, LinkIntentFlavor::MachO,
                          LinkIntentFlavor::PeGnu }) {
        auto s = render_link_intent_flags(leg_intent(), flavour);
        EXPECT_NE(s.find("-L"), std::string::npos) << s;
        EXPECT_NE(s.find("-lmathkit"), std::string::npos) << s;
        EXPECT_EQ(s.find("/LIBPATH:"), std::string::npos) << s;
    }
}

TEST(LinkIntentSpelling, AnExplicitTokenIsPassedThroughUntouched) {
    // A library named with a path or an extension is already a file, not a
    // name to decorate. Decorating it would produce `lib/x.lib.lib`.
    mcpp::manifest::LinkIntent i;
    i.libraries.push_back("lib/x86_64-windows-msvc/mathkit.lib");
    auto s = render_link_intent_flags(i, LinkIntentFlavor::PeMsvc);
    EXPECT_NE(s.find("mathkit.lib"), std::string::npos) << s;
    EXPECT_EQ(s.find("mathkit.lib.lib"), std::string::npos) << s;
}

TEST(LinkIntentSpelling, AnEmptyIntentRendersNothing) {
    // The generated manifest omits the block for legs that do not need it, and
    // an empty intent must not put a stray flag on the line.
    EXPECT_TRUE(render_link_intent_flags({}, LinkIntentFlavor::PeMsvc).empty());
    EXPECT_TRUE(render_link_intent_flags({}, LinkIntentFlavor::Elf).empty());
}

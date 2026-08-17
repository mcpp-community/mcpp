#include <gtest/gtest.h>

import std;
import mcpp.pack.library;
import mcpp.platform.shell;
import mcpp.toolchain.dialect;

using mcpp::pack::archive_remove_command;
using mcpp::platform::shell::quote;
using mcpp::toolchain::gnu_dialect;
using mcpp::toolchain::msvc_dialect;

// `mcpp pack` deletes the published interface's objects from the archive before
// shipping it — leaving them in gives the consumer two definitions of each
// published module's initialiser, resolved by link order.
//
// Two archivers, and they disagree in BOTH directions:
//
//   ar        one verb, then the archive, then every member
//   lib.exe   one flag PER member, and the archive comes LAST
//
// ⚠️ WHY THIS IS A UNIT TEST. mcpp's Windows CI archives with clang's `llvm-ar`,
// which takes the GNU spelling, so no job anywhere executes the MSVC branch. The
// packer originally assumed `ar` syntax on every platform and nothing could have
// caught it: the two constants live in dialect.cppm, the order they are assembled
// in is a third fact, and it had no test because it had no seam. So the assembly
// was given one.
//
// Host-independent by construction: the expectations are built with the same
// `quote` these commands are built with, because what is under test is the
// spelling and the ORDER — shell quoting has its own tests in test_shell.cpp,
// and duplicating its rules here would only pin them twice, differently.

namespace {

const std::vector<std::string> kMembers{ "mathkit.m.o", "api.m.o" };

}  // namespace

TEST(ArchiveRemoveCommand, GnuTakesOneVerbThenTheArchiveThenEveryMember) {
    auto cmd = archive_remove_command("/usr/bin/ar", "bin/libmathkit.a", kMembers,
                                      gnu_dialect().archiveRemoveArg,
                                      gnu_dialect().archiveRemoveTakesArchiveFirst);
    EXPECT_EQ(cmd, quote("/usr/bin/ar") + " d " + quote("bin/libmathkit.a")
                     + " " + quote("mathkit.m.o") + " " + quote("api.m.o"));
}

TEST(ArchiveRemoveCommand, MsvcTakesOneFlagPerMemberAndTheArchiveLast) {
    auto cmd = archive_remove_command("lib.exe", "bin/mathkit.lib", kMembers,
                                      msvc_dialect().archiveRemoveArg,
                                      msvc_dialect().archiveRemoveTakesArchiveFirst);
    EXPECT_EQ(cmd, quote("lib.exe")
                     + " " + quote("/REMOVE:mathkit.m.o")
                     + " " + quote("/REMOVE:api.m.o")
                     + " " + quote("bin/mathkit.lib"));
}

// The two rows in dialect.cppm are the input to the assembly above. Pinned here
// too, so changing one without the other fails rather than producing a command
// that is well-formed for the wrong archiver.
TEST(ArchiveRemoveCommand, TheDialectRowsSupplyTheseTwoSpellings) {
    EXPECT_EQ(gnu_dialect().archiveRemoveArg, "d");
    EXPECT_TRUE(gnu_dialect().archiveRemoveTakesArchiveFirst);
    EXPECT_EQ(msvc_dialect().archiveRemoveArg, "/REMOVE:{}");
    EXPECT_FALSE(msvc_dialect().archiveRemoveTakesArchiveFirst);
}

// The failure that motivated the seam: the packer used to emit `ar` syntax
// unconditionally. Stated as what must NOT appear, because "it contains
// /REMOVE:" alone would stay green for a command that also carried the `d` verb
// or put the archive in the wrong place.
TEST(ArchiveRemoveCommand, TheMsvcFormCarriesNoArVerbAndNoLeadingArchive) {
    auto cmd = archive_remove_command("lib.exe", "bin/mathkit.lib", kMembers,
                                      msvc_dialect().archiveRemoveArg,
                                      msvc_dialect().archiveRemoveTakesArchiveFirst);
    EXPECT_EQ(cmd.find(" d "), std::string::npos);
    EXPECT_LT(cmd.find("/REMOVE:mathkit.m.o"), cmd.find("mathkit.lib"));
}

TEST(ArchiveRemoveCommand, ASingleMemberIsSpelledTheSameWayAsMany) {
    // The per-member branch is a loop; a one-element list is where an
    // "and-then-the-rest" bug hides.
    auto gnu = archive_remove_command("ar", "libx.a", { "only.m.o" },
                                      gnu_dialect().archiveRemoveArg, true);
    EXPECT_EQ(gnu, quote("ar") + " d " + quote("libx.a") + " " + quote("only.m.o"));
    auto msvc = archive_remove_command("lib.exe", "x.lib", { "only.m.o" },
                                       msvc_dialect().archiveRemoveArg, false);
    EXPECT_EQ(msvc, quote("lib.exe") + " " + quote("/REMOVE:only.m.o")
                      + " " + quote("x.lib"));
}

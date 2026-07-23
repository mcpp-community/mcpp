// #273 regression: patchelf_walk followed a symlinked payload directory out
// of the sandbox and corrupted the user's real ~/.xlings gcc installation.
// These tests pin the containment predicate that fences every rewrite in
// post_install: the trust root is the owning sandbox's registry
// (cfg.registryDir), canonicalized ONCE via containment_root() and threaded
// down explicitly; escapes_containment() compares a file's PHYSICAL
// (symlink-resolved) location against it and fails closed.
#include <gtest/gtest.h>

import std;
import mcpp.toolchain.post_install;

namespace fs = std::filesystem;
using mcpp::toolchain::containment_root;
using mcpp::toolchain::escapes_containment;

namespace {

// Temp tree reproducing the incident topology:
//   <tmp>/sandbox/registry/data/xpkgs/pkg-real/1.0/bin/tool     (real file)
//   <tmp>/foreign/pkg/1.0/bin/tool                              (real file)
//   <tmp>/sandbox/registry/data/xpkgs/pkg-link -> <tmp>/foreign/pkg
struct Fixture {
    fs::path tmp;
    fs::path registry, realPayload, foreign, linkedPayload;
    bool hasSymlink = false;   // Windows may lack symlink privilege → SKIP

    Fixture() {
        auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        tmp = fs::temp_directory_path() /
              fs::path(std::format("mcpp-containment-test-{}", tick));
        fs::remove_all(tmp);
        registry    = tmp / "sandbox" / "registry";
        realPayload = registry / "data" / "xpkgs" / "pkg-real" / "1.0";
        foreign     = tmp / "foreign" / "pkg";
        fs::create_directories(realPayload / "bin");
        fs::create_directories(foreign / "1.0" / "bin");
        std::ofstream(realPayload / "bin" / "tool") << "x";
        std::ofstream(foreign / "1.0" / "bin" / "tool") << "x";
        linkedPayload = registry / "data" / "xpkgs" / "pkg-link";
        std::error_code ec;
        fs::create_directory_symlink(foreign, linkedPayload, ec);
        hasSymlink = !ec;
    }
    ~Fixture() {
        std::error_code ec;
        fs::remove_all(tmp, ec);
    }
};

} // namespace

TEST(Containment, RootIsCanonicalRegistryDir) {
    Fixture f;
    EXPECT_EQ(containment_root(f.registry), fs::weakly_canonical(f.registry));
}

TEST(Containment, RealFileInsideRegistryDoesNotEscape) {
    Fixture f;
    auto root = containment_root(f.registry);
    EXPECT_FALSE(escapes_containment(f.realPayload / "bin" / "tool", root));
}

TEST(Containment, FileBehindSymlinkedPayloadEscapes) {
    Fixture f;
    if (!f.hasSymlink) GTEST_SKIP() << "no symlink privilege";
    // The incident's core: spelled under the registry, physically foreign.
    // Canonicalizing the payload path FIRST would make this a tautology —
    // the fence must be the independently-resolved registry root.
    auto root = containment_root(f.registry);
    EXPECT_TRUE(escapes_containment(f.linkedPayload / "1.0" / "bin" / "tool", root));
}

TEST(Containment, SymlinkedPayloadRootItselfEscapes) {
    Fixture f;
    if (!f.hasSymlink) GTEST_SKIP() << "no symlink privilege";
    // What the entry-point ownership guard checks before running any fixup.
    auto root = containment_root(f.registry);
    EXPECT_TRUE(escapes_containment(f.linkedPayload / "1.0", root));
}

TEST(Containment, DotDotTraversalEscapes) {
    Fixture f;
    auto root = containment_root(f.registry);
    // bin → 1.0 → pkg-real → xpkgs → data → registry → sandbox, then into
    // a path outside the registry: physically out even though the spelled
    // prefix starts inside it.
    auto sneaky = f.realPayload / "bin" / ".." / ".." / ".." / ".." / ".." / ".."
                / "elsewhere" / "tool";
    EXPECT_TRUE(escapes_containment(sneaky, root));
}

TEST(Containment, SiblingStringPrefixIsNotContained) {
    Fixture f;
    // "<registry>-evil" shares the raw string prefix but is a different
    // directory — the component-boundary check must reject it. (The old
    // inline guard compared raw strings and passed this.)
    auto evil = fs::path(f.registry.string() + "-evil");
    fs::create_directories(evil);
    std::ofstream(evil / "tool") << "x";
    auto root = containment_root(f.registry);
    EXPECT_TRUE(escapes_containment(evil / "tool", root));
}

TEST(Containment, EmptyRootFailsClosed) {
    Fixture f;
    // containment_root() returns empty when the registry cannot be resolved;
    // the predicate must then refuse everything rather than allow everything.
    EXPECT_TRUE(escapes_containment(f.realPayload / "bin" / "tool", {}));
}

TEST(Containment, RootItselfIsContained) {
    Fixture f;
    auto root = containment_root(f.registry);
    EXPECT_FALSE(escapes_containment(f.registry, root));
}

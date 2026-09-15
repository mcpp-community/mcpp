// #646 F1 -- where a static package under a shared library is linked.
//
// A dependency's shared library used to take only its own package's objects,
// so a static package beneath it went into the program and the library bound
// to the program's copy at run time. That works on ELF alone, and only for a
// program that links the package. `place_static_packages` decides the image
// each static package belongs in; these are the graph shapes the plan record
// enumerates (§1.2 of the implementation plan), asserted before any link line
// depends on them.
//
// Package 0 is the root. `shared` maps each shared package to its image count.

#include <gtest/gtest.h>

import std;
import mcpp.build.plan;

using mcpp::build::place_static_packages;

namespace {

using Edges = std::map<std::size_t, std::vector<std::size_t>>;

}  // namespace

// root -> fw (shared) -> x: x belongs in fw.
TEST(StaticPlacement, AStaticPackageUnderOneSharedLibraryGoesIntoIt) {
    Edges edges{{0, {1}}, {1, {2}}};
    auto p = place_static_packages(edges, {{1, 1}}, {});
    ASSERT_EQ(p.intoImage.size(), 1u);
    EXPECT_EQ(p.intoImage.at(2), 1u);
    EXPECT_TRUE(p.conflicts.empty());
}

// root -> fw -> a -> x, fw -> x: the whole static closure goes into fw.
TEST(StaticPlacement, ADiamondInsideOneImageStaysInThatImage) {
    Edges edges{{0, {1}}, {1, {2, 3}}, {2, {3}}};
    auto p = place_static_packages(edges, {{1, 1}}, {});
    EXPECT_EQ(p.intoImage.size(), 2u);
    EXPECT_EQ(p.intoImage.at(2), 1u);
    EXPECT_EQ(p.intoImage.at(3), 1u);
    EXPECT_TRUE(p.conflicts.empty());
}

// root -> fw1 -> x, root -> fw2 -> x: x has no single image.
TEST(StaticPlacement, TwoImagesReachingOnePackageIsAConflict) {
    Edges edges{{0, {1, 2}}, {1, {3}}, {2, {3}}};
    auto p = place_static_packages(edges, {{1, 1}, {2, 1}}, {});
    EXPECT_TRUE(p.intoImage.empty());
    ASSERT_EQ(p.conflicts.size(), 1u);
    EXPECT_EQ(p.conflicts[0].package, 3u);
    EXPECT_EQ(p.conflicts[0].images, (std::vector<std::size_t>{1, 2}));
    EXPECT_FALSE(p.conflicts[0].root);
}

// root -> fw -> x and root -> x: the program and the library both need it.
TEST(StaticPlacement, ThePackageTheRootAlsoLinksIsAConflictNamingTheRoot) {
    Edges edges{{0, {1, 2}}, {1, {2}}};
    auto p = place_static_packages(edges, {{1, 1}}, {});
    EXPECT_TRUE(p.intoImage.empty());
    ASSERT_EQ(p.conflicts.size(), 1u);
    EXPECT_EQ(p.conflicts[0].package, 2u);
    EXPECT_TRUE(p.conflicts[0].root);
}

// root (itself the shared image) -> x: nothing moves; the root's images
// already take every static package they reach.
TEST(StaticPlacement, ARootOwnedImageKeepsItsStaticPackages) {
    Edges edges{{0, {1}}};
    auto p = place_static_packages(edges, {}, {});
    EXPECT_TRUE(p.intoImage.empty());
    EXPECT_TRUE(p.conflicts.empty());
}

// root -> fw1 -> fw2 -> x: a closure stops at the next shared package, so x
// belongs to fw2 and not to fw1.
TEST(StaticPlacement, AClosureStopsAtTheNextSharedLibrary) {
    Edges edges{{0, {1}}, {1, {2}}, {2, {3}}};
    auto p = place_static_packages(edges, {{1, 1}, {2, 1}}, {});
    ASSERT_EQ(p.intoImage.size(), 1u);
    EXPECT_EQ(p.intoImage.at(3), 2u);
    EXPECT_TRUE(p.conflicts.empty());
}

// A package with two shared targets is two images: its static closure has no
// single home.
TEST(StaticPlacement, OnePackageWithTwoSharedTargetsIsTwoImages) {
    Edges edges{{0, {1}}, {1, {2}}};
    auto p = place_static_packages(edges, {{1, 2}}, {});
    EXPECT_TRUE(p.intoImage.empty());
    ASSERT_EQ(p.conflicts.size(), 1u);
    EXPECT_EQ(p.conflicts[0].package, 2u);
}

// A boundary (a distribution package, or a package providing a target layer)
// is neither placed nor walked through.
TEST(StaticPlacement, ABoundaryIsNeitherPlacedNorTraversed) {
    Edges edges{{0, {1}}, {1, {2}}, {2, {3}}};
    auto p = place_static_packages(edges, {{1, 1}}, {2});
    EXPECT_TRUE(p.intoImage.empty());
    EXPECT_TRUE(p.conflicts.empty());
}

// A header-only package placed into an image contributes no objects, and the
// placement is still well defined.
TEST(StaticPlacement, APackageWithNoObjectsIsPlacedLikeAnyOther) {
    Edges edges{{0, {1}}, {1, {2}}};
    auto p = place_static_packages(edges, {{1, 1}}, {});
    EXPECT_EQ(p.intoImage.at(2), 1u);
}

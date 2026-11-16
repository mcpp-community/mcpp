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


import std;
import mcpplibs.mtest;
import mcpp.build.plan;

using namespace mcpplibs::mtest;

using mcpp::build::place_static_packages;

namespace {

using Edges = std::map<std::size_t, std::vector<std::size_t>>;

}  // namespace

// root -> fw (shared) -> x: x belongs in fw.
UTest a_static_package_under_one_shared_library_goes_into_it{"StaticPlacement/AStaticPackageUnderOneSharedLibraryGoesIntoIt", [] {
    Edges edges{{0, {1}}, {1, {2}}};
    auto p = place_static_packages(edges, {{1, 1}}, {});
    expect(fatal(eq(p.intoImage.size(), 1u)));
    expect(eq(p.intoImage.at(2), 1u));
    expect(p.conflicts.empty());
}};

// root -> fw -> a -> x, fw -> x: the whole static closure goes into fw.
UTest a_diamond_inside_one_image_stays_in_that_image{"StaticPlacement/ADiamondInsideOneImageStaysInThatImage", [] {
    Edges edges{{0, {1}}, {1, {2, 3}}, {2, {3}}};
    auto p = place_static_packages(edges, {{1, 1}}, {});
    expect(eq(p.intoImage.size(), 2u));
    expect(eq(p.intoImage.at(2), 1u));
    expect(eq(p.intoImage.at(3), 1u));
    expect(p.conflicts.empty());
}};

// root -> fw1 -> x, root -> fw2 -> x: x has no single image.
UTest two_images_reaching_one_package_is_a_conflict{"StaticPlacement/TwoImagesReachingOnePackageIsAConflict", [] {
    Edges edges{{0, {1, 2}}, {1, {3}}, {2, {3}}};
    auto p = place_static_packages(edges, {{1, 1}, {2, 1}}, {});
    expect(p.intoImage.empty());
    expect(fatal(eq(p.conflicts.size(), 1u)));
    expect(eq(p.conflicts[0].package, 3u));
    expect(eq(p.conflicts[0].images, (std::vector<std::size_t>{1, 2})));
    expect(!(p.conflicts[0].root));
}};

// root -> fw -> x and root -> x: the program and the library both need it.
UTest the_package_the_root_also_links_is_a_conflict_naming_the_root{"StaticPlacement/ThePackageTheRootAlsoLinksIsAConflictNamingTheRoot", [] {
    Edges edges{{0, {1, 2}}, {1, {2}}};
    auto p = place_static_packages(edges, {{1, 1}}, {});
    expect(p.intoImage.empty());
    expect(fatal(eq(p.conflicts.size(), 1u)));
    expect(eq(p.conflicts[0].package, 2u));
    expect(bool(p.conflicts[0].root));
}};

// root (itself the shared image) -> x: nothing moves; the root's images
// already take every static package they reach.
UTest a_root_owned_image_keeps_its_static_packages{"StaticPlacement/ARootOwnedImageKeepsItsStaticPackages", [] {
    Edges edges{{0, {1}}};
    auto p = place_static_packages(edges, {}, {});
    expect(p.intoImage.empty());
    expect(p.conflicts.empty());
}};

// root -> fw1 -> fw2 -> x: a closure stops at the next shared package, so x
// belongs to fw2 and not to fw1.
UTest a_closure_stops_at_the_next_shared_library{"StaticPlacement/AClosureStopsAtTheNextSharedLibrary", [] {
    Edges edges{{0, {1}}, {1, {2}}, {2, {3}}};
    auto p = place_static_packages(edges, {{1, 1}, {2, 1}}, {});
    expect(fatal(eq(p.intoImage.size(), 1u)));
    expect(eq(p.intoImage.at(3), 2u));
    expect(p.conflicts.empty());
}};

// A package with two shared targets is two images: its static closure has no
// single home.
UTest one_package_with_two_shared_targets_is_two_images{"StaticPlacement/OnePackageWithTwoSharedTargetsIsTwoImages", [] {
    Edges edges{{0, {1}}, {1, {2}}};
    auto p = place_static_packages(edges, {{1, 2}}, {});
    expect(p.intoImage.empty());
    expect(fatal(eq(p.conflicts.size(), 1u)));
    expect(eq(p.conflicts[0].package, 2u));
}};

// A boundary (a distribution package, or a package providing a target layer)
// is neither placed nor walked through.
UTest a_boundary_is_neither_placed_nor_traversed{"StaticPlacement/ABoundaryIsNeitherPlacedNorTraversed", [] {
    Edges edges{{0, {1}}, {1, {2}}, {2, {3}}};
    auto p = place_static_packages(edges, {{1, 1}}, {2});
    expect(p.intoImage.empty());
    expect(p.conflicts.empty());
}};

// A header-only package placed into an image contributes no objects, and the
// placement is still well defined.
UTest a_package_with_no_objects_is_placed_like_any_other{"StaticPlacement/APackageWithNoObjectsIsPlacedLikeAnyOther", [] {
    Edges edges{{0, {1}}, {1, {2}}};
    auto p = place_static_packages(edges, {{1, 1}}, {});
    expect(eq(p.intoImage.at(2), 1u));
}};

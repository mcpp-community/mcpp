#include <gtest/gtest.h>

import std;
import mcpp.toolchain.post_install;

// detect_baked_loader parses gcc SPECS GRAMMAR, not plain text. The baked
// loader path is embedded inside %-spec conditionals, e.g.
//   %{mmusl:/lib/ld-musl-x86_64.so.1;:/baked/dir/ld-linux-x86-64.so.2}
// A scanner that treats "whitespace or :;" as the boundary swallows the
// closing braces; replacing that string then corrupts the spec grammar and
// EVERY subsequent g++ invocation dies with "braced spec body ... is
// invalid" (observed on CI). These tests pin the exact grammar shape.

namespace {

using mcpp::toolchain::detect_baked_loader;

// Realistic *link_spec fragment as xim bakes it (64-bit branch rewritten to
// the installing user's home; other multilib branches pristine).
const std::string kBakedSpecs =
    "*link:\n"
    "%{m16|m32|mx32:;:-m elf_x86_64} %{shared:-shared} %{!shared: %{!static: "
    "%{m16|m32:-dynamic-linker %{muclibc:/lib/ld-uClibc.so.0;:%{mbionic:/system/bin/linker;:"
    "%{mmusl:/lib/ld-musl-i386.so.1;:/lib/ld-linux.so.2}}}} "
    "%{m16|m32|mx32:;:-dynamic-linker %{muclibc:/lib/ld64-uClibc.so.0;:%{mbionic:/system/bin/linker64;:"
    "%{mmusl:/lib/ld-musl-x86_64.so.1;:/opt/other-home/.xlings/data/xpkgs/xim-x-glibc/2.39/lib64/ld-linux-x86-64.so.2}}}}} "
    "%{static:-static}}\n";

TEST(DetectBakedLoader, ExtractsExactPathWithoutSpecBraces) {
    auto got = detect_baked_loader(kBakedSpecs);
    EXPECT_EQ(got,
        "/opt/other-home/.xlings/data/xpkgs/xim-x-glibc/2.39/lib64/ld-linux-x86-64.so.2");
    // The regression: any brace in the result corrupts the specs on rewrite.
    EXPECT_EQ(got.find('}'), std::string::npos);
    EXPECT_EQ(got.find('{'), std::string::npos);
}

TEST(DetectBakedLoader, IgnoresPristineMultilibDefaults) {
    // An unbaked spec (all-standard /lib*/ paths) must not be rewritten.
    const std::string pristine =
        "%{mmusl:/lib/ld-musl-x86_64.so.1;:/lib64/ld-linux-x86-64.so.2} "
        "%{m16|m32:-dynamic-linker /lib/ld-linux.so.2} "
        "%{mx32:-dynamic-linker /libx32/ld-linux-x32.so.2}";
    EXPECT_EQ(detect_baked_loader(pristine), "");
}

TEST(DetectBakedLoader, Aarch64LoaderNameDetected) {
    const std::string specs =
        "-dynamic-linker %{mmusl:/lib/ld-musl-aarch64.so.1;:"
        "/srv/build/.xlings/xpkgs/xim-x-glibc/2.39/lib/ld-linux-aarch64.so.1}";
    EXPECT_EQ(detect_baked_loader(specs),
        "/srv/build/.xlings/xpkgs/xim-x-glibc/2.39/lib/ld-linux-aarch64.so.1");
}

TEST(DetectBakedLoader, EmptyWhenNoGnuLoaderPresent) {
    EXPECT_EQ(detect_baked_loader("no loaders here"), "");
    EXPECT_EQ(detect_baked_loader("%{mmusl:/lib/ld-musl-x86_64.so.1}"), "");
}

}  // namespace

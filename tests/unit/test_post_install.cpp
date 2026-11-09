#include <fstream>

import std;
import mcpplibs.mtest;
import mcpp.toolchain.post_install;
import mcpp.provider.xlings;
import mcpp.config;
import mcpp.toolchain.registry;

using namespace mcpplibs::mtest;

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

UTest extracts_exact_path_without_spec_braces{"DetectBakedLoader/ExtractsExactPathWithoutSpecBraces", [] {
    auto got = detect_baked_loader(kBakedSpecs);
    expect(eq(got, "/opt/other-home/.xlings/data/xpkgs/xim-x-glibc/2.39/lib64/ld-linux-x86-64.so.2"));
    // The regression: any brace in the result corrupts the specs on rewrite.
    expect(eq(got.find('}'), std::string::npos));
    expect(eq(got.find('{'), std::string::npos));
}};

UTest ignores_pristine_multilib_defaults{"DetectBakedLoader/IgnoresPristineMultilibDefaults", [] {
    // An unbaked spec (all-standard /lib*/ paths) must not be rewritten.
    const std::string pristine =
        "%{mmusl:/lib/ld-musl-x86_64.so.1;:/lib64/ld-linux-x86-64.so.2} "
        "%{m16|m32:-dynamic-linker /lib/ld-linux.so.2} "
        "%{mx32:-dynamic-linker /libx32/ld-linux-x32.so.2}";
    expect(eq(detect_baked_loader(pristine), ""));
}};

UTest aarch64_loader_name_detected{"DetectBakedLoader/Aarch64LoaderNameDetected", [] {
    const std::string specs =
        "-dynamic-linker %{mmusl:/lib/ld-musl-aarch64.so.1;:"
        "/srv/build/.xlings/xpkgs/xim-x-glibc/2.39/lib/ld-linux-aarch64.so.1}";
    expect(eq(detect_baked_loader(specs), "/srv/build/.xlings/xpkgs/xim-x-glibc/2.39/lib/ld-linux-aarch64.so.1"));
}};

UTest empty_when_no_gnu_loader_present{"DetectBakedLoader/EmptyWhenNoGnuLoaderPresent", [] {
    expect(eq(detect_baked_loader("no loaders here"), ""));
    expect(eq(detect_baked_loader("%{mmusl:/lib/ld-musl-x86_64.so.1}"), ""));
}};

}  // namespace

// The fixup gate tests (mcpp#427) went with the fixup: a payload is not
// written by mcpp (design 2026-10-16 §7.1), so there is no gate to test.
// What stays is read-only: the baked loader above, the glibc payload below.

// THE DECLARED VERSION NAMES ITS PAYLOAD DIRECTORY EXACTLY (mcpp#660).
//
// The lookup used to accept a unique `2.44.x` for a binding of `2.44` and to
// refuse two of them. That tolerance existed because the declared payload was
// never installed on purpose; it arrived as a side effect of another install,
// under whatever version that install resolved. It concealed the missing
// install, and it failed as soon as a cache held one revision and an install
// added the next:
//
//     error: toolchain post-install fixup: selected RuntimeBinding glibc@2.44
//            requires payload '.../xim-x-glibc/2.44', but no installed payload
//            is its resolution; mcpp will not fall back to another directory
//            entry
//     $ ls .../xim-x-glibc/   ->   2.44.2 2.44.3
//
// `ensure_declared_runtime` now installs the declared version, so the lookup
// is exact and a neighbouring version is never a substitute.
namespace {

// A payload is "installed" for this purpose when it has a lib dir with a
// loader in it — that is what select_glibc_payload_lib returns.
std::filesystem::path make_glibc_payload(const std::filesystem::path& root,
                                         std::string_view version) {
    auto lib = root / std::string(version) / "lib64";
    std::filesystem::create_directories(lib);
    std::ofstream(lib / "ld-linux-x86-64.so.2");
    return lib;
}

struct GlibcRootFixture {
    std::filesystem::path root;
    explicit GlibcRootFixture(std::string_view name)
        : root(std::filesystem::temp_directory_path() / name) {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root);
    }
    ~GlibcRootFixture() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

} // namespace

UTest the_exact_version_is_selected{"GlibcPayload/TheExactVersionIsSelected", [] {
    GlibcRootFixture fx{"mcpp_glibc_exact"};
    auto want = make_glibc_payload(fx.root, "2.44.3");
    make_glibc_payload(fx.root, "2.44.2");
    auto got = mcpp::toolchain::select_glibc_payload_lib(fx.root, "glibc@2.44.3");
    if (auto&& failed = expect(fatal(got.has_value())); !failed) failed << got.error();
    expect(eq(*got, want));
}};

// THE CASE OF mcpp#660. Two revisions of the line are present and the declared
// version is neither; the answer is "not installed", naming the coordinate
// that provides it, and never one of the two.
UTest a_neighbouring_revision_is_not_a_substitute{"GlibcPayload/ANeighbouringRevisionIsNotASubstitute", [] {
    GlibcRootFixture fx{"mcpp_glibc_neighbour"};
    make_glibc_payload(fx.root, "2.44.2");
    make_glibc_payload(fx.root, "2.44.3");
    auto got = mcpp::toolchain::select_glibc_payload_lib(fx.root, "glibc@2.44");
    expect(fatal(!got.has_value()));
    if (auto&& failed = expect(ne(got.error().find("xim:glibc@2.44"), std::string::npos)); !failed) failed << got.error();
    if (auto&& failed = expect(ne(got.error().find("not installed"), std::string::npos)); !failed) failed << got.error();

    // One neighbour is not a substitute either: the old tolerance accepted it.
    GlibcRootFixture one{"mcpp_glibc_one_neighbour"};
    make_glibc_payload(one.root, "2.44.2");
    expect(!mcpp::toolchain::select_glibc_payload_lib(one.root, "glibc@2.44"));
}};

UTest nothing_installed_is_still_refused{"GlibcPayload/NothingInstalledIsStillRefused", [] {
    GlibcRootFixture fx{"mcpp_glibc_empty"};
    auto got = mcpp::toolchain::select_glibc_payload_lib(fx.root, "glibc@2.44");
    expect(!got.has_value());
}};

// A DIRECTORY WITHOUT A LOADER IS REPORTED AS INCOMPLETE, NOT AS ABSENT. The
// two call for different repairs, so the messages must not coincide.
UTest a_payload_without_a_loader_is_incomplete{"GlibcPayload/APayloadWithoutALoaderIsIncomplete", [] {
    GlibcRootFixture fx{"mcpp_glibc_husk"};
    std::filesystem::create_directories(fx.root / "2.44.3");
    auto got = mcpp::toolchain::select_glibc_payload_lib(fx.root, "glibc@2.44.3");
    expect(fatal(!got.has_value()));
    if (auto&& failed = expect(ne(got.error().find("stale/incomplete"), std::string::npos)); !failed) failed << got.error();
}};

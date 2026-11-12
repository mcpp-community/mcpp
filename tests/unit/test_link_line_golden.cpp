// THE LINK LINE'S C-LIBRARY GROUP, AS A TABLE (design 2026-11-12 §5.4).
//
// `compute_flags` renders, for every link, the words that name the C library:
// the crt search prefix, the library directories, the loader and the sysroot.
// Which words appear is decided by four facts -- the compiler family, who
// supplies the C library, whether this machine loads the artifact, and the
// linkage -- and each combination has one right answer. This file writes the
// answers down. A change to the renderer that loses a group shows here as one
// differing line, without a toolchain and without a link.
//
// The measurement behind it: a rewrite of the GCC branch dropped `-B`, `-L`
// and the loader of every gcc build over the host's payload (the crt objects
// then resolved from /lib), and no check of the tree asked for those words.
//
// The fixtures are directories of empty files: the renderer reads paths and
// the presence of a loader and of a driver's `.cfg`, never file contents.

import std;
import mcpplibs.mtest;
import mcpp.build.flags;
import mcpp.build.plan;
import mcpp.manifest;
import mcpp.targetside;
import mcpp.toolchain.base;
import mcpp.toolchain.model;
import mcpplibs.os.platform;

using namespace mcpplibs::mtest;
using namespace mcpp::build;
namespace tc = mcpp::toolchain;
namespace ts = mcpp::targetside;
namespace fs = std::filesystem;

namespace {

struct Tree {
    fs::path root;
    Tree() {
        static std::atomic<int> n{0};
        root = fs::temp_directory_path()
             / std::format("mcpp-link-golden-{}-{}", std::chrono::steady_clock::now().time_since_epoch().count(), n++);
        fs::create_directories(root);
    }
    ~Tree() { std::error_code ec; fs::remove_all(root, ec); }
    fs::path file(const fs::path& rel) const {
        fs::create_directories((root / rel).parent_path());
        std::ofstream(root / rel) << "";
        return root / rel;
    }
    fs::path dir(const fs::path& rel) const {
        fs::create_directories(root / rel);
        return root / rel;
    }
};

BuildPlan plan_for(tc::CompilerId compiler, const fs::path& driver, std::string triple) {
    BuildPlan plan;
    plan.projectRoot = fs::temp_directory_path() / "mcpp-link-golden";
    plan.outputDir = plan.projectRoot / "target" / "test";
    plan.manifest.package.name = "golden";
    plan.toolchain.compiler = compiler;
    plan.toolchain.version = "test";
    plan.toolchain.binaryPath = driver;
    plan.toolchain.targetTriple = std::move(triple);
    const bool clang = compiler == tc::CompilerId::Clang;
    plan.targetSide.compiler  = { ts::Origin::Payload, clang ? "llvm" : "gcc", "", false };
    plan.targetSide.kernelAbi = { ts::Origin::Payload, "linux", "", false };
    plan.targetSide.cAbi      = { ts::Origin::Payload, "glibc", "", false };
    plan.targetSide.cxx       = { ts::Origin::Payload, clang ? "libc++" : "libstdc++", "", false };
    return plan;
}

// The words of `line` that name the C library, in order, with `tree` written
// as `<t>`: what the table below compares.
std::vector<std::string> c_library_words(std::string_view line, const fs::path& tree) {
    std::vector<std::string> out;
    const auto t = tree.generic_string();
    for (auto part : std::views::split(line, ' ')) {
        std::string w(part.begin(), part.end());
        const bool named = w.starts_with("-B") || w.starts_with("-L") || w.starts_with("--sysroot")
            || w.starts_with("-Wl,--sysroot") || w.starts_with("-Wl,-rpath")
            || w.starts_with("-Wl,--dynamic-linker") || w.starts_with("--target=");
        if (!named) continue;
        for (auto at = w.find(t); at != std::string::npos; at = w.find(t, at + 3)) w.replace(at, t.size(), "<t>");
        out.push_back(std::move(w));
    }
    return out;
}

using Words = std::vector<std::string>;

void expect_words(std::string_view what, const Words& got, const Words& want) {
    if (auto&& failed = expect(eq(got, want)); !failed) failed << what;
}

}  // namespace

// gcc over the host's payload: the crt prefix, the library directory with its
// run-time path, the compiler's own run-time directory, and the payload's
// loader. The regression this file exists for emptied this list.
UTest gcc_over_the_hosts_payload{"LinkLineGolden/GccOverTheHostsPayload", [] {
    if constexpr (!mcpplibs::os::is_linux) skip("the ELF link line is rendered on a Linux host");
    Tree t;
    const auto glibc = t.dir("glibc/lib64");
    t.file("glibc/lib64/ld-linux-x86-64.so.2");
    t.dir("glibc/include");
    t.dir("gcc/lib64");
    auto plan = plan_for(tc::CompilerId::GCC, t.file("gcc/bin/g++"), "x86_64-linux-gnu");
    plan.toolchain.base.payload = tc::PayloadPaths{ t.root / "glibc/include", glibc, {} };
    const auto f = compute_flags(plan);
    const Words want{ "-B<t>/glibc/lib64", "-L<t>/glibc/lib64", "-Wl,-rpath,<t>/glibc/lib64",
                      "-L<t>/gcc/lib64", "-Wl,-rpath,<t>/gcc/lib64",
                      "-Wl,--dynamic-linker=<t>/glibc/lib64/ld-linux-x86-64.so.2" };
    expect_words("the C++ link", c_library_words(f.sysroot, t.root), want);
}};

// clang with its configuration over the host's payload: the same group, the
// compiler's run-time being compiler-rt's and not a directory of the list.
UTest clang_over_the_hosts_payload{"LinkLineGolden/ClangOverTheHostsPayload", [] {
    if constexpr (!mcpplibs::os::is_linux) skip("the ELF link line is rendered on a Linux host");
    Tree t;
    const auto glibc = t.dir("glibc/lib64");
    t.file("glibc/lib64/ld-linux-x86-64.so.2");
    t.dir("glibc/include");
    t.file("llvm/bin/clang++.cfg");
    t.dir("llvm/include/c++/v1");
    auto plan = plan_for(tc::CompilerId::Clang, t.file("llvm/bin/clang++"), "x86_64-linux-gnu");
    plan.toolchain.base.payload = tc::PayloadPaths{ t.root / "glibc/include", glibc, {} };
    const auto f = compute_flags(plan);
    const Words want{ "-B<t>/glibc/lib64", "-L<t>/glibc/lib64", "-Wl,-rpath,<t>/glibc/lib64",
                      "-Wl,--dynamic-linker=<t>/glibc/lib64/ld-linux-x86-64.so.2" };
    expect_words("the C++ link", c_library_words(f.ld, t.root), want);
    expect_words("the C link", c_library_words(f.ldC, t.root), want);
}};

// A base package of another machine (SPEC-016 §6): the sysroot and the row's
// loader, and nothing of this machine -- no directory of the payload, no
// run-time path. The artifact's interpreter is a path on the machine that
// runs it.
UTest clang_over_a_base_of_another_machine{"LinkLineGolden/ClangOverABaseOfAnotherMachine", [] {
    if constexpr (!mcpplibs::os::is_linux) skip("the ELF link line is rendered on a Linux host");
    Tree t;
    const auto base = t.dir("base");
    t.file("llvm/bin/clang++.cfg");
    t.dir("llvm/include/c++/v1");
    auto plan = plan_for(tc::CompilerId::Clang, t.file("llvm/bin/clang++"), "aarch64-linux-gnu");
    plan.toolchain.crossTargetFlag = "--target=aarch64-unknown-linux-gnu";
    plan.toolchain.base.root = base;
    plan.toolchain.base.form = tc::base::Form::Package;
    plan.toolchain.base.text = "xim:aarch64-linux-gnu-sysroot@2.44.3";
    plan.toolchain.base.runtime = tc::base::Runtime::Floor;
    plan.toolchain.base.interpreter = "/lib/ld-linux-aarch64.so.1";
    plan.toolchain.base.layers.cAbi = "glibc";
    plan.targetSide.cAbi = { ts::Origin::Xpkg, "glibc", "xim:aarch64-linux-gnu-sysroot@2.44.3", false };
    const auto f = compute_flags(plan);
    const Words want{ "--target=aarch64-unknown-linux-gnu", "--sysroot=<t>/base",
                      "-Wl,--dynamic-linker=/lib/ld-linux-aarch64.so.1" };
    expect_words("the C++ link", c_library_words(f.sysroot, t.root), want);
    // No word of the line names a directory of this machine for the run time.
    expect(eq(f.ld.find("-rpath"), std::string::npos)) << f.ld;
}};

// The same base, linked statically: no interpreter is named, since a static
// program has none.
UTest a_static_link_over_a_base_names_no_loader{"LinkLineGolden/AStaticLinkOverABaseNamesNoLoader", [] {
    if constexpr (!mcpplibs::os::is_linux) skip("the ELF link line is rendered on a Linux host");
    Tree t;
    const auto base = t.dir("base");
    t.file("llvm/bin/clang++.cfg");
    t.dir("llvm/include/c++/v1");
    auto plan = plan_for(tc::CompilerId::Clang, t.file("llvm/bin/clang++"), "aarch64-linux-gnu");
    plan.toolchain.crossTargetFlag = "--target=aarch64-unknown-linux-gnu";
    plan.toolchain.base.root = base;
    plan.toolchain.base.form = tc::base::Form::Package;
    plan.toolchain.base.text = "xim:aarch64-linux-gnu-sysroot@2.44.3";
    plan.toolchain.base.runtime = tc::base::Runtime::Floor;
    plan.targetSide.cAbi = { ts::Origin::Xpkg, "glibc", "xim:aarch64-linux-gnu-sysroot@2.44.3", false };
    const auto f = compute_flags(plan);
    const Words want{ "--target=aarch64-unknown-linux-gnu", "--sysroot=<t>/base" };
    expect_words("the C++ link", c_library_words(f.sysroot, t.root), want);
}};

// The C library from the dependency graph: the host's model contributes no
// word. What the link names is the graph's, stated by its packages.
UTest the_graph_supplies_the_c_library{"LinkLineGolden/TheGraphSuppliesTheCLibrary", [] {
    if constexpr (!mcpplibs::os::is_linux) skip("the ELF link line is rendered on a Linux host");
    Tree t;
    const auto glibc = t.dir("glibc/lib64");
    t.file("glibc/lib64/ld-linux-x86-64.so.2");
    t.file("llvm/bin/clang++.cfg");
    t.dir("llvm/include/c++/v1");
    auto plan = plan_for(tc::CompilerId::Clang, t.file("llvm/bin/clang++"), "x86_64-linux-musl");
    plan.toolchain.base.payload = tc::PayloadPaths{ t.root / "glibc/include", glibc, {} };
    plan.targetSide.kernelAbi = { ts::Origin::Graph, "openkal", "openkal-linux@0", false };
    plan.targetSide.cAbi = { ts::Origin::Graph, "musl", "openkal-musl@0", false };
    plan.targetSide.cxx = { ts::Origin::Graph, "libc++", "openkal-llvm-runtime@0", false };
    const auto f = compute_flags(plan);
    expect_words("the link's own group", c_library_words(f.sysroot, t.root), Words{});
}};

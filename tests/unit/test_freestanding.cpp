#include <gtest/gtest.h>

import std;
import mcpp.freestanding.target;
import mcpp.freestanding.linkline;
import mcpp.freestanding.runner;
import mcpp.toolchain.triple;
import mcpp.platform.xlings;
import mcpp.build.build_program;

using namespace mcpp::freestanding;

// ── the target table ────────────────────────────────────────────────────────

TEST(FreestandingTarget, ResolvesTheKnownBareMetalTriples) {
    auto rv64 = resolve("riscv64-none-elf");
    ASSERT_TRUE(rv64.has_value());
    EXPECT_EQ(rv64->march,  "rv64gc");
    EXPECT_EQ(rv64->mabi,   "lp64d");
    EXPECT_EQ(rv64->libdir, "rv64gc/lp64d");

    auto rv32 = resolve("riscv32-none-elf");
    ASSERT_TRUE(rv32.has_value());
    EXPECT_EQ(rv32->march,  "rv32imac");
    EXPECT_EQ(rv32->mabi,   "ilp32");
    EXPECT_EQ(rv32->libdir, "rv32imac/ilp32");
}

TEST(FreestandingTarget, HostedTriplesResolveToNothing) {
    // The same call answers "is this a target I know how to build
    // freestanding", so a hosted triple must not produce a Spec — a caller
    // that got one would put -ffreestanding on an ordinary Linux build.
    for (const char* s : { "x86_64-linux-gnu", "x86_64-linux-musl",
                           "aarch64-macos", "x86_64-windows-gnu" }) {
        EXPECT_FALSE(resolve(s).has_value()) << s;
    }
}

TEST(FreestandingTarget, UnknownBareMetalTripleIsNotInvented) {
    // Parses as freestanding, but mcpp has no ISA profile for it. Returning a
    // Spec here would mean guessing an -march, and a wrong -march produces an
    // object file that links and then executes garbage.
    auto t = mcpp::toolchain::triple::parse("arm-none-eabi");
    ASSERT_TRUE(t.has_value());
    EXPECT_TRUE(t->is_freestanding());
    EXPECT_FALSE(resolve(*t).has_value());
}

TEST(FreestandingTarget, CompileFlagsCarryTheIsaAndFreestanding) {
    auto s = resolve("riscv64-none-elf");
    ASSERT_TRUE(s.has_value());
    auto fl = compile_flags(*s);
    auto has = [&](std::string_view f) {
        return std::ranges::find(fl, f) != fl.end();
    };
    EXPECT_TRUE(has("-march=rv64gc"));
    EXPECT_TRUE(has("-mabi=lp64d"));
    EXPECT_TRUE(has("-mcmodel=medany"));
    // -ffreestanding is a COMPILE flag, not a link one: it changes what the
    // compiler may assume about the library, and that has to hold for every
    // TU including a dependency's.
    EXPECT_TRUE(has("-ffreestanding"));
    // The toolchain's libc++ headers are the HOST's: `#include <stdio.h>`
    // resolves to libc++'s wrapper, which opens a `__config_site` generated
    // for the host and absent here. A target-side C++ library reaches a
    // consumer as a MODULE, never as an include path.
    EXPECT_TRUE(has("-nostdinc++"));
}

// ── the link line ───────────────────────────────────────────────────────────

TEST(FreestandingLink, CarriesTargetSelectionAndRefusesTheHostedWorld) {
    auto s = resolve("riscv64-none-elf");
    ASSERT_TRUE(s.has_value());
    LinkInputs in;
    in.lld = "/payload/bin/ld.lld";
    auto line = link_flags(*s, in, [](const std::filesystem::path& p) {
        return p.string();
    });

    // --target is the load-bearing one: clang is ONE binary for every target,
    // so without it the link silently produces host objects.
    EXPECT_NE(line.find("--target=riscv64-none-elf"), std::string::npos);
    EXPECT_NE(line.find("-nostdlib"), std::string::npos);
    EXPECT_NE(line.find("-nostartfiles"), std::string::npos);
    EXPECT_NE(line.find("-static"), std::string::npos);
}

TEST(FreestandingLink, AddressesTheLinkerByPathNotByName) {
    auto s = resolve("riscv64-none-elf");
    ASSERT_TRUE(s.has_value());
    LinkInputs in;
    in.lld = "/payload/bin/ld.lld";
    auto line = link_flags(*s, in, [](const std::filesystem::path& p) {
        return p.string();
    });
    // `-fuse-ld=lld` resolves through PATH and finds GNU ld on any machine
    // with binutils earlier on it, which then dies with "unrecognised
    // emulation mode: elf64lriscv". Reproduced 2026-08-19.
    EXPECT_NE(line.find("-fuse-ld=/payload/bin/ld.lld"), std::string::npos);
    EXPECT_EQ(line.find("-fuse-ld=lld "), std::string::npos);
}

TEST(FreestandingLink, LinkerScriptOnlyWhenOneIsSupplied) {
    auto s = resolve("riscv64-none-elf");
    ASSERT_TRUE(s.has_value());
    auto esc = [](const std::filesystem::path& p) { return p.string(); };

    LinkInputs none;
    EXPECT_EQ(link_flags(*s, none, esc).find(" -T "), std::string::npos);

    LinkInputs with;
    with.linkerScript = "/board/link.ld";
    EXPECT_NE(link_flags(*s, with, esc).find(" -T /board/link.ld"),
              std::string::npos);
}

TEST(FreestandingLink, LldIsIdentifiedByItsOwnVersionOutput) {
    // By output, not by filename — the filename is exactly what was wrong in
    // the failure this guards.
    EXPECT_TRUE(version_output_is_lld("LLD 22.1.8 (compatible with GNU linkers)"));
    EXPECT_FALSE(version_output_is_lld("GNU ld (GNU Binutils for Ubuntu) 2.42"));
}

// ── the runner ──────────────────────────────────────────────────────────────

TEST(FreestandingRunner, AppendsTheArtifactWhenNoPlaceholder) {
    std::vector<std::string> tmpl{ "qemu-system-riscv64", "-machine", "virt",
                                   "-kernel" };
    auto argv = expand(tmpl, "/build/firmware");
    ASSERT_EQ(argv.size(), 5u);
    EXPECT_EQ(argv.front(), "qemu-system-riscv64");
    EXPECT_EQ(argv.back(),  "/build/firmware");
}

TEST(FreestandingRunner, SubstitutesThePlaceholderInPlace) {
    // The emulator that wants the image in the middle rather than at the end.
    std::vector<std::string> tmpl{ "runner", "--image={}", "--verbose" };
    auto argv = expand(tmpl, "/build/firmware");
    ASSERT_EQ(argv.size(), 3u);
    EXPECT_EQ(argv[1], "--image=/build/firmware");
    EXPECT_EQ(argv[2], "--verbose");   // nothing appended when substituted
}

TEST(FreestandingRunner, TheMissingRunnerMessageNamesTheKeyAndTheTriple) {
    // The user has just been told the build succeeded and the run did not; the
    // gap between those two is the whole content of the message, so it has to
    // carry something pasteable.
    auto msg = no_runner_message("riscv64-none-elf");
    EXPECT_NE(msg.find("[target.riscv64-none-elf]"), std::string::npos);
    EXPECT_NE(msg.find("runner = ["), std::string::npos);
    EXPECT_NE(msg.find("board-support package"), std::string::npos);
}

// ── `[xlings] deps` → payload directory ─────────────────────────────────────
//
// The channel a board-support package uses to find the libc payload it
// declared. An INTERFACE rather than a naming convention: a build.mcpp that
// reconstructed `<home>/data/xpkgs/<ns>-x-<name>/<version>` would be coupled
// to store internals mcpp is free to change.

TEST(XpkgRef, ParsesEverySpellingAManifestMayWrite) {
    using mcpp::xlings::paths::parse_xpkg_ref;

    auto full = parse_xpkg_ref("xim:picolibc-riscv@1.8.12");
    EXPECT_EQ(full.ns, "xim");
    EXPECT_EQ(full.name, "picolibc-riscv");
    EXPECT_EQ(full.version, "1.8.12");

    // No namespace: the manifest's default, spelled once in the parser rather
    // than at each call site.
    auto bare = parse_xpkg_ref("ninja");
    EXPECT_EQ(bare.ns, "xim");
    EXPECT_EQ(bare.name, "ninja");
    EXPECT_TRUE(bare.version.empty());

    // A non-xim namespace is ordinary, not exotic: `local:` is what a package
    // under development resolves to.
    auto local = parse_xpkg_ref("local:qemu-riscv@9.2.4-1");
    EXPECT_EQ(local.ns, "local");
    EXPECT_EQ(local.name, "qemu-riscv");
    EXPECT_EQ(local.version, "9.2.4-1");
}

TEST(XpkgPayload, APinnedRefResolvesToThatVersionOrToNothing) {
    namespace xp = mcpp::xlings::paths;
    auto base = std::filesystem::temp_directory_path()
              / std::format("mcpp-xpkg-test-{}", ::getpid());
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base / "xim-x-demo" / "1.0.0");
    std::filesystem::create_directories(base / "xim-x-demo" / "2.0.0");

    // ⚠️ The whole point: asking for 1.0.0 and silently getting 2.0.0 is an
    // answer only discovered later, in the artifact.
    auto pinned = xp::xpkg_payload_at(base, xp::parse_xpkg_ref("xim:demo@1.0.0"));
    ASSERT_TRUE(pinned.has_value());
    EXPECT_EQ(pinned->filename(), "1.0.0");

    EXPECT_FALSE(xp::xpkg_payload_at(base, xp::parse_xpkg_ref("xim:demo@9.9.9"))
                     .has_value());

    std::filesystem::remove_all(base);
}

TEST(XpkgPayload, AnUnpinnedRefTakesTheHighestVersionNumerically) {
    namespace xp = mcpp::xlings::paths;
    auto base = std::filesystem::temp_directory_path()
              / std::format("mcpp-xpkg-test2-{}", ::getpid());
    std::filesystem::remove_all(base);
    for (auto v : { "0.4.2", "0.4.9", "0.4.11" })
        std::filesystem::create_directories(base / "xim-x-demo" / v);

    // A plain string sort answers 0.4.9; picking the wrong payload is silent.
    auto latest = xp::xpkg_payload_at(base, xp::parse_xpkg_ref("demo"));
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->filename(), "0.4.11");

    std::filesystem::remove_all(base);
}

TEST(XpkgEnvVar, BothSpellingsAreDerivedFromOneSanitizer) {
    using mcpp::build::xpkg_env_var;
    // The two sides of the channel must agree; drifting apart would make the
    // interface answer "" for a package that is right there.
    EXPECT_EQ(xpkg_env_var("xim", "picolibc-riscv"),
              "MCPP_XPKG_XIM_PICOLIBC_RISCV_DIR");
    EXPECT_EQ(xpkg_env_var("", "picolibc-riscv"),
              "MCPP_XPKG_PICOLIBC_RISCV_DIR");
}

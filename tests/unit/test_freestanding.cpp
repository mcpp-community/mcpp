#include <gtest/gtest.h>

import std;
import mcpp.freestanding.target;
import mcpp.freestanding.linkline;
import mcpp.freestanding.runner;
import mcpp.toolchain.triple;

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

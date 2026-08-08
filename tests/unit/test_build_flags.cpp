#include <gtest/gtest.h>

import std;
import mcpp.build.flags;
import mcpp.modgraph.scanner;

namespace {

struct Tmp {
    std::filesystem::path path;
    Tmp() {
        path = std::filesystem::temp_directory_path()
             / std::format("mcpp_flags_test_{}", std::random_device{}());
        std::filesystem::create_directories(path);
    }
    ~Tmp() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

void touch(const std::filesystem::path& p) {
    std::ofstream(p) << "x";
}

// libatomic (a GCC runtime lib; LLVM ships no equivalent) provides the
// out-of-line __atomic_* libcalls that 16-byte/oversized std::atomic lowers
// to. Compiler drivers don't auto-link it, so mcpp must inject `-latomic`.
// But `--as-needed` does NOT skip a missing library — the linker still has to
// open it — so the flag may only be emitted when a libatomic actually exists
// on the toolchain's link dirs, else it would break toolchains that omit it.

constexpr bool kDynamic = false;  // staticLink arg
constexpr bool kStatic  = true;

TEST(BuildFlagsAtomic, EmittedWhenLibatomicSharedPresent) {
    Tmp dir;
    touch(dir.path / "libatomic.so");
    auto flag = mcpp::build::atomic_link_flag({dir.path}, kDynamic);
    EXPECT_NE(flag.find("-latomic"), std::string::npos);
    EXPECT_NE(flag.find("--as-needed"), std::string::npos);
}

TEST(BuildFlagsAtomic, EmittedWhenLibatomicArchivePresent) {
    Tmp dir;
    touch(dir.path / "libatomic.a");
    auto flag = mcpp::build::atomic_link_flag({dir.path}, kDynamic);
    EXPECT_NE(flag.find("-latomic"), std::string::npos);
}

TEST(BuildFlagsAtomic, EmptyWhenLibatomicAbsent) {
    Tmp dir;
    touch(dir.path / "libc++.so.1");  // some other lib, but no libatomic
    auto flag = mcpp::build::atomic_link_flag({dir.path}, kDynamic);
    EXPECT_TRUE(flag.empty()) << "got: " << flag;
}

// `-latomic` resolves only `libatomic.so` / `libatomic.a` — a bare
// soname-versioned `libatomic.so.1` (no dev symlink, no archive) is NOT
// link-resolvable, so the flag must stay empty or the link breaks with
// "cannot find -latomic".
TEST(BuildFlagsAtomic, EmptyWhenOnlySonameVersionedPresent) {
    Tmp dir;
    touch(dir.path / "libatomic.so.1");
    touch(dir.path / "libatomic.so.1.2.0");
    auto flag = mcpp::build::atomic_link_flag({dir.path}, kDynamic);
    EXPECT_TRUE(flag.empty()) << "got: " << flag;
}

TEST(BuildFlagsAtomic, ScansAllDirsNotJustFirst) {
    Tmp a, b;
    touch(b.path / "libatomic.so");  // link-resolvable, in a later dir
    auto flag = mcpp::build::atomic_link_flag({a.path, b.path}, kDynamic);
    EXPECT_NE(flag.find("-latomic"), std::string::npos);
}

// Cross-platform / cross-linkage: a full-static link (`-static`, e.g. musl
// targets) resolves `-latomic` only from `libatomic.a`. A lone `libatomic.so`
// is not usable, so the flag must stay empty under static linkage to avoid
// "cannot find -latomic".
TEST(BuildFlagsAtomic, StaticLinkEmptyWhenOnlySharedPresent) {
    Tmp dir;
    touch(dir.path / "libatomic.so");
    auto flag = mcpp::build::atomic_link_flag({dir.path}, kStatic);
    EXPECT_TRUE(flag.empty()) << "got: " << flag;
}

TEST(BuildFlagsAtomic, StaticLinkEmittedWhenArchivePresent) {
    Tmp dir;
    touch(dir.path / "libatomic.a");
    auto flag = mcpp::build::atomic_link_flag({dir.path}, kStatic);
    EXPECT_NE(flag.find("-latomic"), std::string::npos);
}

// mcpp#226: normalize_include_flags generalizes the old -I-only
// absolutize_include_flags to the whole include/lib-search-path family, in
// BOTH the joined spelling (`-iquotehdr`) and the separated spelling
// (`-isystem` followed by a standalone next element). All four of these
// project-relative paths must resolve to the same "/proj/hdr" target.
//
// The expected spelling is NATIVE (#390): `-I/abs/hdr` written with forward
// slashes keeps them on MSVC, and the old expectation `(root / "hdr").string()`
// was itself the mixed `"/proj\hdr"` shape this family of bugs produced.
// make_preferred() makes the expectation platform-correct on both sides.
TEST(BuildFlags, NormalizeIncludeFlagsRewritesFullIncludeFamily) {
    std::filesystem::path root = "/proj";
    auto expected = [](const std::filesystem::path& p) {
        auto n = p;
        n.make_preferred();
        return n.string();
    };
    std::vector<std::string> flags = {
        "-Ihdr", "-iquotehdr", "-isystem", "hdr", "-idirafterhdr",
    };

    mcpp::modgraph::normalize_include_flags(root, flags);

    ASSERT_EQ(flags.size(), 5u);
    EXPECT_EQ(flags[0], "-I" + expected(root / "hdr"));
    EXPECT_EQ(flags[1], "-iquote" + expected(root / "hdr"));
    EXPECT_EQ(flags[2], "-isystem");                       // prefix itself untouched
    EXPECT_EQ(flags[3], expected(root / "hdr"));           // separated element rewritten
    EXPECT_EQ(flags[4], "-idirafter" + expected(root / "hdr"));
}

// Absolute paths and root-relative spellings are left alone (matches the
// pre-#226 -I behavior), for both the joined and separated forms — only the
// separator spelling is normalized to native (#390; a no-op on POSIX).
TEST(BuildFlags, NormalizeIncludeFlagsLeavesAbsolutePathsAlone) {
    std::filesystem::path root = "/proj";
    auto expected = [](const std::filesystem::path& p) {
        auto n = p;
        n.make_preferred();
        return n.string();
    };
    std::vector<std::string> flags = {
        "-I/abs/hdr", "-isystem", "/abs/hdr", "-DKEEP",
    };

    mcpp::modgraph::normalize_include_flags(root, flags);

    EXPECT_EQ(flags[0], "-I" + expected("/abs/hdr"));
    EXPECT_EQ(flags[1], "-isystem");
    EXPECT_EQ(flags[2], expected("/abs/hdr"));
    EXPECT_EQ(flags[3], "-DKEEP");
}

}  // namespace

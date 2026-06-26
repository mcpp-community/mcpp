#include <gtest/gtest.h>

import std;
import mcpp.build.flags;

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

}  // namespace

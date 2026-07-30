#include <gtest/gtest.h>
#include <cstdlib>

import std;
import mcpp.platform.env;

namespace {

class ScopedVar {
public:
    ScopedVar(std::string name, const char* value) : name_(std::move(name)) {
        if (const char* old = std::getenv(name_.c_str()); old) { had_ = true; old_ = old; }
        apply(value);
    }
    ~ScopedVar() { apply(had_ ? old_.c_str() : nullptr); }
    ScopedVar(const ScopedVar&) = delete;
    ScopedVar& operator=(const ScopedVar&) = delete;
private:
    void apply(const char* v) {
#if defined(_WIN32)
        ::_putenv_s(name_.c_str(), v ? v : "");
#else
        if (v) ::setenv(name_.c_str(), v, 1); else ::unsetenv(name_.c_str());
#endif
    }
    std::string name_;
    bool had_ = false;
    std::string old_;
};

std::string sep() { return mcpp::platform::env::path_list_separator(); }

std::string join(std::initializer_list<std::string_view> parts) {
    std::string out;
    for (auto p : parts) {
        if (!out.empty()) out += sep();
        out += p;
    }
    return out;
}

}  // namespace

TEST(PlatformEnv, StripPrivateGlibcDropsOnlyPayloadEntries) {
    auto in = join({"/usr/lib", "/home/u/.mcpp/registry/data/xpkgs/xim-x-glibc/2.39/lib64",
                    "/opt/mine"});
    EXPECT_EQ(mcpp::platform::env::strip_private_glibc(in),
              join({"/usr/lib", "/opt/mine"}));
}

TEST(PlatformEnv, StripPrivateGlibcCanEmptyTheList) {
    EXPECT_EQ(mcpp::platform::env::strip_private_glibc(
                  "/x/xpkgs/xim-x-glibc/2.42/lib64"), "");
    EXPECT_EQ(mcpp::platform::env::strip_private_glibc(""), "");
}

// The regression this guards (mcpp#311 investigation): the composed value
// becomes an EXPLICIT override, and process.cppm's merged_environ takes explicit
// overrides verbatim — skipping the sanitation it applies to inherited
// variables. So if the inherited tail carried an outer `mcpp run`'s private
// glibc, it reached the child anyway and a payload tool patched against a
// DIFFERENT glibc segfaulted in the dynamic linker. Sanitizing here is what
// makes process.cppm's guarantee actually hold one hop down.
TEST(PlatformEnv, PrependPathListSanitizesTheInheritedLoaderTail) {
    auto inherited = join({"/first",
                           "/home/u/.mcpp/registry/data/xpkgs/xim-x-glibc/2.39/lib64",
                           "/last"});
    ScopedVar ld("LD_LIBRARY_PATH", inherited.c_str());

    std::vector<std::filesystem::path> dirs{"/payload/lib"};
    EXPECT_EQ(mcpp::platform::env::prepend_path_list("LD_LIBRARY_PATH", dirs),
              join({"/payload/lib", "/first", "/last"}));
}

// A payload dir the CALLER passed is the entry the sandbox binary needs — it
// must survive even though it matches the same pattern.
TEST(PlatformEnv, PrependPathListKeepsExplicitPayloadDirs) {
    ScopedVar ld("LD_LIBRARY_PATH", "/home/u/.mcpp/registry/data/xpkgs/xim-x-glibc/2.39/lib64");

    std::vector<std::filesystem::path> dirs{
        "/home/u/.mcpp/registry/data/xpkgs/xim-x-glibc/2.42/lib64"};
    // Explicit dir kept, inherited (older, mismatched) entry dropped.
    EXPECT_EQ(mcpp::platform::env::prepend_path_list("LD_LIBRARY_PATH", dirs),
              "/home/u/.mcpp/registry/data/xpkgs/xim-x-glibc/2.42/lib64");
}

// PATH is not a loader search path: leave its inherited value alone.
TEST(PlatformEnv, PrependPathListLeavesPathUntouched) {
    auto inherited = join({"/bin", "/x/xpkgs/xim-x-glibc/2.39/lib64"});
    ScopedVar path("PATH", inherited.c_str());

    std::vector<std::filesystem::path> dirs{"/tools/bin"};
    EXPECT_EQ(mcpp::platform::env::prepend_path_list("PATH", dirs),
              join({"/tools/bin", "/bin", "/x/xpkgs/xim-x-glibc/2.39/lib64"}));
}

TEST(PlatformEnv, PrependPathListWithNoInheritedValueIsJustTheDirs) {
    ScopedVar ld("LD_LIBRARY_PATH", nullptr);
    std::vector<std::filesystem::path> dirs{"/a", "/b"};
    EXPECT_EQ(mcpp::platform::env::prepend_path_list("LD_LIBRARY_PATH", dirs),
              join({"/a", "/b"}));
}

#include <cstdlib>

import std;
import mcpplibs.mtest;
import mcpp.invocation;
import mcpp.runtime.launch_env;

using namespace mcpplibs::mtest;

namespace {

class ScopedVar {
public:
    ScopedVar(std::string name, const char* value)
        : inner_(std::move(name), value ? std::optional<std::string>(value) : std::nullopt) {}
private:
    mcpp::invocation::ScopedEnv inner_;   // stated for this invocation, as mcpp states its own
};

std::string sep() { return mcpp::runtime::launch_env::path_list_separator(); }

std::string join(std::initializer_list<std::string_view> parts) {
    std::string out;
    for (auto p : parts) {
        if (!out.empty()) out += sep();
        out += p;
    }
    return out;
}

// An entry the composition writes is in the host's spelling (mcpplibs.os
// join_list): `\` between components on Windows. An inherited value it leaves
// alone keeps the spelling it had.
std::string host(std::string_view p) {
    return std::filesystem::path(p).make_preferred().string();
}

}  // namespace

UTest strip_private_glibc_drops_only_payload_entries{"PlatformEnv/StripPrivateGlibcDropsOnlyPayloadEntries", [] {
    auto in = join({"/usr/lib", "/home/u/.mcpp/registry/data/xpkgs/xim-x-glibc/2.39/lib64",
                    "/opt/mine"});
    expect(eq(mcpp::runtime::launch_env::strip_private_glibc(in), join({host("/usr/lib"), host("/opt/mine")})));
}};

UTest strip_private_glibc_can_empty_the_list{"PlatformEnv/StripPrivateGlibcCanEmptyTheList", [] {
    expect(eq(mcpp::runtime::launch_env::strip_private_glibc(
                  "/x/xpkgs/xim-x-glibc/2.42/lib64"), ""));
    expect(eq(mcpp::runtime::launch_env::strip_private_glibc(""), ""));
}};

// The regression this guards (mcpp#311 investigation): the composed value
// becomes an EXPLICIT override, and process.cppm's merged_environ takes explicit
// overrides verbatim — skipping the sanitation it applies to inherited
// variables. So if the inherited tail carried an outer `mcpp run`'s private
// glibc, it reached the child anyway and a payload tool patched against a
// DIFFERENT glibc segfaulted in the dynamic linker. Sanitizing here is what
// makes process.cppm's guarantee actually hold one hop down.
UTest prepend_path_list_sanitizes_the_inherited_loader_tail{"PlatformEnv/PrependPathListSanitizesTheInheritedLoaderTail", [] {
    auto inherited = join({"/first",
                           "/home/u/.mcpp/registry/data/xpkgs/xim-x-glibc/2.39/lib64",
                           "/last"});
    ScopedVar ld("LD_LIBRARY_PATH", inherited.c_str());

    std::vector<std::filesystem::path> dirs{"/payload/lib"};
    expect(eq(mcpp::runtime::launch_env::prepend_path_list("LD_LIBRARY_PATH", dirs), join({host("/payload/lib"), host("/first"), host("/last")})));
}};

// A payload dir the CALLER passed is the entry the sandbox binary needs — it
// must survive even though it matches the same pattern.
UTest prepend_path_list_keeps_explicit_payload_dirs{"PlatformEnv/PrependPathListKeepsExplicitPayloadDirs", [] {
    ScopedVar ld("LD_LIBRARY_PATH", "/home/u/.mcpp/registry/data/xpkgs/xim-x-glibc/2.39/lib64");

    std::vector<std::filesystem::path> dirs{
        "/home/u/.mcpp/registry/data/xpkgs/xim-x-glibc/2.42/lib64"};
    // Explicit dir kept, inherited (older, mismatched) entry dropped.
    expect(eq(mcpp::runtime::launch_env::prepend_path_list("LD_LIBRARY_PATH", dirs), host("/home/u/.mcpp/registry/data/xpkgs/xim-x-glibc/2.42/lib64")));
}};

// PATH is not a loader search path: leave its inherited value alone.
UTest prepend_path_list_leaves_path_untouched{"PlatformEnv/PrependPathListLeavesPathUntouched", [] {
    auto inherited = join({"/bin", "/x/xpkgs/xim-x-glibc/2.39/lib64"});
    ScopedVar path("PATH", inherited.c_str());

    std::vector<std::filesystem::path> dirs{"/tools/bin"};
    expect(eq(mcpp::runtime::launch_env::prepend_path_list("PATH", dirs), join({host("/tools/bin"), "/bin", "/x/xpkgs/xim-x-glibc/2.39/lib64"})));
}};

UTest prepend_path_list_with_no_inherited_value_is_just_the_dirs{"PlatformEnv/PrependPathListWithNoInheritedValueIsJustTheDirs", [] {
    ScopedVar ld("LD_LIBRARY_PATH", nullptr);
    std::vector<std::filesystem::path> dirs{"/a", "/b"};
    expect(eq(mcpp::runtime::launch_env::prepend_path_list("LD_LIBRARY_PATH", dirs), join({host("/a"), host("/b")})));
}};

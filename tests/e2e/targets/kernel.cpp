// The kernel headers are the kernel-abi layer's (design 2026-11-22, kernel interface and derived
// state, §3.1-§3.6): a statement in a project's `[target.<triple>.layers]` takes effect on a row
// without a platform, the headers in effect are measured and reported, a prebuilt form is refused
// where the row's system does not publish its kernel headers apart from its C library, and the
// platform keys of a row without a platform reach the build (§3.2).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;

namespace {

// A header directory standing in for a release of the kernel's UAPI headers: <linux/version.h>
// with the macros headers_install writes, and one header of its own that a program can include.
std::string version_h(int major, int patch, int sub, bool partsMacros) {
    std::string h = std::format("#define LINUX_VERSION_CODE {}\n", (major << 16) + (patch << 8) + (sub > 255 ? 255 : sub));
    if (partsMacros)
        h += std::format("#define LINUX_VERSION_MAJOR {}\n#define LINUX_VERSION_PATCHLEVEL {}\n"
                         "#define LINUX_VERSION_SUBLEVEL {}\n", major, patch, sub);
    return h;
}

const char* kProgram =
    "#include <stdio.h>\n"
    "#include <linux/version.h>\n"
    "#include <linux/mcpp_probe.h>\n"
    "int main(void) { printf(\"code=%d probe=%d\\n\", LINUX_VERSION_CODE, MCPP_PROBE); return 0; }\n";

std::string manifest(std::string_view extra) {
    return std::format("[package]\nname    = \"kernel-probe\"\nversion = \"0.1.0\"\n\n"
                       "[targets.kernel-probe]\nkind = \"bin\"\nmain = \"src/main.c\"\n\n{}", extra);
}

} // namespace

E2E kernel{"kernel", s::home_fixture, [] {
    "1147 a directory states the kernel headers on a row without a platform"_test[s::elf] = [] {
        // The statement used to be accepted and dropped (the merge of the tiers waited for a
        // platform package). It now supplies the layer: the program compiles against it, and the
        // report gives the measured version.
        s::project p({{"mcpp.toml", manifest(
                          "[target.'cfg(os = \"linux\")'.layers]\nkernel-abi = { path = \"uapi\" }\n")},
                      {"src/main.c", kProgram},
                      {"uapi/linux/version.h", version_h(6, 18, 64, true)},
                      {"uapi/linux/mcpp_probe.h", "#define MCPP_PROBE 7\n"}});
        const auto out = s::text(p.mcpp({"run"}));
        expect(fatal(s::has_line(out, R"(code=397888 probe=7)"))) << "the stated headers must be the ones compiled: " << out;
        expect(fatal(s::has_line(out, R"(kernel-abi +linux 6\.18\.64 .*path .*uapi.*prebuilt)")))
            << "the report must give the measured version and the statement: " << out;
    };

    "1148 a packed version is decoded by the row's widths"_test[s::elf] = [] {
        // linux 4.19.256 defines LINUX_VERSION_CODE alone, with its sublevel saturated at 255.
        s::project p({{"mcpp.toml", manifest(
                          "[target.'cfg(os = \"linux\")'.layers]\nkernel-abi = { path = \"uapi\" }\n")},
                      {"src/main.c", kProgram},
                      {"uapi/linux/version.h", version_h(4, 19, 256, false)},
                      {"uapi/linux/mcpp_probe.h", "#define MCPP_PROBE 1\n"}});
        const auto out = s::text(p.mcpp({"build"}));
        expect(fatal(s::has_line(out, R"(kernel-abi +linux 4\.19\.255 )"))) << out;
    };

    "1149 a prebuilt form is refused where the system data takes none"_test[s::elf] = [] {
        // Another layer of a Linux row takes no prebuilt form: the C library is stated with the base.
        s::project p({{"mcpp.toml", manifest(
                          "[target.'cfg(os = \"linux\")'.layers]\nc-abi = \"xim:glibc@2.39\"\n")},
                      {"src/main.c", "int main(void) { return 0; }\n"}});
        const auto r = p.mcpp({"build"});
        expect(fatal(!e2e::succeeded(r)));
        const auto out = s::text(r);
        expect(fatal(e2e::contains(out, "names a prebuilt supplier"))) << out;
        expect(fatal(e2e::contains(out, "sysroot"))) << "the refusal must name the key that states a base: " << out;
        // A payload without its version is a manifest error, with the example.
        p.write("mcpp.toml", manifest("[target.'cfg(os = \"linux\")'.layers]\nkernel-abi = \"xim:linux-headers\"\n"));
        const auto bare = s::text(p.mcpp({"build"}));
        expect(fatal(e2e::contains(bare, "without its version"))) << bare;
    };

    "1150 the platform keys of a row without a platform reach the build"_test[s::elf] = [] {
        // `link.flags` without ld.lld reaches no program, and the build says so; before, the key
        // was dropped without a word. `carrier` describes a platform's image and is a warning.
        s::project p({{"mcpp.toml", manifest(
                          "[target.'cfg(os = \"linux\")'.link]\nflags = [\"--mcpp-e2e-flag\"]\n\n"
                          "[target.'cfg(os = \"linux\")']\ncarrier = { package = \"none-such@1.0.0\", section = \"x\", main = \"src/main.c\" }\n")},
                      {"src/main.c", "int main(void) { return 0; }\n"}});
        const auto out = s::text(p.mcpp({"build"}));
        expect(fatal(e2e::contains(out, "states `[target.<selector>.link] flags`"))) << out;
        expect(fatal(e2e::contains(out, "`carrier`"))) << out;
    };

    "1151 the default kernel headers are measured and reported"_test[s::elf] = [] {
        // Without a statement the headers are the base's own or the toolchain's declaration, never
        // the shared view's links; either way the report names a measured version.
        s::project p({{"mcpp.toml", manifest("")},
                      {"src/main.c", "#include <linux/version.h>\nint main(void) { return LINUX_VERSION_CODE == 0; }\n"}});
        const auto out = s::text(p.mcpp({"build", "-v"}));
        expect(fatal(s::has_line(out, R"(kernel-abi +linux [0-9]+\.[0-9]+\.[0-9]+ )"))) << out;
    };
}};

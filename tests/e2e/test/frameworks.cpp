// A test framework over the package under test, and the C++ library a test links on macOS
// (formerly tests/e2e/1052 and 94).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

E2E frameworks{"frameworks", [] {
    "1052 a framework that uses the package under test is built against it"_test[tag{"requires:gcc"}] = [] {
        // mtest uses mcpplibs.os, which is tested with mtest: the package being built stands for
        // itself in its own graph, whatever version the framework's edge asked for.
        s::needs({"gcc"});
        s::project t(temp_dir() / "t", {{"core/mcpp.toml", R"([package]
namespace = "acme"
name      = "core"
version   = "0.2.0"

[targets.core]
kind = "lib"

[dev-dependencies]
acme.tf = { path = "../tf" }
)"},
                                       {"core/src/core.cppm", "export module acme.core;\nexport int answer() { return 42; }\n"},
                                       {"core/tests/uses_tf.cpp",
                                        "import acme.core;\nimport acme.tf;\nint main() { return tf::check(answer() == 42) ? 0 : 1; }\n"},
                                       {"tf/mcpp.toml", R"([package]
namespace = "acme"
name      = "tf"
version   = "0.1.0"

[targets.tf]
kind = "lib"

[dependencies]
acme.core = { path = "../core", version = "0.1.0" }
)"},
                                       {"tf/src/tf.cppm", R"(export module acme.tf;
import acme.core;
export namespace tf { bool check(bool held) { return held && answer() == 42; } }
)"}});
        auto r = t.mcpp_in("core", {"test"});
        expect(fatal(e2e::succeeded(r))) << "the test of the package under its own framework failed";
        expect(e2e::contains(s::text(r), "uses_tf ... ok"));
        expect(e2e::contains(s::text(r), "it is the package being built, 0.2.0, which stands for itself"))
            << "the version the edge did not ask for is not said";
        std::size_t n = 0;
        for (const auto& f : s::find_files(t.at("core/target"), R"(core\.m\.o|core\.o)"))
            if (f.generic_string().contains("acme_core") || f.generic_string().contains("/core")) ++n;
        expect(n <= 1u) << "the root's module was compiled more than once (" << n << ")";
    };

    "94 macos test stdlib"_test[tag{"requires:macos"}] = [] {
        // A test program links the toolchain's libc++, not the system's (the header/dylib split of
        // the old -lc++ broke on libc++ 22's out-of-line __hash_memory).
        s::needs({"macos"});
        auto p = s::project::scaffold("hashapp");
        p.write("tests/hash_test.cpp", R"(// Forces the string-hash path (__hash_memory-class symbols on libc++ >= 22).
import std;
int main() {
    std::unordered_map<std::string, int> m;
    for (int i = 0; i < 100; ++i) m["key" + std::to_string(i)] = i;
    return (m.size() == 100 && m.at("key42") == 42) ? 0 : 1;
}
)");
        auto r = p.mcpp({"test"});
        expect(fatal(e2e::succeeded(r)));
        const auto log = s::text(r);
        expect(s::has_line(log, "[0-9]+ passed; 0 failed"));  // the template ships its own test_smoke too
        std::optional<fs::path> bin;
        for (const auto& f : s::find_files(p.at("target"), "hash_test"))
            if (f.parent_path().filename() == "bin") { bin = f; break; }
        expect(fatal(bin.has_value())) << "no bin/hash_test";
        auto otool = e2e::run({.argv = {"otool", "-L", bin->string()}});
        // The link assertion applies to an LLVM toolchain; another resolution was covered above.
        if (e2e::contains(log, "Resolved llvm@") || e2e::contains(otool.out, "libc++"))
            expect(s::lacks(otool.out, "/usr/lib/libc++")) << "the test program links the system libc++";
    };
}};

}  // namespace

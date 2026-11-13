// The case of e2e/targets that holds the machine (nothing else of this program runs beside it):
// `--target x86_64-linux-musl` yields a fully static ELF of that row, and the default GNU build
// still works after it (formerly tests/e2e/28).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

E2E machine{"machine", [] {
    "28 target static"_test[s::musl][s::elf][resource{"machine"}] = [] {
        // The musl toolchain is the one this home has, whichever release that is.
        const auto musl_version = s::musl_gcc_version();
        if (!fs::exists(s::payload_root("xim-x-musl-gcc", musl_version) / "bin/x86_64-linux-musl-g++"))
            skip("no musl-gcc payload in this home: not testing --target=*-linux-musl");
        // Without a GNU gcc the default build would also use musl-gcc, and the regression check
        // at the end would mean nothing.
        std::string gnu_gcc;
        if (fs::is_directory(s::shared_home() / "registry/data/xpkgs/xim-x-gcc"))
            gnu_gcc = s::newest_payload("xim-x-gcc").value_or("");
        auto p = s::project::scaffold("staticapp");
        std::string extra = "\n";
        if (!gnu_gcc.empty()) extra += std::format("[toolchain]\nlinux = \"gcc@{}\"\n\n", gnu_gcc);
        extra += std::format("[target.x86_64-linux-musl]\ntoolchain = \"gcc@{}-musl\"\nlinkage   = \"static\"\n", musl_version);
        p.write("mcpp.toml", p.read("mcpp.toml") + extra);
        auto r = p.mcpp({"build", "--target", "x86_64-linux-musl"});
        expect(fatal(e2e::succeeded(r))) << "build failed: " << s::text(r);
        // The output path includes the triple: target/x86_64-linux-musl/<fp>/bin/staticapp.
        const auto binary = s::find_file(p.at("target/x86_64-linux-musl"), "staticapp");
        expect(fatal(binary.has_value())) << "binary not produced under target/x86_64-linux-musl: " << s::listing(p.at("target")).size() << " entries";
        // `file` reports a statically linked ELF: no PT_INTERP and nothing it needs.
        const auto h = s::read_header(*binary);
        expect(fatal(h.kind == s::format::elf && !h.elf_interpreter && h.needed.empty())) << "binary is not statically linked: interpreter '" << h.interpreter << "', " << h.needed.size() << " DT_NEEDED";
        expect(fatal(e2e::succeeded(s::run(*binary)))) << "static binary failed to run";
        // Defensive: no PT_INTERP and no RUNPATH or RPATH.
        expect(fatal(!h.elf_interpreter)) << "static binary unexpectedly has PT_INTERP: " << h.interpreter;
        expect(fatal(!h.elf_runpath && !h.elf_rpath)) << "static binary unexpectedly has RUNPATH/RPATH: " << h.runpath;
        // The default GNU build still works: --target did not clobber the default path for
        // the commands that follow.
        if (gnu_gcc.empty()) {
            std::println("SKIP: no GNU gcc installed — skipping default-build regression check");
            return;
        }
        r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "default GNU build broke after musl build: " << s::text(r);
        bool gnu_binary = false;
        for (const auto& f : s::find_files(p.at("target"), "staticapp"))
            if (!f.generic_string().contains("/x86_64-linux-musl/")) gnu_binary = true;
        expect(fatal(gnu_binary)) << "default GNU binary missing";
    };
}};

}  // namespace

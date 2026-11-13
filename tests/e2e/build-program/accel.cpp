// The accelerator dimension and the probe channel: which prebuilt artifact a build may take, what
// a build program is told of the accelerator and how its stated facts and floors are compared
// (formerly tests/e2e/600, 605).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

// `mcpp new <name>` with only src/main.cpp (`int main`) and `manifest`.
s::project scaffold_plain(std::string_view name, const std::string& manifest) {
    fs::remove_all(temp_dir() / "new" / name);
    auto p = s::project::scaffold(name);
    p.write("mcpp.toml", manifest);
    p.write("src/main.cpp", "int main() { return 0; }\n");
    for (const auto& m : s::glob(p.at("src"), "*.cppm")) fs::remove(m);
    return p;
}

E2E accel{"accel", [] {
    "600 accel variant selection"_test[tag{"requires:elf"}] = [] {
        // gpuonly publishes one artifact, for sm_90f: an sm_86 build is refused naming the
        // dimension. gpukit publishes a CPU-only artifact first: the same build takes it (the
        // order that keeps an mcpp predating the field working).
        s::needs({"elf"});
        const auto root = temp_dir() / "new";
        constexpr std::string_view triple = "x86_64-linux-gnu";
        auto artifact = [&](std::string_view name, bool device) {
            return std::format("[[runtime.artifacts]]\nrole       = \"static-library\"\npath       = \"lib/lib{}.a\"\nprovenance = \"mcpp-pack/1\"\nabi        = \"{}\"\n{}",
                               name, triple, device ? "accel      = \"cuda12.8+{sm_90f}\"\n" : "");
        };
        auto make_pkg = [&](std::string_view name, const std::string& tables) {
            s::project pkg{root / name, {}};
            pkg.write(std::format("lib/lib{}.a", name), "");
            fs::create_directories(pkg.at("include"));
            pkg.write("mcpp.toml", std::format("[package]\nname    = \"{}\"\nversion = \"0.1.0\"\n[language]\nstandard = \"c++23\"\n", name) + tables);
        };
        make_pkg("gpuonly", artifact("gpuonly", true));
        make_pkg("gpukit", artifact("gpukit", false) + "\n" + artifact("gpukit", true));
        auto consumer = [&](std::string_view dep) {
            return scaffold_plain("app", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[language]\nstandard = \"c++23\"\n[dependencies]\n{0} = {{ path = \"../{0}\" }}\n", dep));
        };

        // 1. A device-only package refuses another architecture, naming both and the remedy that
        // can work (pinning [toolchain] cannot change the GPU architecture).
        auto app = consumer("gpuonly");
        auto r1 = app.mcpp({"build", "--accel", "cuda12.8+{sm_86}"});
        const auto out = s::text(r1);
        expect(r1.started && r1.status != 0) << "sm_86 accepted an sm_90f-only artifact";
        expect(e2e::contains(out, "accel")) << "refusal does not name the accel dimension";
        expect(e2e::contains(out, "sm_90f")) << "refusal does not say what the artifact has";
        expect(e2e::contains(out, "sm_86")) << "refusal does not say what this build asked for";
        expect(e2e::contains(out, "--no-accel")) << "refusal offers no remedy for the device axis";
        expect(e2e::contains(out, "accel=cuda12.8+{sm_90f}")) << "the published listing hides the device dimension";
        // 2. The architecture it was built for, and 3. no accelerator at all.
        expect(e2e::succeeded(app.mcpp({"build", "--accel", "cuda12.8+{sm_90}"}))) << "sm_90 refused by an sm_90f artifact";
        expect(e2e::succeeded(app.mcpp({"build", "--no-accel"}))) << "--no-accel refused";
        // 4. CPU first: an unmatched device build falls back.
        app = consumer("gpukit");
        expect(e2e::succeeded(app.mcpp({"build", "--accel", "cuda12.8+{sm_86}"}))) << "CPU-only artifact listed first did not accept sm_86";
    };

    "605 probe channel and accel reach the build program"_test[tag{"requires:gcc"}] = [] {
        // The package measures (mcpp::fact), states what it needs (mcpp::floor), and the engine
        // compares after the root's build program has run: an unmet floor is refused naming both
        // values, under the reason `version-floor-unmet`. The same program reads MCPP_ACCEL, and
        // `cfg(accelerator = ...)` is fed from the same value. No vendor is named: `widget`.
        s::needs({"gcc"});
        auto p = scaffold_plain("probe", "");
        p.write("src/main.cpp", "#ifndef WIDGET_ON\n#error \"cfg(accelerator = \\\"widget\\\") did not match, so WIDGET_ON is missing\"\n#endif\nint main() { return 0; }\n");
        auto write_manifest = [&](std::string_view floor) {
            p.write("mcpp.toml", R"cpp([package]
name = "probe"
version = "0.1.0"

[language]
standard = "c++23"

[build]
accel = "widget9+{w1,w2}"

[target.'cfg(accelerator = "widget")'.build]
defines = ["WIDGET_ON"]

[targets.probe]
kind = "bin"
main = "src/main.cpp"
)cpp");
            p.write("build.mcpp", std::format(R"cpp(import std;
import mcpp;
int main() {{
    mcpp::warning((std::string("accel=[") + mcpp::accel() + "]").c_str());
    mcpp::fact("widget.driver", "1.2");
    mcpp::floor("{}");
    return 0;
}}
)cpp", floor));
        };
        // One: the floor is above the fact.
        write_manifest("widget.driver >= 2.0");
        auto refused = p.mcpp({"build"});
        expect(refused.started && refused.status != 0) << "a floor above the stated fact was accepted";
        expect(e2e::contains(s::text(refused), "widget.driver")) << "refusal does not name the quantity";
        expect(e2e::contains(s::text(refused), "2.0")) << "refusal does not say what was needed";
        expect(e2e::contains(s::text(refused), "1.2")) << "refusal does not say what is there";
        // `why toolchain` runs the same prepare, build program included.
        auto why = s::json(p.mcpp({"why", "toolchain", "--format", "json"}).out);
        const auto reason = why.contains("data") && why["data"].is_object() ? why["data"].value("reason", std::string("-")) : std::string("-");
        expect(eq(reason, std::string("version-floor-unmet")));
        // Two: a met floor builds; three: the program saw the resolved accel, canonically spelt.
        write_manifest("widget.driver >= 1.0");
        fs::last_write_time(p.at("src/main.cpp"), fs::file_time_type::clock::now());
        auto ok = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(ok))) << "a met floor was refused";
        expect(e2e::contains(s::text(ok), "accel=[widget9+{w1,w2}]")) << "MCPP_ACCEL did not carry the resolved accel";
        // Four: --no-accel empties the layer (main.cpp's #error fires) and the variable (not the
        // display spelling `(none)`).
        auto none = p.mcpp({"build", "--no-accel"});
        expect(none.started && none.status != 0) << "cfg(accelerator) matched under --no-accel";
        expect(e2e::contains(s::text(none), "WIDGET_ON is missing")) << "the build failed for another reason than the layer";
        expect(e2e::contains(s::text(none), "accel=[]")) << "MCPP_ACCEL is not empty under --no-accel";
    };

    "625 a payload can be gated on the accelerator"_test[tag{"requires:elf"}][tag{"requires:gcc"}] = [] {
        // `[target.'cfg(accelerator = ...)'.xlings]`: the accel is an input read before
        // provisioning, so a payload gated on it is installed for a device build and not for a
        // CPU-only one. Both legs; the second is the point. A fresh home and shaderc, as in 618.
        s::needs({"elf", "gcc"});
        s::project p{{"src/main.cpp", "int main() { return 0; }\n"},
                     {"mcpp.toml", "[package]\nname         = \"gated\"\nversion      = \"0.1.0\"\naccelerators = [\"vulkan\"]\n\n[targets.gated]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n\n"
                                   "[target.'cfg(accelerator = \"vulkan\")'.xlings.workspace]\n\"xim:shaderc\" = \"2026.3\"\n"}};
        const s::options fresh{.where = s::home::fresh};
        const auto store = s::case_home() / "registry/data/xpkgs/xim-x-shaderc";
        expect(fatal(e2e::succeeded(p.mcpp({"build"}, fresh)))) << "the CPU-only build was refused";
        expect(!fs::is_directory(store)) << "a CPU-only build installed the device payload";
        expect(fatal(e2e::succeeded(p.mcpp({"build", "--accel", "vulkan1.2"}, fresh)))) << "the device build was refused";
        expect(fs::is_directory(store)) << "the device build did not install the payload its predicate names";
    };

    "626 naming a subset of backends is not a mismatch"_test[tag{"requires:elf"}][tag{"requires:gcc"}] = [] {
        // A constrained glob whose backend the build never named is left out, not refused; one
        // whose backend is named and architecture uncovered still is; a backend the package never
        // declared is refused, naming the misspelling.
        s::needs({"elf", "gcc"});
        auto mk = [](std::string_view accel) {
            fs::remove_all(temp_dir() / "p");
            s::project p{temp_dir() / "p", {{"src/main.cpp", "int main() { return 0; }\n"},
                                            {"src/kernels/k.cu", "extern \"C\" __global__ void k() {}\n"},
                                            {"build.mcpp", "import std;\nimport mcpp;\nint main() { return 0; }\n"}}};
            p.write("mcpp.toml", std::format("[package]\nname         = \"subset\"\nversion      = \"0.1.0\"\naccelerators = [\"cuda\"]\n\n[build]\nsources = [\n  \"src/*.cpp\",\n  {{ glob = \"src/kernels/*.cu\", accel = \"{}\" }},\n]\n\n[targets.subset]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", accel));
            return p;
        };
        // 1. The backend is named: the glob comes through (the device-source audit sees it).
        auto p = mk("cuda12.9+{sm_89}");
        auto r1 = p.mcpp({"build", "--accel", "cuda12.9+{sm_89}"});
        expect(r1.started && r1.status != 0) << "the glob did not reach the build";
        expect(e2e::contains(s::text(r1), "no action compiles")) << "expected the device-source audit";
        // 2. A different backend: left out, as --no-accel leaves it.
        p = mk("cuda12.9+{sm_89}");
        expect(e2e::succeeded(p.mcpp({"build", "--accel", "vulkan1.2"}))) << "naming a different backend was refused";
        // 3. The same backend, an architecture it does not cover: refused.
        p = mk("cuda12.9+{sm_89}");
        auto r3 = p.mcpp({"build", "--accel", "cuda12.9+{sm_80}"});
        expect(r3.started && r3.status != 0) << "an uncovered architecture built";
        expect(e2e::contains(s::text(r3), "does not cover")) << "expected the accel mismatch refusal";
        // 4. A backend [package] accelerators does not list: refused, naming it.
        p = mk("cude12.9+{sm_89}");
        auto r4 = p.mcpp({"build", "--accel", "cuda12.9+{sm_89}"});
        expect(r4.started && r4.status != 0) << "an undeclared backend built";
        expect(e2e::contains(s::text(r4), "does not declare")) << "expected the undeclared-backend refusal";
        expect(e2e::contains(s::text(r4), "cude")) << "the refusal does not name the misspelling";
    };

    "1011 accelerator requests are complete and canonical"_test[tag{"requires:gcc"}][tag{"requires:unix-shell"}] = [] {
        // Invalid accelerator syntax does not become a successful CPU build, an implementation
        // cannot occur twice with order-dependent coverage, and equivalent requests are one
        // variant. (The script required python3 and did not use it.)
        s::needs({"gcc", "unix-shell"});
        const s::options home{.where = s::home::inherited};
        const std::string manifest = "[package]\nname = \"accel-request\"\nversion = \"0.1.0\"\n\n[targets.accel-request]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n";
        s::project p{{"mcpp.toml", manifest}, {"src/main.cpp", "int main(){return 0;}\n"}};
        for (auto request : {"sycl+{sm_89", "sycl+{sm_89}garbage", "sycl+{sm_89,,sm_90}", "sycl+{sm_89},sycl+{sm_90}", "sycl+{sm_90},sycl+{sm_89}"}) {
            auto r = p.mcpp({"build", "--accel", request}, home);
            expect(r.started && r.status != 0) << "accepted malformed or repeated request: " << request;
            expect(e2e::contains(s::text(r), "invalid accelerator request at byte")) << "missing syntax location";
            expect(e2e::contains(s::text(r), "--accel")) << "missing request source";
        }
        p.write("mcpp.toml", manifest + "\n[build]\naccel = \"sycl+{sm_89\"\n");
        auto m = p.mcpp({"build"}, home);
        expect(m.started && m.status != 0) << "accepted malformed manifest request";
        expect(e2e::contains(s::text(m), "[build] accel")) << "missing manifest field";
        p.write("mcpp.toml", manifest + "\n[build]\nsources = [{ glob = \"src/main.cpp\", accel = \"sycl+{sm_90\" }]\n");
        auto c = p.mcpp({"build", "--no-accel"}, home);
        expect(c.started && c.status != 0) << "ignored malformed source constraint";
        expect(e2e::contains(s::text(c), "sources entry 'src/main.cpp'")) << "missing source constraint";
        expect(e2e::contains(s::text(c), "invalid accelerator request")) << "missing constraint syntax error";
        p.write("mcpp.toml", manifest + "\n[build]\nsources = [{ glob = \"src/main.cpp\", accel = \"sycl+{sm_90}\" }]\n");
        expect(fatal(e2e::succeeded(p.mcpp({"build", "--accel", "sycl+{sm_90,sm_89},newmodel2+{opaque}"}, home)))) << "valid open-vocabulary request did not build";
        const auto before = s::glob(p.at("target"), "*/*/build.ninja");
        expect(fatal(!before.empty())) << "no build graph was generated";
        auto again = p.mcpp({"build", "--accel", "newmodel2+{opaque},sycl+{sm_89,sm_90,sm_89}"}, home);
        expect(fatal(e2e::succeeded(again))) << "equivalent request did not build";
        expect(eq(s::glob(p.at("target"), "*/*/build.ninja"), before)) << "equivalent sets allocated different variants";
        expect(s::lacks(s::text(again), "fingerprint changed")) << "equivalent sets changed the fingerprint";
        expect(e2e::succeeded(p.mcpp({"run", "--accel", "sycl+{sm_89,sm_90},newmodel2+{opaque}"}, home))) << "the canonical variant did not run";
    };
}};

}  // namespace

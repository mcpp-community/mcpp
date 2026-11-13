// What a dependency states it needs of the graph, the machine and mcpp: a standard, a version
// floor, the oldest mcpp release (formerly tests/e2e/326, 603, 822).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

E2E floors{"floors", [] {
    "326 dependency standard floor"_test[s::gcc] = [] {
        // A module graph has one standard, the root's; a dependency that declared a higher one is
        // built at the root's, and that is reported (degraded, an error under --strict), naming
        // the package, the level and the one-place fix. Silent when it declared nothing, the
        // graph's own level, or once the root is raised.
        const auto tmp = s::case_dir();
        s::project lib{tmp / "lib", {{"mcpp.toml", "[package]\nname = \"flib\"\nversion = \"0.1.0\"\nstandard = \"c++26\"\n[targets.flib]\nkind = \"lib\"\n"},
                                     {"src/flib.cppm", "export module flib;\nexport int f(){ return 1; }\n"}}};
        s::project app{tmp / "app", {{"mcpp.toml", "[package]\nname = \"fapp\"\nversion = \"0.1.0\"\nstandard = \"c++23\"\n[dependencies]\nflib = { path = \"../lib\" }\n"},
                                     {"src/main.cpp", "import flib;\nint main(){ return f()==1 ? 0 : 1; }\n"}}};
        auto warn = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(warn))) << "the floor check must be degraded, not an error";
        expect(e2e::contains(s::text(warn), "flib") && e2e::contains(s::text(warn), "c++26")) << "the diagnostic does not name the package and the level";
        expect(e2e::contains(s::text(warn), "workspace.package")) << "the hint does not name the one-place fix";
        app.remove("target");
        expect(failed(app.mcpp({"build", "--strict"}))) << "--strict did not promote the degradation";

        auto rebuild_silent = [&](std::string_view lib_manifest, std::string_view why) {
            lib.write("mcpp.toml", lib_manifest);
            app.remove("target");
            auto r = app.mcpp({"build"});
            expect(fatal(e2e::succeeded(r))) << "build failed";
            expect(s::lacks(s::text(r), "declares standard")) << why;
        };
        rebuild_silent("[package]\nname = \"flib\"\nversion = \"0.1.0\"\n[targets.flib]\nkind = \"lib\"\n", "fired for a dependency that declared nothing");
        rebuild_silent("[package]\nname = \"flib\"\nversion = \"0.1.0\"\nstandard = \"c++23\"\n[targets.flib]\nkind = \"lib\"\n", "fired for a dependency at the graph's own level");
        app.write("mcpp.toml", "[package]\nname = \"fapp\"\nversion = \"0.1.0\"\nstandard = \"c++26\"\n[dependencies]\nflib = { path = \"../lib\" }\n");
        rebuild_silent("[package]\nname = \"flib\"\nversion = \"0.1.0\"\nstandard = \"c++26\"\n[targets.flib]\nkind = \"lib\"\n", "the documented fix does not silence the diagnostic");
    };

    "603 version floor"_test = [] {
        // A package needing more of the machine than it has is refused before compiling, naming
        // the requirement, both versions and who stated the fact; a met floor builds, and a
        // floor nobody answered is silent.
        const auto parent = s::case_dir() / "new";
        fs::create_directories(parent);
        auto mkfact = [&](std::string_view name, std::string_view fact) {
            s::project{parent / name, {}}.write("mcpp.toml", std::format("[package]\nname = \"{0}\"\nversion = \"0.1.0\"\n[language]\nstandard = \"c++23\"\n[runtime]\nprovides = [\"{1}\"]\n[targets.{0}]\nkind = \"lib\"\n", name, fact));
            s::project{parent / name, {}}.write(std::format("src/{}.c", name), std::format("int {}_anchor(void){{return 0;}}\n", name));
        };
        auto mkneed = [&](std::string_view name, std::string_view need) {
            s::project{parent / name, {}}.write("mcpp.toml", std::format("[package]\nname = \"{0}\"\nversion = \"0.1.0\"\n[language]\nstandard = \"c++23\"\n[[runtime.requirements]]\nkind = \"version-floor\"\nvalue = \"{1}\"\n[targets.{0}]\nkind = \"lib\"\n", name, need));
            s::project{parent / name, {}}.write(std::format("src/{}.c", name), std::format("int {}_anchor(void){{return 0;}}\n", name));
        };
        auto consumer = [&](std::vector<std::string_view> deps) {
            fs::remove_all(parent / "app");
            auto app = s::project::scaffold("app");
            std::string manifest = "[package]\nname=\"app\"\nversion=\"0.1.0\"\n[language]\nstandard=\"c++23\"\n[dependencies]\n";
            for (auto d : deps) manifest += std::format("{0} = {{ path = \"../{0}\" }}\n", d);
            app.write("mcpp.toml", manifest);
            for (const auto& f : s::glob(app.at("src"), "*.cppm")) fs::remove(f);
            app.write("src/main.cpp", "int main(){return 0;}\n");
            return app;
        };
        mkfact("driverfact", "cuda.driver=12.4");
        mkneed("toolkitnew", "cuda.driver >= 13.0");
        auto out = consumer({"driverfact", "toolkitnew"}).mcpp({"build"});
        expect(fatal(failed(out))) << "a floor above the stated fact was accepted";
        expect(e2e::contains(s::text(out), "cuda.driver")) << "refusal does not name what is short";
        expect(e2e::contains(s::text(out), "13.0")) << "refusal does not say what was needed";
        expect(e2e::contains(s::text(out), "12.4")) << "refusal does not say what is there";
        expect(e2e::contains(s::text(out), "driverfact")) << "refusal does not say who stated the fact";
        mkneed("toolkitok", "cuda.driver >= 12.0");
        expect(e2e::succeeded(consumer({"driverfact", "toolkitok"}).mcpp({"build"}))) << "a met floor was refused";
        mkneed("futureneed", "some.future.thing >= 4.2.1");
        expect(e2e::succeeded(consumer({"futureneed"}).mcpp({"build"}))) << "a floor nobody answered was refused";
    };

    "822 a package states the oldest mcpp it supports"_test[s::unix_shell][resource{"machine"}] = [] {
        // `[package] mcpp = ">=V"` states the oldest mcpp release a package supports: an engine
        // below it stops first, naming the package, the floor, its own release and the way to
        // upgrade; a bare release or a non-release is explained; a workspace floor reaches its
        // members; an unknown [lib] key is named.
        const auto tmp = s::case_dir();
        std::string have;
        {
            auto v = s::mcpp({"--version"}, tmp);
            std::istringstream words{s::last_line(v.out)};
            words >> have >> have;
        }
        s::project p{tmp / "p", {{"src/main.cpp", "int main() { return 0; }\n"}}};
        auto manifest = [&](std::string_view floor) {
            p.write("mcpp.toml", std::format("[package]\nname = \"floor822\"\nversion = \"0.1.0\"\nmcpp = {}\n", floor));
        };
        manifest("\">=2999.1.1\"");
        auto f1 = p.mcpp({"build"});
        expect(fatal(failed(f1))) << "F1: a floor above " << have << " built";
        expect(e2e::contains(s::text(f1), std::format("requires mcpp >= 2999.1.1; this is mcpp {}", have))) << "F1: the message does not name the floor and this release";
        expect(e2e::contains(s::text(f1), "xlings install mcpp@2999.1.1")) << "F1: the message does not say how to upgrade";
        manifest(std::format("\">={}\"", have));
        expect(e2e::succeeded(p.mcpp({"build"}))) << "F2: a floor equal to this release did not build";
        manifest("\">=2026.1.1\"");
        expect(e2e::succeeded(p.mcpp({"build"}))) << "F2: a floor below this release did not build";
        manifest(std::format("\"{}\"", have));
        auto f3 = p.mcpp({"build"});
        expect(failed(f3)) << "F3: a bare release was accepted";
        expect(e2e::contains(s::text(f3), std::format("names one release exactly; a floor is written \">={}\"", have))) << "F3: the bare release is not explained";
        manifest("\">=banana\"");
        auto f3b = p.mcpp({"build"});
        expect(failed(f3b)) << "F3: a non-release was accepted";
        expect(e2e::contains(s::text(f3b), "'banana' is not an mcpp release")) << "F3: the non-release is not explained";

        s::project ws{tmp / "ws", {{"mcpp.toml", "[workspace]\nmembers = [\"app\"]\n[workspace.package]\nversion = \"0.1.0\"\nmcpp    = \">=2999.1.1\"\n"},
                                   {"app/mcpp.toml", "[package]\nname = \"app822\"\n"},
                                   {"app/src/main.cpp", "int main() { return 0; }\n"}}};
        auto f4 = ws.mcpp({"build", "-p", "app"});
        expect(failed(f4)) << "F4: the workspace floor did not reach the member";
        expect(s::has_line(s::text(f4), "package 'app822'.*requires mcpp >= 2999\\.1\\.1")) << "F4: the member is not named with the inherited floor";

        p.write("mcpp.toml", "[package]\nname = \"floor822\"\nversion = \"0.1.0\"\n\n[lib]\npth = \"src/x.cppm\"\n");
        auto f5 = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(f5))) << "F5: the build failed";
        expect(e2e::contains(s::text(f5), "[lib] has unsupported key 'pth' (ignored). Supported keys: path.")) << "F5: [lib] did not name the unknown key";
    };
}};

}  // namespace

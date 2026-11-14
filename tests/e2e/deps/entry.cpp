// A program that begins in a dependency's `kind = "entry"` target (formerly tests/e2e/946).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

constexpr std::string_view fw_manifest = R"cpp([package]
name    = "fw"
version = "0.1.0"
[features]
app = {}
[targets.fw]
kind = "lib"
[targets.player]
kind    = "entry"
main    = "src/entry/player.cpp"
defines = ["FW_PLAYER"]
[targets.app]
kind              = "entry"
main              = "src/entry/app.cpp"
required_features = ["app"]
[target.'cfg(os = "linux")'.targets.app]
main = "src/entry/app_linux.cpp"
[test]
main = { from = "fw/player" }
)cpp";

constexpr std::string_view fw_module = R"cpp(export module fw;
import std;
// The callback an application defines. Declared with C++ language linkage at
// global scope: a declaration attached to this module could not be defined
// outside it.
export extern "C++" int fw_app();
export namespace fw {
int twice(int v) { return 2 * v; }
}
)cpp";

constexpr std::string_view player = R"cpp(import std;
import fw;
#ifndef FW_PLAYER
#error the entry is compiled with its own target's defines
#endif
int main() {
    std::println("player {}", fw::twice(fw_app()));
    return 0;
}
)cpp";

E2E entry{"entry", [] {
    "946 a program begins in a dependency entry"_test[s::gcc][s::elf] = [] {
        // A framework supplies `main` as an entry target; the application writes only the
        // callback. The entry is in no image that does not select it; required features gate
        // it; a selection naming nothing is refused with the candidates; the entry function
        // follows the entry; a platform row replaces its file; test programs begin in an entry;
        // `start` on a hosted target is refused; the result record names the beginning.
        const auto tmp = s::case_dir();
        s::project fw{tmp / "fw", {{"mcpp.toml", fw_manifest},
                                   {"src/fw.cppm", fw_module},
                                   {"src/entry/player.cpp", player},
                                   {"src/entry/app.cpp", "import std;\nimport fw;\nint main() { std::println(\"app {}\", fw_app()); return 0; }\n"},
                                   {"src/entry/app_linux.cpp", "import std;\nimport fw;\nint main() { std::println(\"app-linux {}\", fw_app()); return 0; }\n"},
                                   {"tests/callback.cpp", "int fw_app() { return 21; }\n"}}};
        s::project notes{tmp / "notes", {{"src/app.cpp", "int fw_app() { return 21; }\n"}}};
        auto manifest = [&](std::string_view keys, std::string_view dep = "fw = { path = \"../fw\" }") {
            notes.write("mcpp.toml", std::format("[package]\nname    = \"notes\"\nversion = \"0.1.0\"\n[dependencies]\n{}\n[targets.notes]\nkind = \"bin\"\n{}\n", dep, keys));
        };

        manifest("main = { from = \"fw/player\" }");
        expect(fatal(e2e::succeeded(notes.mcpp({"build"})))) << "A: the program over fw's entry did not build";
        auto a = notes.mcpp({"run"});
        expect(fatal(e2e::succeeded(a))) << "A: the program did not run";
        expect(e2e::contains(s::text(a), "player 42")) << "A: the program did not begin in fw's entry with its flags";

        expect(fatal(e2e::succeeded(fw.mcpp({"build"})))) << "B: fw as the root did not build";
        s::project other{tmp / "other", {{"mcpp.toml", "[package]\nname    = \"other\"\nversion = \"0.1.0\"\n[dependencies]\nfw = { path = \"../fw\" }\n"},
                                         {"src/main.cpp", "import std;\nimport fw;\nint fw_app() { return 1; }\nint main() { std::println(\"own {}\", fw::twice(3)); }\n"}}};
        auto b = other.mcpp({"run"});
        expect(fatal(e2e::succeeded(b))) << "B: a program with its own main over fw did not link";
        expect(e2e::contains(s::text(b), "own 6")) << "B: the program with its own main did not run";

        manifest("main = { from = \"fw/app\" }");
        auto c = notes.mcpp({"build"});
        expect(fatal(failed(c))) << "C: an entry with an inactive required feature was linked";
        expect(e2e::contains(s::text(c), "requires the feature \"app\"")) << "C: the refusal does not name the feature";
        expect(e2e::contains(s::text(c), "fw = { ..., features = [\"app\"] }")) << "C: the refusal does not give the dependency line";
        manifest("main = { from = \"fw/app\" }", "fw = { path = \"../fw\", features = [\"app\"] }");
        auto c_run = notes.mcpp({"run"});
        expect(fatal(e2e::succeeded(c_run))) << "C: the entry with its feature did not build";

        manifest("main = { from = \"nope/player\" }");
        auto d1 = notes.mcpp({"build"});
        expect(failed(d1)) << "D: an unknown dependency was accepted";
        expect(e2e::contains(s::text(d1), "'nope' is neither a dependency of 'notes' nor its own name (its dependencies: fw)")) << "D: the unknown dependency is not named with the candidates";
        manifest("main = { from = \"fw/fw\" }");
        auto d2 = notes.mcpp({"build"});
        expect(failed(d2)) << "D: a library target was accepted as an entry";
        expect(e2e::contains(s::text(d2), "declares no entry target 'fw' (its `kind = \"entry\"` targets: app, player)")) << "D: the entry targets are not listed";
        manifest("main = { from = \"fw/player\" }\ndefines = [\"X=1\"]");
        auto d3 = notes.mcpp({"build"});
        expect(failed(d3)) << "D: flags for the program's own entry were accepted beside from";
        expect(e2e::contains(s::text(d3), "targets.notes.defines applies to the target's own entry translation unit")) << "D: the flags refusal does not say why";

        std::string with_entry{fw_manifest};
        const std::string defines = "defines = [\"FW_PLAYER\"]\n";
        with_entry.insert(with_entry.find(defines) + defines.size(), "windows_entry = \"main\"\n");
        fw.write("mcpp.toml", with_entry);
        manifest("main = { from = \"fw/player\" }\nwindows_entry = \"wmain\"");
        auto e = notes.mcpp({"build"});
        expect(failed(e)) << "E: two entry functions were accepted";
        expect(s::has_line(s::text(e), "windows_entry = \"wmain\", and its beginning .fw/player. defines `main`")) << "E: the conflict does not name both statements";

        expect(e2e::contains(s::text(c_run), "app-linux 21")) << "F: the row did not replace the entry's file";

        auto g = fw.mcpp({"test"});
        expect(fatal(e2e::succeeded(g))) << "G: the test over the package's own entry failed";
        expect(e2e::contains(s::text(g), "callback")) << "G: the test did not run";

        manifest("main  = { from = \"fw/player\" }\nstart = \"_start\"");
        auto h = notes.mcpp({"build"});
        expect(failed(h)) << "H: start on a hosted target was accepted";
        expect(e2e::contains(s::text(h), "the image is entered by the C runtime")) << "H: the refusal does not name the C runtime";

        manifest("main = { from = \"fw/player\" }");
        auto i = notes.mcpp({"build", "--message-format", "json"});
        expect(fatal(e2e::succeeded(i))) << "I: the json build failed";
        expect(e2e::contains(i.out, "\"entry\":{\"from\":\"fw/player\"}")) << "I: the result record does not name where the program begins";
    };
}};

}  // namespace

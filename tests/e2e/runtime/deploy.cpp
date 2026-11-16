// `runtime.deploy` and a build program's deploy: a file placed in a directory relative to the
// executable (the root's and a dependency's), seen by a test program too; two sources for one
// destination placed once when their bytes agree and refused at build time when they do not; a
// destination outside the executable's directory refused (formerly tests/e2e/646, 810).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

std::optional<fs::path> graph_of(const fs::path& root) {
    for (const auto& f : s::find_files(root / "target", R"(build\.ninja)"))
        if (!f.generic_string().contains("/.build-mcpp/")) return f;
    return std::nullopt;
}

std::vector<std::string> split(std::string_view text, char sep) {
    std::vector<std::string> out;
    for (auto part : std::views::split(text, sep)) out.emplace_back(part.begin(), part.end());
    return out;
}

// The lines of `text` that match `pattern` (`grep -E`).
std::vector<std::string> matching(std::string_view text, std::string_view pattern) {
    const std::regex re{std::string(pattern)};
    std::vector<std::string> out;
    for (auto& l : split(text, '\n'))
        if (std::regex_search(l, re)) out.push_back(std::move(l));
    return out;
}

E2E deploy{"deploy", [] {
    "646 runtime deploy places files in a directory"_test = [] {
        // The root's entries land at bin/<to>/<file> (`.` beside the executable), a dependency's
        // `from` is its own, a test binary finds the layout, two different sources for one
        // destination are refused at build time and identical ones merge, and a destination
        // outside the executable's directory is refused.
        const auto tmp = s::case_dir();
        s::project icd(tmp / "icd", {{"share/vulkan/icd.d/lvp_icd.json", "{\"ICD\": {\"library_path\": \"libvulkan_lvp.so\"}}\n"},
                                     {"src/icd.cppm", "export module icd;\nexport int icd_value() { return 3; }\n"},
                                     {"mcpp.toml", "[package]\nname    = \"icd\"\nversion = \"0.1.0\"\n\n[targets.icd]\nkind = \"lib\"\n\n[runtime]\ndeploy = [ { from = \"share/vulkan/icd.d/lvp_icd.json\", to = \"vulkan/icd.d\" } ]\n"}});
        s::project app(tmp / "app", {{"assets/readme.txt", "root readme\n"},
                                     {"assets/layers/lvp_icd.json", "layer manifest\n"},
                                     {"src/main.cpp", "import icd;\nint main() { return icd_value() == 3 ? 0 : 1; }\n"}});
        auto write_manifest = [&](std::string_view entries) {
            app.write("mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\nicd = {{ path = \"../icd\" }}\n\n[runtime]\ndeploy = [ {} ]\n", entries));
        };
        auto holds = [](const fs::path& f, std::string_view needle) { return fs::is_regular_file(f) && s::read_file(f).contains(needle); };
        // 1, 2, and the first half of 3
        write_manifest(R"({ from = "assets/readme.txt", to = "." }, { from = "assets/layers/lvp_icd.json", to = "layers" })");
        auto r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: the build failed: " << s::text(r);
        std::optional<fs::path> exe;
        for (const auto& f : s::find_files(app.at("target"), R"(app(\.exe)?)"))
            if (f.generic_string().contains("/bin/")) { exe = f; break; }
        expect(fatal(exe.has_value())) << "FAIL: no executable under target/: " << s::text(r);
        const auto bin = exe->parent_path();
        expect(fatal(fs::is_regular_file(bin / "readme.txt"))) << "FAIL: to = \".\" did not place readme.txt beside the executable";
        expect(fatal(holds(bin / "layers/lvp_icd.json", "layer manifest"))) << "FAIL: bin/layers/lvp_icd.json is missing or is not the root's file";
        expect(fatal(holds(bin / "vulkan/icd.d/lvp_icd.json", "library_path"))) << "FAIL: bin/vulkan/icd.d/lvp_icd.json is missing or is not the dependency's file";
        expect(fatal(!fs::exists(bin / "lvp_icd.json"))) << "FAIL: an entry with a directory was also flattened beside the executable";
        r = app.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: the program did not run: " << s::text(r);
        app.write("tests/layout.cpp", R"(#include <filesystem>
int main(int, char** argv) {
    const auto dir = std::filesystem::absolute(argv[0]).parent_path();
    return std::filesystem::exists(dir / "vulkan" / "icd.d" / "lvp_icd.json")
        && std::filesystem::exists(dir / "layers" / "lvp_icd.json") ? 0 : 1;
}
)");
        r = app.mcpp({"test"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: a test binary did not find the deployed layout beside itself: " << s::text(r);
        app.remove("tests");
        // 3: two different sources for one destination
        write_manifest(R"({ from = "assets/layers/lvp_icd.json", to = "vulkan/icd.d" })");
        r = app.mcpp({"build"});
        const auto collision = s::text(r);
        expect(fatal(!r)) << "FAIL: two different sources for bin/vulkan/icd.d/lvp_icd.json were accepted: " << collision;
        expect(fatal(e2e::contains(collision, "sources disagree"))) << "FAIL: the refusal does not say the sources disagree: " << collision;
        expect(fatal(e2e::contains(collision, "vulkan/icd.d/lvp_icd.json"))) << "FAIL: the refusal does not name the destination: " << collision;
        expect(fatal(e2e::contains(collision, "layers/lvp_icd.json"))) << "FAIL: the refusal does not name the root's source: " << collision;
        expect(fatal(e2e::contains(collision, "icd/share/vulkan/icd.d/lvp_icd.json"))) << "FAIL: the refusal does not name the dependency's source: " << collision;
        // The same shape with identical bytes merges into one stage edge.
        fs::create_directories(app.at("assets/icd2"));
        fs::copy_file(icd.at("share/vulkan/icd.d/lvp_icd.json"), app.at("assets/icd2/lvp_icd.json"));
        write_manifest(R"({ from = "assets/icd2/lvp_icd.json", to = "vulkan/icd.d" })");
        r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: identical bytes for one destination were refused: " << s::text(r);
        expect(fatal(holds(bin / "vulkan/icd.d/lvp_icd.json", "library_path"))) << "FAIL: the merged destination does not carry the shared bytes";
        const auto g = graph_of(app.dir());
        expect(fatal(g.has_value())) << "no build.ninja";
        const auto graph = s::read_file(*g);
        if (s::has_line(graph, R"(^build .*vulkan/icd\.d/lvp_icd\.json.* : stage_list)")) {
            const auto list = s::read_file(g->parent_path() / "placements.list");
            const auto placed = matching(list, R"(bin/vulkan/icd\.d/lvp_icd\.json$)");
            expect(fatal(placed.size() == 2u)) << "FAIL: expected the destination twice in the placement list, found " << placed.size() << ": " << list;
            expect(fatal(std::ranges::any_of(placed, [](const auto& l) { return l.contains("icd2/lvp_icd.json"); }))) << "FAIL: the placement list does not name the root's second source: " << list;
        } else {
            const auto edges = matching(graph, R"(^build .*vulkan/icd\.d/lvp_icd\.json : stage_file)");
            expect(fatal(edges.size() == 1u)) << "FAIL: expected exactly one stage_file edge, found " << edges.size();
            expect(fatal(edges.front().contains("icd2/lvp_icd.json"))) << "FAIL: the merged edge does not list the root's second source: " << edges.front();
        }
        // 4
        write_manifest(R"({ from = "assets/readme.txt", to = "../outside" })");
        r = app.mcpp({"build"});
        expect(fatal(!r)) << "FAIL: a destination outside the executable's directory was accepted: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "runtime.deploy[1]: `to` has a `.` or `..` component"))) << "FAIL: the refusal does not name the entry and the component: " << s::text(r);
    };

    "810 two packages deploy the same bytes to one name"_test[s::gcc][s::elf] = [] {
        // Two build programs deploy a generated file to one name: identical bytes give one
        // stage_file edge with both sources and `emit build-database` succeeds; different bytes
        // are refused at build time naming both sources and the destination.
        const auto tmp = s::case_dir();
        const std::string gen_sh = "#!/usr/bin/env bash\nset -e\ncp \"$1\" \"$2\"\n";
        auto build_mcpp = [](std::string_view id) {
            return std::format(R"(import mcpp;
#include <string>
int main() {{
    const std::string root = mcpp::manifest_dir();
    const std::string src  = root + "/res.txt";
    const std::string out  = std::string(mcpp::out_dir()) + "/gen/shared.bin";

    mcpp::action a;
    a.id   = "{}";
    a.role = "source";
    a.arg((root + "/gen.sh").c_str()).arg(src.c_str()).arg(out.c_str())
     .input(src.c_str())
     .output(out.c_str())
     .submit();

    mcpp::deploy(out.c_str(), "shared");
    return 0;
}}
)", id);
        };
        auto write_dep = [&](std::string_view content) {
            s::project dep(tmp / "dep", {{"mcpp.toml", "[package]\nname    = \"dep\"\nversion = \"0.1.0\"\n\n[targets.dep]\nkind = \"lib\"\n"},
                                         {"src/lib.cppm", "export module dep;\nexport int dep_value() { return 1; }\n"},
                                         {"res.txt", std::string(content) + "\n"},
                                         {"build.mcpp", build_mcpp("dep-gen-shared")}});
            s::write_executable(dep.at("gen.sh"), gen_sh);
        };
        auto write_app = [&](std::string_view content) {
            s::project app(tmp / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\ndep = { path = \"../dep\" }\n"},
                                         {"src/main.cpp", "import dep;\nint main() { return dep_value() == 1 ? 0 : 1; }\n"},
                                         {"res.txt", std::string(content) + "\n"},
                                         {"build.mcpp", build_mcpp("app-gen-shared")}});
            s::write_executable(app.at("gen.sh"), gen_sh);
            return app;
        };
        // 1, 2, 3: identical bytes
        write_dep("shared payload");
        auto app = write_app("shared payload");
        auto r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "the build failed on identical bytes: " << s::text(r);
        std::optional<fs::path> deployed;
        for (const auto& f : s::find_files(app.at("target"), R"(shared\.bin)"))
            if (f.generic_string().ends_with("/bin/shared/shared.bin")) { deployed = f; break; }
        expect(fatal(deployed.has_value())) << "bin/shared/shared.bin was not placed: " << s::text(r);
        expect(fatal(s::has_line(s::read_file(*deployed), "^shared payload$"))) << "the placed file does not carry the shared bytes";
        const auto g = graph_of(app.dir());
        expect(fatal(g.has_value())) << "no build.ninja";
        const auto edges = matching(s::read_file(*g), R"(^build .*shared/shared\.bin : stage_file)");
        expect(fatal(edges.size() == 1u)) << "expected exactly one stage_file edge for shared/shared.bin, found " << edges.size();
        const auto inputs_text = edges.front().substr(edges.front().find(": stage_file ") + 13);
        std::vector<std::string> inputs;
        for (auto& w : split(inputs_text, ' '))
            if (!w.empty()) inputs.push_back(std::move(w));
        expect(fatal(inputs.size() == 2u)) << "expected exactly two inputs on the stage_file edge, got: " << inputs_text;
        static const std::regex dep_src{R"(\.build-mcpp/out/[0-9a-f]*/[a-z.]*dep/gen/shared\.bin$)"};
        static const std::regex app_src{R"(\.build-mcpp/out/[0-9a-f]*/app/gen/shared\.bin$)"};
        expect(fatal(std::ranges::any_of(inputs, [](const auto& w) { return std::regex_search(w, dep_src); }))) << "the edge does not list the dependency's source: " << inputs_text;
        expect(fatal(std::ranges::any_of(inputs, [](const auto& w) { return std::regex_search(w, app_src); }))) << "the edge does not also list the app's own source: " << inputs_text;
        r = app.mcpp({"emit", "build-database"});
        expect(fatal(e2e::succeeded(r))) << "emit build-database failed on identical bytes: " << s::text(r);
        // 4: different bytes are refused at build time, naming both sources
        write_app("a different payload");
        fs::last_write_time(app.at("src/main.cpp"), fs::file_time_type::clock::now());
        r = app.mcpp({"build"});
        const auto log = s::text(r);
        expect(fatal(!r)) << "different bytes for one destination were accepted: " << log;
        expect(fatal(e2e::contains(log, "disagree"))) << "the refusal does not say the sources disagree: " << log;
        expect(fatal(s::has_line(log, R"(\.build-mcpp/out/[0-9a-f]*/[a-z.]*dep/gen/shared\.bin)"))) << "the refusal does not name the dependency's source: " << log;
        expect(fatal(s::has_line(log, R"(\.build-mcpp/out/[0-9a-f]*/app/gen/shared\.bin)"))) << "the refusal does not name the app's own source: " << log;
        expect(fatal(e2e::contains(log, "shared.bin"))) << "the refusal does not name the destination: " << log;
    };
}};

}  // namespace

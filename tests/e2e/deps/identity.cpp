// A path dependency's identity is its manifest's; the resolved graph is recorded and printed
// (formerly tests/e2e/679, 682).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

// The first resolution.json under `root` outside the build programs' trees.
std::optional<fs::path> resolution(const fs::path& root) {
    for (const auto& f : s::find_files(root, "resolution\\.json"))
        if (f.generic_string().find("/.build-mcpp/") == std::string::npos) return f;
    return std::nullopt;
}

E2E identity{"identity", [] {
    "679 a path dependency takes its manifests identity"_test = [] {
        // A `path` or `git` dependency's identity is the one its manifest declares, whatever key
        // reached it: one package, compiled once, recorded with both keys; two identities over one
        // source are refused before scanning; a namespace mismatch is one grouped warning.
        const auto tmp = s::case_dir();
        const std::string fw_manifest = "[package]\nnamespace = \"huxdemo\"\nname      = \"fw\"\nversion   = \"0.1.0\"\n[targets.fw]\nkind = \"lib\"\n";
        const std::string fw_module = "export module fw;\nexport int fw_anchor() { return 7; }\n";
        const std::string comp_manifest = "[package]\nnamespace = \"huxdemo\"\nname      = \"comp\"\nversion   = \"0.1.0\"\n[targets.comp]\nkind = \"lib\"\n[dependencies]\nfw = { path = \"../fw\" }\n";
        const std::string comp_module = "export module comp;\nimport fw;\nexport int comp_anchor() { return fw_anchor(); }\n";
        const std::string main = "import fw;\nimport comp;\nint main() { return fw_anchor() + comp_anchor() == 14 ? 0 : 1; }\n";
        s::project{tmp / "fw", {{"mcpp.toml", fw_manifest}, {"src/fw.cppm", fw_module}}};
        s::project comp{tmp / "comp", {{"mcpp.toml", comp_manifest}, {"src/comp.cppm", comp_module}}};
        s::project app{tmp / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[dependencies]\nhuxdemo.fw   = { path = \"../fw\" }\nhuxdemo.comp = { path = \"../comp\" }\n"},
                                     {"src/main.cpp", main}}};
        auto a = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(a))) << "A: build failed";
        expect(fatal(e2e::succeeded(app.mcpp({"run"})))) << "A: the program did not exit 0";
        expect(s::no_line(s::text(a), "declared identity is used|that identity is used")) << "A: a bare key produced an identity warning";
        std::size_t units = 0;
        for (const auto& f : s::find_files(app.at("target"), "fw\\.m\\.o"))
            if (f.generic_string().find("/obj/") != std::string::npos) ++units;
        expect(eq(units, std::size_t{1})) << "A: fw.cppm compiled " << units << " times";
        const auto json = resolution(app.at("target"));
        expect(fatal(json.has_value())) << "A: no resolution.json";
        std::string keys;
        {
            const auto doc = s::json_file(*json);
            std::vector<nlohmann::json> fws;
            for (const auto& p : doc["graph"]["packages"])
                if (p["package"]["name"] == "fw") fws.push_back(p);
            keys = std::to_string(fws.size());
            if (!fws.empty()) {
                std::vector<std::string> ks;
                for (const auto& r : fws[0]["requested_by"]) ks.push_back(r["key"].get<std::string>());
                std::ranges::sort(ks);
                keys += " ";
                for (std::size_t i = 0; i < ks.size(); ++i) keys += (i ? " " : "") + ks[i];
            }
        }
        expect(eq(keys, std::string{"1 fw huxdemo.fw"})) << "A: graph record reads '" << keys << "'";

        comp.write("mcpp.toml", "[package]\nnamespace = \"huxdemo\"\nname      = \"comp\"\nversion   = \"0.1.0\"\n[targets.comp]\nkind = \"lib\"\n[dependencies]\nhuxdemo.fw = { path = \"../fw\" }\n");
        app.remove("target");
        auto b = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(b))) << "B: build failed";
        expect(s::lacks(s::text(b), "declared identity is used")) << "B: matching keys produced an identity warning";

        s::project{tmp / "nons/fw", {{"mcpp.toml", "[package]\nname = \"fw\"\nversion = \"0.1.0\"\n[targets.fw]\nkind = \"lib\"\n"}, {"src/fw.cppm", fw_module}}};
        s::project nons{tmp / "nons/app", {{"mcpp.toml", "[package]\nname = \"app\"\nversion = \"0.1.0\"\n[dependencies]\na.fw = { path = \"../fw\" }\nb.fw = { path = \"../fw\" }\n"},
                                           {"src/main.cpp", "import fw;\nint main() { return 0; }\n"}}};
        auto c = nons.mcpp({"build"});
        expect(fatal(failed(c))) << "C: two identities over one source built";
        expect(e2e::contains(s::text(c), "one source is reached as two packages")) << "C: the refusal does not name the two identities";
        expect(s::no_line(s::text(c), "already provided|is provided by package")) << "C: the refusal came from the scanner, not before scanning";

        // D: comp (bare `fw` key again) and fw both reached under acme while they declare huxdemo.
        s::project grp{tmp / "grp/app", {{"libs/fw/mcpp.toml", fw_manifest}, {"libs/fw/src/fw.cppm", fw_module},
                                         {"libs/comp/mcpp.toml", comp_manifest}, {"libs/comp/src/comp.cppm", comp_module},
                                         {"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[dependencies]\nacme.fw   = { path = \"libs/fw\" }\nacme.comp = { path = \"libs/comp\" }\n"},
                                         {"src/main.cpp", main}}};
        auto d = grp.mcpp({"build"});
        expect(fatal(e2e::succeeded(d))) << "D: build failed";
        const auto warned = s::lines_with(s::text(d), "declared identity is used");
        expect(fatal(eq(warned.size(), std::size_t{1}))) << "D: expected one grouped warning, saw " << warned.size();
        expect(e2e::contains(warned[0], "mcpp.toml names 2 dependencies in namespace acme")) << "D: the warning does not name the manifest by its relative path and the count";
        expect(e2e::contains(warned[0], "declare huxdemo")) << "D: the warning does not name the declared namespace";
        expect(e2e::contains(warned[0], "acme.fw") && e2e::contains(warned[0], "acme.comp")) << "D: the warning does not list the keys";
        expect(e2e::contains(s::text(d), "[dependencies.huxdemo] table, for example")) << "D: the hint is not written in TOML";
        expect(s::lacks(s::text(d), "libs/comp/mcpp.toml names")) << "D: the bare key inside libs/comp was reported";
    };

    "682 why deps prints the resolved graph"_test = [] {
        // `resolution.json` records the resolved dependency graph and `mcpp why deps` prints it,
        // path dependencies included.
        const auto tmp = s::case_dir();
        s::project{tmp / "fw", {{"mcpp.toml", "[package]\nnamespace = \"demo\"\nname = \"fw\"\nversion = \"0.1.0\"\n[targets.fw]\nkind = \"lib\"\n"},
                                {"src/fw.cppm", "export module fw;\nexport int fw_anchor() { return 41; }\n"}}};
        s::project app{tmp / "app", {{"mcpp.toml", "[package]\nname = \"app\"\nversion = \"0.1.0\"\n[dependencies]\ndemo.fw = { path = \"../fw\" }\n"},
                                     {"src/main.cpp", "import fw;\nint main() { return fw_anchor() == 41 ? 0 : 1; }\n"}}};
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "build failed";
        auto why = app.mcpp({"why", "deps"});
        expect(fatal(e2e::succeeded(why))) << "A: why deps failed";
        expect(s::has_line(s::text(why), "^dependency graph:")) << "A: no graph section";
        expect(e2e::contains(s::text(why), "demo.fw@0.1.0")) << "A: the path dependency is not listed";
        expect(e2e::contains(s::text(why), "requested by mcpplibs.app@0.1.0 as 'demo.fw' in [dependencies]")) << "A: the request is not listed";
        expect(e2e::contains(s::text(why), "linked static (default)")) << "A: the link form is not listed";
        const auto json = resolution(app.at("target"));
        expect(fatal(json.has_value())) << "B: no resolution.json";
        const auto doc = s::json_file(*json);
        expect(eq(doc["schema_version"].get<int>(), 2)) << "B: schema_version";
        expect(doc.contains("toolchain") && doc.contains("runtime")) << "B: the record lacks toolchain or runtime";
        const auto& pkgs = doc["graph"]["packages"];
        expect(fatal(!pkgs.empty())) << "B: no packages";
        expect(pkgs[0]["root"] == true && pkgs[0]["requested_by"].empty()) << "B: the first package is not the root: " << pkgs[0].dump();
        std::vector<nlohmann::json> fw;
        for (const auto& p : pkgs)
            if (p["package"]["canonical"] == "demo.fw@0.1.0") fw.push_back(p);
        expect(eq(fw.size(), std::size_t{1})) << "B: demo.fw@0.1.0 recorded " << fw.size() << " times";
        expect(!fw.empty() && fw[0]["package"]["source"].get<std::string>().starts_with("path+")) << "B: the path dependency's source is not path+";
    };
}};

}  // namespace

// A package's features as machine-readable answers (mcpp#992; design 2026-11-21, package
// features; SPEC-004 §16). `mcpp search --format json` answers each package of an mcpp index for
// its newest version: from the index for a descriptor that carries its manifest inline,
// from the installed payload for one whose manifest is inside it, and `not-installed` with no list
// when the payload is absent -- the query installs nothing. `mcpp why features` states each package
// of a project's graph with the features it declares and those the build activates.
// `--protocol-version` lists `package-features` (the golden protocol, cli::golden/1123).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

// The package whose manifest is inside its payload: the test framework of this repository, whose
// descriptor names `*/mcpp.toml` and whose manifest declares features.
constexpr std::string_view kPointerPackage = "mcpplibs:mtest";

// Every record of `mcpp search --format json`, by its id.
std::map<std::string, nlohmann::json> catalogue(const fs::path& cwd) {
    auto r = s::mcpp({"search", "--format", "json"}, cwd);
    expect(fatal(e2e::succeeded(r))) << "mcpp search --format json failed\n" << s::text(r);
    const auto d = s::json(r.out);
    expect(fatal(d["kind"] == "mcpp.search")) << r.out;
    std::map<std::string, nlohmann::json> out;
    for (auto const& p : d["data"]["packages"]) out.emplace(p["id"].get<std::string>(), p);
    return out;
}

// What every answer holds: a known state, a list exactly when the state is `declared`.
void answers_are_well_formed(const std::map<std::string, nlohmann::json>& all) {
    for (auto const& [id, p] : all) {
        if (!p.contains("features")) continue;
        auto const& f = p["features"];
        const auto state = f.value("state", std::string{});
        expect(state == "declared" || state == "not-installed" || state == "unreadable") << id << ": " << f.dump();
        expect(f["from"] == "index" || f["from"] == "payload") << id << ": " << f.dump();
        expect(eq(f["list"].is_array(), state == "declared")) << id << ": a list where the answer is not known, or none where it is\n" << f.dump();
        expect(!p["versions"].empty() && f["version"] == p["versions"].front()) << id << ": not the newest version\n" << p.dump();
    }
}

}  // namespace

E2E features{"features", s::home_fixture, [] {
    "992 a package's features are read from the index or the installed payload, and never installed"_test[s::gcc] = [] {
        // A home of the case over the machine's payloads: its own store holds nothing yet.
        s::own_home(s::home::inherited);
        const auto tmp = s::case_dir();
        auto before = catalogue(tmp);
        answers_are_well_formed(before);

        // An inline descriptor answers from the index, a payload of the provider not at all.
        expect(fatal(std::ranges::any_of(before, [](auto const& kv) {
            auto const& f = kv.second.value("features", nlohmann::json::object());
            return f.value("from", std::string{}) == "index" && f.value("state", std::string{}) == "declared";
        }))) << "no package answered from the index";
        if (auto x = before.find("xim:gcc"); x != before.end())
            expect(!x->second.contains("features")) << "a payload of the provider is answered as an mcpp package\n" << x->second.dump();

        // A manifest inside a payload the home does not hold: not installed, and no list.
        const auto it = before.find(std::string(kPointerPackage));
        expect(fatal(it != before.end())) << kPointerPackage << " is not in the index";
        const auto f = it->second["features"];
        expect(fatal(f["from"] == "payload")) << f.dump();
        expect(fatal(f["state"] == "not-installed")) << "the answer for a payload the home lacks\n" << f.dump();
        expect(fatal(f["list"].is_null())) << f.dump();
        const auto version = f["version"].get<std::string>();
        expect(fatal(!fs::exists(s::case_home(s::home::inherited) / "xpkgs" / "mcpplibs-x-mtest" / version)))
            << "the query installed the payload";

        // Once the payload is installed, its manifest answers.
        s::project p(tmp / "p", {{"mcpp.toml", std::format("[package]\nname = \"uses\"\nversion = \"0.1.0\"\n\n"
                                                           "[dependencies]\nmcpplibs.mtest = \"{}\"\n", version)},
                                 {"src/main.cpp", "int main() { return 0; }\n"}});
        auto r = p.mcpp({"fetch"});
        expect(fatal(e2e::succeeded(r))) << "mcpp fetch failed\n" << s::text(r);
        auto after = catalogue(tmp);
        answers_are_well_formed(after);
        const auto g = after.at(std::string(kPointerPackage))["features"];
        expect(fatal(g["state"] == "declared" && g["from"] == "payload")) << "the installed payload did not answer\n" << g.dump();
        expect(fatal(g["list"].is_array() && !g["list"].empty())) << g.dump();
        for (auto const& e : g["list"])
            expect(e.contains("name") && e.contains("default") && e.contains("implies") && e.contains("enables")) << e.dump();
    };

    "992 why features states what each package declares and what the build activates"_test[s::gcc] = [] {
        const auto tmp = s::case_dir();
        s::project lib(tmp / "lib", {{"mcpp.toml", R"([package]
name = "flib"
version = "0.1.0"

[targets.flib]
kind = "lib"

[features]
default = ["fast"]
fast = ["simd"]
simd = []
unused = []
extra = { implies = ["simd"], description = "Extra helpers" }
)"}, {"src/flib.cppm", "export module flib;\nexport int flib_v() { return 3; }\n"}});
        s::project app(tmp / "app", {{"mcpp.toml", R"([package]
name = "fapp"
version = "0.1.0"

[dependencies]
flib = { path = "../lib", features = ["extra"] }
)"}, {"src/main.cpp", "import std;\nimport flib;\nint main() { std::println(\"{}\", flib_v()); }\n"}});

        auto r = app.mcpp({"why", "features", "--format", "json"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        const auto d = s::json(r.out);
        expect(fatal(d["kind"] == "mcpp.why.features" && d["data"]["status"] == "ok")) << r.out;
        auto const& packages = d["data"]["packages"];
        expect(fatal(packages.size() == 2u)) << r.out;
        expect(packages[0]["root"] == true && packages[0]["features"]["list"].empty()) << packages[0].dump();
        auto const& f = packages[1]["features"];
        std::map<std::string, nlohmann::json> declared;
        for (auto const& e : f["list"]) declared.emplace(e["name"].get<std::string>(), e);
        expect(fatal(declared.size() == 4u)) << f.dump();
        expect(declared["fast"]["default"] == true && declared["simd"]["default"] == true) << f.dump();
        expect(declared["extra"]["default"] == false && declared["extra"]["description"] == "Extra helpers") << f.dump();
        std::set<std::string> active;
        for (auto const& a : f["active"]) active.insert(a.get<std::string>());
        expect(eq(active, std::set<std::string>{"extra", "fast", "simd"})) << f.dump();

        r = app.mcpp({"why", "features"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(s::has_line(r.out, R"(^ +extra +active +implies simd$)")) << r.out;
        expect(s::has_line(r.out, R"(^ +Extra helpers$)")) << r.out;
        expect(s::has_line(r.out, R"(^ +unused$)")) << "a declared feature the build does not activate\n" << r.out;
    };
}};

// The dependency graph as a whole: a miss anywhere in it asks for a refresh, an upgrade restages,
// one name in two namespaces, merges and overrides (formerly tests/e2e/945, 948, 980, 1018, 1066,
// 1071).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

E2E graph{"graph", [] {
    "945 a miss anywhere in the graph asks for a refresh"_test[s::gcc][s::fresh_sandbox] = [] {
        // A request by version the local index cannot answer asks for a refresh wherever it is
        // in the graph, not only in the root's [dependencies]: declined offline, declined inside
        // the debounce window, and the refusal names the selector and the index.
        const auto index = s::case_home(s::home::inherited) / "indices/mcpplibs";
        fs::create_directories(index / "pkgs/w");
        s::project idx{index, {{".xlings-index-version", "abc1234\n"}, {".mcpp-index-updated", ""}}};
        fs::last_write_time(idx.at(".mcpp-index-updated"), fs::file_time_type::clock::now() - std::chrono::days{30});
        const auto tmp = s::case_dir();
        s::project{tmp / "mid", {{"mcpp.toml", "[package]\nname    = \"mid\"\nversion = \"0.1.0\"\n[targets.mid]\nkind = \"lib\"\n[dependencies]\nabsentpkg = \"1.0.0\"\n"},
                                 {"src/mid.cppm", "export module mid;\nexport int mid_value() { return 1; }\n"}}};
        s::project app{tmp / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n[dependencies]\nmid = { path = \"../mid\" }\n"},
                                     {"src/main.cpp", "int main() { return 0; }\n"}}};
        auto a = app.mcpp({"build"}, {.where = s::home::inherited, .env = {{"MCPP_VERBOSE", "1"}, {"MCPP_OFFLINE", "1"}}});
        expect(fatal(failed(a))) << "A: the build resolved a package no index carries";
        expect(s::has_line(s::text(a), R"(skip refresh for (mcpplibs[.:])?absentpkg: offline mode)")) << "A: the miss in a path dependency did not reach the refresh policy";
        idx.write(".mcpp-index-updated", "");
        auto b = app.mcpp({"build"}, {.where = s::home::inherited, .env = {{"MCPP_VERBOSE", "1"}}});
        expect(fatal(failed(b))) << "B: the build resolved a package no index carries";
        expect(s::has_line(s::text(b), R"(skip refresh for (mcpplibs[.:])?absentpkg: index was just refreshed)")) << "B: the debounce did not decide the miss in a path dependency";
        expect(s::no_line(s::text(b), "Refreshing +package index")) << "B: a sync inside the debounce window";
        expect(e2e::contains(s::text(b), "dependency 'absentpkg': no package found for exact selector")) << "C: the refusal does not name the selector";
        expect(e2e::contains(s::text(b), "index: local index abc1234")) << "C: the refusal does not name the index";
    };

    "948 an upgraded dependency is restaged at the same size"_test[s::gcc][s::fresh_sandbox] = [] {
        // A dependency upgraded to a version the build cache already holds is staged over the
        // previous version's files, whatever their size (here the BMI's size does not change).
        const auto tmp = s::case_dir();
        const s::options inherited{.where = s::home::inherited};
        s::project{tmp / "local-index", {{"pkgs/c/constant.lua", R"cpp(package = {
    spec = "1",
    name = "constant",
    description = "One constant",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux = {
            ["1.0.0"] = {
                url = "https://example.invalid/constant-1.0.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
            ["1.0.1"] = {
                url = "https://example.invalid/constant-1.0.1.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
    },
    mcpp = {
        language = "c++23",
        import_std = true,
        sources = { "src/**/*.cppm" },
        targets = { ["constant"] = { kind = "lib" } },
        deps = {},
    },
}
)cpp"}}};
        auto make_project = [&](std::string_view dir, std::string_view version) {
            s::project p{tmp / dir, {{"src/main.cpp", "import std;\nimport constant;\nint main() { std::println(\"{} {}\", value, stated()); return 0; }\n"}}};
            for (auto [v, n] : {std::pair{"1.0.0", 41}, std::pair{"1.0.1", 42}})
                p.write(std::format(".mcpp/.xlings/data/xpkgs/local-dev.constant/{}/src/constant.cppm", v),
                        std::format("export module constant;\nexport constexpr int value = {};\nexport int stated() {{ return value; }}\n", n));
            p.write("mcpp.toml", std::format("[package]\nname = \"{0}\"\nversion = \"0.1.0\"\n[indices]\nlocal-dev = {{ path = \"{1}\" }}\n[dependencies]\n\"local-dev.constant\" = \"{2}\"\n[targets.{0}]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", dir, (tmp / "local-index").generic_string(), version));
            return p;
        };
        auto prints = [](const s::project& p, std::string_view name) {
            auto bins = s::glob(p.at("target"), std::format("*/*/bin/{}", name));
            expect(fatal(!bins.empty())) << name << " built no program";
            auto out = s::run(bins.front()).out;
            while (!out.empty() && out.back() == '\n') out.pop_back();
            return out;
        };
        auto omega = make_project("omega", "1.0.1");
        expect(fatal(e2e::succeeded(omega.mcpp({"build"}, inherited)))) << "omega did not build";
        expect(fatal(eq(prints(omega, "omega"), std::string{"42 42"}))) << "omega's output";
        auto alpha = make_project("alpha", "1.0.0");
        expect(fatal(e2e::succeeded(alpha.mcpp({"build"}, inherited)))) << "alpha did not build";
        expect(fatal(eq(prints(alpha, "alpha"), std::string{"41 41"}))) << "alpha's output";
        std::optional<fs::path> bmi;
        for (const auto& f : s::find_files(alpha.at("target"), "constant\\.gcm"))
            if (!bmi && f.generic_string().find("gcm.cache") != std::string::npos) bmi = f;
        expect(fatal(bmi.has_value())) << "no BMI of the dependency in the build directory";
        const auto before = fs::file_size(*bmi);
        alpha.write("mcpp.toml", std::format("[package]\nname = \"alpha\"\nversion = \"0.1.0\"\n[indices]\nlocal-dev = {{ path = \"{}\" }}\n[dependencies]\n\"local-dev.constant\" = \"1.0.1\"\n[targets.alpha]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", (tmp / "local-index").generic_string()));
        auto up = alpha.mcpp({"build"}, inherited);
        expect(fatal(e2e::succeeded(up))) << "the upgraded build failed";
        expect(fatal(s::has_line(s::text(up), R"(Cached local-dev\.constant v1\.0\.1)"))) << "1.0.1 was not served from the cache, so nothing here is staged";
        expect(fatal(eq(fs::file_size(*bmi), before))) << "the BMI's size changed; the case is not the same-size one";
        expect(eq(prints(alpha, "alpha"), std::string{"42 42"})) << "alpha's output after the upgrade";
        std::optional<fs::path> cached;
        for (const auto& f : s::find_files(s::case_home(s::home::inherited) / "build-cache", "constant\\.gcm"))
            if (!cached && f.generic_string().find("constant@1.0.1") != std::string::npos) cached = f;
        expect(cached.has_value() && s::read_file(*bmi) == s::read_file(*cached)) << "the staged BMI is not 1.0.1's";
    };

    "980 an ambiguous dependency name is no packages"_test[s::elf][s::gcc] = [] {
        // Two dependencies whose manifests both say `rules`: the bare name is no package's — not
        // published, not warned about on every run, MCPP_DEP_AMBIGUOUS_RULES names both — and
        // each qualified name still reaches its own directory; dep_dir("rules") is refused.
        const auto tmp = s::case_dir();
        for (std::string_view ns : {"openmui", "gongfu"}) {
            s::project rules{tmp / std::format("{}-rules", ns), {}};
            rules.write("mcpp.toml", std::format("[package]\nnamespace = \"{}\"\nname      = \"rules\"\nversion   = \"0.1.0\"\n[modules]\nsources = [\"src/**/*.cpp\"]\n[targets.rules]\nkind = \"lib\"\n", ns));
            rules.write("src/r.cpp", std::format("int {}_rules_touch() {{ return 1; }}\n", ns));
        }
        s::project app{tmp / "app", {{"src/main.cpp", "import std;\nextern const char* seen();\nint main() { std::println(\"{}\", seen()); }\n"},
                                     {"build.mcpp", R"cpp(#include <cstdio>
#include <cstdlib>
#include <string>
import mcpp;
int main() {
    auto env = [](const char* n) { const char* v = std::getenv(n); return std::string(v ? v : "<unset>"); };
    std::string line = "bare=" + env("MCPP_DEP_RULES_DIR")
        + " ambiguous=" + env("MCPP_DEP_AMBIGUOUS_RULES")
        + " openmui=" + env("MCPP_DEP_OPENMUI_RULES_DIR")
        + " gongfu=" + env("MCPP_DEP_GONGFU_RULES_DIR");
    std::string out = "src/seen.cpp";
    std::FILE* f = std::fopen(out.c_str(), "w");
    std::fprintf(f, "const char* seen() { return R\"(%s)\"; }\n", line.c_str());
    std::fclose(f);
    mcpp::generated("src/seen.cpp");
    return 0;
}
)cpp"},
                                     {"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[modules]\nsources = [\"src/**/*.cpp\"]\n[dependencies]\nopenmui.rules = { path = \"../openmui-rules\" }\ngongfu.rules  = { path = \"../gongfu-rules\" }\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"}}};
        auto b = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(b))) << "build failed";
        expect(s::lacks(s::text(b), "collides on")) << "an ambiguous name still warns";
        const auto seen = s::last_line(app.mcpp({"run"}).out);
        expect(e2e::contains(seen, "bare=<unset>")) << "the bare name is bound: " << seen;
        expect(e2e::contains(seen, "ambiguous=gongfu.rules,openmui.rules") || e2e::contains(seen, "ambiguous=openmui.rules,gongfu.rules")) << "MCPP_DEP_AMBIGUOUS_RULES does not name both: " << seen;
        expect(s::has_line(seen, "openmui=.*openmui-rules")) << "openmui.rules lost its directory: " << seen;
        expect(s::has_line(seen, "gongfu=.*gongfu-rules")) << "gongfu.rules lost its directory: " << seen;
        app.write("build.mcpp", "import mcpp;\nint main() { return mcpp::dep_dir(\"rules\")[0] ? 0 : 1; }\n");
        app.remove("src/seen.cpp");
        auto ask = app.mcpp({"build"});
        expect(fatal(failed(ask))) << "dep_dir(\"rules\") answered";
        expect(e2e::contains(s::text(ask), "dep_dir(\"rules\") names more than one dependency")) << "the program did not say the name is ambiguous";
        expect(e2e::contains(s::text(ask), "openmui.rules") && e2e::contains(s::text(ask), "gongfu.rules")) << "the refusal does not name both packages";
    };

    "1018 a package and a namespaced package of one name both link"_test = [] {
        // `foo` and `ns.foo` are two packages, each reached by the edge that names it (mcpp#896).
        s::project ws{{{"mcpp.toml", "[workspace]\nmembers = [\"foo\", \"nsfoo\", \"app\"]\n"},
                       {"foo/mcpp.toml", "[package]\nname = \"foo\"\nversion = \"0.1.0\"\n[targets.foo]\nkind = \"lib\"\n[build]\nsources = [\"src/*.cppm\", \"src/*.cpp\"]\n"},
                       {"nsfoo/mcpp.toml", "[package]\nnamespace = \"ns\"\nname = \"foo\"\nversion = \"0.1.0\"\n[targets.ns-foo]\nkind = \"lib\"\n[build]\nsources = [\"src/*.cppm\", \"src/*.cpp\"]\n"},
                       {"app/mcpp.toml", "[package]\nname = \"app\"\nversion = \"0.1.0\"\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n[dependencies]\nfoo = { path = \"../foo\" }\nns.foo = { path = \"../nsfoo\" }\n"},
                       {"foo/src/foo.cppm", "export module foo;\nexport namespace foo { int one(); }\n"},
                       {"foo/src/foo.cpp", "module foo;\nnamespace foo { int one() { return 1; } }\n"},
                       {"nsfoo/src/foo.cppm", "export module ns.foo;\nexport namespace ns::foo { int two(); }\n"},
                       {"nsfoo/src/foo.cpp", "module ns.foo;\nnamespace ns::foo { int two() { return 2; } }\n"},
                       {"app/src/main.cpp", "import std;\nimport foo;\nimport ns.foo;\nint main() { std::println(\"sum {}\", foo::one() + ns::foo::two()); }\n"}}};
        auto r = ws.mcpp({"run", "-p", "app"});
        expect(fatal(e2e::succeeded(r))) << "the member reaching foo and ns.foo did not link and run";
        expect(e2e::contains(s::text(r), "sum 3")) << "the program's output";
    };

    "1066 a merged version withdraws its requests"_test = [] {
        // The app asks for mcpplibs.os ^0.4.1 (the newest, over openkal 0.25); a path library pins
        // 0.4.1 (over openkal 0.24). The merge takes os down to 0.4.1, and the requests the newer
        // version made leave the graph with it.
        const auto tmp = s::case_dir();
        s::project{tmp / "anchor", {{"mcpp.toml", "[package]\nname    = \"anchor\"\nversion = \"0.1.0\"\n[targets.anchor]\nkind = \"lib\"\n[dependencies]\nmcpplibs.os = \"0.4.1\"\n"},
                                    {"src/anchor.cppm", "export module anchor;\nexport int anchor_answer() { return 1; }\n"}}};
        s::project app{tmp / "app", {{"src/main.cpp", "import anchor;\nint main() { return anchor_answer() == 1 ? 0 : 1; }\n"}}};
        app.write("mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[dependencies]\nmcpplibs.os = \"^0.4.1\"\nanchor = {{ path = \"{}/anchor\" }}\n", tmp.generic_string()));
        auto plan = app.mcpp({"build", "--configure-only"});
        expect(fatal(e2e::succeeded(plan))) << "the merged graph was refused";
        expect(s::lacks(s::text(plan), "irreconcilable")) << "a request of a replaced version was kept";
        expect(s::has_line(s::text(plan), R"(Merged mcpplibs\.os 0\.4\.[2-9] ⨯ 0\.4\.1 → v0\.4\.1)")) << "os was not merged down to the pinned version";
        // `grep -A2 '^[package."<name>"]' mcpp.lock`: the header and the two lines after it.
        const auto lock = app.read("mcpp.lock");
        auto entry = [&](std::string_view name) {
            std::string out;
            std::istringstream in{lock};
            int left = 0;
            for (std::string line; std::getline(in, line);) {
                if (line == std::format("[package.\"{}\"]", name)) left = 3;
                if (left > 0) { out += line + "\n"; --left; }
            }
            return out;
        };
        expect(e2e::contains(entry("openkal"), "version = \"0.24.0\"")) << "openkal is not the 0.24 the pinned os asks for";
        for (auto [name, series] : {std::pair{"openkal-linux", "0.26"}, std::pair{"openkal-macos", "0.23"}, std::pair{"openkal-windows", "0.21"}})
            expect(s::lacks(entry(name), std::format("version = \"{}.", series))) << name << " " << series << " of the replaced version is in the graph";
    };

    "1071 the root overrides a dependency for the whole graph"_test = [] {
        // Two requests no merge reconciles are refused naming the override line; the root's
        // `override = true` chooses one version for the whole graph, names the replaced request
        // once, is locked and checked by --locked; a library's override is ignored, reported.
        const auto tmp = s::case_dir();
        s::project anchor{tmp / "anchor", {{"mcpp.toml", "[package]\nname    = \"anchor\"\nversion = \"0.1.0\"\n[targets.anchor]\nkind = \"lib\"\n[dependencies]\nmcpplibs.os = \"0.4.1\"\n"},
                                           {"src/anchor.cppm", "export module anchor;\nexport int anchor_answer() { return 1; }\n"}}};
        s::project app{tmp / "app", {{"src/main.cpp", "import anchor;\nint main() { return anchor_answer() == 1 ? 0 : 1; }\n"}}};
        auto write_app = [&](std::string_view os) {
            app.write("mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[dependencies]\nmcpplibs.os = {}\nanchor = {{ path = \"{}/anchor\" }}\n", os, tmp.generic_string()));
        };
        // The locked version of mcpplibs.os, and " override" when the entry says so (python3 read
        // it for the script): the entry is the text up to the next `[`.
        auto locked_os = [&] {
            const auto lock = app.read("mcpp.lock");
            const auto at = lock.find("[package.\"mcpplibs.os\"]");
            if (at == std::string::npos) return std::string{};
            const auto start = at + std::string_view{"[package.\"mcpplibs.os\"]"}.size();
            const auto body = lock.substr(start, lock.find('[', start) - start);
            std::smatch v;
            std::string out = std::regex_search(body, v, std::regex{R"re(version\s*=\s*"([^"]+)")re"}) ? v[1].str() : "";
            if (s::has_line(body, R"(^override\s*=\s*true)")) out += " override";
            return out;
        };
        write_app("\"^0.4.4\"");
        auto a = app.mcpp({"build", "--configure-only"});
        expect(fatal(failed(a))) << "A: two requests no merge reconciles were accepted";
        expect(e2e::contains(s::text(a), "irreconcilable versions")) << "A: the refusal does not name the conflict";
        expect(s::has_line(s::text(a), R"(mcpplibs\.os = \{ version = "[0-9.]*", override = true \})")) << "A: the refusal does not name the override";
        write_app("{ version = \"0.4.4\", override = true }");
        auto b = app.mcpp({"build", "--configure-only"});
        expect(fatal(e2e::succeeded(b))) << "B: the override was refused";
        expect(s::has_line(s::text(b), R"(Overrode +mcpplibs\.os 0\.4\.1 \(requested by [^)]*anchor[^)]*\) -> 0\.4\.4 \(\[dependencies\] of app\))")) << "B: the replaced request is not named";
        expect(eq(s::count_lines(s::text(b), "Overrode"), std::size_t{1})) << "B: the replaced request is named more than once";
        expect(eq(locked_os(), std::string{"0.4.4 override"})) << "B: mcpp.lock does not record the override";
        write_app("{ version = \"0.4.4\", override = true }");
        expect(fatal(e2e::succeeded(app.mcpp({"build", "--configure-only"})))) << "C: the override was refused";
        {
            std::istringstream in{app.read("mcpp.lock")};
            std::string out;
            for (std::string line; std::getline(in, line);) out += (line == "override = true" ? "" : line) + "\n";
            app.write("mcpp.lock", out);
        }
        auto c3 = app.mcpp({"build", "--configure-only", "--locked"});
        expect(failed(c3)) << "C: --locked accepted a lock without the override";
        expect(e2e::contains(s::text(c3), "mcpplibs.os 0.4.4 -> 0.4.4 (override)")) << "C: the drift does not name the override";
        anchor.write("mcpp.toml", "[package]\nname    = \"anchor\"\nversion = \"0.1.0\"\n[targets.anchor]\nkind = \"lib\"\n[dependencies]\nmcpplibs.os = { version = \"0.4.1\", override = true }\n");
        auto d = app.mcpp({"build", "--configure-only"});
        expect(fatal(e2e::succeeded(d))) << "D: the build failed";
        expect(e2e::contains(s::text(d), "`override` on 'mcpplibs.os' is ignored")) << "D: a library's override is not reported";
    };
}};

}  // namespace

// A workspace as one graph per configuration: its build directories and product directories, one
// plan and a replay in one check, its build programs named, ordered, cached and compiled at the
// same time, a shared member compiled once per configuration and whatever the selection, and a
// member's executable beside its shared library (formerly tests/e2e/833, 834, 839, 851, 860, 872,
// 880, 912).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

const s::options inherited{.where = s::home::inherited};

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

std::string escaped(std::string_view name) { return std::regex_replace(std::string(name), std::regex{R"([.])"}, R"(\.)"); }

// The first executable `name` under `root` in a `bin` directory (`find -path '*/bin/*' -name`).
std::optional<fs::path> bin_of(const fs::path& root, std::string_view name) {
    auto all = s::find_files(root, escaped(s::exe(name)));
    std::ranges::sort(all);
    for (const auto& f : all)
        if (f.generic_string().contains("/bin/")) return f;
    return std::nullopt;
}

// The file under `root` whose path ends with `tail` (`find -path '*<tail>'`).
std::optional<fs::path> ending(const fs::path& root, std::string_view tail) {
    const auto name = fs::path(std::string(tail)).filename().string();
    for (const auto& f : s::find_files(root, escaped(name)))
        if (f.generic_string().ends_with(tail)) return f;
    return std::nullopt;
}

// The program's standard output without carriage returns or the final newline.
std::string output_of(const std::optional<fs::path>& program) {
    expect(fatal(program.has_value())) << "no such program was built";
    auto r = s::run(*program);
    auto out = r.out;
    std::erase(out, '\r');
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

bool runs(const std::optional<fs::path>& program) { return program && s::run(*program).status == 0; }

// The directories of the graphs under target, outside the build programs' tree, sorted.
std::vector<fs::path> graph_dirs(const fs::path& root) {
    std::vector<fs::path> out;
    for (const auto& f : s::find_files(root / "target", R"(build\.ninja)"))
        if (!f.generic_string().contains("/.build-mcpp/")) out.push_back(f.parent_path());
    std::ranges::sort(out);
    return out;
}

std::size_t matching(const std::vector<std::string>& outputs, const std::regex& re, std::size_t from = 0) {
    std::size_t n = 0;
    for (std::size_t i = from; i < outputs.size(); ++i) n += std::regex_search(outputs[i], re) ? 1 : 0;
    return n;
}

// The lines of `.ninja_log` as their columns (start, end, mtime, output), the header skipped.
std::vector<std::vector<std::string>> ninja_rows(const fs::path& log) {
    std::vector<std::vector<std::string>> rows;
    std::istringstream in(s::read_file(log));
    for (std::string l; std::getline(in, l);) {
        if (l.starts_with('#')) continue;
        std::vector<std::string> cols;
        for (auto c : std::views::split(l, '\t')) cols.emplace_back(c.begin(), c.end());
        if (cols.size() >= 4) rows.push_back(std::move(cols));
    }
    return rows;
}

// The members whose build program a build reports as run, in order (`build.mcpp <m> ... ran N`).
std::vector<std::string> ran(std::string_view text, std::string_view members = "[a-z0-9]+") {
    std::vector<std::string> out;
    const std::regex re{"^ *build\\.mcpp (" + std::string(members) + ") .*ran [0-9]"};
    std::istringstream in{std::string(text)};
    for (std::string l; std::getline(in, l);) {
        std::smatch m;
        if (std::regex_search(l, m, re)) out.push_back(m[1]);
    }
    return out;
}

// A database's units by (set, source): their arguments, the database's own directory replaced.
std::map<std::pair<std::string, std::string>, std::vector<std::string>> units(std::string_view db) {
    std::map<std::pair<std::string, std::string>, std::vector<std::string>> out;
    static const std::regex own{R"(build-database[/\\][0-9a-f]+)"};
    for (const auto& set : s::json(db)["sets"])
        for (const auto& u : set["translation-units"]) {
            std::vector<std::string> args;
            for (const auto& a : u["arguments"]) args.push_back(std::regex_replace(a.get<std::string>(), own, "<db>"));
            out[{set["name"].get<std::string>(), u["source"].get<std::string>()}] = std::move(args);
        }
    return out;
}

// 872's comparison: the units both databases hold, `member`'s among them, have the same arguments,
// except for words that all contain `allowed`. What differs, or "" when nothing does.
std::string differences(std::string_view a, std::string_view b, std::string_view member, std::string_view allowed = "") {
    const auto ua = units(a), ub = units(b);
    bool any = false;
    std::string out;
    for (const auto& [k, args] : ua) {
        const auto other = ub.find(k);
        if (other == ub.end()) continue;
        any = any || k.first == member;
        if (args == other->second) continue;
        std::set<std::string> sa(args.begin(), args.end()), sb(other->second.begin(), other->second.end()), diff;
        std::ranges::set_symmetric_difference(sa, sb, std::inserter(diff, diff.end()));
        if (!allowed.empty() && !diff.empty() && std::ranges::all_of(diff, [&](const std::string& t) { return t.contains(allowed); })) continue;
        out += k.second + " (" + k.first + ") differs:";
        for (const auto& t : diff) out += " " + t;
        out += "\n";
    }
    if (!any) out += "no source of " + std::string(member) + " is in both databases\n";
    return out;
}

bool is_dual_dll(const fs::path& f) {
    const auto n = f.filename().string();
    return n.ends_with("dual_dll.dll") || n.contains("dual_dll.so") || n.ends_with("dual_dll.dylib");
}

// Moves every dual_dll library under target/*/bin/ into `hidden`: how many.
int hide_dual_dll(const fs::path& root, const fs::path& hidden, std::string_view prefix) {
    fs::create_directories(hidden);
    int n = 0;
    for (const auto& f : s::find_files(root / "target", ".*dual_dll.*"))
        if (f.generic_string().contains("/bin/") && is_dual_dll(f)) fs::rename(f, hidden / std::format("{}{}", prefix, ++n));
    return n;
}

E2E graph{"graph", [] {
    "833 a workspace is one graph per configuration"_test[s::unix_shell] = [] {
        // G1 a shared member compiled once; G2 a selection replays the workspace's graph; G3 a
        // member's flag edit rebuilds that member alone; G4 one build directory per
        // configuration; G5 G6 each member's products under bin/<package>/ (a namespaced one
        // under bin/<ns>.<name>/); G7 no member receives a build directory, and `clean --stale`
        // removes a member's old one but keeps its build-program outputs; G8 G9 a requested
        // artifact and runtime files in the consumer's product directory alone.
        s::project ws({{"mcpp.toml", "[workspace]\nmembers = [\"core\", \"cli\", \"gui\", \"ns1/tool\", \"ns2/tool\", \"modern\", \"helper\"]\n"},
                       {"core/mcpp.toml", "[package]\nname = \"core\"\nversion = \"0.1.0\"\n\n[targets.core]\nkind = \"lib\"\n"},
                       {"core/src/core.cppm", "export module shared_core;\nexport int core_v() { return 40; }\n"},
                       {"cli/src/main.cpp", "#include <cstdio>\nimport shared_core;\nint main() { std::printf(\"%d\\n\", core_v() + 1); return 0; }\n"},
                       {"gui/src/main.cpp", "#include <cstdio>\nimport shared_core;\nint main() { std::printf(\"%d\\n\", core_v() + 2); return 0; }\n"},
                       {"cli/data/cli.dat", "cli\n"},
                       {"helper/data/helper.dat", "helper\n"},
                       {"helper/mcpp.toml", "[package]\nname = \"helper\"\nversion = \"0.1.0\"\n\n[targets.helper]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n\n[runtime]\ndeploy_files = [\"data/helper.dat\"]\n"},
                       {"helper/src/main.cpp", "int main() { return 0; }\n"},
                       {"modern/mcpp.toml", "[package]\nname = \"modern\"\nversion = \"0.1.0\"\nstandard = \"c++26\"\n\n[targets.modern]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                       {"modern/src/main.cpp", "int main() { return 0; }\n"}});
        for (std::string m : {"cli", "gui"})
            ws.write(m + "/mcpp.toml", std::format("[package]\nname = \"{}\"\nversion = \"0.1.0\"\n\n[dependencies]\ncore = {{ path = \"../core\" }}\n\n"
                                                   "[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", m));
        ws.write("cli/mcpp.toml", ws.read("cli/mcpp.toml") + "\n[runtime]\ndeploy_files = [\"data/cli.dat\"]\n");
        ws.write("gui/mcpp.toml", ws.read("gui/mcpp.toml") + "\n[dependencies.helper]\npath = \"../helper\"\nartifacts = [\"helper\"]\n");
        for (std::string ns : {"ns1", "ns2"}) {
            ws.write(ns + "/tool/mcpp.toml", std::format("[package]\nnamespace = \"{}\"\nname = \"tool\"\nversion = \"0.1.0\"\n\n[targets.tool]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", ns));
            ws.write(ns + "/tool/src/main.cpp", "int main() { return 0; }\n");
        }
        auto r = ws.mcpp({"build", "--workspace"});
        expect(fatal(e2e::succeeded(r))) << "the workspace build failed: " << s::text(r);
        std::vector<fs::path> dirs;
        for (const auto& d : s::glob(ws.at("target"), "*/*"))
            if (fs::is_directory(d)) dirs.push_back(d);
        expect(fatal(dirs.size() == 2u)) << "G4: expected two build directories, got " << dirs.size();
        fs::path main_dir;
        for (const auto& d : dirs)
            if (fs::is_regular_file(d / "bin/cli" / s::exe("app"))) main_dir = d;
        expect(fatal(!main_dir.empty())) << "G5: bin/cli/app not found in any build directory";
        expect(fatal(fs::is_regular_file(main_dir / "bin/gui" / s::exe("app")))) << "G5: bin/gui/app not beside bin/cli/app";
        const auto other = dirs[0] == main_dir ? dirs[1] : dirs[0];
        expect(fatal(fs::is_regular_file(other / "bin/modern" / s::exe("modern")))) << "G4: the c++26 member is not in its own build directory";
        expect(fatal(output_of(main_dir / "bin/cli" / s::exe("app")) == "41")) << "G5: bin/cli/app is not cli's program";
        expect(fatal(output_of(main_dir / "bin/gui" / s::exe("app")) == "42")) << "G5: bin/gui/app is not gui's program";
        expect(fatal(fs::is_regular_file(main_dir / "bin/ns1.tool" / s::exe("tool")))) << "G6: bin/ns1.tool/tool missing";
        expect(fatal(fs::is_regular_file(main_dir / "bin/ns2.tool" / s::exe("tool")))) << "G6: bin/ns2.tool/tool missing";
        expect(fatal(fs::is_regular_file(main_dir / "bin/gui" / s::exe("helper")))) << "G8: bin/gui/helper is missing";
        expect(fatal(fs::is_regular_file(main_dir / "bin/helper" / s::exe("helper")))) << "G8: bin/helper/helper is missing";
        expect(fatal(fs::is_regular_file(main_dir / "bin/gui/helper.dat"))) << "G9: bin/gui/helper.dat is missing";
        expect(fatal(fs::is_regular_file(main_dir / "bin/cli/cli.dat"))) << "G9: bin/cli/cli.dat is missing";
        expect(fatal(!fs::exists(main_dir / "bin/gui/cli.dat"))) << "G9: cli's runtime file is in gui's product directory";

        auto outputs = s::ninja_log_outputs(main_dir);
        expect(fatal(matching(outputs, std::regex{R"(core\.m\.o)"}) == 1u)) << "G1: core's object was built " << matching(outputs, std::regex{R"(core\.m\.o)"}) << " times";
        const std::regex compiled{R"(\.(o|obj|gcm|pcm|ifc)$|^bin/cli/app)"};
        const auto before = matching(outputs, compiled);
        r = ws.mcpp({"build", "-p", "cli"});
        expect(fatal(e2e::succeeded(r))) << "G2: -p cli failed: " << s::text(r);
        expect(fatal(matching(s::ninja_log_outputs(main_dir), compiled) == before)) << "G2: -p cli recompiled after --workspace";

        std::this_thread::sleep_for(1100ms);
        ws.write("gui/mcpp.toml", ws.read("gui/mcpp.toml") + "\n[build]\ncxxflags = [\"-DGUI_EXTRA=1\"]\n");
        const auto lines = s::ninja_log_outputs(main_dir).size();
        r = ws.mcpp({"build", "--workspace"});
        expect(fatal(e2e::succeeded(r))) << "G3: the build after the edit failed: " << s::text(r);
        outputs = s::ninja_log_outputs(main_dir);
        std::string g3;
        for (std::size_t i = lines; i < outputs.size(); ++i)
            if (std::regex_search(outputs[i], std::regex{R"(\.(o|obj|gcm|pcm|ifc)$|/app(\.exe)?$)"})) g3 += outputs[i] + "\n";
        expect(fatal(e2e::contains(g3, "gui"))) << "G3: gui was not rebuilt after its flags changed: " << g3;
        expect(fatal(s::no_line(g3, "core|/cli/|_cli|ns1|ns2"))) << "G3: an edit to gui's flags rebuilt another member";
        expect(fatal(fs::is_directory(main_dir))) << "G3: the flag edit moved the build directory";
        for (std::string m : {"core", "cli", "gui", "ns1/tool", "ns2/tool", "modern", "helper"})
            expect(fatal(!fs::is_directory(ws.at(m + "/target") / main_dir.parent_path().filename()))) << "G7: " << m << " received a build directory";
        fs::create_directories(ws.at("cli/target/x86_64-linux-gnu/0123456789abcdef"));
        ws.write("cli/target/.build-mcpp/keep", "keep\n");
        r = ws.mcpp({"clean", "--stale"});
        expect(fatal(e2e::succeeded(r))) << "G7: clean --stale failed: " << s::text(r);
        expect(fatal(!fs::is_directory(ws.at("cli/target/x86_64-linux-gnu/0123456789abcdef")))) << "G7: clean --stale kept a member's old build directory";
        expect(fatal(ws.exists("cli/target/.build-mcpp/keep"))) << "G7: clean --stale removed a member's build-program outputs";
    };

    "834 a workspace plans once and replays in one check"_test[s::unix_shell][s::python3] = [] {
        // P1 P2: --workspace and -p resolve the toolchain once; P3: an unchanged build replays
        // without planning, in under a second, for --workspace, -p and from inside a member.
        s::project ws({{"mcpp.toml", "[workspace]\nmembers = [\"lib1\", \"lib2\", \"lib3\", \"lib4\", \"lib5\", \"app\"]\n"},
                       {"lib1/mcpp.toml", "[package]\nname = \"lib1\"\nversion = \"0.1.0\"\n\n[targets.lib1]\nkind = \"lib\"\n"},
                       {"lib1/src/lib1.cppm", "export module lib1;\nexport int v1() { return 1; }\n"},
                       {"app/mcpp.toml", "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\nlib5 = { path = \"../lib5\" }\n"},
                       {"app/src/main.cpp", "import lib5;\nint main() { return v5() == 5 ? 0 : 1; }\n"}});
        for (int i = 2; i <= 5; ++i) {
            ws.write(std::format("lib{}/mcpp.toml", i), std::format("[package]\nname = \"lib{0}\"\nversion = \"0.1.0\"\n\n[targets.lib{0}]\nkind = \"lib\"\n\n"
                                                                     "[dependencies]\nlib{1} = {{ path = \"../lib{1}\" }}\n", i, i - 1));
            ws.write(std::format("lib{0}/src/lib{0}.cppm", i), std::format("export module lib{0};\nimport lib{1};\nexport int v{0}() {{ return v{1}() + 1; }}\n", i, i - 1));
        }
        auto resolutions = [](const e2e::outcome& r) { return s::count_lines(s::text(r), "Resolving toolchain"); };
        auto r = ws.mcpp({"build", "--workspace"});
        expect(fatal(e2e::succeeded(r))) << "P1: the workspace build failed: " << s::text(r);
        expect(fatal(resolutions(r) == 1u)) << "P1: --workspace resolved the toolchain " << resolutions(r) << " times";
        fs::remove_all(ws.at("target"));
        r = ws.mcpp({"build", "-p", "app"});
        expect(fatal(e2e::succeeded(r))) << "P2: -p app failed: " << s::text(r);
        expect(fatal(resolutions(r) == 1u)) << "P2: -p app resolved the toolchain " << resolutions(r) << " times";
        const std::vector<std::pair<std::string, std::vector<std::string>>> selections{
            {"", {"build", "--workspace"}}, {"", {"build", "-p", "app"}}, {"app", {"build"}}};
        for (const auto& [dir, args] : selections) {
            ws.mcpp(args);
            r = ws.mcpp_in(dir, args);
            expect(fatal(e2e::succeeded(r))) << "P3: " << r.line << " failed: " << s::text(r);
            r = ws.mcpp_in(dir, args);
            expect(fatal(e2e::succeeded(r))) << "P3: the replay of " << r.line << " failed: " << s::text(r);
            expect(fatal(resolutions(r) == 0u)) << "P3: " << r.line << " with nothing changed was planned: " << s::text(r);
            expect(fatal(r.duration < 1000ms)) << "P3: " << r.line << " with nothing changed took " << r.duration.count() << " ms";
        }
    };

    "839 a workspaces build programs are named ordered and cached"_test = [] {
        // B1 each member's build program runs, B2 the dependency's first; B3 B4 a selection, a
        // source edit and `emit` replay them from the cache; B5 a member is named by its
        // directory, not by its source kind.
        s::project ws({{"mcpp.toml", "[workspace]\nmembers = [\"core\", \"cli\", \"gui\"]\n"},
                       {"core/mcpp.toml", "[package]\nname = \"core\"\nversion = \"0.1.0\"\n\n[targets.core]\nkind = \"lib\"\n"},
                       {"core/src/core.cppm", "export module bp_core;\nexport int core_v() { return 1; }\n"}});
        for (std::string m : {"cli", "gui"}) {
            ws.write(m + "/mcpp.toml", std::format("[package]\nname = \"{0}\"\nversion = \"0.1.0\"\n\n[dependencies]\ncore = {{ path = \"../core\" }}\n\n"
                                                   "[targets.{0}]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", m));
            ws.write(m + "/src/main.cpp", "import bp_core;\nint main() { return core_v() == 1 ? 0 : 1; }\n");
        }
        for (std::string p : {"core", "cli", "gui"})
            ws.write(p + "/build.mcpp", std::format("import mcpp;\nint main() {{ mcpp::define(\"BP_{}=1\"); return 0; }}\n", p));
        auto b1 = ws.mcpp({"build", "--workspace"});
        expect(fatal(e2e::succeeded(b1))) << "the first build failed: " << s::text(b1);
        const auto t1 = s::text(b1);
        for (std::string p : {"core", "cli", "gui"})
            expect(fatal(s::has_line(t1, "^ *build\\.mcpp " + p + " .* ran [0-9]"))) << "B1: no line states that " << p << "'s program ran: " << t1;
        const auto order = ran(t1);
        expect(fatal(!order.empty() && order.front() == "core")) << "B2: core's program did not run first: " << t1;
        auto b2 = ws.mcpp({"build", "-p", "cli", "-v"});
        expect(fatal(e2e::succeeded(b2))) << "-p cli failed: " << s::text(b2);
        fs::last_write_time(ws.at("core/src/core.cppm"), fs::file_time_type::clock::now());
        auto b3 = ws.mcpp({"build", "--workspace", "-v"});
        expect(fatal(e2e::succeeded(b3))) << "the second --workspace failed: " << s::text(b3);
        fs::last_write_time(ws.at("core/src/core.cppm"), fs::file_time_type::clock::now());
        auto b4 = ws.mcpp({"build", "-p", "gui", "-v"});
        expect(fatal(e2e::succeeded(b4))) << "-p gui failed: " << s::text(b4);
        for (const auto* r : {&b2, &b3, &b4}) {
            expect(fatal(s::no_line(s::text(*r), R"(^ *build\.mcpp .* ran [0-9])"))) << "B3: a program reran in " << r->line << ": " << s::text(*r);
            expect(fatal(s::has_line(s::text(*r), R"(^ *build\.mcpp .* cached)"))) << "B3: " << r->line << " shows no program at all: " << s::text(*r);
        }
        auto e = ws.mcpp({"emit", "build-database", "--format", "json", "-o", "db.json"});
        expect(fatal(e2e::succeeded(e))) << "emit failed: " << s::text(e);
        expect(fatal(s::no_line(s::text(e), R"(^ *build\.mcpp .* ran [0-9])"))) << "B4: emit reran a program";
        expect(fatal(s::has_line(t1, R"(^   Compiling core v0\.1\.0 \(core\)$)"))) << "B5: core is not named by its directory: " << t1;
        expect(fatal(s::has_line(s::text(b2), R"(^ +(Compiling|Fresh) core v0\.1\.0 \(core\)$)"))) << "B5: under -p cli, core is not named by its directory";
        expect(fatal(s::lacks(t1 + s::text(b2), "(path)"))) << "B5: a package is named by its source kind, not its directory";
    };

    "851 a shared member is compiled once per configuration"_test[s::unix_shell] = [] {
        // A: a library two configurations share is compiled once in each, with each
        // configuration's features and standard. B: two consumers in one configuration share one
        // compile with the union of their features, and a consumer's own cxxflags do not reach
        // it. C: `-p` compiles the library for that consumer's closure alone, in the same
        // build directories.
        s::project ws({{"mcpp.toml", "[workspace]\nmembers = [\"lib\", \"app23\", \"app26\"]\n"},
                       {"lib/mcpp.toml", "[package]\nname    = \"lib\"\nversion = \"0.1.0\"\n\n[features]\nextra = []\n\n[targets.lib]\nkind = \"lib\"\n"},
                       {"lib/src/lib.cppm", R"(export module shared_lib;
export long std_v() { return __cplusplus; }
export int extra_v() {
#ifdef MCPP_FEATURE_EXTRA
    return 1;
#else
    return 0;
#endif
}
export int consumer_flag_v() {
#ifdef CONSUMER_ONLY_FLAG
    return 1;
#else
    return 0;
#endif
}
)"},
                       {"app23/mcpp.toml", "[package]\nname    = \"app23\"\nversion = \"0.1.0\"\n\n[dependencies]\nlib = { path = \"../lib\", features = [\"extra\"] }\n\n"
                                           "[targets.app23]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                       {"app26/mcpp.toml", "[package]\nname     = \"app26\"\nversion  = \"0.1.0\"\nstandard = \"c++26\"\n\n[dependencies]\nlib = { path = \"../lib\" }\n\n"
                                           "[targets.app26]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                       {"app23b/mcpp.toml", "[package]\nname    = \"app23b\"\nversion = \"0.1.0\"\n\n[dependencies]\nlib = { path = \"../lib\" }\n\n"
                                            "[build]\ncxxflags = [\"-DCONSUMER_ONLY_FLAG=1\"]\n\n[targets.app23b]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"}});
        for (std::string a : {"app23", "app23b", "app26"})
            ws.write(a + "/src/main.cpp", "#include <cstdio>\nimport shared_lib;\nint main() { std::printf(\"%ld %d %d\\n\", std_v(), extra_v(), consumer_flag_v()); }\n");
        auto values = [&](std::string_view m) {
            const auto p = ending(ws.at("target"), std::format("/bin/{0}/{0}", m));
            std::istringstream in(output_of(p));
            long v = 0;
            int x = -1, f = -1;
            in >> v >> x >> f;
            return std::tuple{v, x, f};
        };
        const std::regex lib_object{R"((^|/)lib/src/lib\.m\.o$)"};
        auto r = ws.mcpp({"build", "--workspace"});
        expect(fatal(e2e::succeeded(r))) << "A: the workspace did not build: " << s::text(r);
        const auto dirs = graph_dirs(ws.dir());
        expect(fatal(dirs.size() == 2u)) << "A: expected two build directories, got " << dirs.size();
        for (const auto& d : dirs)
            expect(fatal(matching(s::ninja_log_outputs(d), lib_object) == 1u)) << "A: lib's object was compiled " << matching(s::ninja_log_outputs(d), lib_object) << " times in " << d;
        const auto [v23, x23, f23] = values("app23");
        const auto [v26, x26, f26] = values("app26");
        expect(fatal(x23 == 1 && x26 == 0)) << "A: the features crossed configurations (app23 " << x23 << ", app26 " << x26 << ")";
        expect(fatal(v23 > 0 && v26 > v23)) << "A: app26 does not link the lib compiled for its own standard (" << v23 << " against " << v26 << ")";

        ws.write("mcpp.toml", "[workspace]\nmembers = [\"lib\", \"app23\", \"app23b\", \"app26\"]\n");
        r = ws.mcpp({"build", "--workspace"});
        expect(fatal(e2e::succeeded(r))) << "B: the workspace did not build: " << s::text(r);
        const auto d23 = ending(ws.at("target"), "/bin/app23/" + s::exe("app23"));
        expect(fatal(d23.has_value())) << "B: no app23";
        const auto dir23 = d23->parent_path().parent_path().parent_path();
        expect(fatal(matching(s::ninja_log_outputs(dir23), lib_object) == 1u)) << "B: lib's object was compiled more than once for its two consumers";
        const auto [vb, xb, fb] = values("app23b");
        expect(fatal(fb == 0)) << "B: a consumer's own cxxflags reached lib's compile";
        expect(fatal(xb == 1)) << "B: lib was not compiled with the union of its consumers' features";
        r = ws.mcpp({"build", "-p", "app23b"});
        expect(fatal(e2e::succeeded(r))) << "C: -p app23b did not build: " << s::text(r);
        const auto [vc, xc, fc] = values("app23b");
        expect(fatal(xc == 0)) << "C: lib kept a feature app23b's closure does not ask for";
        expect(fatal(graph_dirs(ws.dir()).size() == 2u)) << "C: -p made a third build directory";
    };

    "860 a workspaces programs compile at the same time"_test = [] {
        // A: at -j 4 the members' build programs compile at the same time, and three run. B: at
        // -j 1 they compile one after another and run in the same order. C: when two do not
        // compile, the failure reported is that of the first to run, at either -j.
        s::project ws(s::case_dir() / "ws", {{"mcpp.toml", "[workspace]\nmembers = [\"pa\", \"pb\", \"pc\"]\n"}});
        for (std::string m : {"pa", "pb", "pc"}) {
            ws.write(m + "/mcpp.toml", std::format("[package]\nname    = \"{0}\"\nversion = \"0.1.0\"\n\n[targets.{0}]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", m));
            ws.write(m + "/src/main.cpp", "int main() { return 0; }\n");
            // Headers that take a while to parse, so that a compile is long enough for another
            // to be under way at the same time.
            ws.write(m + "/build.mcpp", std::format("#include <algorithm>\n#include <functional>\n#include <map>\n#include <regex>\n#include <sstream>\n#include <string>\n"
                                                    "import mcpp;\nint main() {{\n    std::regex re(\"a+b\");\n    mcpp::define(\"T860_{}=1\");\n"
                                                    "    return std::regex_match(\"aab\", re) ? 0 : 1;\n}}\n", m));
        }
        auto span = [&](const std::string& m) -> std::optional<std::pair<long, long>> {
            const auto logs = s::find_files(ws.at("target/.build-mcpp/host"), R"(\.ninja_log)");
            if (logs.empty()) return std::nullopt;
            const std::regex re{"^obj/" + m + "-[0-9a-f]+/.*build\\.mcpp\\.o$"};
            for (const auto& row : ninja_rows(logs.front()))
                if (std::regex_search(row[3], re)) return std::pair{std::stol(row[0]), std::stol(row[1])};
            return std::nullopt;
        };
        auto clean = [&] {
            for (std::string d : {"target", "pa/target", "pb/target", "pc/target"}) fs::remove_all(ws.at(d));
        };
        auto r = ws.mcpp({"build", "--workspace", "-j", "4"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "A: the workspace did not build at -j 4: " << s::text(r);
        long latest_start = 0, earliest_end = std::numeric_limits<long>::max();
        for (std::string m : {"pa", "pb", "pc"}) {
            const auto sp = span(m);
            expect(fatal(sp.has_value())) << "A: the build programs' .ninja_log does not record every compile";
            latest_start = std::max(latest_start, sp->first);
            earliest_end = std::min(earliest_end, sp->second);
        }
        expect(fatal(latest_start < earliest_end)) << "A: the compiles did not overlap at -j 4 (latest start " << latest_start << ", earliest end " << earliest_end << ")";
        const auto order = ran(s::text(r), "p[abc]");
        expect(fatal(order.size() == 3u)) << "A: expected three programs to run, got " << order.size() << ": " << s::text(r);
        clean();
        r = ws.mcpp({"build", "--workspace", "-j", "1"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "B: the workspace did not build at -j 1: " << s::text(r);
        std::vector<std::pair<long, long>> spans;
        for (std::string m : {"pa", "pb", "pc"}) {
            const auto sp = span(m);
            expect(fatal(sp.has_value())) << "B: the build programs' .ninja_log does not record the compile of " << m;
            spans.push_back(*sp);
        }
        std::ranges::sort(spans);
        long prev_end = 0;
        for (const auto& [st, en] : spans) {
            expect(fatal(st >= prev_end)) << "B: two compiles overlapped at -j 1";
            prev_end = en;
        }
        expect(fatal(ran(s::text(r), "p[abc]") == order)) << "B: the programs ran in another order at -j 1 than at -j 4";
        const auto first = order[0], second = order[1];
        ws.write(first + "/build.mcpp", "#include <regex>\n#include <map>\n#error T860-failure-in-" + first + "\nint main() { return 0; }\n");
        ws.write(second + "/build.mcpp", "#error T860-failure-in-" + second + "\nint main() { return 0; }\n");
        clean();
        r = ws.mcpp({"build", "--workspace", "-j", "4"}, inherited);
        expect(fatal(failed(r))) << "C: the workspace built although two programs do not compile";
        expect(fatal(e2e::contains(s::text(r), "T860-failure-in-" + first))) << "C: the failure reported at -j 4 is not the one of " << first << ", the first to run: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), "T860-failure-in-" + second))) << "C: the failure of " << second << ", which runs after " << first << ", was reported beside it";
        clean();
        r = ws.mcpp({"build", "--workspace", "-j", "1"}, inherited);
        expect(fatal(failed(r))) << "C: the workspace built although two programs do not compile (-j 1)";
        expect(fatal(e2e::contains(s::text(r), "T860-failure-in-" + first))) << "C: the failure reported at -j 1 is not the one of " << first << ": " << s::text(r);
    };

    "872 a member compiles once whatever the selection"_test[s::python3] = [] {
        // A B: after --workspace, every selection (and --release likewise, F) compiles nothing.
        // C: `emit build-database` gives each member's closure the same arguments whatever the
        // selection, names only argument files that exist, and names module maps. D: lib differs
        // between -p b and --workspace only by the feature a asks for. E: a cached dependency's
        // partition BMI is staged below its package's directory.
        const auto tmp = s::case_dir();
        s::project ws(tmp / "ws", {
            {"mcpp.toml", "[workspace]\nmembers = [\"core\", \"app\", \"tool\", \"tool2\", \"dso\"]\n"},
            {"shared/m.cppm", "export module m;\nexport int answer() { return 42; }\n"},
            {"core/src/core.cppm", "export module corelib;\nimport m;\nexport int core_value() { return answer(); }\n"},
            {"core/mcpp.toml", "[package]\nname    = \"core\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/*.cppm\", \"../shared/m.cppm\"]\n\n[targets.core]\nkind = \"lib\"\n"},
            {"app/src/main.cpp", "#include <cstdio>\nimport corelib;\nint main() { std::printf(\"%d\\n\", core_value()); }\n"},
            {"app/mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\ncore = { path = \"../core\" }\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
            {"tool/src/main.cpp", "#include <cstdio>\nimport m;\nint main() { std::printf(\"%d\\n\", answer() + 1); }\n"},
            {"tool/mcpp.toml", "[package]\nname    = \"tool\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/*.cpp\", \"../shared/m.cppm\"]\n\n[targets.tool]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
            {"tool2/src/m.cppm", "export module m;\nexport int answer() { return 7; }\n"},
            {"tool2/src/main.cpp", "#include <cstdio>\nimport m;\nint main() { std::printf(\"%d\\n\", answer()); }\n"},
            {"tool2/mcpp.toml", "[package]\nname    = \"tool2\"\nversion = \"0.1.0\"\n\n[targets.tool2]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
            {"dso/src/dso.cppm", "export module dso;\nexport int dso_value() { return 5; }\n"},
            {"dso/mcpp.toml", "[package]\nname    = \"dso\"\nversion = \"0.1.0\"\n\n[targets.dso]\nkind = \"shared\"\n"},
        });
        auto r = ws.mcpp({"build", "--workspace"});
        expect(fatal(e2e::succeeded(r))) << "A: the workspace did not build: " << s::text(r);
        for (const auto& [name, value] : std::vector<std::pair<std::string, std::string>>{{"app", "42"}, {"tool", "43"}, {"tool2", "7"}})
            expect(fatal(output_of(bin_of(ws.at("target"), name)) == value)) << "A: " << name << " does not print " << value;
        const std::regex object{R"(\.(o|obj)$)"};
        auto dirs = graph_dirs(ws.dir());
        expect(fatal(dirs.size() == 1u)) << "B: expected one build directory, got " << dirs.size();
        const auto dev = dirs[0];
        auto none_after = [&](const fs::path& dir, std::vector<std::string> args, std::string_view label) {
            const auto before = s::ninja_log_outputs(dir).size();
            auto b = ws.mcpp(args);
            expect(fatal(e2e::succeeded(b))) << label << ": " << b.line << " failed: " << s::text(b);
            const auto n = matching(s::ninja_log_outputs(dir), object, before);
            expect(fatal(n == 0u)) << label << ": " << b.line << " compiled " << n << " units after --workspace";
        };
        for (std::string sel : {"core", "app", "tool", "tool2", "dso", "app", "tool2", "app"}) none_after(dev, {"build", "-p", sel}, "B");
        none_after(dev, {"build", "--workspace"}, "B");

        r = ws.mcpp({"build", "--workspace", "--release"});
        expect(fatal(e2e::succeeded(r))) << "F: the release workspace did not build: " << s::text(r);
        fs::path rel;
        for (const auto& d : graph_dirs(ws.dir()))
            if (d != dev) rel = d;
        expect(fatal(!rel.empty())) << "F: no release build directory";
        expect(fatal(e2e::contains(s::read_file(rel / "build.ninja"), "file-prefix-map"))) << "F: the release build trims no path, so this criterion measured nothing";
        for (std::string sel : {"app", "tool", "app", "core", "tool2", "app"}) none_after(rel, {"build", "--release", "-p", sel}, "F");
        none_after(rel, {"build", "--release", "--workspace"}, "F");

        auto emit = [](const s::project& p, std::vector<std::string> sel) {
            sel.insert(sel.begin(), {"emit", "build-database"});
            auto e = p.mcpp(sel);
            expect(fatal(e2e::succeeded(e))) << e.line << " failed: " << e.err;
            return e.out;
        };
        const auto c_ws = emit(ws, {"--workspace"});
        for (std::string m : {"core", "app", "tool", "tool2", "dso"}) {
            const auto diff = differences(emit(ws, {"-p", m}), c_ws, m);
            expect(fatal(diff.empty())) << "C: the argument lists of " << m << "'s closure depend on the selection:\n" << diff;
        }
        std::string missing;
        for (const auto& set : s::json(c_ws)["sets"])
            for (const auto& u : set["translation-units"])
                for (const auto& a : u["arguments"]) {
                    const auto w = a.get<std::string>();
                    if (w.starts_with('@') && !fs::is_regular_file(w.substr(1))) missing += w + "\n";
                }
        expect(fatal(missing.empty())) << "C: the database names an argument file that does not exist:\n" << missing;
        expect(fatal(e2e::contains(c_ws, ".modmap") || e2e::contains(c_ws, "fmodule-mapper"))) << "C: no unit names a module map, so the comparison above measured nothing";

        s::project fws(tmp / "fws", {
            {"mcpp.toml", "[workspace]\nmembers = [\"lib\", \"a\", \"b\"]\n"},
            {"lib/mcpp.toml", "[package]\nname    = \"lib\"\nversion = \"0.1.0\"\n\n[features]\nextra = []\n\n[targets.lib]\nkind = \"lib\"\n"},
            {"lib/src/featlib.cppm", "export module featlib;\nexport int v() { return 1; }\n"},
        });
        for (std::string m : {"a", "b"}) {
            fws.write(m + "/mcpp.toml", std::format("[package]\nname    = \"{0}\"\nversion = \"0.1.0\"\n\n[dependencies]\nlib = {{ path = \"../lib\"{1} }}\n\n"
                                                    "[targets.{0}]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", m, m == "a" ? ", features = [\"extra\"]" : ""));
            fws.write(m + "/src/main.cpp", "import featlib;\nint main() { return v() == 1 ? 0 : 1; }\n");
        }
        const auto d_ws = emit(fws, {"--workspace"}), d_b = emit(fws, {"-p", "b"});
        const auto allowed = differences(d_b, d_ws, "lib", "FEATURE_EXTRA");
        expect(fatal(allowed.empty())) << "D: lib differs by more than the feature that only a asks for:\n" << allowed;
        expect(fatal(!differences(d_b, d_ws, "lib").empty())) << "D: the feature union did not reach lib's compile, so this criterion measured nothing";

        s::project e(tmp / "e", {
            {".mcpp/.xlings/data/xpkgs/local-dev.qdep/1.0.0/mcpp.toml", "[package]\nname    = \"qdep\"\nversion = \"1.0.0\"\n\n[targets.qdep]\nkind = \"lib\"\n"},
            {".mcpp/.xlings/data/xpkgs/local-dev.qdep/1.0.0/src/inner.cppm", "export module qdep.part:inner;\nexport int inner() { return 40; }\n"},
            {".mcpp/.xlings/data/xpkgs/local-dev.qdep/1.0.0/src/part.cppm", "export module qdep.part;\nexport import :inner;\nexport int value() { return inner() + 2; }\n"},
            {"src/main.cpp", "#include <cstdio>\nimport qdep.part;\nint main() { std::printf(\"%d\\n\", value()); }\n"},
            {"mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[indices]\nlocal-dev = {{ path = \"{}\" }}\n\n"
                                      "[dependencies]\n\"local-dev.qdep\" = \"1.0.0\"\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n",
                                      (tmp / "local-index").generic_string())},
        });
        s::project(tmp / "local-index", {{"pkgs/q/qdep.lua", R"(package = {
    spec = "1",
    name = "qdep",
    description = "A package that provides a module",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux = {
            ["1.0.0"] = {
                url = "https://example.invalid/qdep-1.0.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
    },
}
)"}});
        r = e.mcpp({"build"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "E: the first build failed: " << s::text(r);
        e.mcpp({"clean"}, inherited);
        r = e.mcpp({"build"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "E: the build served from the cache failed: " << s::text(r);
        expect(fatal(s::has_line(s::text(r), R"(Cached local-dev\.qdep v1\.0\.0)"))) << "E: the dependency was not served from the cache, so nothing below is exercised: " << s::text(r);
        bool staged = false;
        for (const auto& f : s::find_files(e.at("target"), R"(qdep\.part\..*)"))
            staged = staged || std::regex_search(f.generic_string(), std::regex{R"(\.cache/.*qdep.*/qdep\.part\.[^/]*$)"});
        expect(fatal(staged)) << "E: the staged BMI does not lie below its package's directory";
        expect(fatal(output_of(bin_of(e.at("target"), "app")) == "42")) << "E: the program does not print 42";
    };

    "880 a members executable keeps its own implementation"_test = [] {
        // A member with a shared library and an executable: the executable holds the member's
        // implementation (it runs without the library) while a consumer uses the library; the
        // same for a selected member, a requested artifact, a host tool and a dual-role provider;
        // a static placed in the owner's image keeps its shared dependency.
        const auto tmp = s::case_dir();
        const auto home = s::case_home(s::home::fresh);
        s::project(home, {{"config.toml", std::format("[xlings]\nhome = \"{}\"\n", (s::shared_home() / "registry").generic_string())}});
        const s::options o{.where = s::home::fresh};
        s::project ws(tmp / "ws", {
            {"mcpp.toml", "[workspace]\nmembers = [\"dual\", \"client\", \"support\"]\n"},
            {"support/mcpp.toml", "[package]\nname = \"support\"\nversion = \"0.1.0\"\n\n[targets.support]\nkind = \"shared\"\n"},
            {"support/src/support.cppm", "export module t880_support;\nexport int delta() { return 1; }\n"},
            {"common/mcpp.toml", "[package]\nname = \"common\"\nversion = \"0.1.0\"\n\n[targets.common]\nkind = \"lib\"\n"},
            {"common/src/base.cppm", "export module t880_base;\nexport int base_value() { return 41; }\n"},
            {"dual/mcpp.toml", "[package]\nname = \"dual\"\nversion = \"0.1.0\"\n\n[dependencies]\ncommon = { path = \"../common\" }\nsupport = { path = \"../support\" }\n\n"
                               "[targets.dual_dll]\nkind = \"shared\"\n\n[targets.dual]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
            {"dual/src/dual.cppm", "export module t880_dual;\nexport int interface_value() { return 1; }\nexport int answer();\n"},
            {"dual/src/impl.cpp", "module t880_dual;\nimport t880_base;\nimport t880_support;\nint answer() { return base_value() + delta(); }\n"},
            {"dual/src/main.cpp", "import t880_dual;\nint main() { return answer() == 42 && interface_value() == 1 ? 0 : 1; }\n"},
            {"client/mcpp.toml", "[package]\nname = \"client\"\nversion = \"0.1.0\"\n\n[dependencies]\ndual = { path = \"../dual\" }\n\n[targets.client]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
            {"client/src/main.cpp", "import t880_dual;\nint main() { return answer() == 42 && interface_value() == 1 ? 0 : 1; }\n"},
        });
        auto r = ws.mcpp({"build", "--workspace"}, o);
        expect(fatal(e2e::succeeded(r))) << "the workspace did not build: " << s::text(r);
        const auto dual = bin_of(ws.at("target"), "dual"), client = bin_of(ws.at("target"), "client");
        expect(fatal(dual && client)) << "a member's executable is missing";
        expect(fatal(runs(dual))) << "the owner's executable did not run";
        expect(fatal(runs(client))) << "the shared-library consumer did not run";
        expect(fatal(hide_dual_dll(ws.dir(), ws.at("hidden"), "") > 0)) << "the sibling shared library was not built";
        expect(fatal(runs(dual))) << "the owner's executable depends on its sibling shared library";
        expect(fatal(!runs(client))) << "the external consumer did not use the shared library";
        r = ws.mcpp({"build", "-p", "dual"}, o);
        expect(fatal(e2e::succeeded(r))) << "the selected member did not build: " << s::text(r);
        expect(fatal(runs(bin_of(ws.at("target"), "dual")))) << "the selected member's executable did not run";

        ws.write("dual/mcpp.toml", ws.read("dual/mcpp.toml") + "required_features = [\"exe\"]\n\n[features]\nexe = []\n");
        ws.write("shipped/mcpp.toml", "[package]\nname = \"shipped\"\nversion = \"0.1.0\"\n\n[dependencies]\ndual = { path = \"../dual\", artifacts = [\"dual\"], features = [\"exe\"] }\n"
                                      "support = { path = \"../support\" }\n\n[targets.shipped]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n");
        ws.write("shipped/src/main.cpp", "import t880_support;\nint main() { return delta() == 1 ? 0 : 1; }\n");
        ws.write("mcpp.toml", "[workspace]\nmembers = [\"dual\", \"client\", \"support\", \"shipped\"]\n");
        r = ws.mcpp({"build", "-p", "shipped"}, o);
        expect(fatal(e2e::succeeded(r))) << "the requested artifact did not build: " << s::text(r);
        const auto artifact = ending(ws.at("target"), "/bin/shipped/" + s::exe("dual"));
        expect(fatal(artifact.has_value())) << "the requested artifact was not placed beside its consumer";
        expect(fatal(runs(artifact))) << "the requested artifact did not run";
        expect(fatal(runs(bin_of(ws.at("target"), "shipped")))) << "the artifact's consumer did not run";
        r = ws.mcpp({"build", "--workspace"}, o);
        expect(fatal(e2e::succeeded(r))) << "the workspace with an artifact request did not build: " << s::text(r);
        expect(fatal(runs(artifact))) << "the workspace's requested artifact did not run";
        expect(fatal(runs(dual))) << "the workspace's owner executable did not run";
        hide_dual_dll(ws.dir(), ws.at("hidden"), "artifact-");
        expect(fatal(runs(artifact))) << "the requested artifact depends on its sibling shared library";

        s::project tooldual(tmp / "tooldual", {
            {"src/dual.cppm", ws.read("dual/src/dual.cppm")},
            {"src/main.cpp", ws.read("dual/src/main.cpp")},
            {"src/impl.cpp", "module t880_dual;\nimport t880_base;\nint answer() { return base_value() + 1; }\n"},
            {"mcpp.toml", "[package]\nname = \"tooldual\"\nversion = \"0.1.0\"\n\n[dependencies]\ncommon = { path = \"../ws/common\" }\n\n[features]\nexe = []\n\n"
                          "[targets.dual_dll]\nkind = \"shared\"\n\n[targets.dual]\nkind = \"bin\"\nmain = \"src/main.cpp\"\nrequired_features = [\"exe\"]\n"},
        });
        s::project toolapp(tmp / "toolapp", {
            {"mcpp.toml", "[package]\nname = \"toolapp\"\nversion = \"0.1.0\"\n\n[build-dependencies]\ntooldual = { path = \"../tooldual\", tools = [\"dual\"], features = [\"exe\"] }\n"},
            {"src/main.cpp", "int main() { return 0; }\n"},
            {"build.mcpp", R"(#include <cstdlib>
#include <string>
import mcpp;
int main() {
    const char* tool = mcpp::dep_bin("tooldual", "dual");
    if (!tool || !*tool) return 1;
    return std::system((std::string("\"") + tool + "\"").c_str()) == 0 ? 0 : 1;
}
)"},
        });
        r = toolapp.mcpp({"build"}, o);
        expect(fatal(e2e::succeeded(r))) << "the host tool did not build and run: " << s::text(r);
        expect(fatal(runs(bin_of(toolapp.at("target"), "toolapp")))) << "the host tool's consumer did not run";
        toolapp.write("mcpp.toml", toolapp.read("mcpp.toml") + "\n[dependencies]\ntooldual = { path = \"../tooldual\" }\n");
        toolapp.write("src/main.cpp", tooldual.read("src/main.cpp"));
        r = toolapp.mcpp({"build"}, o);
        expect(fatal(e2e::succeeded(r))) << "the dual-role provider did not build: " << s::text(r);
        expect(fatal(runs(bin_of(toolapp.at("target"), "toolapp")))) << "the dual-role provider's consumer did not run";
        bool library = false;
        for (const auto& f : s::find_files(toolapp.at("target"), ".*dual_dll.*")) library = library || (f.generic_string().contains("/bin/") && is_dual_dll(f));
        expect(fatal(library)) << "the dual-role provider's shared library is missing";

        s::project ws2(tmp / "ws2", {
            {"mcpp.toml", "[workspace]\nmembers = [\"dual\", \"client\", \"support\"]\n"},
            {"support/mcpp.toml", ws.read("support/mcpp.toml")},
            {"support/src/support.cppm", ws.read("support/src/support.cppm")},
            {"common/mcpp.toml", "[package]\nname = \"common\"\nversion = \"0.1.0\"\n\n[dependencies]\nsupport = { path = \"../support\" }\n\n[targets.common]\nkind = \"lib\"\n"},
            {"common/src/base.cppm", "export module t880_base;\nimport t880_support;\nexport int base_value() { return 40 + delta(); }\n"},
            {"dual/mcpp.toml", "[package]\nname = \"dual\"\nversion = \"0.1.0\"\n\n[dependencies]\ncommon = { path = \"../common\" }\n\n"
                               "[targets.dual_dll]\nkind = \"shared\"\n\n[targets.dual]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
            {"dual/src/dual.cppm", ws.read("dual/src/dual.cppm")},
            {"dual/src/impl.cpp", "module t880_dual;\nimport t880_base;\nint answer() { return base_value() + 1; }\n"},
            {"dual/src/main.cpp", ws.read("dual/src/main.cpp")},
            {"client/mcpp.toml", ws.read("client/mcpp.toml")},
            {"client/src/main.cpp", ws.read("client/src/main.cpp")},
        });
        r = ws2.mcpp({"build", "--workspace"}, o);
        expect(fatal(e2e::succeeded(r))) << "a static placed in the owner's image lost its shared dependency: " << s::text(r);
        expect(fatal(runs(bin_of(ws2.at("target"), "dual")))) << "the owner's executable with a placed static did not run";
        expect(fatal(runs(bin_of(ws2.at("target"), "client")))) << "the consumer of an image holding a placed static did not run";
    };

    "912 two configuration groups share the build programs graph"_test[s::unix_shell] = [] {
        // Two members in two configurations whose build programs share a build dependency: one
        // graph of build programs, the library compiled once, each member's value right.
        const auto tmp = s::case_dir();
        s::project(tmp / "shared", {{"mcpp.toml", "[package]\nname = \"shared\"\nversion = \"0.1.0\"\n[targets.shared]\nkind = \"lib\"\n"},
                                    {"src/shared.cppm", "export module shared;\nexport int shared_value() { return 42; }\n"}});
        s::project ws(tmp / "ws", {{"mcpp.toml", "[workspace]\nmembers = [\"a\", \"b\"]\n"}});
        for (std::string m : {"a", "b"}) {
            ws.write(m + "/mcpp.toml", std::format("[package]\nname = \"{0}\"\nversion = \"0.1.0\"\n[build-dependencies]\nshared = {{ path = \"../../shared\" }}\n"
                                                   "[targets.{0}]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", m));
            ws.write(m + "/src/main.cpp", "#include <cstdio>\nint main() { std::printf(\"V=%d\\n\", V); }\n");
            ws.write(m + "/build.mcpp", "import std;\nimport mcpp;\nimport shared;\nint main() { mcpp::cxxflag(std::format(\"-DV={}\", shared_value()).c_str()); }\n");
        }
        ws.write("a/mcpp.toml", ws.read("a/mcpp.toml") + "[profile.dev]\nopt = 1\n");
        auto r = ws.mcpp({"build", "--workspace"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "A: the workspace did not build: " << s::text(r);
        expect(fatal(graph_dirs(ws.dir()).size() == 2u)) << "A: the members were not two configuration groups";
        for (std::string m : {"a", "b"})
            expect(fatal(s::last_starting(ws.mcpp({"run", "-p", m}, inherited).out, "V=") == "V=42")) << "A: " << m << "'s value";
        std::size_t planned = 0;
        for (const auto& e : fs::directory_iterator(ws.at("target/.build-mcpp/host")))
            if (e.is_directory() && e.path().filename() != "digests") ++planned;
        expect(fatal(planned == 1u)) << "B: the groups' build programs were planned in " << planned << " directories";
        std::size_t compiles = 0;
        for (const auto& log : s::find_files(ws.at("target/.build-mcpp/host"), R"(\.ninja_log)"))
            compiles += matching(s::ninja_log_outputs(log.parent_path()), std::regex{R"(shared\.m\.o$)"});
        expect(fatal(compiles == 1u)) << "B: the library was compiled " << compiles << " times";
    };
}};

}  // namespace

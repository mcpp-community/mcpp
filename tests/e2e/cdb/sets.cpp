// The sets of a workspace's database and of the build programs, and what emit watches (formerly
// tests/e2e/840, 913, 921).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

const s::options at_root{.env = {{"MCPP_COMPILE_COMMANDS", "compile_commands.json"}}};

std::string slash(std::string p) {
    std::ranges::replace(p, '\\', '/');
    return p;
}

std::string prefix_of(const std::string& n) { return n.contains('/') ? n.substr(0, n.find('/')) : std::string(); }
std::string base_of(const std::string& n) { return n.substr(n.rfind('/') + 1); }

E2E sets{"sets", [] {
    "840 a workspace database describes each package once"_test[tag{"requires:python3"}] = [] {
        // A: one configuration: each set and each source once, names unprefixed; B: two
        // configurations: names prefixed by their fingerprint, core once in each, the root
        // database one command per file and output; C: a member that fails is one error, the
        // others one configuration still.
        auto member = [](const s::project& ws, const std::string& name, const std::string& standard) {
            ws.write(name + "/mcpp.toml", std::format("[package]\nname = \"{0}\"\nversion = \"0.1.0\"\n{1}\n[dependencies]\ncore = {{ path = \"../core\" }}\n\n"
                                                      "[targets.{0}]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n",
                                                      name, standard.empty() ? "" : "standard = \"" + standard + "\"\n"));
            ws.write(name + "/src/main.cpp", "import db_core;\nint main() { return core_v() == 1 ? 0 : 1; }\n");
        };
        auto core = [](const s::project& ws) {
            ws.write("core/mcpp.toml", "[package]\nname = \"core\"\nversion = \"0.1.0\"\n\n[targets.core]\nkind = \"lib\"\n");
            ws.write("core/src/core.cppm", "export module db_core;\nexport int core_v() { return 1; }\n");
        };
        auto check = [](const s::project& ws, std::string_view part) {
            const auto d = s::json_file(ws.at("db.json"));
            const auto& sets = d["data"]["database"]["sets"];
            std::vector<std::string> names;
            std::map<std::pair<std::string, std::string>, int> units;
            std::map<std::string, std::string> kinds;
            for (const auto& st : sets) {
                const std::string n = st["name"];
                names.push_back(n);
                for (const auto& t : st["translation-units"]) ++units[{prefix_of(n), t["source"].get<std::string>()}];
                kinds[base_of(n)] = st["ide"]["kind"];
            }
            expect(fatal(std::set(names.begin(), names.end()).size() == names.size())) << part << ": a set is described twice";
            expect(fatal(std::ranges::all_of(units, [](const auto& u) { return u.second == 1; }))) << part << ": a source is described twice in one configuration";
            return std::tuple{d, names, kinds};
        };
        auto root_db = [](const s::project& ws, int cores, std::vector<std::string> want) {
            std::map<std::pair<std::string, std::string>, int> pairs;
            std::vector<std::string> files;
            for (const auto& e : s::json_file(ws.at("compile_commands.json"))) {
                const auto f = slash(e["file"]);
                ++pairs[{f, slash(e.value("output", std::string{}))}];
                files.push_back(f);
            }
            expect(fatal(std::ranges::all_of(pairs, [](const auto& p) { return p.second == 1; }))) << "a file and output have two commands";
            const auto n = std::ranges::count_if(files, [](const std::string& f) { return f.ends_with("core/src/core.cppm"); });
            expect(fatal(n == cores)) << "core has " << n << " commands, expected one per configuration (" << cores << ")";
            for (const auto& w : want)
                expect(fatal(std::ranges::any_of(files, [&](const std::string& f) { return f.ends_with(w); }))) << w << " is not described";
        };
        const auto tmp = s::case_dir();
        s::project a(tmp / "a", {{"mcpp.toml", "[workspace]\nmembers = [\"core\", \"cli\", \"gui\"]\n"},
                                 {"cli/tests/check.cpp", "import db_core;\nint main() { return core_v() == 1 ? 0 : 1; }\n"}});
        core(a);
        member(a, "cli", "");
        member(a, "gui", "");
        auto r = a.mcpp({"emit", "build-database", "--format", "json", "-o", "db.json"}, at_root);
        expect(fatal(e2e::succeeded(r))) << "A: emit failed: " << s::text(r);
        {
            const auto [d, names, kinds] = check(a, "A");
            expect(fatal(std::ranges::none_of(names, [](const std::string& n) { return n.contains('/'); }))) << "A: a set of one configuration is prefixed";
            expect(fatal(std::ranges::count(names, "core") == 1)) << "A: core";
            expect(fatal(kinds.contains("cli") && kinds.at("cli") == "executable" && kinds.contains("cli:test") && kinds.at("cli:test") == "test")) << "A: kinds";
        }
        r = a.mcpp({"build", "--configure-only"}, at_root);
        expect(fatal(e2e::succeeded(r))) << "A: --configure-only failed: " << s::text(r);
        root_db(a, 1, {"cli/src/main.cpp"});

        s::project b(tmp / "b", {{"mcpp.toml", "[workspace]\nmembers = [\"core\", \"cli\", \"modern\"]\n"}});
        core(b);
        member(b, "cli", "");
        member(b, "modern", "c++26");
        r = b.mcpp({"emit", "build-database", "--format", "json", "-o", "db.json"}, at_root);
        expect(fatal(e2e::succeeded(r))) << "B: emit failed: " << s::text(r);
        {
            const auto [d, names, kinds] = check(b, "B");
            expect(fatal(std::ranges::all_of(names, [](const std::string& n) { return std::regex_match(n, std::regex{"[0-9a-f]{16}/.+"}); }))) << "B: names";
            std::set<std::string> prefixes;
            int cores = 0;
            for (const auto& n : names)
                if (base_of(n) == "core") { ++cores; prefixes.insert(prefix_of(n)); }
            expect(fatal(cores == 2 && prefixes.size() == 2u)) << "B: core per configuration";
        }
        for (const auto& cmd : std::vector<std::vector<std::string>>{{"build", "--configure-only"}, {"build", "--workspace"}}) {
            b.remove("compile_commands.json");
            r = b.mcpp(cmd, at_root);
            expect(fatal(e2e::succeeded(r))) << "B: " << r.line << " failed: " << s::text(r);
            root_db(b, 2, {"cli/src/main.cpp", "modern/src/main.cpp"});
        }

        s::project c(tmp / "c", {{"mcpp.toml", "[workspace]\nmembers = [\"core\", \"cli\", \"gui\", \"broken\"]\n"},
                                 {"broken/mcpp.toml", "[package]\nname = \"broken\"\nversion = \"0.1.0\"\n\n[dependencies]\nmissing = { path = \"../missing\" }\n"},
                                 {"broken/src/main.cpp", "int main() { return 0; }\n"}});
        core(c);
        member(c, "cli", "");
        member(c, "gui", "");
        r = c.mcpp({"emit", "build-database", "--format", "json", "-o", "db.json"}, at_root);
        expect(fatal(e2e::exited(r, 1))) << "C: emit exited " << r.status << ", expected 1: " << s::text(r);
        const auto [d, names, kinds] = check(c, "C");
        std::vector<nlohmann::json> errors;
        for (const auto& x : d["diagnostics"]) if (x["severity"] == "error") errors.push_back(x);
        expect(fatal(errors.size() == 1u && slash(errors[0].value("path", std::string{})) == "broken/mcpp.toml")) << "C: diagnostics " << d["diagnostics"].dump();
        std::set<std::string> bases;
        for (const auto& n : names) bases.insert(base_of(n));
        expect(fatal(bases.contains("core") && bases.contains("cli") && bases.contains("gui"))) << "C: the other members";
        // Planned one by one, the members are still one configuration.
        expect(fatal(std::ranges::none_of(names, [](const std::string& n) { return n.contains('/'); }))) << "C: the fallback split the configuration";
    };
    "913 the build database describes the build programs"_test[tag{"requires:python3"}] = [] {
        // A: the build programs' sets (the program, its build dependency, the mcpp core module
        // and the host std) are of the build-program configuration, beside the dev one; B: the
        // program's unit has the arguments the build programs' graph compiled it with; C: no
        // compile-commands rendering carries them; D: the core set's sources are the engine's
        // modules `self env` names.
        const s::options o{.where = s::home::inherited, .env = {{"MCPP_COMPILE_COMMANDS", "compile_commands.json"}}};
        const auto tmp = s::case_dir();
        s::project(tmp / "lib", {{"mcpp.toml", "[package]\nname = \"lib\"\nversion = \"0.1.0\"\n[targets.lib]\nkind = \"lib\"\n"},
                                 {"src/lib.cppm", "export module lib;\nexport int lib_value() { return 42; }\n"}});
        s::project app(tmp / "app", {{"mcpp.toml", "[package]\nname = \"app\"\nversion = \"0.1.0\"\n[build-dependencies]\nlib = { path = \"../lib\" }\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                                     {"src/main.cpp", "#include <cstdio>\nint main() { std::printf(\"V=%d\\n\", V); }\n"},
                                     {"build.mcpp", "import std;\nimport mcpp;\nimport lib;\nint main() { mcpp::cxxflag(std::format(\"-DV={}\", lib_value()).c_str()); }\n"}});
        auto r = app.mcpp({"build"}, o);
        expect(fatal(e2e::succeeded(r))) << "the project did not build: " << s::text(r);
        const auto db = app.mcpp({"emit", "build-database", "--format", "json"}, o);
        expect(fatal(e2e::succeeded(db))) << "emit build-database failed: " << db.err;
        const auto cc = app.mcpp({"emit", "build-database", "--spec", "compile-commands"}, o);
        expect(fatal(e2e::succeeded(cc))) << "--spec compile-commands failed: " << cc.err;
        const auto env = app.mcpp({"self", "env", "--format", "json"}, o);
        expect(fatal(e2e::succeeded(env))) << "mcpp self env failed: " << env.err;
        std::optional<fs::path> host_cc;
        for (const auto& f : s::find_files(app.at("target/.build-mcpp/host"), R"(compile_commands\.json)"))
            if (std::ranges::distance(fs::relative(f, app.at("target/.build-mcpp/host"))) <= 2) { host_cc = f; break; }
        expect(fatal(host_cc.has_value())) << "the build programs' graph wrote no compile_commands.json";
        std::map<std::string, nlohmann::json> sets;
        for (const auto& st : s::json(db.out)["data"]["database"]["sets"]) sets[st["name"]] = st;
        for (const auto& [name, kind] : std::vector<std::pair<std::string, std::string>>{
                 {"app:build", "executable"}, {"lib:host", "library"}, {"mcpp:core", "library"}, {"mcpp:std:host", ""}}) {
            std::string found;
            for (const auto& [n, _] : sets) if (found.empty() && (n == name || n.ends_with("." + name))) found = n;
            expect(fatal(!found.empty())) << "A: no set " << name;
            expect(fatal(sets[found]["ide"]["configuration"] == "build-program")) << "A: " << name << " " << sets[found]["ide"].dump();
            if (!kind.empty()) expect(fatal(sets[found]["ide"]["kind"] == kind)) << "A: " << name << " " << sets[found]["ide"].dump();
        }
        expect(fatal(sets["app:build"]["family-name"] == "app")) << "A: family-name";
        expect(fatal(sets.contains("app") && sets["app"]["ide"]["configuration"] == "dev")) << "A: the dev set";
        nlohmann::json unit, built;
        for (const auto& u : sets["app:build"]["translation-units"]) if (unit.is_null() && u["source"].get<std::string>().ends_with("build.mcpp")) unit = u;
        for (const auto& e : s::json_file(*host_cc)) if (built.is_null() && e["file"].get<std::string>().ends_with("build.mcpp")) built = e;
        // One separator, and the directory above the graph named alike.
        auto norm = [](const nlohmann::json& args, const std::string& wd) {
            const auto w = slash(wd);
            const auto root = w.substr(0, w.find("/target/.build-mcpp/host/"));
            std::vector<std::string> out;
            for (const auto& a : args) {
                auto x = slash(a);
                for (auto at = x.find(root + "/target/"); at != std::string::npos; at = x.find(root + "/target/")) x.replace(at, root.size() + 8, "<root>/target/");
                out.push_back(x);
            }
            return out;
        };
        expect(fatal(norm(unit["arguments"], unit["work-directory"]) == norm(built["arguments"], built["directory"]))) << "B: the program's arguments differ";
        for (const auto& entries : {s::json(cc.out), s::json_file(app.at("compile_commands.json"))}) {
            std::vector<std::string> names;
            for (const auto& e : entries) names.push_back(fs::path(slash(e["file"])).filename().string());
            expect(fatal(std::ranges::find(names, "main.cpp") != names.end())) << "C: no main.cpp";
            for (std::string_view unwanted : {"build.mcpp", "lib.cppm", "mcpp.cppm", "mcpp_core.cppm"})
                expect(fatal(std::ranges::find(names, unwanted) == names.end())) << "C: " << unwanted;
        }
        const fs::path engine = s::json(env.out)["data"]["engineModules"].get<std::string>();
        expect(fatal(fs::is_regular_file(engine / "mcpp.cppm"))) << "D: " << engine;
        for (const auto& u : sets["mcpp:core"]["translation-units"])
            expect(fatal(fs::weakly_canonical(fs::path(u["source"].get<std::string>()).parent_path()) == fs::weakly_canonical(engine))) << "D: " << u["source"].dump();
    };

    "921 emit watches inputs not host outputs"_test[tag{"requires:python3"}] = [] {
        // emit watches the project's and an editable build dependency's inputs, not the build
        // programs' outputs or an index payload; a repeated emit leaves the host lock it wrote
        // as it was (time and text) and the fingerprint the same; an edit of the editable
        // dependency changes it.
        const s::options o{.where = s::home::inherited};
        const auto home = s::case_home(s::home::inherited);
        const auto tmp = s::case_dir();
        constexpr std::string_view zero = "0000000000000000000000000000000000000000000000000000000000000000";
        s::project(tmp / "index", {{"pkgs/f/fixture.lockdep.lua", std::format(R"lua(package = {{
    spec = "1", namespace = "fixture", name = "fixture.lockdep",
    description = "Host lock fixture", licenses = {{"MIT"}}, type = "package",
    xpm = {{
        linux = {{["1.0.0"] = {{url = "https://example.invalid/lockdep.tar.gz", sha256 = "{0}"}}}},
        windows = {{["1.0.0"] = {{url = "https://example.invalid/lockdep.zip", sha256 = "{0}"}}}},
        macosx = {{["1.0.0"] = {{url = "https://example.invalid/lockdep.tar.gz", sha256 = "{0}"}}}},
    }},
    mcpp = {{ language = "c++23", import_std = false,
             sources = {{"src/dep.cpp"}}, targets = {{lockdep = {{kind = "lib"}}}}, deps = {{}} }},
}}
)lua", zero)}});
        for (const auto& base : {tmp / "app/.mcpp/.xlings/data/xpkgs", home / "xpkgs"})
            s::project(base / "fixture-x-lockdep/1.0.0", {{"src/dep.cpp", "int lockdep_value() { return 42; }\n"}});
        s::project editable(tmp / "editable", {{"mcpp.toml", "[package]\nname = \"editable\"\nversion = \"0.1.0\"\n[build]\nsources = [\"src/*.cpp\"]\n[targets.editable]\nkind = \"lib\"\n"},
                                               {"src/lib.cpp", "int editable_value() { return 7; }\n"}});
        s::project app(tmp / "app", {{"mcpp.toml", std::format("[package]\nname = \"watch-probe\"\nversion = \"0.1.0\"\n[indices]\nfixture = {{ path = \"{}\" }}\n"
                                                               "[build-dependencies.fixture]\nlockdep = \"1.0.0\"\n[build-dependencies]\neditable = {{ path = \"../editable\" }}\n"
                                                               "[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", (tmp / "index").generic_string())},
                                     {"src/main.cpp", "int main() { return 0; }\n"},
                                     {"build.mcpp", "#include <cstdio>\nint main() { std::puts(\"cxxflag=-DHOST_PROBE=1\"); }\n"}});
        const auto first = app.mcpp({"emit", "build-database", "--format", "json"}, o);
        expect(fatal(e2e::succeeded(first))) << "first emit: " << first.err;
        const auto locks = s::glob(home, "cache/build-database/*/target/.build-mcpp/host/*/mcpp.lock");
        expect(fatal(!locks.empty())) << "host fixture must produce a nonempty lock";
        std::map<fs::path, std::pair<fs::file_time_type, std::string>> snapshot;
        // os.utime(ns=10**18): a time far from now, which a rewrite would replace.
        const auto stamp = std::chrono::time_point_cast<fs::file_time_type::duration>(
            fs::file_time_type::clock::from_sys(std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::nanoseconds{1000000000000000000LL}}));
        for (const auto& l : locks) {
            expect(fatal(e2e::contains(s::read_file(l), "[package."))) << "an empty lock " << l;
            fs::last_write_time(l, stamp);
            snapshot[l] = {fs::last_write_time(l), s::read_file(l)};
        }
        const auto second = app.mcpp({"emit", "build-database", "--format", "json"}, o);
        expect(fatal(e2e::succeeded(second))) << "second emit: " << second.err;
        const auto d1 = s::json(first.out)["data"], d2 = s::json(second.out)["data"];
        expect(fatal(d1["inputs-fingerprint"] == d2["inputs-fingerprint"])) << "the fingerprint moved";
        std::vector<std::string> watch;
        for (const auto& w : d2["watch"]) watch.push_back(slash(w));
        for (std::string_view w : {"mcpp.lock", "mcpp.toml", "build.mcpp"}) expect(fatal(std::ranges::find(watch, w) != watch.end())) << "watch lacks " << w;
        expect(fatal(std::ranges::any_of(watch, [](const std::string& w) { return w.ends_with("editable/mcpp.toml"); }))) << "watch lacks editable/mcpp.toml";
        expect(fatal(std::ranges::any_of(watch, [](const std::string& w) { return w.ends_with("editable/src/lib.cpp"); }))) << "watch lacks editable/src/lib.cpp";
        expect(fatal(std::ranges::none_of(watch, [](const std::string& w) { return w.contains("/.build-mcpp/host/") || w.contains("fixture-x-lockdep/"); }))) << "watch names an output or a payload";
        for (const auto& [l, before] : snapshot)
            expect(fatal(fs::last_write_time(l) == before.first && s::read_file(l) == before.second)) << "the host lock was written again: " << l;
        editable.write("src/lib.cpp", editable.read("src/lib.cpp") + "\n// editable input changed\n");
        const auto third = app.mcpp({"emit", "build-database", "--format", "json"}, o);
        expect(fatal(e2e::succeeded(third))) << "changed editable input: " << third.err;
        expect(fatal(d2["inputs-fingerprint"] != s::json(third.out)["data"]["inputs-fingerprint"])) << "editable host input did not invalidate fingerprint";
    };
}};


}  // namespace

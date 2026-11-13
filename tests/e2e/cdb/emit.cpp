// `emit build-database`: the S1 document and its envelope, what it leaves untouched, its sets,
// roles and arguments, its compile-commands rendering, its refusals, its watch list and
// fingerprint, a planning pass that compiles nothing, the words of a flag, what a rule generates,
// and the private work directory (formerly tests/e2e/688, 736, 815, 817).
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

// tree_digest: every directory and file under `root` with the SHA-256 of its bytes.
std::string tree_digest(const fs::path& root) {
    std::string all;
    for (const auto& rel : s::listing(root)) {
        const auto p = root / rel;
        all += fs::is_directory(p) ? "D " + rel + "\n" : "F " + rel + " " + s::sha256(s::read_file(p)) + "\n";
    }
    return s::sha256(all);
}

std::string basename(const std::string& p) { return fs::path(slash(p)).filename().string(); }

bool same(const std::string& word, const std::string& path, const std::string& cwd) {
    return (fs::path(cwd) / word).lexically_normal() == fs::path(path).lexically_normal();
}

// Files under `root` (not below `prune`) that a compile or a link writes.
std::vector<fs::path> compiled(const fs::path& root, const fs::path& prune = {}) {
    std::vector<fs::path> out;
    for (const auto& f : s::find_files(root, R"(.*\.(o|obj|gcm|pcm|ifc)|mcpp-clean-link\.specs)"))
        if (prune.empty() || !f.generic_string().starts_with(prune.generic_string() + "/")) out.push_back(f);
    return out;
}

E2E emit{"emit", [] {
    "688 emit build database"_test[tag{"requires:python3"}] = [] {
        const auto tmp = s::case_dir() / "measured";
        const auto out = s::case_dir() / "out";
        fs::create_directories(out);
        s::project(tmp / "devkit", {{"mcpp.toml", "[package]\nname = \"devkit\"\nversion = \"0.1.0\"\n\n[build]\ninclude_dirs = [\"include\"]\n"},
                                    {"include/devkit.hpp", "#define DEVKIT_MARKER 1\n"}});
        s::project hello(tmp / "hello", {
            {"mcpp.toml", "[package]\nname = \"hello\"\nversion = \"0.1.0\"\nstandard = \"c++23\"\n\n[dev-dependencies]\ndevkit = { path = \"../devkit\" }\n"},
            {"src/greet.cppm", "export module hello.greet;\nexport import :detail;\nimport std;\nexport std::string greet();\n"},
            {"src/detail.cppm", "export module hello.greet:detail;\nexport int answer() { return 42; }\n"},
            {"src/impl.cppm", "module hello.greet:impl;\nint hidden() { return 7; }\n"},
            {"src/greet_impl.cpp", "module hello.greet;\nimport :impl;\nstd::string greet() { return \"hi\"; }\n"},
            {"src/main.cpp", "import hello.greet;\nimport std;\nint main() { std::println(\"{}\", greet()); }\n"},
            {"tests/test_smoke.cpp", "#include <devkit.hpp>\n/*\nimport in.comment;\n*/\nconst char* text = R\"x(\nimport in.raw;\n)x\";\nint main() { return DEVKIT_MARKER - 1; }\n"},
        });
        // A: the envelope and the protocol; B: the project tree is unchanged.
        const auto before = tree_digest(tmp);
        const auto env = hello.mcpp({"emit", "build-database", "--format", "json"}, at_root);
        expect(fatal(e2e::succeeded(env))) << "A: emit build-database exited non-zero: " << env.err;
        const auto after = tree_digest(tmp);
        const auto e = s::json(env.out);
        expect(fatal(e["kind"] == "mcpp.build-database" && e["kindVersion"] == 1)) << "A: the envelope: " << e["kind"].dump();
        const auto effects = e["effects"].get<std::vector<std::string>>();
        expect(fatal(std::ranges::find(effects, "write-project") == effects.end() && std::ranges::find(effects, "read-project") != effects.end())) << "A: effects " << e["effects"].dump();
        const auto& d = e["data"];
        expect(fatal(d["spec"] == nlohmann::json{{"name", "s1"}, {"version", "0.3.0"}})) << "A: spec " << d["spec"].dump();
        expect(fatal(d["database"]["ide"]["profile-version"] == "0.3.0")) << "A: profile-version";
        expect(fatal(d["inputs-fingerprint"].get<std::string>().starts_with("fnv1a:"))) << "A: fingerprint " << d["inputs-fingerprint"].dump();
        const auto proto = s::json(hello.mcpp({"--protocol-version"}).out);
        expect(fatal(proto["kinds"]["mcpp.build-database"] == 1)) << "A: the protocol document";
        const auto fx = proto["commands"]["emit build-database"]["effects"].get<std::vector<std::string>>();
        expect(fatal(std::ranges::find(fx, "write-project") == fx.end() && std::ranges::find(fx, "exec-build-script") != fx.end())) << "A: the protocol's effects";
        expect(fatal(before == after)) << "B: the project tree changed";

        // C: the document validates against S1, by tests/e2e's schema subset checker.
        const auto db = d["database"];
        std::ofstream(out / "db.json") << db.dump();
        const auto python = s::on_path("python3");
        expect(fatal(python.has_value())) << "python3 runs the schema check";
        auto v = s::run(*python, {"-I", (s::repository() / "tests/e2e/_json_schema_subset.py").string(),
                                  (s::repository() / "tests/e2e/fixtures/s1/s1-build-database.schema.json").string(), (out / "db.json").string()});
        expect(fatal(e2e::succeeded(v))) << "C: the document does not validate against S1: " << s::text(v);

        // D/E: sets, visibility, roles, provides and requires, the std unit.
        std::map<std::string, nlohmann::json> sets;
        for (const auto& st : db["sets"]) sets[st["name"].get<std::string>()] = st;
        expect(fatal(sets.size() == 3u && sets.contains("hello") && sets.contains("hello:test") && sets.contains("mcpp:std"))) << "D: sets " << db["sets"].size();
        for (const auto& [name, st] : sets) {
            auto visible = st["visible-sets"].get<std::vector<std::string>>();
            std::ranges::sort(visible);
            std::vector<std::string> others;
            for (const auto& [n, _] : sets) if (n != name) others.push_back(n);
            expect(fatal(visible == others)) << "D: visible-sets of " << name;
            expect(fatal(db["ide"]["toolchains"].contains(st["ide"]["toolchain"].get<std::string>()))) << "D: toolchain of " << name;
        }
        const std::map<std::string, std::string> want{{"greet.cppm", "module-interface"}, {"detail.cppm", "module-partition-interface"},
                                                      {"impl.cppm", "module-partition-implementation"}, {"greet_impl.cpp", "module-implementation"},
                                                      {"main.cpp", "non-module"}};
        std::map<std::string, std::string> roles;
        for (const auto& u : sets["hello"]["translation-units"]) roles[basename(u["source"])] = u["ide"]["role"];
        expect(fatal(roles == want)) << "E: roles";
        expect(fatal(sets["hello"]["ide"]["kind"] == "executable")) << "E: hello's kind";
        std::vector<std::string> tests;
        for (const auto& u : sets["hello:test"]["translation-units"]) tests.push_back(basename(u["source"]));
        expect(fatal(tests == std::vector<std::string>{"test_smoke.cpp"})) << "E: the test set";
        expect(fatal(sets["hello:test"]["ide"]["kind"] == "test")) << "E: the test set's kind";
        nlohmann::json greet;
        for (const auto& u : sets["hello"]["translation-units"]) if (u["source"].get<std::string>().ends_with("greet.cppm")) greet = u;
        expect(fatal(greet["provides"] == nlohmann::json{{"hello.greet", ""}})) << "E: provides " << greet["provides"].dump();
        expect(fatal(greet["requires"] == nlohmann::json::array({"hello.greet:detail", "std"}))) << "E: requires " << greet["requires"].dump();
        std::vector<nlohmann::json> stds;
        for (const auto& u : sets["mcpp:std"]["translation-units"]) if (u["provides"].contains("std")) stds.push_back(u);
        expect(fatal(stds.size() == 1u)) << "E: std units " << stds.size();
        const auto std_src = stds[0]["source"].get<std::string>();
        expect(fatal(fs::is_regular_file(std_src))) << "E: " << std_src;
        const auto driver = db["ide"]["toolchains"].begin().value()["driver"];
        expect(fatal(stds[0]["arguments"][0] == driver)) << "E: the std unit's driver";
        bool named = false;
        for (const auto& a : stds[0]["arguments"]) named = named || a == std_src || a.get<std::string>().ends_with(basename(std_src));
        expect(fatal(named)) << "E: the std unit's source is not in its arguments";

        // J/K: arguments decompose into driver, baseline, local and the compile's tail; config
        // files exist; the test's commented and raw-string imports are not scanned.
        bool bypassed = false;
        for (const auto& st : db["sets"]) {
            const auto base = st["baseline-arguments"].get<std::vector<std::string>>();
            for (const auto& u : st["translation-units"]) {
                const auto a = u["arguments"].get<std::vector<std::string>>();
                auto head = std::vector<std::string>{a[0]};
                head.insert(head.end(), base.begin(), base.end());
                for (const auto& l : u["local-arguments"]) head.push_back(l);
                expect(fatal(a.size() >= head.size() && std::equal(head.begin(), head.end(), a.begin()))) << "J: " << u["source"].dump();
                const std::vector<std::string> tail(a.begin() + static_cast<std::ptrdiff_t>(head.size()), a.end());
                const auto cwd = u["work-directory"].get<std::string>();
                expect(fatal(tail.empty() || (tail.size() == 4 && tail[0] == "-c" && tail[2] == "-o" && same(tail[1], u["source"], cwd) && same(tail[3], u["object"], cwd))))
                    << "J: the tail of " << u["source"].dump();
                expect(fatal(u["private"] == false)) << "J: private " << u["source"].dump();
                bypassed = bypassed || std::ranges::find(a, "--no-default-config") != a.end();
            }
        }
        const auto hb = sets["hello"]["baseline-arguments"].get<std::vector<std::string>>();
        expect(fatal(std::ranges::find(hb, "-std=c++23") != hb.end() || std::ranges::find(hb, "/std:c++latest") != hb.end())) << "J: hello's standard";
        for (const auto& item : db["ide"]["toolchains"].items())
            for (const auto& f : item.value()["config-files"]) {
                const auto tid = item.key();
                const auto p = f.get<std::string>();
                expect(fatal(fs::path(p).is_absolute() && fs::is_regular_file(p))) << "K: " << tid << " " << p;
                expect(fatal(!(bypassed && p.ends_with(".cfg")))) << "K: " << tid << " " << p;
            }
        expect(fatal(sets["hello:test"]["translation-units"][0]["requires"].empty())) << "K: the test scan read a commented import";

        // F: --spec compile-commands renders the same units.
        const auto cc = hello.mcpp({"emit", "build-database", "--spec", "compile-commands"}, at_root);
        expect(fatal(e2e::succeeded(cc))) << "F: --spec compile-commands exited non-zero: " << cc.err;
        std::map<std::string, nlohmann::json> s1units, cdb;
        for (const auto& st : db["sets"]) for (const auto& u : st["translation-units"]) s1units[u["source"]] = u["arguments"];
        const auto ccj = s::json(cc.out);
        for (const auto& en : ccj) cdb[en["file"]] = en["arguments"];
        expect(fatal(s1units == cdb)) << "F: the two renderings disagree";

        // G: refusals.
        auto g = hello.mcpp({"emit", "build-database", "--format", "ndjson"}, at_root);
        expect(fatal(e2e::exited(g, 2) && g.out.empty())) << "G: --format ndjson rc=" << g.status << ": " << s::text(g);
        g = hello.mcpp({"emit", "build-database", "--spec", "nope"}, at_root);
        expect(fatal(e2e::exited(g, 2) && g.out.empty())) << "G: --spec nope rc=" << g.status << ": " << s::text(g);
        fs::create_directories(out / "empty");
        g = s::mcpp({"emit", "build-database", "--format", "json"}, out / "empty", at_root);
        expect(fatal(e2e::exited(g, 1))) << "G: outside a project rc=" << g.status << ": " << s::text(g);
        const auto ge = s::json(g.out);
        expect(fatal(!ge.contains("data") && ge["diagnostics"][0]["code"] == "MCPP_BUILD_DATABASE_NO_PROJECT" && ge["diagnostics"][0]["severity"] == "error"))
            << "G: the failure envelope: " << g.out;

        // H: the bare document, and -o.
        const auto bare = hello.mcpp({"emit", "build-database"}, at_root);
        expect(fatal(e2e::succeeded(bare))) << "H: bare document: " << bare.err;
        const auto o = hello.mcpp({"emit", "build-database", "-o", (out / "file.json").string()}, at_root);
        expect(fatal(e2e::succeeded(o))) << "H: -o: " << o.err;
        expect(fatal(o.out.empty())) << "H: -o also wrote to stdout";
        const auto a = s::json(bare.out), b = s::json_file(out / "file.json");
        expect(fatal(a["version"] == 1 && a.contains("sets") && a == b)) << "H: bare and -o documents";

        // I: the watch list and a fingerprint that follows an edit.
        const auto env2 = s::json(hello.mcpp({"emit", "build-database", "--format", "json"}, at_root).out)["data"];
        hello.write("src/detail.cppm", hello.read("src/detail.cppm") + "\n// edited\n");
        const auto env3 = s::json(hello.mcpp({"emit", "build-database", "--format", "json"}, at_root).out)["data"];
        const auto w = d["watch"].get<std::vector<std::string>>();
        for (std::string_view entry : {"mcpp.toml", "mcpp.lock", "tests/**/*.cpp"}) expect(fatal(std::ranges::find(w, entry) != w.end())) << "I: watch lacks " << entry;
        expect(fatal(std::ranges::any_of(w, [](const std::string& x) { return x.starts_with("src/**/"); }))) << "I: watch lacks src/**/";
        expect(fatal(std::ranges::any_of(w, [](const std::string& x) { return slash(x).ends_with("devkit/mcpp.toml"); }))) << "I: watch lacks the dev-dependency's manifest";
        expect(fatal(d["inputs-fingerprint"] == env2["inputs-fingerprint"])) << "I: the fingerprint is not stable";
        expect(fatal(d["inputs-fingerprint"] != env3["inputs-fingerprint"])) << "I: the fingerprint did not follow an edit";

        // L: the planning pass writes its resolution, no compile or link outputs, and compiles
        // nothing into a home whose std cache is cold (configure-only, the control, does).
        std::string object;
        for (const auto& u : sets["hello"]["translation-units"]) { object = u["object"]; break; }
        const auto work = fs::path(object.substr(0, slash(object).find("/target/")));
        expect(fatal(!s::find_files(work, R"(resolution\.json)").empty())) << "L: no resolution record under the work directory " << work;
        expect(fatal(compiled(work).empty())) << "L: the planning pass wrote compile or link outputs: " << compiled(work).front();
        const auto cold = s::case_home(s::home::inherited);
        const s::options cold_home{.where = s::home::inherited, .env = {{"MCPP_OFFLINE", "1"}, {"MCPP_COMPILE_COMMANDS", "compile_commands.json"}}};
        const auto ce = hello.mcpp({"emit", "build-database", "--format", "json"}, cold_home);
        expect(fatal(e2e::succeeded(ce))) << "L: emit in a home with a cold std cache: " << ce.err;
        std::size_t cold_std = 0;
        for (const auto& st : s::json(ce.out)["data"]["database"]["sets"])
            if (st["name"] == "mcpp:std") for (const auto& u : st["translation-units"]) cold_std += u["provides"].contains("std") ? 1 : 0;
        expect(fatal(cold_std == 1u)) << "L: the cold-home document";
        expect(fatal(compiled(cold, cold / "registry").empty())) << "L: emit compiled into a cold home";
        if (fs::exists(work)) fs::remove_all(work);

        // F: configure-only writes the same entries, mapped from its work directory.
        auto r = hello.mcpp({"build", "--configure-only"}, at_root);
        expect(fatal(e2e::succeeded(r))) << "F: configure-only: " << s::text(r);
        expect(fatal(tree_digest(tmp) != before)) << "B control: configure-only did not change the tree";
        expect(fatal(hello.exists("compile_commands.json"))) << "F: configure-only wrote no compile_commands.json";
        const auto written = s::json_file(hello.at("compile_commands.json"));
        auto write_root = [](const nlohmann::json& entries) {
            for (const auto& en : entries) {
                const auto sl = slash(en["output"]);
                if (sl.contains("/target/")) return sl.substr(0, sl.find("/target/"));
            }
            return std::string();
        };
        const auto wr = write_root(ccj), pr = write_root(written);
        expect(fatal(!wr.empty() && !pr.empty())) << "F: no project entry (an /target/ output) found";
        std::map<std::string, std::vector<std::string>> em, wm;
        for (const auto& en : ccj) {
            std::vector<std::string> args;
            for (const auto& x : en["arguments"]) {
                auto sl = slash(x);
                for (auto at = sl.find(wr); at != std::string::npos; at = sl.find(wr, at + pr.size())) sl.replace(at, wr.size(), pr);
                args.push_back(sl);
            }
            em[slash(en["file"])] = args;
        }
        for (const auto& en : written) {
            std::vector<std::string> args;
            for (const auto& x : en["arguments"]) args.push_back(slash(x));
            wm[slash(en["file"])] = args;
        }
        for (const auto& [f, args] : em) {
            expect(fatal(wm.contains(f))) << "F: configure-only lacks " << f;
            expect(fatal(wm[f] == args)) << "F: compile-commands differs from configure-only for " << f;
        }
        const auto copy = out / "copy";
        fs::create_directories(copy);
        fs::copy(tmp / "hello", copy / "hello", fs::copy_options::recursive);
        fs::copy(tmp / "devkit", copy / "devkit", fs::copy_options::recursive);
        fs::remove_all(copy / "hello/target");
        fs::remove(copy / "hello/compile_commands.json");
        r = s::mcpp({"build", "--configure-only"}, copy / "hello", cold_home);
        expect(fatal(e2e::succeeded(r))) << "L control: configure-only in the cold home: " << s::text(r);
        expect(fatal(!compiled(cold, cold / "registry").empty())) << "L control: configure-only compiled nothing into the home";
    };
    "736 compile flag words reach the compiler and the databases"_test[tag{"requires:python3"}] = [] {
        // A: a cxxflags or defines element reaches the compiler as the words a shell would read
        // from it (quotes removed, `\"` kept, a -D element with a space one word); B C: both
        // databases list those words, with no quote left over; D: the first plan names an
        // element whose words differ from its text, and no other; E: a repeated plan repeats
        // no note.
        s::project p({{"mcpp.toml", R"toml([package]
name    = "flagwords"
version = "0.1.0"

[build]
cxxflags = [
  "-DV_ESC=\\\"esc.h\\\"",
  "-DV_MID=\"mid\"",
  "-DV_SQ='sq'",
  "'-DV_SP=a b'",
  "-DV_DOL=a$b",
  "-O0 -DV_A=1",
  "-DV_B=long long",
]
defines = [
  "V_DEF=\"def\"",
  "V_DSP=a b",
]
)toml"},
                      {"src/main.cpp", R"(#include <cstdio>
#define STR2(...) #__VA_ARGS__
#define STR(...) STR2(__VA_ARGS__)
static void show(const char* name, const char* value) {
    std::printf("%s=[%s]\n", name, value);
}
int main() {
    show("V_ESC", STR(V_ESC));
    show("V_MID", STR(V_MID));
    show("V_SQ",  STR(V_SQ));
    show("V_SP",  STR(V_SP));
    show("V_DOL", STR(V_DOL));
    show("V_A",   STR(V_A));
    show("V_B",   STR(V_B));
    show("V_DEF", STR(V_DEF));
    show("V_DSP", STR(V_DSP));
    return 0;
}
)"}});
        const auto b1 = p.mcpp({"build"}, at_root);
        expect(fatal(e2e::succeeded(b1))) << "the first build failed: " << s::text(b1);
        auto r = p.mcpp({"run"}, at_root);
        expect(fatal(e2e::succeeded(r))) << "the program did not run: " << s::text(r);
        auto run = s::text(r);
        std::erase(run, '\r');
        for (std::string_view want : {R"(V_ESC=["esc.h"])", "V_MID=[mid]", "V_SQ=[sq]", "V_SP=[a b]", "V_DOL=[a$b]", "V_A=[1]", "V_B=[long long]",
                                      R"(V_DEF=["def"])", "V_DSP=[a b]"})
            expect(fatal(s::has_line(run, "^" + std::regex_replace(std::string(want), std::regex{R"([\[\]\.\$\"])"}, R"(\$&)") + "$"))) << "A: the program did not print " << want << ": " << run;
        const std::vector<std::string> words{R"(-DV_ESC="esc.h")", "-DV_MID=mid", "-DV_SQ=sq", "-DV_SP=a b", "-DV_DOL=a$b", "-O0", "-DV_A=1", "-DV_B=long long",
                                             R"(-DV_DEF="def")", "-DV_DSP=a b"};
        auto lists_words = [&](const std::vector<std::string>& args) {
            const bool all = std::ranges::all_of(words, [&](const std::string& w) { return std::ranges::find(args, w) != args.end(); });
            const bool clean = std::ranges::none_of(args, [](const std::string& a) { return a.starts_with("-DV_") && (a.contains("\\\"") || a.ends_with('\'')); });
            return all && clean;
        };
        std::optional<fs::path> cdb;
        for (const auto& f : s::find_files(p.at("target"), R"(compile_commands\.json)"))
            if (!f.generic_string().contains("/.build-mcpp/")) { cdb = f; break; }
        if (!cdb) cdb = p.at("compile_commands.json");
        expect(fatal(fs::is_regular_file(*cdb))) << "B: no compile_commands.json";
        const auto main = s::cdb_arguments(*cdb, "src/main.cpp");
        expect(fatal(main && lists_words(*main))) << "B: compile_commands.json does not list the words";
        const auto db = p.mcpp({"emit", "build-database", "--format", "json"}, at_root);
        expect(fatal(e2e::succeeded(db))) << "C: emit failed: " << db.err;
        // S1 may split a unit's arguments into a set's baseline and the unit's own; the words
        // are looked for across every list of strings that carries one.
        std::vector<std::string> flat;
        std::function<void(const nlohmann::json&)> walk = [&](const nlohmann::json& n) {
            if (n.is_object()) for (const auto& v : n) walk(v);
            if (!n.is_array()) return;
            if (!n.empty() && std::ranges::all_of(n, [](const nlohmann::json& x) { return x.is_string(); }) &&
                std::ranges::any_of(n, [](const nlohmann::json& x) { return x.get<std::string>().starts_with("-DV_"); }))
                for (const auto& x : n) flat.push_back(x);
            for (const auto& v : n) walk(v);
        };
        walk(s::json(db.out));
        expect(fatal(lists_words(flat))) << "C: the build database does not list the words";
        const auto t1 = s::text(b1);
        for (std::string_view element : {R"(V_DEF="def")", "-DV_DOL=a$b"})
            expect(fatal(e2e::contains(t1, std::format("element '{}' reaches the compiler as", element)))) << "D: the first plan does not name " << element << ": " << t1;
        for (const auto& l : s::lines_with(t1, "reaches the compiler as"))
            for (std::string_view element : {R"(-DV_MID="mid")", "V_DSP=a b", "-DV_ESC=", "-DV_B=long long", "-O0 -DV_A=1"})
                expect(fatal(!l.contains(std::format("element '{}", element)))) << "D: the first plan names " << element << ", whose words did not change: " << l;
        fs::last_write_time(p.at("src/main.cpp"), fs::file_time_type::clock::now());
        r = p.mcpp({"build"}, at_root);
        expect(fatal(e2e::succeeded(r))) << "the second build failed: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), "reaches the compiler as"))) << "E: a repeated plan repeated the notes: " << s::text(r);
    };
    "815 the database describes what a rule generates"_test[tag{"requires:unix-shell"}][tag{"requires:python3"}] = [] {
        // A: each set the generated header reaches names it, its generator (id, inputs,
        // arguments) and its directory; B: their build paths lie under the project's target, and
        // the build writes the header there; C: compile_commands.json carries no such field.
        s::project p({{"mcpp.toml", "[package]\nname    = \"gendb\"\nversion = \"0.1.0\"\n"},
                      {"templates/answer.h.in", "#pragma once\ninline int generated_answer() { return 42; }\n"},
                      {"src/main.cpp", "#include \"answer.h\"\nint main() { return generated_answer() == 42 ? 0 : 1; }\n"},
                      {"tests/test_answer.cpp", "#include \"answer.h\"\nint main() { return generated_answer() == 42 ? 0 : 1; }\n"},
                      {"build.mcpp", R"(import std;
import mcpp;
int main() {
    const std::string gen = std::string(mcpp::out_dir()) + "/gen";
    const std::string in  = std::string(mcpp::manifest_dir()) + "/templates/answer.h.in";
    const std::string out = gen + "/answer.h";
    mcpp::action a;
    a.id   = "gen:answer";
    a.role = mcpp::roles::source;
    a.arg("cp").arg(in.c_str()).arg(out.c_str())
     .input(in.c_str())
     .output(out.c_str())
     .submit();
    mcpp::include_dir(gen.c_str());
}
)"}});
        auto r = p.mcpp({"emit", "build-database", "--format", "json"}, at_root);
        expect(fatal(e2e::succeeded(r))) << "the plan failed: " << r.err;
        const auto root = fs::canonical(p.dir());
        const auto sets = s::json(r.out)["data"]["database"]["sets"];
        std::vector<nlohmann::json> headers, dirs;
        std::map<std::string, nlohmann::json> by_set;
        for (const auto& st : sets) {
            const auto gen = st.contains("ide") ? st["ide"].value("generated", nlohmann::json::array()) : nlohmann::json::array();
            by_set[st["name"]] = gen;
            for (const auto& g : gen) (g["kind"] == "header" ? headers : dirs).push_back(g);
        }
        expect(fatal(!headers.empty())) << "A: no generated header entry";
        const auto h = headers[0];
        expect(fatal(h["path"].get<std::string>().ends_with((fs::path("gen") / "answer.h").string()))) << "A: " << h.dump();
        expect(fatal(h["generator"]["id"] == "gen:answer" && h["generator"]["arguments"][0] == "cp")) << "A: " << h.dump();
        expect(fatal(std::ranges::any_of(h["generator"]["inputs"], [](const nlohmann::json& i) { return i.get<std::string>().ends_with("answer.h.in"); }))) << "A: " << h.dump();
        expect(fatal(!dirs.empty() && dirs[0]["kind"] == "directory" && dirs[0]["path"].get<std::string>().ends_with("gen"))) << "A: no generated directory entry";
        expect(fatal(by_set.contains("gendb:test"))) << "A: no test set";
        for (std::string name : {"gendb", "gendb:test"})
            expect(fatal(std::ranges::any_of(by_set[name], [](const nlohmann::json& g) { return g["kind"] == "header" && g["generator"]["id"] == "gen:answer"; })))
                << "A: set " << name << " does not name the generated header";
        for (const auto& g : {h, dirs[0]}) {
            const auto bp = fs::weakly_canonical(g["build-path"].get<std::string>()).string();
            expect(fatal(bp.starts_with((root / "target").string() + "/"))) << "B: " << g["build-path"].dump() << " is not under " << root << "/target";
        }
        r = p.mcpp({"build"}, at_root);
        expect(fatal(e2e::succeeded(r))) << "the build failed: " << s::text(r);
        expect(fatal(fs::is_regular_file(h["build-path"].get<std::string>()))) << "B: the build did not write the header at the stated build-path";
        r = p.mcpp({"emit", "build-database", "--spec", "compile-commands"}, at_root);
        expect(fatal(e2e::succeeded(r))) << "the compile database failed: " << r.err;
        expect(fatal(s::lacks(r.out, R"("generated")"))) << "C: compile_commands.json carries a generated field";
    };
    "817 emit writes no xlings json under private work dir"_test[tag{"requires:unix-shell"}][tag{"requires:python3"}] = [] {
        // A project declaring [xlings] deps: emit leaves the project tree as it was, writes no
        // <root>/.mcpp/.xlings.json, states no write-project effect, and writes the declaration
        // into its private work directory's .xlings.json instead (xlings a stub).
        const auto tmp = s::case_dir() / "measured";
        const auto out = s::case_dir() / "out";
        const auto home = s::case_home(s::home::inherited);
        s::write_executable(out / "bin/xlings", R"(#!/usr/bin/env bash
[[ "${1:-}" == "interface" && "${2:-}" == "--version" ]] && { echo '{"protocol_version":"1.13","version":"9999.1.1.1","capabilities":["install_packages"],"home_layouts":[2],"features":["index_dir","index_store","upstreams","payload_status","sync_index","payload_currency","binding_state","network_diagnostics","download_socket_budgets","read_only_queries"]}'; exit 0; }
echo "$*" >> "${STUB_LOG:?}"
exit 0
)");
        // config.toml without its `binary` lines and its [xlings] header, then [xlings] binary.
        std::string config;
        if (fs::exists(home / "config.toml"))
            for (const auto& l : std::views::split(s::read_file(home / "config.toml"), '\n')) {
                const std::string line(l.begin(), l.end());
                if (!line.starts_with("binary") && !line.starts_with("[xlings]")) config += line + "\n";
            }
        config += std::format("\n[xlings]\nbinary = \"{}\"\n", (out / "bin/xlings").string());
        std::ofstream(home / "config.toml", std::ios::trunc) << config;
        s::project p(tmp / "proj", {{"mcpp.toml", "[package]\nname    = \"proj817\"\nversion = \"0.1.0\"\n\n[xlings]\ndeps = [\"definitely-not-a-real-package-817\"]\n"},
                                    {"src/main.cpp", "int main() { return 0; }\n"}});
        const auto before = tree_digest(tmp);
        const auto r = p.mcpp({"emit", "build-database", "--format", "json"}, {.where = s::home::inherited, .env = {{"STUB_LOG", (out / "xlings.log").string()}}});
        expect(fatal(e2e::succeeded(r))) << "emit build-database exited non-zero: " << r.err;
        expect(fatal(tree_digest(tmp) == before)) << "A: the project tree changed";
        expect(fatal(!p.exists(".mcpp/.xlings.json"))) << "B: emit wrote <root>/.mcpp/.xlings.json";
        const auto e = s::json(r.out);
        const auto effects = e["effects"].get<std::vector<std::string>>();
        expect(fatal(std::ranges::find(effects, "write-project") == effects.end())) << "C: effects " << e["effects"].dump();
        std::string work;
        for (const auto& st : e["data"]["database"]["sets"])
            for (const auto& u : st["translation-units"]) {
                const auto norm = slash(u["object"]);
                if (work.empty() && norm.contains("/target/")) work = norm.substr(0, norm.find("/target/"));
            }
        expect(fatal(!work.empty())) << "D: could not recover the private work directory";
        const auto found = s::find_file(work, ".xlings.json");
        expect(fatal(found.has_value())) << "D: no .xlings.json under the private work directory " << work;
        expect(fatal(e2e::contains(s::read_file(*found), "definitely-not-a-real-package-817"))) << "D: the private .xlings.json does not name the declared dependency";
    };
}};




}  // namespace

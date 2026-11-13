// What a runner receives: the files the artifact carries (MCPP_RUNTIME_FILES), a format run by
// the runner named after it, the runner contract, and a launch description on the fast path
// (formerly tests/e2e/672, 674, 927, 1024).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

std::string host_triple(const s::project& p) {
    std::vector<std::string> triples;
    for (const auto& e : fs::directory_iterator(p.at("target")))
        if (!e.path().filename().string().starts_with(".")) triples.push_back(e.path().filename().string());
    std::ranges::sort(triples);
    expect(fatal(!triples.empty())) << "could not determine the host triple from target/";
    return triples.front();
}

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string l; std::getline(in, l);) out.push_back(l);
    return out;
}

// The source a record pairs with `destination` (`awk -F'\t' '$1 == d { print $2 }'`).
std::string field(const fs::path& rec, std::string_view destination) {
    for (const auto& l : lines_of(s::read_file(rec))) {
        const auto tab = l.find('\t');
        if (tab != std::string::npos && std::string_view(l).substr(0, tab) == destination) return l.substr(tab + 1);
    }
    return {};
}

// The source of the first destination matching `pattern`.
std::string field_matching(const fs::path& rec, std::string_view pattern) {
    const std::regex re{std::string(pattern)};
    for (const auto& l : lines_of(s::read_file(rec))) {
        const auto tab = l.find('\t');
        if (tab != std::string::npos && std::regex_search(l.substr(0, tab), re)) return l.substr(tab + 1);
    }
    return {};
}

std::string list_of(const fs::path& rec) {
    const auto l = s::last_starting(s::read_file(rec), "LIST=");
    return l.empty() ? l : l.substr(5);
}

bool received(const fs::path& rec) { return fs::exists(rec) && s::no_line(s::read_file(rec), "^UNSET$"); }

E2E contract{"contract", [] {
    "672 a runner receives the files the artifact carries"_test[tag{"requires:unix-shell"}] = [] {
        // A runner receives MCPP_RUNTIME_FILES, a list of `<destination>\t<absolute source>`
        // naming the deployed files and the dependency's shared library, for `run` and for each
        // test (a test in a subdirectory names its files with ../, one list per test); the
        // program inherits it; --no-runner runs without it; a project with nothing to carry, and
        // a distributable run with --pkg-format, receive an existing empty list.
        const auto tmp = s::case_dir();
        const auto rec_dir = tmp / "rec";
        fs::create_directories(rec_dir);
        const auto runner = tmp / "runner.sh";
        s::write_executable(runner, std::format(R"(#!/bin/sh
name=$(basename "$1")
if [ -n "${{MCPP_RUNTIME_FILES+x}}" ]; then
    {{ printf 'LIST=%s\n' "$MCPP_RUNTIME_FILES"; cat "$MCPP_RUNTIME_FILES"; }} > "{0}/$name.rec"
else
    echo "UNSET" > "{0}/$name.rec"
fi
exec "$@"
)", rec_dir.string()));
        auto clear = [&] {
            for (const auto& f : s::glob(rec_dir, "*.rec")) fs::remove(f);
        };
        s::project(tmp / "fw", {{"mcpp.toml", "[package]\nname    = \"fw\"\nversion = \"0.1.0\"\n\n[targets.fw]\nkind = \"shared\"\n"},
                                {"src/fw.cppm", "export module fw;\nexport int fw_value() { return 42; }\n"}});
        s::project app(tmp / "app", {
            {"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n\n"
                          "[dependencies]\nfw = { path = \"../fw\" }\n\n[runtime]\ndeploy = [ { from = \"share/data.txt\", to = \"data\" } ]\n"},
            {"share/data.txt", "payload\n"},
            {"src/main.cpp", "import std;\nimport fw;\nint main() {\n    std::println(\"APP-RAN {}\", fw_value());\n"
                             "    std::println(\"RUNTIME_FILES={}\", std::getenv(\"MCPP_RUNTIME_FILES\") ? \"set\" : \"unset\");\n    return 0;\n}\n"},
            {"tests/reads.cpp", "import std;\nimport fw;\nint main() { return fw_value() == 42 ? 0 : 1; }\n"},
        });
        auto r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "the initial build failed: " << s::text(r);
        const auto host = host_triple(app);
        const auto runner_table = std::format("\n[target.{}]\nrunner = [\"{}\"]\n", host, runner.string());
        app.write("mcpp.toml", app.read("mcpp.toml") + runner_table);
        r = app.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "mcpp run through the runner failed: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "APP-RAN 42"))) << "the program did not run through the runner: " << s::text(r);
        auto rec = rec_dir / "app.rec";
        expect(fatal(fs::exists(rec))) << "the runner recorded nothing";
        expect(fatal(received(rec))) << "the runner did not receive MCPP_RUNTIME_FILES";
        expect(fatal(fs::is_regular_file(list_of(rec)))) << "MCPP_RUNTIME_FILES does not name an existing file: " << s::read_file(rec);
        const auto data = field(rec, "data/data.txt");
        expect(fatal(!data.empty())) << "the list does not name data/data.txt: " << s::read_file(rec);
        expect(fatal(data.starts_with('/'))) << "the source of data/data.txt is not absolute: " << data;
        expect(fatal(s::read_file(data) == "payload\n")) << "data/data.txt pairs with a file that is not the deployed one";
        const auto lib = field_matching(rec, R"(^libfw\.(so|dylib)$)");
        expect(fatal(!lib.empty() && fs::is_regular_file(lib))) << "the list does not name the dependency's shared library: " << s::read_file(rec);
        for (const auto& l : lines_of(s::read_file(rec)))
            expect(fatal(l.starts_with("LIST=") || std::ranges::count(l, '\t') == 1)) << "a line of the list is not <destination><TAB><source>: " << l;
        expect(fatal(e2e::contains(s::text(r), "RUNTIME_FILES=set"))) << "the program under the runner did not inherit the variable: " << s::text(r);

        clear();
        r = app.mcpp({"test"});
        expect(fatal(e2e::succeeded(r))) << "mcpp test through the runner failed: " << s::text(r);
        rec = rec_dir / "reads.rec";
        expect(fatal(fs::exists(rec))) << "the runner recorded nothing for test reads";
        expect(fatal(received(rec))) << "test reads' runner did not receive the variable";
        expect(fatal(!field(rec, "data/data.txt").empty())) << "test reads does not carry data/data.txt: " << s::read_file(rec);
        expect(fatal(!field_matching(rec, R"(^libfw\.(so|dylib)$)").empty())) << "test reads does not carry the shared library: " << s::read_file(rec);
        r = app.mcpp({"run", "--no-runner"});
        expect(fatal(e2e::succeeded(r))) << "mcpp run --no-runner failed: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "RUNTIME_FILES=unset"))) << "a run without a runner received the variable: " << s::text(r);

        r = s::mcpp({"new", "deep"}, tmp);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        s::project deep(tmp / "deep", {});
        deep.write("share/data.txt", "payload\n");
        deep.write("mcpp.toml", deep.read("mcpp.toml") + "\n[runtime]\ndeploy = [ { from = \"share/data.txt\", to = \"data\" } ]\n");
        deep.write("tests/unit/nested.cpp", "int main() { return 0; }\n");
        r = deep.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "the project with a nested test did not build: " << s::text(r);
        deep.write("mcpp.toml", deep.read("mcpp.toml") + runner_table);
        clear();
        r = deep.mcpp({"test"});
        expect(fatal(e2e::succeeded(r))) << "mcpp test of the nested test failed: " << s::text(r);
        for (std::string t : {"nested", "test_smoke"})
            expect(fatal(received(rec_dir / (t + ".rec")))) << "test " << t << "'s runner did not receive the variable: " << s::text(r);
        const auto nested = field(rec_dir / "nested.rec", "../data/data.txt");
        expect(fatal(!nested.empty() && fs::is_regular_file(nested))) << "test unit/nested does not name its data with ../";
        expect(fatal(!field(rec_dir / "test_smoke.rec", "data/data.txt").empty())) << "the top-level test does not name its data without ../";
        expect(fatal(list_of(rec_dir / "nested.rec") != list_of(rec_dir / "test_smoke.rec"))) << "two test programs share one list";

        r = s::mcpp({"new", "lone"}, tmp);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        s::project lone(tmp / "lone", {});
        r = lone.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "the plain project did not build: " << s::text(r);
        lone.write("mcpp.toml", lone.read("mcpp.toml") + runner_table);
        r = lone.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "mcpp run of the plain project failed: " << s::text(r);
        rec = rec_dir / "lone.rec";
        expect(fatal(received(rec))) << "the plain project's runner did not receive the variable";
        auto list = list_of(rec);
        expect(fatal(fs::is_regular_file(list) && fs::file_size(list) == 0)) << "the plain project's list is not an existing empty file";
        s::write_executable(lone.at("copy.sh"), "#!/usr/bin/env bash\nset -e\ncp \"$1\" \"$2\"\nchmod +x \"$2\"\n");
        lone.write("build.mcpp", R"(import mcpp;
#include <string>
#include <string_view>
int main() {
    mcpp::provides_pack_format("blob");
    if (std::string_view(mcpp::pack_format()) != "blob") return 0;
    const std::string root = mcpp::manifest_dir();
    const std::string out  = std::string(mcpp::out_dir()) + "/lone.blob";
    mcpp::action a;
    a.id          = "blob";
    a.role        = "artifact";
    a.description = "blob";
    a.arg((root + "/copy.sh").c_str())
     .arg("${mcpp.target_file:lone}")
     .arg(out.c_str())
     .input("${mcpp.target_file:lone}")
     .output(out.c_str())
     .submit();
    return 0;
}
)");
        r = lone.mcpp({"run", "--pkg-format", "blob"});
        expect(fatal(e2e::succeeded(r))) << "mcpp run --pkg-format blob failed: " << s::text(r);
        rec = rec_dir / "lone.blob.rec";
        expect(fatal(received(rec))) << "the distributable's runner did not receive the variable: " << s::text(r);
        list = list_of(rec);
        expect(fatal(fs::is_regular_file(list) && fs::file_size(list) == 0)) << "the distributable's list is not an existing empty file";
    };
    "674 a format is run by the runner named after it"_test[tag{"requires:unix-shell"}] = [] {
        // `run --pkg-format <f>` runs the distributable through the runner named <f> (a typed
        // --runner wins), returning the program's status; a plain run executes the link output
        // directly; a directory no runner reaches is refused (126) naming the runner and the
        // table, before the spawn; a manifest runner of that name, or a default runner, runs it.
        const auto tmp = s::case_dir();
        for (std::string r : {"blob", "other", "bundle", "default"})
            s::write_executable(tmp / ("runner-" + r + ".sh"), "#!/bin/sh\nprintf 'RUNNER-" + r + ": %s\\n' \"$1\"\n"
                                                                "if [ -d \"$1\" ]; then exec \"$1/Contents/MacOS/app\"; fi\nexec \"$@\"\n");
        s::project app(tmp / "app", {
            {"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
            {"src/main.cpp", "#include <cstdio>\nint main() { std::puts(\"PROGRAM-RAN\"); return 7; }\n"},
            {"build.mcpp", std::format(R"(import mcpp;
#include <string>
#include <string_view>
int main() {{
    mcpp::provides_pack_format("blob");
    mcpp::provides_pack_format("bundle");
    mcpp::runner("blob", "{}");
    const std::string_view fmt = mcpp::pack_format();
    if (fmt != "blob" && fmt != "bundle") return 0;
    const std::string root = mcpp::manifest_dir();
    const bool dir = fmt == "bundle";
    const std::string out = std::string(mcpp::out_dir()) + (dir ? "/app.bundle" : "/app.blob");
    mcpp::action a;
    a.id          = dir ? "bundle" : "blob";
    a.role        = "artifact";
    a.description = a.id;
    a.arg((root + (dir ? "/bundle.sh" : "/copy.sh")).c_str())
     .arg("${{mcpp.target_file:app}}")
     .arg(out.c_str())
     .input("${{mcpp.target_file:app}}")
     .output(out.c_str())
     .submit();
    return 0;
}}
)", (tmp / "runner-blob.sh").string())},
        });
        s::write_executable(app.at("copy.sh"), "#!/usr/bin/env bash\nset -e\ncp \"$1\" \"$2\"\nchmod +x \"$2\"\n");
        s::write_executable(app.at("bundle.sh"), "#!/usr/bin/env bash\nset -e\nrm -rf \"$2\"\nmkdir -p \"$2/Contents/MacOS\"\ncp \"$1\" \"$2/Contents/MacOS/app\"\nchmod +x \"$2/Contents/MacOS/app\"\n");
        auto r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "the initial build failed: " << s::text(r);
        const auto host = host_triple(app);
        app.write("mcpp.toml", app.read("mcpp.toml") + std::format("\n[target.{}.runners]\nother = [\"{}\"]\n", host, (tmp / "runner-other.sh").string()));
        const auto base = app.read("mcpp.toml");
        r = app.mcpp({"run", "--pkg-format", "blob"});
        auto t = s::text(r);
        expect(fatal(s::has_line(t, R"(RUNNER-blob: .*/app\.blob$)"))) << "the runner named 'blob' did not receive the distributable: " << t;
        expect(fatal(e2e::contains(t, "PROGRAM-RAN"))) << "the program did not run through the runner: " << t;
        expect(fatal(e2e::exited(r, 7))) << "the program's status 7 did not come back: " << t;
        expect(fatal(s::has_line(t, "Running `.*runner-blob.sh"))) << "the status line does not say Running for the format's runner: " << t;
        r = app.mcpp({"run", "--pkg-format", "blob", "--runner", "other"});
        t = s::text(r);
        expect(fatal(s::has_line(t, R"(RUNNER-other: .*/app\.blob$)"))) << "--runner other did not win: " << t;
        expect(fatal(s::lacks(t, "RUNNER-blob"))) << "the format's runner ran although --runner was typed: " << t;
        expect(fatal(e2e::exited(r, 7))) << "--runner other lost the program's status: " << t;
        r = app.mcpp({"run"});
        t = s::text(r);
        expect(fatal(s::lacks(t, "RUNNER-"))) << "a plain run went through a runner: " << t;
        expect(fatal(e2e::contains(t, "PROGRAM-RAN"))) << "a plain run did not run the program: " << t;
        expect(fatal(e2e::exited(r, 7))) << "a plain run lost the program's status: " << t;
        r = app.mcpp({"run", "--pkg-format", "bundle"});
        t = s::text(r);
        expect(fatal(e2e::exited(r, 126))) << "the directory without a runner did not exit 126: " << t;
        expect(fatal(e2e::contains(t, "produced a directory"))) << "the refusal does not say the distributable is a directory: " << t;
        expect(fatal(e2e::contains(t, "a runner named 'bundle'"))) << "the refusal does not name the runner 'bundle': " << t;
        expect(fatal(e2e::contains(t, "[target." + host + ".runners]"))) << "the refusal does not name the manifest table: " << t;
        expect(fatal(s::no_line(t, "Permission denied|Is a directory"))) << "the directory reached the kernel: " << t;
        expect(fatal(s::no_line(t, "PROGRAM-RAN|RUNNER-"))) << "something ran despite the refusal: " << t;
        app.write("mcpp.toml", base + std::format("bundle = [\"{}\"]\n", (tmp / "runner-bundle.sh").string()));
        r = app.mcpp({"run", "--pkg-format", "bundle"});
        t = s::text(r);
        expect(fatal(s::has_line(t, R"(RUNNER-bundle: .*/app\.bundle$)"))) << "the runner named 'bundle' did not receive the directory: " << t;
        expect(fatal(e2e::exited(r, 7))) << "the bundle's program status did not come back: " << t;
        app.write("mcpp.toml", base + std::format("\n[target.{}]\nrunner = [\"{}\"]\n", host, (tmp / "runner-default.sh").string()));
        r = app.mcpp({"run", "--pkg-format", "bundle"});
        t = s::text(r);
        expect(fatal(s::has_line(t, R"(RUNNER-default: .*/app\.bundle$)"))) << "the default runner did not receive the directory: " << t;
        expect(fatal(s::lacks(t, "no runner reaches it"))) << "a refusal was printed although a default runner exists: " << t;
        expect(fatal(e2e::exited(r, 7))) << "the default runner lost the status: " << t;
    };

    // It writes the shared home's config.toml (restored at its end): the machine resource.
    "927 the runner contract"_test[tag{"requires:gcc"}][tag{"requires:unix-shell"}][resource{"machine"}] = [] {
        // run_under composes in front of the artifact with the contract variables and a closure
        // manifest, returns the program's status and states where it was declared; the
        // environment's `..._RUN_UNDER=none` and --no-runner drop it; config.toml's run_under is
        // the default; a missing program fails with 2 naming it; `test` runs through it too.
        s::needs({"gcc", "unix-shell"});
        const auto work = s::case_dir();
        const auto record = work / "record";
        s::write_executable(work / "bin/via-host", R"(#!/bin/sh
{
  echo "argv: $*"
  echo "triple=$MCPP_RUN_TRIPLE kind=$MCPP_RUN_KIND mode=$MCPP_RUN_MODE"
  echo "id=$MCPP_RUN_ID"
  echo "closure=$MCPP_RUN_CLOSURE"
} >> "$RECORD"
shift
exec "$@"
)");
        const auto path = (work / "bin").string() + ":" + s::env("PATH").value_or("");
        const s::options o{.env = {{"NO_COLOR", "1"}, {"PATH", path}, {"RECORD", record.string()}}};
        s::project app(work / "app", {{"src/main.cpp", "int main(int argc, char**) { return argc == 2 ? 7 : 3; }\n"}});
        auto machine = s::run(*s::on_path("uname"), {"-m"}).out;
        machine.erase(machine.find_last_not_of("\r\n") + 1);
        const auto triple = machine + "-linux-gnu";
        auto manifest = [&](std::string_view row) {
            app.write("mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[toolchain]\ndefault = \"gcc@{}\"\n[target.{}]\n{}\n",
                                               s::gcc_version(), triple, row));
        };
        manifest(R"(run_under = ["via-host", "team-mac"])");
        fs::remove(record);
        auto r = app.mcpp({"run", "--", "one"}, o);
        auto t = s::text(r);
        expect(fatal(e2e::exited(r, 7))) << "the program's exit status did not come back through run_under: " << t;
        expect(fatal(s::has_line(t, R"(Via via-host team-mac  \[.*mcpp.toml:7\])"))) << "no Via line naming where run_under was stated: " << t;
        const auto rec = s::read_file(record);
        expect(fatal(s::has_line(rec, "argv: team-mac .*/bin/app one"))) << "run_under was not composed in front of the artifact: " << rec;
        expect(fatal(e2e::contains(rec, "triple=" + triple + " kind=exe mode=run"))) << "the contract variables are missing: " << rec;
        const auto closure = s::last_starting(rec, "closure=").substr(8);
        expect(fatal(fs::is_regular_file(closure))) << "MCPP_RUN_CLOSURE names no file";
        const auto cl = s::read_file(closure);
        expect(fatal(e2e::contains(cl, R"("schema": 1)"))) << "the closure manifest carries no schema: " << cl;
        expect(fatal(s::has_line(cl, R"("artifact": ".*/bin/app")"))) << "the closure manifest does not name the artifact: " << cl;
        fs::remove(record);
        std::string var = "MCPP_TARGET_" + triple + "_RUN_UNDER";
        for (auto& c : var) c = c == '-' ? '_' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        auto none = o;
        none.env.emplace_back(var, "none");
        r = app.mcpp({"run", "--", "one"}, none);
        expect(fatal(e2e::exited(r, 7) && !fs::exists(record))) << "`..._RUN_UNDER=none` did not clear run_under";
        r = app.mcpp({"run", "--no-runner", "--", "one"}, o);
        expect(fatal(e2e::exited(r, 7) && !fs::exists(record))) << "--no-runner did not drop run_under";

        manifest("");
        const auto config = s::shared_home() / "config.toml";
        const auto saved = s::read_file(config);
        {
            std::ofstream out(config, std::ios::app);
            out << std::format("\n[target.{}]\nrun_under = [\"via-host\", \"my-vm\"]\n", triple);
        }
        r = app.mcpp({"run", "--", "one"}, o);
        {
            std::ofstream out(config, std::ios::trunc | std::ios::binary);
            out << saved;
        }
        expect(fatal(e2e::exited(r, 7))) << "config.toml's run_under failed: " << s::text(r);
        expect(fatal(e2e::contains(s::read_file(record), "argv: my-vm "))) << "config.toml's [target.<triple>].run_under was not the default";
        manifest(R"(run_under = ["no-such-wrapper", "x"])");
        r = app.mcpp({"run"}, o);
        expect(fatal(e2e::exited(r, 2))) << "a missing run_under program did not fail with 2: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "no-such-wrapper"))) << "the missing program was not named: " << s::text(r);
        manifest(R"(run_under = ["via-host", "team-mac"])");
        app.write("tests/t.cpp", "int main() { return 0; }\n");
        fs::remove(record);
        r = app.mcpp({"test"}, o);
        expect(fatal(e2e::succeeded(r))) << "mcpp test through run_under failed: " << s::text(r);
        expect(fatal(e2e::contains(s::read_file(record), "mode=test"))) << "a test was not run through run_under with MCPP_RUN_MODE=test";
    };
    "1024 a launch description takes the fast path"_test[tag{"requires:python3"}] = [] {
        // A: `run --print-launch` of an unchanged project takes the fast path where `run` does,
        // and describes what the full path does (plus the program's arguments). B: after a
        // change, the description is the same. C: a launch record carrying a foreign field is not
        // used, and the full path records it again. D: with --message-format json it streams
        // NDJSON ending in the envelope; without --print-launch that is refused (2). E: the
        // protocol states launch-stream.
        auto p = s::project::scaffold("app");
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "the project does not build: " << s::text(r);
        const auto full = p.mcpp({"run", "--print-launch"});
        expect(fatal(e2e::succeeded(full))) << "A: the first description failed: " << full.err;
        r = p.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "A: mcpp run failed: " << r.err;
        const bool fast_here = s::lacks(r.err, "Resolving") ? true : false;
        const auto fast = p.mcpp({"run", "--print-launch", "--", "one", "two"});
        expect(fatal(e2e::succeeded(fast))) << "A: the second description failed: " << fast.err;
        if (fast_here) {
            const bool resolved = e2e::contains(fast.err, "Resolving") ? true : false;
            std::string why;
            if (resolved) why = s::text(p.mcpp({"run", "--print-launch", "--verbose"}));
            expect(fatal(!resolved)) << "A: the unchanged description resolved again (the full path) where mcpp run does not: "
                                     << (why.empty() ? std::string() : s::lines_with(why, "fast-path").empty() ? why : s::lines_with(why, "fast-path").front());
        }
        const auto jf = s::json(full.out), jg = s::json(fast.out);
        expect(fatal(jg["kind"] == "mcpp.launch")) << fast.out;
        auto g = jg["data"];
        auto argv = g["argv"].get<std::vector<std::string>>();
        expect(fatal(argv.size() >= 2 && argv[argv.size() - 2] == "one" && argv.back() == "two")) << g["argv"].dump();
        argv.resize(argv.size() - 2);
        g["argv"] = argv;
        expect(fatal(jf["data"] == g)) << "A: the fast description is not the full path's:\n" << full.out << "\n" << fast.out;

        auto sources = s::glob(p.at("src"), "*.cpp");
        std::ranges::sort(sources);
        const auto src = sources.front();
        { std::ofstream(src, std::ios::app) << "\n// changed\n"; }
        const auto changed = p.mcpp({"run", "--print-launch"});
        expect(fatal(e2e::succeeded(changed))) << "B: the changed description failed: " << changed.err;
        expect(fatal(e2e::contains(changed.err, "Compiling"))) << "B: the changed source was not compiled: " << changed.err;
        expect(fatal(jf["data"] == s::json(changed.out)["data"])) << "B: the description after a change differs";

        std::optional<fs::path> record;
        for (const auto& f : s::find_files(p.at("target"), R"(.*\.json)"))
            if (f.parent_path().filename() == "launch") { record = f; break; }
        expect(fatal(record.has_value())) << "C: no launch is recorded in the configuration's directory";
        {
            auto rec = s::json_file(*record);
            rec["env"]["MCPP_E2E_FOREIGN"] = "1";
            std::ofstream(*record, std::ios::trunc) << rec.dump();
        }
        const auto again = p.mcpp({"run", "--print-launch"});
        expect(fatal(e2e::succeeded(again))) << "C: the description failed: " << again.err;
        expect(fatal(!fast_here || e2e::contains(again.err, "Resolving"))) << "C: a foreign record was used by the fast path: " << again.err;
        expect(fatal(s::lacks(again.out, "MCPP_E2E_FOREIGN"))) << "C: the foreign field reached the description";
        expect(fatal(s::lacks(s::read_file(*record), "MCPP_E2E_FOREIGN"))) << "C: the full path did not record the launch again";

        { std::ofstream(src, std::ios::app) << "\n// changed again\n"; }
        const auto stream = p.mcpp({"run", "--print-launch", "--message-format", "json"});
        expect(fatal(e2e::succeeded(stream))) << "D: the streamed description failed: " << stream.err;
        std::vector<nlohmann::json> lines;
        std::istringstream in(stream.out);
        for (std::string l; std::getline(in, l);)
            if (l.find_first_not_of(" \t\r") != std::string::npos) lines.push_back(nlohmann::json::parse(l));
        expect(fatal(!lines.empty())) << "D: the stream is empty";
        bool progress = false;
        for (std::size_t i = 0; i + 1 < lines.size(); ++i) progress = progress || lines[i].value("event", "") == "progress";
        expect(fatal(progress)) << "D: the stream carries no progress event: " << stream.out;
        expect(fatal(lines.back()["kind"] == "mcpp.launch" && !lines.back()["data"]["argv"].empty())) << "D: the stream does not end in the envelope: " << stream.out;
        r = p.mcpp({"run", "--message-format", "json"});
        expect(fatal(e2e::exited(r, 2))) << "D: --message-format json without --print-launch exited " << r.status << ", not 2: " << r.err;
        expect(fatal(e2e::contains(r.err, "print-launch"))) << "D: the refusal does not name --print-launch: " << r.err;
        r = p.mcpp({"--protocol-version"});
        expect(fatal(e2e::succeeded(r))) << "E: --protocol-version failed: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), R"("launch-stream")"))) << "E: launch-stream is not stated: " << s::text(r);
    };
}};



}  // namespace

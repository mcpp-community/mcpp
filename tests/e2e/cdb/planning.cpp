// What `emit build-database` describes when planning fails or defers: each member planned on its
// own, an unbuilt host tool deferred, a member past a failed build program, and the caller's pipe
// (formerly tests/e2e/731, 787, 788, 789, 809).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

std::vector<std::string> set_names(const nlohmann::json& envelope) {
    std::vector<std::string> out;
    for (const auto& st : envelope["data"]["database"]["sets"]) out.push_back(st["name"]);
    return out;
}

E2E planning{"planning", [] {
    "787 emit plans every member on its own"_test[tag{"requires:python3"}] = [] {
        // A member whose plan fails leaves the others described (exit 1, one diagnostic naming
        // its manifest, both members watched); the bare document likewise, the reason on standard
        // error; when every member fails, the envelope has no data and a diagnostic per member.
        s::project ws(s::case_dir() / "ws", {
            {"mcpp.toml", "[workspace]\nmembers = [\"good\", \"bad\"]\n"},
            {"good/mcpp.toml", "[package]\nname    = \"good\"\nversion = \"0.1.0\"\n"},
            {"good/src/main.cpp", "int main() { return 0; }\n"},
            {"bad/mcpp.toml", "[package]\nname    = \"bad\"\nversion = \"0.1.0\"\n\n[dependencies]\nmissing = { path = \"../does-not-exist\" }\n"},
            {"bad/src/main.cpp", "int main() { return 0; }\n"},
        });
        auto r = ws.mcpp({"emit", "build-database", "--workspace", "--format", "json"});
        expect(fatal(e2e::exited(r, 1))) << "A: exit status " << r.status << ", expected 1: " << r.err;
        auto e = s::json(r.out);
        expect(fatal(set_names(e) == std::vector<std::string>{"good"})) << "A: sets " << r.out;
        expect(fatal(e["diagnostics"].size() == 1u)) << "A: diagnostics " << e["diagnostics"].dump();
        const auto diag = e["diagnostics"][0];
        expect(fatal(diag["code"] == "MCPP_BUILD_DATABASE_PLAN_FAILED" && diag["severity"] == "error" && diag["path"] == "bad/mcpp.toml")) << "A: " << diag.dump();
        expect(fatal(diag["message"].get<std::string>().contains("bad"))) << "A: " << diag["message"].dump();
        const auto watch = e["data"]["watch"].get<std::vector<std::string>>();
        expect(fatal(std::ranges::find(watch, "bad/mcpp.toml") != watch.end())) << "A: watch " << e["data"]["watch"].dump();
        expect(fatal(std::ranges::any_of(watch, [](const std::string& w) { return w.starts_with("good/"); }))) << "A: watch " << e["data"]["watch"].dump();
        r = ws.mcpp({"emit", "build-database", "--workspace"});
        expect(fatal(e2e::exited(r, 1))) << "B: bare invocation exit status " << r.status << ", expected 1";
        expect(fatal(!r.out.empty())) << "B: the bare document is empty: " << r.err;
        expect(fatal(s::json(r.out)["sets"][0]["name"] == "good")) << "B: the bare document's set: " << r.out;
        expect(fatal(e2e::contains(r.err, "bad"))) << "B: the failed member's reason is not on stderr: " << r.err;
        ws.write("good/mcpp.toml", "[package]\nname    = \"good\"\nversion = \"0.1.0\"\n\n[dependencies]\nmissing2 = { path = \"../also-does-not-exist\" }\n");
        r = ws.mcpp({"emit", "build-database", "--workspace", "--format", "json"});
        expect(fatal(e2e::exited(r, 1))) << "C: exit status " << r.status << ", expected 1: " << r.err;
        e = s::json(r.out);
        expect(fatal(!e.contains("data"))) << "C: the all-failed envelope has data: " << r.out;
        expect(fatal(e["diagnostics"].size() == 2u)) << "C: diagnostics " << e["diagnostics"].dump();
        std::vector<std::string> paths;
        for (const auto& d : e["diagnostics"]) {
            paths.push_back(d["path"]);
            expect(fatal(d["code"] == "MCPP_BUILD_DATABASE_PLAN_FAILED" && d["severity"] == "error")) << "C: " << d.dump();
        }
        std::ranges::sort(paths);
        expect(fatal(paths == std::vector<std::string>{"bad/mcpp.toml", "good/mcpp.toml"})) << "C: paths";
    };

    "788 emit defers an unbuilt host tool"_test[tag{"requires:gcc"}][tag{"requires:python3"}] = [] {
        // A: planning neither builds a dependency's host tool nor runs its build program's
        // check; it notes the tool as deferred. B: the build runs the blocking check, whose
        // failure it shows. C: once the tool is built, emit notes nothing.
        s::needs({"gcc", "python3"});
        const auto tmp = s::case_dir();
        s::project tool(tmp / "tool", {
            {"mcpp.toml", "[package]\nname    = \"tool\"\nversion = \"0.1.0\"\n\n[targets.t]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
            {"src/main.cpp", "int main() { return 0; }\n"},
            {"build.mcpp", R"(#include <string>
import mcpp;
int main() {
    const std::string root = mcpp::manifest_dir();
    mcpp::action a;
    a.id       = "gate";
    a.role     = "check";
    a.blocking = true;
    a.arg((root + "/check.sh").c_str())
     .output("${mcpp.out_dir}/gate.stamp")
     .submit();
}
)"},
        });
        s::write_executable(tool.at("check.sh"), "#!/usr/bin/env bash\necho \"the check says no\" >&2\nexit 1\n");
        s::project user(tmp / "user", {{"mcpp.toml", "[package]\nname    = \"user\"\nversion = \"0.1.0\"\n\n[dependencies]\ntool = { path = \"../tool\", tools = [\"t\"] }\n"},
                                       {"src/main.cpp", "int main() { return 0; }\n"}});
        auto r = user.mcpp({"emit", "build-database", "--format", "json"});
        expect(fatal(e2e::succeeded(r))) << "A: emit exited " << r.status << ", expected 0: " << r.err;
        auto e = s::json(r.out);
        expect(fatal(set_names(e) == std::vector<std::string>{"user"})) << "A: sets " << r.out;
        expect(fatal(e["diagnostics"].size() == 1u)) << "A: diagnostics " << e["diagnostics"].dump();
        const auto diag = e["diagnostics"][0];
        expect(fatal(diag["code"] == "MCPP_BUILD_DATABASE_HOST_TOOL_DEFERRED" && diag["severity"] == "note")) << "A: " << diag.dump();
        const auto message = diag["message"].get<std::string>();
        expect(fatal(message.contains("'t'") && message.contains("'tool'"))) << "A: " << message;
        expect(fatal(s::no_line(r.err, "Building.*host tool|the check says no"))) << "A: planning built the tool or ran its check: " << r.err;
        r = user.mcpp({"build"});
        expect(fatal(r.started && r.status != 0)) << "B: mcpp build succeeded despite the failing blocking check";
        expect(fatal(e2e::contains(s::text(r), "the check says no"))) << "B: the check's own failure is not on the build's output: " << s::text(r);
        s::write_executable(tool.at("check.sh"), "#!/usr/bin/env bash\nexit 0\n");
        r = user.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "C: the build with a passing check failed: " << s::text(r);
        r = user.mcpp({"emit", "build-database", "--format", "json"});
        expect(fatal(e2e::succeeded(r))) << "C: emit exited " << r.status << ", expected 0: " << r.err;
        expect(fatal(s::json(r.out)["diagnostics"].empty())) << "C: diagnostics " << r.out;
    };

    "789 emit describes a member past a failed build program"_test[tag{"requires:gcc"}][tag{"requires:python3"}] = [] {
        // A build program that exits 1 (its directives dropped) or does not compile (the
        // compiler's diagnostic carried) leaves its package described with one diagnostic
        // (exit 1); a package with nothing to describe has no data and a plan diagnostic.
        s::needs({"gcc", "python3"});
        const auto tmp = s::case_dir();
        s::project exits1(tmp / "exits1", {{"mcpp.toml", "[package]\nname    = \"exits1\"\nversion = \"0.1.0\"\n"},
                                           {"src/main.cpp", "int main() { return 0; }\n"},
                                           {"build.mcpp", "#include <cstdio>\nint main() { std::printf(\"mcpp:cxxflag=-DSHOULD_NOT_APPEAR\\n\"); return 1; }\n"}});
        auto r = exits1.mcpp({"emit", "build-database", "--format", "json"});
        expect(fatal(e2e::exited(r, 1))) << "A: exit status " << r.status << ", expected 1: " << r.err;
        auto e = s::json(r.out);
        nlohmann::json set;
        for (const auto& st : e["data"]["database"]["sets"]) if (st["name"] == "exits1") set = st;
        expect(fatal(!set.is_null())) << "A: no exits1 set";
        const auto unit = set["translation-units"][0];
        expect(fatal(unit["source"].get<std::string>().ends_with("main.cpp"))) << "A: " << unit.dump();
        const auto args = unit["arguments"].get<std::vector<std::string>>();
        expect(fatal(std::ranges::find(args, "-DSHOULD_NOT_APPEAR") == args.end())) << "A: the failed program's directive reached the unit";
        expect(fatal(e["diagnostics"].size() == 1u)) << "A: diagnostics " << e["diagnostics"].dump();
        auto diag = e["diagnostics"][0];
        expect(fatal(diag["code"] == "MCPP_BUILD_DATABASE_PROGRAM_FAILED" && diag["severity"] == "error" && diag["path"] == "build.mcpp")) << "A: " << diag.dump();

        s::project nocompile(tmp / "nocompile", {{"mcpp.toml", "[package]\nname    = \"nocompile\"\nversion = \"0.1.0\"\n"},
                                                 {"src/main.cpp", "int main() { return 0; }\n"},
                                                 {"build.mcpp", "int main() { this is not valid c++ }\n"}});
        r = nocompile.mcpp({"emit", "build-database", "--format", "json"});
        expect(fatal(e2e::exited(r, 1))) << "B: exit status " << r.status << ", expected 1: " << r.err;
        e = s::json(r.out);
        expect(fatal(std::ranges::contains(set_names(e), std::string("nocompile")))) << "B: no nocompile set";
        expect(fatal(e["diagnostics"].size() == 1u)) << "B: diagnostics " << e["diagnostics"].dump();
        diag = e["diagnostics"][0];
        expect(fatal(diag["code"] == "MCPP_BUILD_DATABASE_PROGRAM_FAILED" && diag["path"] == "build.mcpp")) << "B: " << diag.dump();
        // Not a bare exit-code message: an actual compiler diagnostic reached it.
        expect(fatal(diag["message"].get<std::string>().contains("error"))) << "B: " << diag["message"].dump();

        s::project libonly(tmp / "libonly", {{"mcpp.toml", "[package]\nname      = \"libonly\"\nversion   = \"0.1.0\"\nstandard  = \"c++23\"\n\n[targets.lib]\nkind = \"lib\"\n"},
                                             {"build.mcpp", "int main() { return 1; }\n"}});
        r = libonly.mcpp({"emit", "build-database", "--format", "json"});
        expect(fatal(e2e::exited(r, 1))) << "C: exit status " << r.status << ", expected 1: " << r.err;
        e = s::json(r.out);
        expect(fatal(!e.contains("data"))) << "C: the envelope has data: " << r.out;
        expect(fatal(e["diagnostics"].size() == 1u)) << "C: diagnostics " << e["diagnostics"].dump();
        diag = e["diagnostics"][0];
        expect(fatal(diag["code"] == "MCPP_BUILD_DATABASE_PLAN_FAILED" && diag["path"] == "mcpp.toml")) << "C: " << diag.dump();
    };
    "809 failed build program hides no device source"_test[tag{"requires:python3"}] = [] {
        // A build program that does not compile, in a package whose device sources a rule
        // module compiles: one program diagnostic, naming no device source.
        const auto tmp = s::case_dir();
        s::project(tmp / "rules", {{"mcpp.toml", "[package]\nname        = \"rules\"\nnamespace   = \"t809\"\nversion     = \"0.1.0\"\n\n[features]\ndefault = []\n\n"
                                                 "[features.x]\nsources           = [\"src/rules.cppm\"]\nrule_module       = \"t809.rules\"\ndevice_extensions = [\".dev\"]\n"},
                                   {"src/rules.cppm", "export module t809.rules;\nexport namespace t809::rules {\ninline bool noop() { return true; }\n}\n"}});
        s::project app(tmp / "app", {{"mcpp.toml", "[package]\nname    = \"app809\"\nversion = \"0.1.0\"\n\n[language]\nstandard = \"c++23\"\n\n"
                                                   "[build-dependencies]\nt809.rules = { path = \"../rules\", features = [\"x\"] }\n\n[build]\nsources = [\"src/*.cpp\", \"src/kernels/*.dev\"]\n"},
                                     {"src/main.cpp", "int main() { return 0; }\n"},
                                     {"src/kernels/k.dev", "not a real kernel\n"},
                                     {"build.mcpp", "int main() { this is not valid c++ }\n"}});
        auto r = app.mcpp({"emit", "build-database", "--format", "json"});
        expect(fatal(e2e::exited(r, 1))) << "A: exit status " << r.status << ", expected 1: " << r.err;
        const auto e = s::json(r.out);
        expect(fatal(e.contains("data"))) << "B: no data: " << r.out;
        expect(fatal(std::ranges::contains(set_names(e), std::string("app809")))) << "B: no app809 set";
        expect(fatal(e["diagnostics"].size() == 1u)) << "C: diagnostics " << e["diagnostics"].dump();
        const auto diag = e["diagnostics"][0];
        expect(fatal(diag["code"] == "MCPP_BUILD_DATABASE_PROGRAM_FAILED" && diag["severity"] == "error" && diag["path"] == "build.mcpp")) << "C: " << diag.dump();
        const auto message = diag["message"].get<std::string>();
        expect(fatal(!message.contains("k.dev") && !message.contains("device sources that no action compiles"))) << "D: " << message;
    };
    "731 planning children do not inherit the callers pipe"_test[tag{"requires:elf"}] = [] {
        // A build program that planning runs holds no descriptor of the pipe the caller reads
        // emit's output from.
        s::needs({"elf"});
        const auto tmp = s::case_dir();
        s::project app(tmp / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                                     {"src/main.cpp", "int main() { return 0; }\n"},
                                     {"build.mcpp", std::format(R"(import std;
import mcpp;
int main() {{
    std::ofstream out("{}");
    for (auto const& e : std::filesystem::directory_iterator("/proc/self/fd")) {{
        std::error_code ec;
        out << e.path().filename().string() << " "
            << std::filesystem::read_symlink(e.path(), ec).string() << "\n";
    }}
    return 0;
}}
)", (tmp / "fds.txt").string())}});
        // The caller's pipe is the script's shell pipeline, run as it ran it: the reader records
        // the pipe it reads (readlink /proc/self/fd/0).
        const auto sh = s::on_path("sh");
        expect(fatal(sh.has_value())) << "no sh";
        const auto line = std::format(R"("{}" emit build-database --format json 2> err.txt | {{ readlink /proc/self/fd/0 > "{}"; cat > "{}"; }})",
                                      s::mcpp_path(), (tmp / "reader.txt").string(), (tmp / "out.json").string());
        auto r = s::run(*sh, {"-c", line}, app.dir(), {{"MCPP_OFFLINE", "1"}, {"MCPP_HOME", s::shared_home().string()}});
        expect(fatal(fs::exists(tmp / "fds.txt") && fs::file_size(tmp / "fds.txt") > 0)) << "the build program did not run: " << (app.exists("err.txt") ? app.read("err.txt") : s::text(r));
        auto pipe = s::read_file(tmp / "reader.txt");
        pipe.erase(pipe.find_last_not_of("\r\n") + 1);
        expect(fatal(pipe.starts_with("pipe:"))) << "the reader's stdin is not a pipe: " << pipe;
        expect(fatal(s::lacks(s::read_file(tmp / "fds.txt"), " " + pipe))) << "a build program holds the caller's pipe " << pipe << ":\n" << s::read_file(tmp / "fds.txt");
    };
}};



}  // namespace

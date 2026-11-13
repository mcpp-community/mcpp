// The runner of a target: a declared runner and its fast path, --no-runner, a missing runner and
// an artifact this host cannot execute, named runners and the ones a dependency's features
// choose, --locked and the SBOM, and `run` with features and a profile (formerly tests/e2e/330,
// 333, 337).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

// `printf '\xff\xff' | dd of=<file> bs=1 seek=18 conv=notrunc`: the ELF's e_machine spoiled.
void spoil_machine(const fs::path& file) {
    std::fstream f(file, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(18);
    f.write("\xff\xff", 2);
}

// `od -An -tx1 -j18 -N2`: the two bytes at offset 18 are 0xff 0xff.
bool spoiled(const fs::path& file) {
    const auto bytes = s::read_file(file);
    return bytes.size() > 19 && static_cast<unsigned char>(bytes[18]) == 0xff && static_cast<unsigned char>(bytes[19]) == 0xff;
}

E2E runners{"runners", [] {
    "330 runner hosted targets"_test[tag{"requires:unix-shell"}] = [] {
        // A runner declared under [target.<host>] runs the artifact (argv[0] the artifact), on
        // the fast path too; --no-runner bypasses it with a note; a missing runner exits 2 naming
        // it and what was searched, for `run` and `test`; an artifact this host cannot execute
        // exits 126 (`run`) and 2 (`test`, its tests not run, in text and JSON) with the kernel's
        // answer and the shape of a runner declaration, and is not rebuilt; a misspelt key is
        // reported with the supported keys.
        auto p = s::project::scaffold("app");
        for (const auto& t : s::glob(p.at("tests"), "*.cpp")) fs::remove(t);
        p.write("src/main.cpp", "#include <cstdio>\nint main() { std::puts(\"ARTIFACT-RAN\"); return 0; }\n");
        p.write("tests/one.cpp", "int main() { return 0; }\n");
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "initial build: " << s::text(r);
        std::vector<std::string> triples;
        for (const auto& e : fs::directory_iterator(p.at("target")))
        if (!e.path().filename().string().starts_with(".")) triples.push_back(e.path().filename().string());
        std::ranges::sort(triples);
        expect(fatal(!triples.empty())) << "could not determine the host triple from target/";
        const auto host = triples.front();
        const auto log = s::case_dir() / "runner.log";
        s::write_executable(s::case_dir() / "runner.sh", "#!/bin/sh\nprintf '%s\\n' \"$@\" >> \"$RUNNER_LOG\"\nexec \"$@\"\n");
        const s::options o{.env = {{"RUNNER_LOG", log.string()}}};
        const auto base = p.read("mcpp.toml");
        p.write("mcpp.toml", base + std::format("\n[target.{}]\nrunner = [\"{}\"]\n", host, (s::case_dir() / "runner.sh").string()));
        auto ran = [&] { return fs::exists(log) && fs::file_size(log) > 0; };

        r = p.mcpp({"run"}, o);
        expect(fatal(e2e::succeeded(r))) << "run through runner: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "ARTIFACT-RAN"))) << "artifact output missing: " << s::text(r);
        expect(fatal(ran())) << "runner was not invoked (declared under [target." << host << "]): " << s::text(r);
        const auto exe = s::read_file(log).substr(0, s::read_file(log).find('\n'));
        expect(fatal(exe.ends_with("/bin/app"))) << "runner argv[0] is not the artifact: " << exe;
        std::ofstream{log, std::ios::trunc};
        r = p.mcpp({"run"}, o);
        expect(fatal(e2e::succeeded(r))) << "second run (fast path): " << s::text(r);
        expect(fatal(ran())) << "the run fast path bypassed the declared runner: " << s::text(r);
        std::ofstream{log, std::ios::trunc};
        r = p.mcpp({"run", "--no-runner"}, o);
        expect(fatal(e2e::succeeded(r))) << "--no-runner: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "ARTIFACT-RAN"))) << "--no-runner lost the program output: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "no-runner"))) << "--no-runner printed no note: " << s::text(r);
        expect(fatal(!ran())) << "--no-runner still invoked the runner";

        p.write("mcpp.toml", base + std::format("\n[target.{}]\nrunner = [\"mcpp-e2e-no-such-runner\"]\n", host));
        r = p.mcpp({"run"}, o);
        expect(fatal(e2e::exited(r, 2))) << "missing runner: want 2: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "runner 'mcpp-e2e-no-such-runner' for '" + host + "' was not found"))) << "missing runner not named: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "Searched:"))) << "search list missing: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), "ARTIFACT-RAN"))) << "artifact ran without its runner: " << s::text(r);
        r = p.mcpp({"test"}, o);
        expect(fatal(e2e::exited(r, 2))) << "test with missing runner: want 2: " << s::text(r);
        expect(fatal(s::has_line(s::text(r), R"(NOT RUN\. 0 passed; 0 failed; 1 not run \(runner 'mcpp-e2e-no-such-runner')"))) << "test summary with missing runner: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "one ... not run"))) << "per-test not-run line missing: " << s::text(r);
        r = p.mcpp({"test", "--no-runner"}, o);
        expect(fatal(e2e::succeeded(r))) << "test --no-runner: " << s::text(r);
        expect(fatal(s::has_line(s::text(r), R"(ok\. 1 passed; 0 failed; finished)"))) << "test --no-runner summary: " << s::text(r);

        if (s::on_linux()) {
            p.write("mcpp.toml", base + "\n");
            expect(fatal(s::lacks(p.read("mcpp.toml"), "runner"))) << "runner key still present after removal";
            r = p.mcpp({"build"}, o);
            expect(fatal(e2e::succeeded(r))) << "rebuild without runner: " << s::text(r);
            const auto bins = s::glob(p.at("target"), "*/*/bin/app");
            expect(fatal(!bins.empty())) << "no artifact at target/*/*/bin/app";
            const auto bin = bins.front();
            spoil_machine(bin);
            r = p.mcpp({"run"}, o);
            const auto t = s::text(r);
            expect(fatal(e2e::exited(r, 126))) << "unrunnable artifact: want 126: " << t;
            expect(fatal(e2e::contains(t, "this host cannot execute"))) << "unrunnable message missing: " << t;
            expect(fatal(e2e::contains(t, "Exec format error"))) << "the kernel's answer is missing: " << t;
            expect(fatal(e2e::contains(t, "[target." + host + "]"))) << "paste-able key missing: " << t;
            expect(fatal(e2e::contains(t, R"(runner = ["<program>", "{}"])"))) << "the runner declaration's shape is missing: " << t;
            expect(fatal(s::lacks(t, "qemu"))) << "the message names an emulator: " << t;
            expect(fatal(e2e::contains(t, "--no-runner"))) << "escape hatch not mentioned: " << t;
            expect(fatal(spoiled(bin))) << "the artifact was rebuilt under the run; the criterion measured nothing";
            for (int n = 1; n <= 2; ++n) {
                if (n == 2) p.write("tests/two.cpp", "int main() { return 0; }\n");
                for (const auto& f : s::glob(p.at("tests"), "*.cpp")) fs::last_write_time(f, fs::file_time_type::clock::now());
                r = p.mcpp({"test"}, o);
                expect(fatal(e2e::succeeded(r))) << "test build+run before patching (" << n << "): " << s::text(r);
                expect(fatal(s::has_line(s::text(r), std::format(R"(ok\. {} passed; 0 failed)", n)))) << "sanity: tests should pass unpatched (" << n << "): " << s::text(r);
                std::vector<fs::path> tbins;
                for (const auto& f : s::find_files(p.at("target"), "one|two"))
                    if ((fs::status(f).permissions() & fs::perms::owner_exec) != fs::perms::none) tbins.push_back(f);
                expect(fatal(tbins.size() == static_cast<std::size_t>(n))) << "expected " << n << " test binaries, found " << tbins.size();
                for (const auto& f : tbins) spoil_machine(f);
                r = p.mcpp({"test"}, o);
                const auto tt = s::text(r);
                expect(fatal(e2e::exited(r, 2))) << "test unrunnable (" << n << "): want 2: " << tt;
                expect(fatal(s::has_line(tt, std::format(R"(NOT RUN\. 0 passed; 0 failed; {} not run \(this host cannot execute {} artifacts: Exec format error)", n, host))))
                    << "test summary (" << n << "): " << tt;
                expect(fatal(s::count_lines(tt, " ... not run") == static_cast<std::size_t>(n))) << "per-test not-run lines (" << n << "): " << tt;
                expect(fatal(s::count_lines(tt, "cannot execute " + host + " artifacts") == 2u)) << "reason printed other than once + summary (" << n << "): " << tt;
                for (const auto& f : tbins) expect(fatal(spoiled(f))) << "test binary was rebuilt under the run (" << n << ")";
                r = p.mcpp({"test", "--message-format", "json"}, o);
                expect(fatal(e2e::exited(r, 2))) << "json exit (" << n << "): want 2";
                expect(fatal(s::count_lines(r.out, R"("status":"not_run")") == static_cast<std::size_t>(n))) << "json not_run records (" << n << "): " << r.out;
                expect(fatal(e2e::contains(r.out, std::format(R"("not_run":{},)", n)))) << "json summary not_run (" << n << "): " << r.out;
                expect(fatal(e2e::contains(r.out, R"("not_run_reason":"this host cannot execute)"))) << "json summary reason (" << n << "): " << r.out;
            }
        }
        p.write("mcpp.toml", p.read("mcpp.toml") + std::format("\n[target.{}]\nrunnerX = [\"x\"]\n", host));
        r = p.mcpp({"build"}, o);
        expect(fatal(e2e::contains(s::text(r), "unsupported key 'runnerX'"))) << "array typo not reported: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "Supported keys: cxx_runtime, linkage, min_api_level, platform, run_under, runner, sysroot, toolchain")))
            << "runner missing from the supported-keys list: " << s::text(r);
    };

    "333 named runners locked and sbom"_test[tag{"requires:gcc"}][tag{"requires:unix-shell"}][tag{"requires:python3"}] = [] {
        // Named runners ([target.*.runners]) run their own argv, an undeclared name is refused
        // listing those there are, the launch description names the custom profile and the
        // runners; --locked passes on a matching lock and names the package that moved; the SBOM
        // is CycloneDX 1.5 read from the lock; a dependency's features choose its default and
        // named runners, and a name the chosen environment lacks is refused.
        s::needs({"gcc", "unix-shell", "python3"});
        const auto work = s::case_dir();
        s::project p(work / "p", {{"src/main.cpp", "int main() { return 0; }\n"},
                                  {"mcpp.toml", "[package]\nname    = \"p\"\nversion = \"0.1.0\"\n\n[target.x86_64-linux-gnu.runners]\n"
                                                "flash   = [\"/bin/sh\", \"-c\", \"echo FLASHED $0\"]\nmonitor = [\"/bin/sh\", \"-c\", \"echo MONITORED $0\"]\n"}});
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "build: " << s::text(r);
        r = p.mcpp({"run", "--runner", "flash"});
        expect(fatal(e2e::succeeded(r))) << "run --runner flash exited non-zero: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "FLASHED"))) << "flash did not perform the declared argv: " << s::text(r);
        r = p.mcpp({"run", "--runner", "monitor"});
        expect(fatal(e2e::succeeded(r))) << "run --runner monitor exited non-zero: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "MONITORED"))) << "monitor did not perform its own slot: " << s::text(r);
        r = p.mcpp({"run", "--runner", "debug"});
        expect(fatal(failed(r))) << "an undeclared runner name succeeded";
        expect(fatal(e2e::contains(s::text(r), "no runner named"))) << "the refusal does not name the slot: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "Available:"))) << "the refusal does not list what this project has: " << s::text(r);
        p.write("mcpp.toml", p.read("mcpp.toml") + "\n[profile.inspect]\nopt = 0\ndebug = true\n");
        r = p.mcpp({"run", "--profile", "inspect", "--print-launch"});
        expect(fatal(e2e::succeeded(r))) << "custom-profile launch: " << r.err;
        const auto configuration = s::json(r.out)["data"]["configuration"];
        expect(fatal(configuration["profile"] == "inspect")) << configuration.dump();
        expect(fatal(configuration["runners"] == nlohmann::json::array({"flash", "monitor"}))) << configuration.dump();

        s::project q(work / "q", {{"mcpp.toml", "[package]\nname    = \"q\"\nversion = \"0.1.0\"\n\n[dependencies]\ncmdline = \"0.0.1\"\n"},
                                  {"src/main.cpp", "int main() { return 0; }\n"}});
        r = q.mcpp({"build"});
        if (!r) skip("cmdline@0.0.1 unavailable");
        expect(fatal(q.exists("mcpp.lock"))) << "no mcpp.lock after a build with a dependency";
        r = q.mcpp({"build", "--locked"});
        expect(fatal(e2e::succeeded(r))) << "--locked rejected a matching lock: " << s::text(r);
        const auto lock = q.read("mcpp.lock");
        auto moved = [&](std::string_view to) { return std::regex_replace(lock, std::regex{R"(version = "0\.0\.1")"}, std::format("version = \"{}\"", to)); };
        q.write("mcpp.lock", moved("9.9.9"));
        r = q.mcpp({"build", "--locked"});
        expect(fatal(failed(r))) << "--locked accepted a lock that does not describe this resolution";
        expect(fatal(e2e::contains(s::text(r), "differs from mcpp.lock"))) << "the refusal does not say what is wrong: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "9.9.9 -> 0.0.1"))) << "the refusal does not name which package moved, and to what: " << s::text(r);
        q.write("mcpp.lock", lock);
        r = q.mcpp({"emit", "sbom", "-o", "sbom.json"});
        expect(fatal(e2e::succeeded(r))) << "mcpp emit sbom: " << s::text(r);
        auto d = s::json_file(q.at("sbom.json"));
        expect(fatal(d["bomFormat"] == "CycloneDX")) << d["bomFormat"].dump();
        expect(fatal(d["specVersion"] == "1.5")) << d["specVersion"].dump();
        expect(fatal(d["metadata"]["component"]["name"] == "q")) << d["metadata"]["component"].dump();
        bool named = false;
        for (const auto& c : d["components"]) {
            named = named || c["name"] == "cmdline";
            // A component with no licence says so rather than omit the field.
            expect(fatal(c.contains("licenses"))) << c["name"].dump();
            expect(fatal(c["purl"].get<std::string>().starts_with("pkg:mcpp/"))) << c["purl"].dump();
        }
        expect(fatal(named)) << "cmdline is not a component";
        q.write("mcpp.lock", moved("7.7.7"));
        r = q.mcpp({"emit", "sbom", "-o", "sbom2.json"});
        expect(fatal(e2e::succeeded(r))) << "mcpp emit sbom (2): " << s::text(r);
        std::vector<std::string> versions;
        for (const auto& c : s::json_file(q.at("sbom2.json"))["components"])
            if (c["name"] == "cmdline") versions.push_back(c["version"]);
        expect(fatal(versions == std::vector<std::string>{"7.7.7"})) << "sbom re-resolved instead of reading the lock";
        q.write("mcpp.lock", lock);

        s::project(work / "bsp", {{"mcpp.toml", "[package]\nname    = \"demo-board-rt\"\nversion = \"0.1.0\"\n\n[features]\ndefault  = [\"emulator\"]\nemulator = []\nhardware = []\n"},
                                  {"build.mcpp", R"(import mcpp;
import std;
int main() {
    if (mcpp::has_feature("hardware")) {
        for (auto a : {"/bin/sh", "-c", "echo PROBE-RUN $0"}) mcpp::runner(a);
        for (auto a : {"/bin/sh", "-c", "echo GDBSERVER $0"}) mcpp::runner("debug", a);
        mcpp::run_exclusive();
    } else {
        for (auto a : {"/bin/sh", "-c", "echo EMULATOR-RUN $0"}) mcpp::runner(a);
    }
    return 0;
}
)"},
                                  {"src/board.cppm", "export module demo_board;\n"}});
        s::project consumer(work / "consumer", {{"src/main.cpp", "int main() { return 0; }\n"}});
        auto manifest = [&](std::string_view extra) {
            consumer.write("mcpp.toml", std::format("[package]\nname = \"consumer\"\nversion = \"0.1.0\"\n[dependencies]\ndemo-board-rt = {{ path = \"../bsp\"{} }}\n", extra));
            fs::remove_all(consumer.at("target"));
        };
        manifest("");
        r = consumer.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "run under the default feature: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "EMULATOR-RUN"))) << "default feature did not select the emulator argv: " << s::text(r);
        manifest(R"(, features = ["hardware"])");
        r = consumer.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "run under features=[hardware]: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "PROBE-RUN"))) << "the feature did not move the default runner: " << s::text(r);
        r = consumer.mcpp({"run", "--runner", "debug"});
        expect(fatal(e2e::succeeded(r))) << "debug under features=[hardware]: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "GDBSERVER"))) << "debug slot did not arrive: " << s::text(r);
        manifest("");
        r = consumer.mcpp({"run", "--runner", "debug"});
        expect(fatal(failed(r))) << "--runner debug succeeded under emulator, which supplies none";
    };

    "337 run takes features and profile"_test[tag{"requires:gcc"}][tag{"requires:unix-shell"}] = [] {
        // `run --features` and `--release` take effect, alone and together, and a plain run
        // after them is plain again; the fast path's record keeps the features of the last build.
        s::needs({"gcc", "unix-shell"});
        s::project p({{"mcpp.toml", "[package]\nname    = \"featrun\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/main.cpp\"]\n\n[features]\nloud = { defines = [\"LOUD=1\"] }\n"},
                      {"src/main.cpp", "#include <cstdio>\nint main() {\n#ifdef LOUD\n    std::printf(\"LOUD\\n\");\n#else\n    std::printf(\"quiet\\n\");\n#endif\n"
                                       "#ifdef NDEBUG\n    std::printf(\"release\\n\");\n#else\n    std::printf(\"dev\\n\");\n#endif\n    return 0;\n}\n"}});
        auto run = [&](std::vector<std::string> args) {
            args.insert(args.begin(), "run");
            return s::text(p.mcpp(args));
        };
        auto t = run({});
        expect(fatal(e2e::contains(t, "quiet"))) << "a plain run should not have the feature: " << t;
        expect(fatal(e2e::contains(t, "dev"))) << "a plain run should be the dev profile: " << t;
        t = run({"--features", "loud"});
        expect(fatal(e2e::contains(t, "LOUD"))) << "--features was accepted and ignored (the fast path took a stale entry): " << t;
        expect(fatal(s::lacks(t, "quiet"))) << "--features did not take effect: " << t;
        t = run({"--release"});
        expect(fatal(e2e::contains(t, "release"))) << "--release was accepted and ignored: " << t;
        t = run({"--features", "loud", "--release"});
        expect(fatal(e2e::contains(t, "LOUD"))) << "--features lost when combined with --release: " << t;
        expect(fatal(e2e::contains(t, "release"))) << "--release lost when combined with --features: " << t;
        t = run({});
        expect(fatal(e2e::contains(t, "quiet"))) << "a plain run inherited the previous --features: " << t;
        expect(fatal(e2e::contains(t, "dev"))) << "a plain run inherited the previous --release: " << t;
        auto mru_features = [&] {
            const auto line = s::last_starting(p.exists("target/.build_cache") ? p.read("target/.build_cache") : "", "features=");
            return line.empty() ? line : line.substr(std::string_view("features=").size());
        };
        fs::remove_all(p.at("target"));
        p.mcpp({"build"});
        expect(fatal(mru_features().empty())) << "a plain build recorded features '" << mru_features() << "'";
        p.mcpp({"build", "--features", "loud"});
        expect(fatal(mru_features() == "loud")) << "--features loud recorded '" << mru_features() << "', expected loud";
        p.mcpp({"build"});
        expect(fatal(mru_features().empty())) << "a plain build after --features matched an entry recorded as '" << mru_features() << "'";
    };
    "656 run hands the distributable to the runner"_test[tag{"requires:elf"}][tag{"requires:gcc"}] = [] {
        // `run --pkg-format <f>` hands the runner the format's terminal artifact (not an
        // intermediate one) and reports it as packed; a plain run hands it bin/app; an unknown
        // format is refused naming what is available; --pkg-format with --no-runner is refused
        // before the program runs; a plain build afterwards has a plain graph (dist=none).
        s::needs({"elf", "gcc"});
        s::project p({{"mcpp.toml", "[package]\nname        = \"app\"\nversion     = \"1.0.0\"\ndescription = \"a program with one dispatched distributable\"\n"
                                    "license     = \"Apache-2.0\"\nauthors     = [\"Ada <ada@example.org>\"]\nrepo        = \"https://example.org/app\"\n\n"
                                    "[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                      {"src/main.cpp", "#include <cstdio>\nint main() { std::puts(\"1-2-3\"); return 0; }\n"},
                      {"build.mcpp", R"(import mcpp;
#include <string>
#include <string_view>
int main() {
    mcpp::provides_pack_format("blob");
    if (std::string_view(mcpp::pack_format()) != "blob") return 0;

    const std::string root = mcpp::manifest_dir();
    const std::string out  = std::string(mcpp::out_dir()) + "/app.blob";
    mcpp::action a;
    a.id          = "blob";
    a.role        = "artifact";
    a.description = "blob";
    a.arg((root + "/copy.sh").c_str())
     .arg("${mcpp.target_file:app}")
     .arg(out.c_str())
     .input("${mcpp.target_file:app}")
     .output(out.c_str())
     .submit();
    const std::string fin = std::string(mcpp::out_dir()) + "/app.final";
    mcpp::action b;
    b.id          = "final";
    b.role        = "artifact";
    b.description = "final";
    b.arg((root + "/copy.sh").c_str())
     .arg(out.c_str())
     .arg(fin.c_str())
     .input(out.c_str())
     .output(fin.c_str())
     .submit();
    return 0;
}
)"}});
        s::write_executable(p.at("copy.sh"), "#!/usr/bin/env bash\nset -e\ncp \"$1\" \"$2\"\nchmod +x \"$2\"\n");
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "the initial build failed: " << s::text(r);
        std::vector<std::string> triples;
        for (const auto& e : fs::directory_iterator(p.at("target")))
        if (!e.path().filename().string().starts_with(".")) triples.push_back(e.path().filename().string());
        std::ranges::sort(triples);
        const auto host = triples.front();
        const auto graphs = s::glob(p.at("target"), "*/*/build.ninja");
        expect(fatal(!graphs.empty())) << "no build.ninja after the initial build";
        const auto g0 = graphs.front();
        s::write_executable(s::case_dir() / "runner.sh", "#!/bin/sh\nprintf 'RUNNER: %s\\n' \"$1\"\nexec \"$@\"\n");
        p.write("mcpp.toml", p.read("mcpp.toml") + std::format("\n[target.{}]\nrunner = [\"{}\"]\n", host, (s::case_dir() / "runner.sh").string()));
        r = p.mcpp({"run", "--pkg-format", "blob"});
        auto t = s::text(r);
        expect(fatal(e2e::succeeded(r))) << "mcpp run --pkg-format blob failed: " << t;
        expect(fatal(s::has_line(t, R"(RUNNER: .*/app\.final$)"))) << "the runner's operand was not the terminal artifact app.final: " << t;
        expect(fatal(s::no_line(t, R"(RUNNER: .*/app\.blob$)"))) << "the runner received the intermediate app.blob: " << t;
        expect(fatal(s::has_line(t, R"(Packed .*app\.final)"))) << "the Packed line does not name the terminal artifact: " << t;
        expect(fatal(s::no_line(t, R"(Packed .*app\.blob)"))) << "the intermediate app.blob was reported as Packed: " << t;
        expect(fatal(e2e::contains(t, "1-2-3"))) << "the program's marker did not print through the runner: " << t;
        r = p.mcpp({"run"});
        t = s::text(r);
        expect(fatal(e2e::succeeded(r))) << "mcpp run failed: " << t;
        expect(fatal(s::has_line(t, "RUNNER: .*/bin/app$"))) << "a plain run's operand was not bin/app: " << t;
        expect(fatal(e2e::contains(t, "1-2-3"))) << "a plain run lost the program's marker: " << t;
        r = p.mcpp({"run", "--pkg-format", "bogus"});
        t = s::text(r);
        expect(fatal(failed(r))) << "mcpp run --pkg-format bogus was accepted: " << t;
        expect(fatal(e2e::contains(t, "unknown --pkg-format 'bogus'"))) << "the refusal does not name the value: " << t;
        expect(fatal(s::has_line(t, "available in this build:.*blob"))) << "the refusal does not name what is available: " << t;
        r = p.mcpp({"run", "--pkg-format", "blob", "--no-runner"});
        t = s::text(r);
        expect(fatal(failed(r))) << "--format with --no-runner was accepted: " << t;
        expect(fatal(e2e::contains(t, "--pkg-format and --no-runner cannot be combined"))) << "the refusal does not name the pairing: " << t;
        expect(fatal(s::lacks(t, "1-2-3"))) << "the program ran despite the refusal: " << t;
        for (const auto& f : s::find_files(p.at("target"), R"(app\.blob)")) fs::remove(f);
        r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "the build after run --pkg-format failed: " << s::text(r);
        expect(fatal(fs::is_regular_file(g0))) << "the plain build's own build.ninja disappeared";
        const auto graph = s::read_file(g0);
        std::istringstream lines(graph);
        std::string second;
        std::getline(lines, second);
        std::getline(lines, second);
        std::smatch m;
        const auto dist = std::regex_search(second, m, std::regex{";dist=([^;]*)"}) ? m[1].str() : second;
        expect(fatal(dist == "none")) << "the graph after run --pkg-format is not dist=none: " << second;
        expect(fatal(s::lacks(graph, "blob"))) << "a plain build's graph still carries the blob edge";
        expect(fatal(s::find_files(p.at("target"), R"(app\.blob)").empty())) << "a plain build regenerated the distributable";
    };

    "684 a runner supplied twice is refused"_test[tag{"requires:unix-shell"}] = [] {
        // A runner name a dependency's build program supplies alone is accepted (3); supplied
        // by the project's build program too, it is refused naming both (1); the manifest's
        // [target.<row>.runners] settles it and is used (2).
        const auto tmp = s::case_dir();
        for (std::string_view r : {"A", "B", "C"})
            s::write_executable(tmp / std::format("run-{}.sh", r), std::format("#!/bin/sh\nprintf \"RUNNER-{}\\n\"\nexec \"$@\"\n", r));
        s::project(tmp / "dep", {{"mcpp.toml", "[package]\nnamespace = \"demo\"\nname      = \"dep\"\nversion   = \"0.1.0\"\n[targets.dep]\nkind = \"lib\"\n"},
                                 {"src/dep.cppm", "export module dep;\nexport int dep_anchor() { return 0; }\n"},
                                 {"build.mcpp", std::format("import std;\nimport mcpp;\nint main() {{\n    mcpp::runner(\"app\", \"{}\");\n    return 0;\n}}\n", (tmp / "run-A.sh").string())}});
        s::project app(tmp / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[dependencies]\ndemo.dep = { path = \"../dep\" }\n"},
                                     {"src/main.cpp", "import dep;\nint main() { return dep_anchor(); }\n"}});
        auto r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "3: a runner only the dependency supplies was refused: " << s::text(r);
        app.write("build.mcpp", std::format("import std;\nimport mcpp;\nint main() {{\n    mcpp::runner(\"app\", \"{}\");\n    return 0;\n}}\n", (tmp / "run-B.sh").string()));
        r = app.mcpp({"build"});
        expect(fatal(failed(r))) << "1: a runner supplied twice was accepted";
        expect(fatal(e2e::contains(s::text(r), "the dependency 'dep' and this project's build program both supply a runner named 'app'")))
            << "1: the refusal does not name the dependency and the runner: " << s::text(r);
        std::string row;
        const auto arch = s::run(*s::on_path("uname"), {"-m"}).out;
        const auto machine = arch.substr(0, arch.find_first_of("\r\n")) == "arm64" ? std::string("aarch64") : arch.substr(0, arch.find_first_of("\r\n"));
        if (s::on_linux()) row = machine + "-linux-gnu";
        else if (s::on_macos()) row = machine + "-macos";
        else skip("no canonical host row for this system");
        app.write("mcpp.toml", app.read("mcpp.toml") + std::format("\n[target.{}.runners]\napp = [\"{}\"]\n", row, (tmp / "run-C.sh").string()));
        r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "2: the manifest's runner did not settle it: " << s::text(r);
        r = app.mcpp({"run", "--runner", "app"});
        expect(fatal(e2e::contains(s::text(r), "RUNNER-C"))) << "2: the manifest's runner was not used: " << s::text(r);
    };
}};


}  // namespace

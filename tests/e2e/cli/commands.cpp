// The command line: --help and --version, the error paths of a manifest and of a source, usage
// errors kept off standard output, the exit-code contract, --jobs and the `--` boundary,
// --toolchain on build, run, test and pack (formerly tests/e2e/01, 05, 202, 231, 329, 671).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

std::string lower(std::string_view text) {
    std::string out(text);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// The first `Resolved <spec> ` of a build's output (`sed -n 's/.*Resolved \([^ ]*\) .*/\1/p' | head -1`).
std::string resolved(std::string_view text) {
    std::match_results<std::string_view::const_iterator> m;
    static const std::regex re{"Resolved ([^ \n]*) "};
    return std::regex_search(text.begin(), text.end(), m, re) ? m[1].str() : std::string();
}

// The first build.ninja under the project's target, outside the build programs' tree.
std::optional<fs::path> graph_of(const s::project& p) {
    auto all = s::find_files(p.at("target"), R"(build\.ninja)");
    std::ranges::sort(all);
    for (const auto& f : all)
        if (!f.generic_string().contains("/.build-mcpp/")) return f;
    return std::nullopt;
}

std::string graph_text(const s::project& p) {
    const auto g = graph_of(p);
    expect(fatal(g.has_value())) << "no build.ninja generated";
    return s::read_file(*g);
}

E2E commands{"commands", [] {
    "01 help and version"_test = [] {
        // --version is `mcpp <the manifest's version>`; --help names the commands; an unknown
        // command exits 127.
        const fs::path here = fs::current_path();
        auto r = s::mcpp({"--version"}, here);
        const auto out = r.out.substr(0, r.out.find('\n'));
        expect(fatal(e2e::matches(out, R"(^mcpp [0-9]+\.[0-9]+\.[0-9]+)"))) << "Bad version output: " << out;
        std::string expected;
        {
            std::istringstream in(s::read_file(s::repository() / "mcpp.toml"));
            static const std::regex v{R"(^version[ \t]*=[^"]*"([^"]*)\")"};
            for (std::string l; std::getline(in, l);) {
                std::smatch m;
                if (std::regex_search(l, m, v)) { expected = m[1]; break; }
            }
        }
        expect(fatal(!expected.empty())) << "Failed to read mcpp.toml package version";
        expect(fatal(out.starts_with("mcpp " + expected))) << "Version mismatch: mcpp.toml=" << expected << ", --version='" << out << "'";
        r = s::mcpp({"--help"}, here);
        for (std::string_view w : {"Usage:", "mcpp new", "mcpp build", "--configure-only"})
            expect(fatal(e2e::contains(r.out, w))) << "--help missing '" << w << "'";
        r = s::mcpp({"doesnotexist"}, here);
        expect(fatal(e2e::exited(r, 127))) << "expected exit 127 for unknown command";
    };

    "05 errors"_test = [] {
        // No manifest, a missing version, a conditional import and a header unit are refused,
        // each naming its cause; a module named outside the package's prefix builds.
        // A directory with no manifest above it either: the case's own directory lies inside the
        // repository, whose mcpp.toml mcpp would find.
        const auto empty = s::case_home() / "empty";
        fs::create_directories(empty);
        auto r = s::mcpp({"build"}, empty);
        expect(fatal(failed(r))) << "expected failure but build succeeded";
        expect(fatal(e2e::contains(s::text(r), "no mcpp.toml"))) << "wrong error: " << s::text(r);

        s::project bad(s::case_dir() / "bad-manifest", {{"mcpp.toml", R"([package]
name = "bad"
[language]
[modules]
sources = ["src/**/*.cppm"]
[targets.bad]
kind = "bin"
main = "src/main.cpp"
)"}});
        r = bad.mcpp({"build"});
        expect(fatal(failed(r))) << "expected failure";
        expect(fatal(e2e::contains(s::text(r), "package.version"))) << "wrong error: " << s::text(r);

        auto cond = s::project::scaffold("bad-cond");
        cond.write("src/main.cpp", "import std;\n#ifdef WANT_FOO\nimport foo;\n#endif\nint main() {}\n");
        r = cond.mcpp({"build"});
        expect(fatal(failed(r))) << "expected failure";
        expect(fatal(e2e::contains(s::text(r), "conditional preprocessor"))) << "wrong error: " << s::text(r);

        auto header = s::project::scaffold("bad-header");
        header.write("src/main.cpp", "import std;\nimport \"header.h\";\nint main() {}\n");
        r = header.mcpp({"build"});
        expect(fatal(failed(r))) << "expected failure";
        expect(fatal(e2e::contains(s::text(r), "header units"))) << "wrong error: " << s::text(r);

        auto naming = s::project::scaffold("naming-ok");
        auto manifest = naming.read("mcpp.toml");
        const std::string from = "name        = \"naming-ok\"";
        if (auto at = manifest.find(from); at != std::string::npos)
            manifest.replace(at, from.size(), "name        = \"myorg.something\"");
        naming.write("mcpp.toml", manifest);
        naming.write("src/foo.cppm", "export module differentprefix;\nimport std;\n");
        r = naming.mcpp({"build"});
        expect(e2e::succeeded(r)) << "expected success but build failed\n" << s::text(r);
    };

    "202 machine output contract"_test = [] {
        // A usage error writes nothing to standard output, says something on standard error and
        // exits 2; the supported formats are JSON envelopes on standard output.
        // In a project of its own: the case's directory lies inside the repository, and `pack`
        // builds the project it finds before it reads --pkg-format.
        s::project own({{"mcpp.toml", "[package]\nname = \"usage\"\nversion = \"0.1.0\"\n"}, {"src/main.cpp", "int main() {}\n"}});
        const fs::path here = own.dir();
        const std::vector<std::pair<std::string, std::vector<std::string>>> usage{
            {"unknown option (cache list)", {"cache", "list", "--no-such-option"}},
            {"unknown option (self env)", {"self", "env", "--no-such-option"}},
            {"unknown option (top level)", {"--no-such-option"}},
            {"unsupported value (self env)", {"self", "env", "--format", "yaml"}},
            {"unsupported value (cache list)", {"cache", "list", "--format", "yaml"}},
            {"unsupported value (pack)", {"pack", "--pkg-format", "bogus"}},
        };
        for (const auto& [what, args] : usage) {
            auto r = s::mcpp(args, here);
            expect(r.out.empty()) << what << " wrote to stdout: " << r.out.substr(0, 70);
            expect(!r.err.empty()) << what << " said nothing on stderr";
            expect(e2e::exited(r, 2)) << what;
        }
        for (const std::vector<std::string> cmd : {std::vector<std::string>{"self", "env"}, std::vector<std::string>{"cache", "list"}}) {
            auto args = cmd;
            args.insert(args.end(), {"--format", "json"});
            auto r = s::mcpp(args, here);
            expect(e2e::succeeded(r)) << cmd[0] << " " << cmd[1] << " --format json exited non-zero";
            expect(e2e::contains(r.out, "\"schemaVersion\"")) << cmd[0] << " " << cmd[1] << " --format json has no schemaVersion";
            expect(e2e::contains(r.out, "\"kind\"")) << cmd[0] << " " << cmd[1] << " --format json has no kind";
        }
    };

    "231 jobs option"_test = [] {
        // --jobs N and auto build, a bad value warns and builds, `--` keeps -j for the program;
        // --toolchain selects one build's toolchain over the manifest; the split schedule is the
        // manifest key's and the variable's, emits its edges and rebuilds nothing.
        const auto llvm = s::llvm_version();
        s::project p({{"mcpp.toml", std::format(R"([package]
name    = "jobsopt"
version = "0.1.0"

[toolchain]
default = "gcc@16.1.0"
macos   = "llvm@{0}"
windows = "llvm@{0}"
)", llvm)},
                      {"src/echo.cppm", "module;\n#include <cstdio>\nexport module jobsopt.echo;\nexport void echo_arg(const char* s) { std::printf(\"%s\\n\", s); }\n"},
                      {"src/main.cpp", "import jobsopt.echo;\nint main(int argc, char** argv) {\n    for (int i = 1; i < argc; ++i) echo_arg(argv[i]);\n}\n"}});
        auto r = p.mcpp({"build", "--release", "--jobs", "2"});
        expect(fatal(e2e::succeeded(r))) << "--jobs 2 failed:\n" << s::text(r);
        r = p.mcpp({"build", "--release", "--jobs", "auto"});
        expect(fatal(e2e::succeeded(r))) << "--jobs auto failed:\n" << s::text(r);
        expect(fatal(s::lacks(lower(s::text(r)), "invalid job count"))) << "'auto' was rejected as an invalid job count";
        r = p.mcpp({"build", "--release", "--jobs", "bogus"});
        expect(fatal(e2e::succeeded(r))) << "a bad --jobs value should warn, not fail the build:\n" << s::text(r);
        expect(fatal(s::contains_nocase(s::text(r), "invalid job count"))) << "a bad --jobs value was accepted silently";
        r = p.mcpp({"run", "--", "-j", "bogus"});
        expect(fatal(e2e::succeeded(r))) << "run with trailing program args failed:\n" << s::text(r);
        expect(fatal(s::has_line(s::text(r), "^-j$"))) << "'-j' did not reach the program";
        expect(fatal(s::has_line(s::text(r), "^bogus$"))) << "'bogus' did not reach the program";
        expect(fatal(s::lacks(lower(s::text(r)), "invalid job count"))) << "mcpp consumed a flag that belonged to the program";

        const auto own = resolved(s::text(p.mcpp({"build", "--release"})));
        expect(fatal(!own.empty())) << "could not learn this platform's toolchain";
        r = p.mcpp({"build", "--release", "--toolchain", own});
        expect(fatal(e2e::succeeded(r))) << "--toolchain " << own << " failed:\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "Resolved " + own))) << "--toolchain did not reach toolchain resolution:\n" << s::text(r);
        if (s::on_linux()) {
            r = p.mcpp({"build", "--release", "--toolchain", "llvm@" + llvm});
            expect(fatal(e2e::contains(s::text(r), "Resolved llvm@" + llvm))) << "--toolchain lost to the manifest pin:\n" << s::text(r);
        }

        (void)p.mcpp({"build", "--release"});
        expect(fatal(e2e::contains(graph_text(p), "schedule=none"))) << "default should not use the split schedule";
        p.remove("target");
        const auto manifest = p.read("mcpp.toml");
        p.write("mcpp.toml", manifest + "\n[build]\nbmi_schedule = \"on\"\n");
        r = p.mcpp({"build", "--release"});
        expect(fatal(e2e::succeeded(r))) << "bmi_schedule = \"on\" failed to build:\n" << s::text(r);
        expect(fatal(e2e::matches(graph_text(p), "schedule=(detach-codegen|two-phase)"))) << "[build] bmi_schedule = \"on\" did not select a split schedule";
        p.write("mcpp.toml", manifest);

        p.remove("target");
        const s::options on{.env = {{"MCPP_BMI_SCHEDULE", "on"}}};
        r = p.mcpp({"build", "--release"}, on);
        expect(fatal(e2e::succeeded(r))) << "schedule=on failed to build:\n" << s::text(r);
        const auto graph = graph_text(p);
        expect(fatal(e2e::matches(graph, "schedule=(detach-codegen|two-phase|none)"))) << "graph does not declare its schedule";
        if (graph.contains("schedule=detach-codegen")) {
            expect(fatal(e2e::contains(graph, ": cxx_module_bmi "))) << "graph declares detach-codegen but emits no BMI edge";
        } else if (graph.contains("schedule=two-phase")) {
            expect(fatal(e2e::contains(graph, ": cxx_precompile "))) << "graph declares two-phase but emits no BMI edge";
            expect(fatal(e2e::contains(graph, ": cxx_module_object "))) << "graph declares two-phase but emits no object edge";
        }
        if (e2e::matches(graph, "schedule=(detach-codegen|two-phase)")) {
            std::this_thread::sleep_for(std::chrono::seconds{1});
            const auto mark = fs::file_time_type::clock::now();
            (void)p.mcpp({"build", "--release"}, on);
            std::vector<std::string> rebuilt;
            for (const auto& f : s::find_files(p.at("target"), R"(.*\.(o|pcm|gcm))"))
                if (fs::last_write_time(f) > mark) rebuilt.push_back(f.string());
            expect(rebuilt.empty()) << "second build under the split schedule rebuilt:" << rebuilt;
        }
    };

    "329 exit codes match the contract"_test[s::unix_shell] = [] {
        // 0 success, 2 a usage error (nothing on stdout), 127 an unknown command, 1 a command that
        // ran and failed (stderr says why), and a rejected descriptor's envelope on stdout.
        s::project p({{"split.lua", R"(package = {
    name      = "acme.sub.widget",
    namespace = "acme",
}
xpm = { linux = { ["1.0.0"] = { url = "https://example.invalid/w.tar.gz", sha256 = "0" } } }
)"}});
        auto r = p.mcpp({"self", "env", "--format", "json"});
        expect(fatal(e2e::exited(r, 0))) << "self env --format json";
        expect(fatal(e2e::contains(r.out, "\"schemaVersion\""))) << "no envelope on stdout";
        r = p.mcpp({"self", "env", "--format", "yaml"});
        expect(fatal(e2e::exited(r, 2))) << "self env --format yaml";
        expect(fatal(r.out.empty())) << "a usage error wrote to stdout: " << r.out;
        r = p.mcpp({"definitely-not-a-subcommand-540"});
        expect(fatal(e2e::exited(r, 127))) << "an unknown subcommand";
        r = p.mcpp({"index"});
        expect(fatal(e2e::exited(r, 2))) << "a command without its subcommand";
        expect(fatal(e2e::contains(r.err, "a subcommand is required"))) << r.err;
        expect(fatal(r.out.empty())) << "a usage error wrote to stdout: " << r.out;
        r = p.mcpp({"xpkg", "parse", "./no-such-descriptor.lua", "--format", "json"});
        expect(fatal(e2e::exited(r, 1))) << "xpkg parse on a missing file";
        expect(fatal(!r.err.empty())) << "a non-zero exit left stderr empty (SPEC-003 §3.5)";
        r = p.mcpp({"xpkg", "parse", "./split.lua", "--format", "json"});
        expect(fatal(e2e::exited(r, 1))) << "a rejected descriptor should exit 1";
        expect(fatal(e2e::contains(r.out, "\"schemaVersion\""))) << "exit 1 with --format json must still carry the envelope on stdout";
    };

    "671 toolchain is an option of run test and pack"_test = [] {
        // run, test and pack hand --toolchain to resolution; the default toolchain named with it
        // builds, runs and tests; a second installed llvm compiles the tests; a refused spec is
        // credited to --toolchain.
        const auto llvm = s::llvm_version();
        auto p = s::project::scaffold("tcopt");
        for (const std::vector<std::string> words : {std::vector<std::string>{"run"}, {"test"}, {"pack", "--pkg-format", "dir"}}) {
            auto args = words;
            args.insert(args.end(), {"--toolchain", "nosuch@0.0.1"});
            auto r = p.mcpp(args);
            expect(fatal(failed(r))) << "'mcpp " << words[0] << " --toolchain nosuch@0.0.1' succeeded";
            expect(fatal(s::lacks(s::text(r), "unknown option"))) << "'mcpp " << words[0] << "' refused --toolchain as an unknown option";
            expect(fatal(e2e::contains(s::text(r), "unknown toolchain 'nosuch'"))) << "'mcpp " << words[0] << "' did not hand the value to toolchain resolution\n" << s::text(r);
        }
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "the initial build failed\n" << s::text(r);
        const auto own = resolved(s::text(r));
        expect(fatal(!own.empty())) << "could not learn this host's toolchain from the build";
        r = p.mcpp({"test", "--toolchain", own});
        expect(fatal(e2e::succeeded(r))) << "mcpp test --toolchain " << own << " failed\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "test result ok"))) << "mcpp test --toolchain " << own << " did not pass";
        r = p.mcpp({"run", "--toolchain", own});
        expect(fatal(e2e::succeeded(r))) << "mcpp run --toolchain " << own << " failed\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "Hello from tcopt"))) << "mcpp run --toolchain " << own << " did not run the program";

        const auto store = s::shared_home() / "registry/data/xpkgs/xim-x-llvm" / llvm / "bin";
        if (own.starts_with("llvm@")) {
            std::println("NOT MEASURED: the default toolchain is already {}", own);
        } else if (fs::exists(store / "clang++") || fs::exists(store / "clang++.exe")) {
            r = p.mcpp({"test", "--toolchain", "llvm@" + llvm});
            expect(fatal(e2e::succeeded(r))) << "mcpp test --toolchain llvm@" << llvm << " failed\n" << s::text(r);
            expect(fatal(e2e::contains(s::text(r), "Resolved llvm@" + llvm))) << "the test build did not resolve llvm@" << llvm;
            bool compiles = false;
            for (const auto& g : s::glob(p.at("target"), "*/*/build.ninja"))
                compiles = compiles || s::read_file(g).contains(std::format("xim-x-llvm/{}/bin/clang++", llvm));
            expect(fatal(compiles)) << "no build graph compiles with the llvm payload's clang++";
        } else {
            std::println("NOT MEASURED: llvm@{} is not installed in this home", llvm);
        }

        r = p.mcpp({"build", "--toolchain", "nosuch@1.0"});
        expect(fatal(failed(r))) << "an unknown toolchain from --toolchain was accepted";
        expect(fatal(s::has_line(s::text(r), "^error: --toolchain = 'nosuch@1\\.0'"))) << "the refusal does not name --toolchain as where the value was written\n" << s::text(r);
        expect(fatal(s::lacks(s::text(r), "[toolchain]."))) << "the refusal credits a manifest key with a command-line value";
    };
}};

}  // namespace

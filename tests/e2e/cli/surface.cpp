// The commands and flags of the surface that no other case reaches (design 2026-11-20 §5.16): each
// is run here once, in a home of the case, and answers as its help states. The surface's ledger
// (tests/e2e/mtest.toml) counts an item as covered when a passing case used it; these were listed
// as missing by the first run that kept the ledger.
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

E2E surface{"surface", s::home_fixture, [] {
    "1125 the maintenance commands answer"_test = [] {
        s::own_home();
        const auto dir = s::case_dir();
        auto run = [&](std::vector<std::string> args) {
            auto r = s::mcpp(std::move(args), dir);
            expect(fatal(e2e::succeeded(r))) << s::text(r);
            return s::text(r);
        };
        for (auto args : {std::vector<std::string>{"version"}, std::vector<std::string>{"self", "version"}})
            expect(s::has_line(run(args), R"(^mcpp [0-9]+\.[0-9]+)")) << "no version line";
        for (auto args : {std::vector<std::string>{"explain", "E0001"}, std::vector<std::string>{"self", "explain", "E0001"}})
            expect(e2e::contains(run(args), "E0001: dependency name mismatch"));
        // A registry added is listed until it is removed.
        expect(e2e::contains(run({"index", "add", "acme1125", "https://example.invalid/acme-index.git"}), "Added registry `acme1125`"));
        expect(s::has_line(run({"index", "list"}), R"(^  acme1125 +https://example\.invalid/acme-index\.git)")) << "an added registry is not listed";
        expect(e2e::contains(run({"index", "remove", "acme1125"}), "Removed registry `acme1125`"));
        expect(s::lacks(run({"index", "list"}), "acme1125")) << "a removed registry is still listed";
        // A payload this home holds that an upstream also has is listed, and --apply replaces it
        // with a link; then nothing is left to replace. A home that installed every payload by
        // link answers so at once (a host whose provider copies one answers it after --apply).
        const auto listed = run({"self", "store", "dedupe"});
        expect(e2e::contains(listed, "no payload here is also in an upstream") || e2e::contains(listed, "could be links to an upstream's")) << listed;
        (void)run({"self", "store", "dedupe", "--apply"});
        expect(e2e::contains(run({"self", "store", "dedupe"}), "no payload here is also in an upstream")) << "--apply left a payload that an upstream has";
        expect(e2e::contains(run({"cache", "clean", "--deps"}), "Cleaned package entries"));
        expect(e2e::contains(run({"cache", "clean", "--tools"}), "Cleaned host tool entries"));
        expect(e2e::contains(run({"self", "config", "--cache-unused-days", "30"}), "cache.unused_days = 30 days"));
    };

    "1126 a test run's selection flags"_test[s::gcc] = [] {
        // `--seed` reaches the program, `--shard 1/1` runs every test, `--split none` keeps a
        // program one process, `--failed` runs what failed last, and `--frozen` builds a project with nothing to lock.
        s::own_home();
        auto p = s::project::scaffold("flags1126");
        p.write("tests/cases.cpp", R"(import std;
import mcpplibs.mtest;
using namespace mcpplibs::mtest;
UTest seeded{"seeded", [] { expect(std::getenv("MTEST_SEED") != nullptr && std::string_view(std::getenv("MTEST_SEED")) == "1126"); }};
)");
        auto r = p.mcpp({"test", "--seed", "1126"});
        expect(fatal(e2e::succeeded(r))) << "the seed did not reach the program: " << s::text(r);
        r = p.mcpp({"test", "--seed", "1126", "--shard", "1/1", "--split", "none"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(e2e::contains(s::text(r), "cases ... ok")) << "the one shard did not run every test: " << s::text(r);
        p.write("tests/broken.cpp", "int main() { return 1; }\n");
        r = p.mcpp({"test", "--seed", "1126"});
        expect(fatal(r.started && r.status != 0)) << "a failing test passed: " << s::text(r);
        r = p.mcpp({"test", "--failed", "--seed", "1126"});
        expect(fatal(r.started && r.status != 0)) << s::text(r);
        expect(e2e::contains(s::text(r), "broken ... FAIL")) << "--failed did not run the failed test";
        expect(s::lacks(s::text(r), "cases ... ok")) << "--failed ran a test that passed";
        p.remove("tests/broken.cpp");
        // A project whose only dependencies are its tests' resolves nothing a lock holds: under
        // --frozen it builds as it is.
        expect(fatal(e2e::succeeded(p.mcpp({"build"}))));
        expect(!fs::exists(p.at("mcpp.lock"))) << "a project with no dependency wrote a lock";
        r = p.mcpp({"--frozen", "build"});
        expect(e2e::succeeded(r)) << "a build with nothing to lock was refused under --frozen: " << s::text(r);
    };

    "1127 the build commands' shared flags answer"_test[s::gcc] = [] {
        // The flags the build commands share, each given where it states what already holds, so
        // the command answers as it does without it: `none` as a platform is the engine's own
        // conventions, which a project that names no platform has; the base named is the one the
        // default resolves to (`why`'s binding); --play-game draws nothing where the status row
        // is off. The manifest states the [build] keys no other case states, and an archive
        // directory of this machine, which the build notes once (docs/25, "Archive directories").
        s::own_home();
        const auto dir = s::case_dir();
        const auto archives = dir / "archives";
        fs::create_directories(archives);
        s::project p(dir / "p", {{"mcpp.toml", std::format(R"([package]
name = "flags1127"
version = "0.1.0"

[features]
extra = []

[build]
memory_threshold = "256MiB"
private_fragment = "as-written"
platform-dependencies = "refuse"
profile = "dev"
ios_deployment_target = "18.0"

[host]
platform = "none"

[xlings]
archives = ["{}"]
)", archives.generic_string())},
                                 {"src/main.cpp", "import std;\nint main() { std::println(\"flags 1127\"); }\n"},
                                 {"tests/smoke.cpp", "int main() { return 0; }\n"}});
        auto run = [&](std::vector<std::string> args) {
            auto r = p.mcpp(std::move(args));
            expect(fatal(e2e::succeeded(r))) << s::text(r);
            return s::text(r);
        };
        auto out = run({"--no-color", "build"});
        expect(s::lacks(out, "\x1b[")) << "--no-color printed an escape sequence";
        expect(e2e::contains(out, "a directory of this machine")) << "the archive directory of this machine was not noted\n" << out;

        const auto why = run({"why"});
        std::smatch m;
        expect(fatal(std::regex_search(why, m, std::regex{R"((?:^|\n)binding: (glibc@[^ \n]+) via)"}))) << "why names no glibc binding\n" << why;
        const auto base = m[1].str();
        expect(fatal(std::regex_search(why, m, std::regex{R"((?:^|\n)target: ([^ \n]+))"}))) << why;
        const auto triple = m[1].str();
        const auto gcc = std::format("gcc@{}", s::gcc_version());

        for (auto args : {std::vector<std::string>{"why", "--platform", "none", "--host-platform", "none", "--sysroot", base},
                          std::vector<std::string>{"resolve", "--explain"}})
            expect(e2e::contains(run(args), "toolchain: gcc")) << args.front() << " named no toolchain";
        for (auto args : {std::vector<std::string>{"--release", "--static", "--strict", "--no-accel", "--features", "extra",
                                                   "--cap", "blas=openblas", "--platform", "none", "--host-platform", "none",
                                                   "--sysroot", base},
                          std::vector<std::string>{"--dev"}, std::vector<std::string>{"--profile", "release"}}) {
            args.insert(args.begin(), {"emit", "build-database", "--format", "json"});
            auto r = p.mcpp(args);
            expect(fatal(e2e::succeeded(r))) << s::text(r);
            expect(s::json(r.out)["kind"] == "mcpp.build-database") << r.out;
        }

        (void)run({"build", "--static", "--host-platform", "none", "--sysroot", base});
        out = run({"run", "--dev", "--no-cache", "--target-triple", triple, "--play-game=snake",
                   "--platform", "none", "--host-platform", "none", "--sysroot", base});
        expect(e2e::contains(out, "flags 1127")) << "the program did not run\n" << out;
        expect(e2e::contains(out, "the status row's screen is off here")) << "--play-game did not say where it draws\n" << out;
        expect(e2e::contains(run({"run", "--format", "single-file"}), "flags 1127")) << "the single file did not run";
        out = run({"test", "--cache", "global", "--no-accel", "--cap", "blas=openblas", "--platform", "none",
                   "--host-platform", "none", "--sysroot", base, "--play-game"});
        expect(e2e::contains(out, "smoke ... ok")) << out;
        expect(e2e::contains(run({"test", "--no-cache"}), "smoke ... ok"));
        out = run({"fetch", "--features", "extra", "--profile", "release", "--target", triple, "--toolchain", gcc,
                   "--sysroot", base, "--platform", "none"});
        expect(e2e::contains(out, "Fetched")) << out;
        expect(e2e::contains(run({"pack", "--no-universal", "--host-platform", "none", "--sysroot", base}), "Packing flags1127"));

        // The descriptions of the package and of the surface.
        const auto lua = run({"emit", "xpkg", "--namespace", "acme", "--version", "0.2.0"});
        expect(e2e::contains(lua, "namespace = \"acme\"") && e2e::contains(lua, "0.2.0")) << lua;
        p.write("flags1127.lua", p.mcpp({"emit", "xpkg"}).out);
        expect(e2e::contains(run({"xpkg", "parse", "--all-os", "--allow-split-name", p.at("flags1127.lua").string()}), "flags1127"));
        (void)run({"emit", "surface", "--output", (dir / "surface.txt").string()});
        expect(e2e::contains(s::read_file(dir / "surface.txt"), "flag emit surface --output")) << "the surface written lacks its own flag";
        expect(s::lacks(run({"search", "mtest"}), "0.10.0") && e2e::contains(run({"search", "--all-versions", "mtest"}), "0.11.0, 0.10.0"))
            << "--all-versions listed what the newest few list";
        expect(e2e::contains(run({"self", "env", "--host", "--host-platform", "none"}), "host conventions"));
        expect(e2e::contains(run({"toolchain", "install", gcc, "--target", triple, "--platform", "none"}), "Installed"));
        expect(e2e::contains(run({"toolchain", "default", gcc, "--target", triple}), "Default set to"));

        // A workspace member left out of the fetch and of the publication.
        s::project ws(dir / "ws", {{"mcpp.toml", "[workspace]\nmembers = [\"a\", \"b\"]\n"}, {".gitignore", "target/\n"}});
        for (std::string member : {"a", "b"}) {
            ws.write(member + "/mcpp.toml", std::format("[package]\nname = \"{}\"\nversion = \"0.1.0\"\nrepo = \"https://example.invalid/ws\"\n", member));
            ws.write(member + "/src/main.cpp", "int main() { return 0; }\n");
        }
        const auto git = s::on_path("git");
        expect(fatal(git.has_value())) << "no git on PATH";
        for (std::vector<std::string> args : {std::vector<std::string>{"init", "-q"}, {"add", "-A"},
                                              {"-c", "user.name=e2e", "-c", "user.email=e2e@localhost", "commit", "-qm", "v1"}})
            expect(fatal(e2e::succeeded(s::run(*git, args, ws.dir()))));
        auto r = ws.mcpp({"fetch", "--workspace", "--exclude", "b"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(s::lacks(s::text(r), "member 'b'")) << "an excluded member was fetched for\n" << s::text(r);
        r = ws.mcpp({"publish", "--workspace", "--exclude", "b", "--dry-run"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(e2e::contains(r.out, "name = \"a\"") && s::lacks(r.out, "name = \"b\"")) << "the excluded member was published\n" << r.out;
    };
}};

}  // namespace

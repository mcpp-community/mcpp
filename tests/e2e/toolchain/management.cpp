// Which toolchain a project gets and how it is managed: a pin resolves to the home's payload, no
// toolchain is a hard error, the toolchain commands install, list, set the default and remove in an
// isolated home, partial versions resolve, the system toolchain is refused, a toolchain declared for
// a target this host cannot serve is still installed, and `--toolchain` replacing a declared one
// says so (formerly tests/e2e/13, 14, 26, 29, 325, 890, 899); a partial request has one answer
// and a stated one is recorded in mcpp.lock; a moved default is said once; a host bootstrap tool is
// said once.
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

// The text contains `a`, and `b` after it (a shell pattern `*a*b*`).
bool in_order(std::string_view text, std::string_view a, std::string_view b) {
    const auto at = text.find(a);
    return at != std::string_view::npos && text.find(b, at + a.size()) != std::string_view::npos;
}

// `mcpp new <name>` in `parent` with these options: the project it made.
s::project made(const fs::path& parent, std::string_view name, const s::options& o) {
    fs::create_directories(parent);
    auto r = s::mcpp({"new", std::string(name)}, parent, o);
    expect(fatal(e2e::succeeded(r))) << "mcpp new " << name << " failed: " << s::text(r);
    return s::project(parent / name, {});
}

// The mirror the e2e run names, when it names one (MCPP_E2E_TOOLCHAIN_MIRROR).
void configure_e2e_mirror(const s::options& o) {
    const auto mirror = s::env("MCPP_E2E_TOOLCHAIN_MIRROR");
    if (!mirror || mirror->empty()) return;
    auto r = s::mcpp({"self", "config", "--mirror", *mirror}, s::case_dir(), o);
    expect(fatal(e2e::succeeded(r))) << "failed to configure e2e mirror: " << s::text(r);
}

E2E management{"management", s::home_fixture, [] {
    "13 toolchain pin"_test = [] {
        // A project's `[toolchain]` pin resolves to the payload's absolute path in mcpp's home.
        const auto gcc = s::gcc_version();
        const fs::path home_store = s::shared_home() / "registry/data/xpkgs/xim-x-gcc";
        const fs::path user_store = fs::path(s::env("HOME").value_or("")) / ".xlings/data/xpkgs/xim-x-gcc";
        fs::path seed;
        for (const auto& store : {home_store, user_store})
            if (fs::is_directory(store / gcc)) { seed = store; break; }
        if (seed.empty()) skip(std::format("neither mcpp's home nor the system xlings has gcc {}", gcc));
        const auto sandbox = s::case_home(s::home::fresh);
        fs::create_directories(sandbox / "registry/data/xpkgs");
        fs::create_directory_symlink(seed, sandbox / "registry/data/xpkgs/xim-x-gcc");
        const s::options o{.where = s::home::fresh};
        auto p = made(s::case_dir() / "new", "myapp", o);
        p.write("mcpp.toml", std::format(R"([package]
name        = "myapp"
version     = "0.1.0"

[language]
standard   = "c++23"
modules    = true
import_std = true

[toolchain]
linux = "gcc@{0}"

[modules]
sources = ["src/**/*.cppm", "src/**/*.cpp"]

[targets.myapp]
kind = "bin"
main = "src/main.cpp"
)", gcc));
        const auto out = s::text(p.mcpp({"build"}, o));
        expect(fatal(e2e::contains(out, "Resolving"))) << "missing Resolving: " << out;
        expect(fatal(e2e::contains(out, "toolchain"))) << "missing toolchain: " << out;
        expect(fatal(s::has_line(out, std::format("Resolved.*gcc@{}", gcc)))) << "missing Resolved gcc@" << gcc;
        expect(fatal(e2e::contains(out, std::format("@mcpp/registry/data/xpkgs/xim-x-gcc/{}/bin/g++", gcc)))) << "Resolved path doesn't reference the mcpp sandbox xpkg: " << out;
    };

    "14 toolchain fallback"_test = [] {
        // With no toolchain configured at all (a fresh home, no first-run install) the build is a
        // hard error that suggests the install; `default = "system"` is refused on its own terms.
        const s::options o{.where = s::home::fresh, .env = {{"MCPP_NO_AUTO_INSTALL", "1"}}};
        auto p = made(s::case_dir() / "new", "myapp", o);
        auto r = p.mcpp({"build"}, o);
        expect(fatal(!r)) << "FAIL: build should hard-error without toolchain";
        expect(fatal(e2e::contains(s::text(r), "no toolchain configured"))) << "FAIL: error message not helpful: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "mcpp toolchain install"))) << "FAIL: error doesn't suggest install: " << s::text(r);
        p.write("mcpp.toml", p.read("mcpp.toml") + "\n[toolchain]\ndefault = \"system\"\n");
        const auto out = s::text(p.mcpp({"build"}, o));
        expect(fatal(s::lacks(out, "no toolchain configured"))) << "FAIL: 'system' fell through to the unconfigured path";
        expect(fatal(e2e::contains(out, "toolchains it manages"))) << "FAIL: 'system' was not refused on its own terms: " << out;
        expect(fatal(e2e::contains(out, "msvc@system"))) << "FAIL: the refusal does not name the one supported exception: " << out;
    };

    "26 toolchain management"_test[s::gcc] = [] {
        // `toolchain install / list / default / remove` in an isolated home: the build uses the
        // home's private compiler and nothing of the system's, `self env` names the default, and
        // remove needs the exact version.
        const auto home = s::case_home(s::home::fresh);
        const s::options o{.where = s::home::fresh, .env = {{"MCPP_NO_AUTO_INSTALL", "1"}}};
        const auto cwd = s::case_dir();
        auto tc = [&](std::vector<std::string> args) {
            args.insert(args.begin(), "toolchain");
            return s::mcpp(args, cwd, o);
        };
        auto r = tc({"list"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "no toolchains installed"))) << "FAIL: empty list: " << s::text(r);
        s::project p(cwd / "proj", {{"mcpp.toml", "[package]\nname    = \"tinybin\"\nversion = \"0.1.0\"\n"},
                                    {"src/main.cpp", "import std;\nint main() { std::println(\"ok\"); return 0; }\n"}});
        r = p.mcpp({"build"}, o);
        expect(fatal(!r)) << "FAIL: build should hard-error without toolchain";
        expect(fatal(e2e::contains(s::text(r), "no toolchain configured"))) << "FAIL: error not helpful: " << s::text(r);
        if (auto mirror = s::env("MCPP_E2E_TOOLCHAIN_MIRROR"); mirror && !mirror->empty()) {
            expect(fatal(e2e::succeeded(s::mcpp({"self", "config", "--mirror", *mirror}, cwd, o))));
            expect(fatal(e2e::succeeded(s::mcpp({"self", "config"}, cwd, o))));
        }
        r = tc({"install", "gcc", "16.1.0"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: install exited " << r.status << "; log: " << s::text(r) << "\nxpkgs: " << nlohmann::json(s::listing(home / "registry/data/xpkgs")).dump();
        expect(fatal(e2e::contains(s::text(r), "Installed"))) << "FAIL: no Installed line: " << s::text(r);
        r = tc({"list"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "gcc"))) << "FAIL: list missing gcc: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "16.1.0"))) << "FAIL: list missing version: " << s::text(r);
        r = tc({"default", "gcc@16.1.0"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "Default"))) << "FAIL: no Default msg: " << s::text(r);
        r = tc({"list"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "*"))) << "FAIL: default mark missing: " << s::text(r);
        r = tc({"default", "gcc", "16.1.0"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: space-separated default failed: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "Default"))) << "FAIL: no Default msg for space-separated form: " << s::text(r);
        r = p.mcpp({"build"}, o);
        expect(fatal(e2e::succeeded(r))) << "FAIL: build failed: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "Finished"))) << "FAIL: no Finished: " << s::text(r);
        const auto triples = s::glob(p.at("target"), "x86_64-linux-*");
        expect(fatal(!triples.empty())) << "no target/x86_64-linux-* directory";
        const auto fps = s::glob(triples.front(), "*");
        expect(fatal(fps.size() == 1u)) << "expected one fingerprint directory under " << triples.front().string();
        const auto fp = fps.front();
        auto run = s::run(fp / "bin/tinybin");
        auto out = run.out;
        while (out.ends_with('\n')) out.pop_back();
        expect(fatal(out == "ok")) << "FAIL: runtime: " << s::text(run);
        const auto ninja = s::read_file(fp / "build.ninja");
        expect(fatal(e2e::contains(ninja, (home / "registry/data/xpkgs/xim-x-gcc/16.1.0/bin/g++").string()))) << "FAIL: build.ninja doesn't reference private binary";
        expect(fatal(s::lacks(ninja, "/usr/bin/g++") && s::lacks(ninja, "/usr/local/bin/g++"))) << "FAIL: build.ninja referenced system path";
        r = s::mcpp({"self", "env"}, cwd, o);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "default toolchain   = gcc@16.1.0"))) << "FAIL: env missing default toolchain line";
        p.remove("target");
        r = tc({"remove", "gcc"});
        expect(fatal(e2e::exited(r, 2))) << "FAIL: remove without a version should exit 2 (got " << r.status << "): " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "needs the exact version"))) << "FAIL: remove without a version did not name the missing version: " << s::text(r);
        r = tc({"remove", "gcc", "16.1.0"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "Removed"))) << "FAIL: no Removed msg for space-separated form: " << s::text(r);
        r = tc({"remove", "gcc", "16.1.0", "--target", "x86_64-linux-musl"});
        expect(fatal(!r)) << "FAIL: removing an absent target toolchain should fail";
        expect(fatal(e2e::contains(s::text(r), "is not installed"))) << "FAIL: split form with --target did not reach installed-payload check: " << s::text(r);
        r = tc({"remove", "gcc@16.1.0"});
        expect(fatal(!r)) << "FAIL: removing an absent toolchain should fail";
        expect(fatal(e2e::contains(s::text(r), "is not installed"))) << "FAIL: @-form remove did not reach installed-payload check: " << s::text(r);
        r = tc({"list"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(s::no_line(s::text(r), R"(^gcc.*15\.1\.0)"))) << "FAIL: list still shows removed toolchain";
    };

    "29 toolchain partial versions"_test[s::gcc] = [] {
        // A partial version (`gcc 16.1`, `gcc@16.1`) has one answer in `toolchain install`,
        // `toolchain default` and a build (SPEC-006 §2.6, §2.7); a project that states one records the
        // answer in mcpp.lock and `mcpp update` drops it; a first run in a home with no default
        // picks the host's first-run toolchain (`self env`'s `defaultToolchain`, the built-in
        // table's), prints the First-run banner once, and persists it. The
        // request is `16.1` rather than `16`: what `16` answers moves with the index, and the case
        // asserts the agreement, not a release.
        const auto h1 = s::case_home(s::home::payloads);
        const s::options o1{.where = s::home::payloads};
        configure_e2e_mirror(o1);
        auto r = s::mcpp({"toolchain", "list"}, s::case_dir(), o1);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(in_order(s::text(r), "gcc", "16.1.0"))) << "gcc 16.1.0 missing from list: " << s::text(r);
        // The list reads the upstreams; a default names a toolchain of this home, which installs it
        // from them by link.
        r = s::mcpp({"toolchain", "install", "gcc@16.1"}, s::case_dir(), o1);
        expect(fatal(e2e::succeeded(r))) << "install 'gcc@16.1' from the upstream failed: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "gcc@16.1 → gcc@16.1.0"))) << "install did not say what 'gcc@16.1' resolved to: " << s::text(r);
        r = s::mcpp({"toolchain", "default", "gcc", "16.1"}, s::case_dir(), o1);
        expect(fatal(e2e::succeeded(r))) << "default 'gcc 16.1' failed: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "gcc@16.1.0"))) << "default 'gcc 16.1' didn't resolve to 16.1.0: " << s::text(r);
        r = s::mcpp({"toolchain", "default", "gcc@16.1"}, s::case_dir(), o1);
        expect(fatal(e2e::succeeded(r))) << "default 'gcc@16.1' failed: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "gcc@16.1.0"))) << "default 'gcc@16.1' didn't resolve to 16.1.0: " << s::text(r);
        {
            auto stated = made(s::case_dir() / "stated", "stated", o1);
            stated.write("mcpp.toml", stated.read("mcpp.toml") + "\n[toolchain]\ndefault = \"gcc@16.1\"\n");
            r = stated.mcpp({"build"}, o1);
            expect(fatal(e2e::succeeded(r))) << "a build stating gcc@16.1 failed: " << s::text(r);
            expect(fatal(e2e::contains(s::text(r), "xim-x-gcc/16.1.0/"))) << "the build took another version than install and default: " << s::text(r);
            expect(fatal(stated.exists("mcpp.lock") && e2e::contains(stated.read("mcpp.lock"), "[toolchain.\"xim:gcc@16.1\"]\nversion = \"16.1.0\"")))
                << "the stated partial request was not recorded: " << (stated.exists("mcpp.lock") ? stated.read("mcpp.lock") : "no mcpp.lock");
            r = stated.mcpp({"update", "gcc"}, o1);
            expect(fatal(e2e::succeeded(r))) << s::text(r);
            expect(fatal(s::lacks(stated.read("mcpp.lock"), "[toolchain."))) << "update gcc kept the toolchain's record: " << stated.read("mcpp.lock");
            stated.write("mcpp.toml", std::regex_replace(stated.read("mcpp.toml"), std::regex("gcc@16\\.1\""), "gcc@16.1.0\""));
            stated.remove("mcpp.lock");
            r = stated.mcpp({"build"}, o1);
            expect(fatal(e2e::succeeded(r))) << s::text(r);
            expect(fatal(!stated.exists("mcpp.lock"))) << "a full version wrote a lock of its own: " << stated.read("mcpp.lock");
        }
        (void)h1;
        const auto h2 = s::another_home(s::home::payloads);
        const s::options o2{.env = {{"MCPP_HOME", h2.string()}}};
        configure_e2e_mirror(o2);
        auto p = made(s::case_dir() / "proj", "hello", o2);
        expect(fatal(s::no_line(p.read("mcpp.toml"), R"(^\[toolchain\])"))) << "scaffolding regression: mcpp new now adds [toolchain]; update this test";
        r = p.mcpp({"self", "env", "--format", "json"}, o2);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        const auto firstRun = s::json(r.out)["data"].value("defaultToolchain", std::string{});
        expect(fatal(firstRun.starts_with("gcc@"))) << "the host's first run is not a glibc gcc: " << r.out;
        r = p.mcpp({"build"}, o2);
        const auto first = s::text(r);
        expect(fatal(e2e::succeeded(r))) << "first-run build failed: " << first;
        expect(fatal(e2e::contains(first, "First run"))) << "missing First-run banner: " << first;
        expect(fatal(e2e::contains(first, "installing " + firstRun + " "))) << "first run didn't pick " << firstRun << " as default: " << first;
        expect(fatal(e2e::contains(first, "Finished"))) << "build did not finish: " << first;
        const auto binary = s::find_file(p.at("target"), "hello");
        expect(fatal(binary.has_value() && (fs::status(*binary).permissions() & fs::perms::owner_exec) != fs::perms::none)) << "no hello binary produced";
        const auto h = s::read_header(*binary);
        expect(fatal(h.elf_interpreter || !h.needed.empty())) << "first-run default unexpectedly produced a static binary (glibc default expected)";
        auto ran = s::run(*binary);
        expect(fatal(e2e::succeeded(ran))) << "first-run binary did not run (exit " << ran.status << "): " << s::text(ran)
                                           << "\n  PT_INTERP: " << (h.interpreter.empty() ? "<none>" : h.interpreter)
                                           << (h.interpreter.empty() ? "" : fs::exists(h.interpreter) ? " (exists)" : " (MISSING)")
                                           << "\n  RUNPATH: " << (h.runpath.empty() ? "<none>" : h.runpath);
        r = p.mcpp({"build"}, o2);
        expect(fatal(e2e::succeeded(r))) << "second build failed: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), "First run"))) << "second build re-printed First-run banner — default not persisted";
    };

    "325 system toolchain refused"_test[s::gcc] = [] {
        // `system` names no toolchain mcpp manages: refused for a plain project, for one with a
        // build program (before the program starts) and through MCPP_TOOLCHAIN, each refusal
        // showing what to write instead; a project with no [toolchain] still builds.
        s::project p({{"src/main.cpp", "#include <cstdio>\nint main(){ std::printf(\"hi\\n\"); return 0; }\n"}});
        auto manifest = [&](std::string_view extra) {
            p.write("mcpp.toml", std::format("[package]\nname = \"systc\"\nversion = \"0.1.0\"\nstandard = \"c++23\"\n{}", extra));
        };
        // What to write instead is the host's first run, the one answer `self env` reports.
        manifest("");
        const auto firstRun = s::json(p.mcpp({"self", "env", "--format", "json"}).out)["data"].value("defaultToolchain", std::string{});
        expect(fatal(!firstRun.empty())) << "self env reports no default toolchain";
        auto expect_refusal = [&](const std::string& out, std::string_view what) {
            expect(fatal(e2e::contains(out, "toolchains it manages"))) << "FAIL: " << what << " was not refused on the toolchain policy's own terms: " << out;
            expect(fatal(e2e::contains(out, std::format("= \"{}\"", firstRun)))) << "FAIL: the refusal does not show what to write instead (" << firstRun << "): " << out;
            expect(fatal(e2e::contains(out, "mcpp toolchain"))) << "FAIL: the refusal does not name the command that lists the choices: " << out;
            expect(fatal(e2e::contains(out, "msvc@system"))) << "FAIL: the refusal does not name the one supported exception: " << out;
            expect(fatal(s::contains_nocase(out, "librar"))) << "FAIL: the refusal does not distinguish the library axis: " << out;
        };
        manifest("\n[toolchain]\nlinux   = \"system\"\nmacos   = \"system\"\nwindows = \"system\"\n");
        p.remove("target");
        auto r = p.mcpp({"build"});
        expect(fatal(!r)) << "FAIL: [toolchain] system built; mcpp builds only with managed toolchains";
        expect_refusal(s::text(r), "a plain project");
        // The refusal's branch has no token of its own: the machine's answer is `other`, never
        // `none` beside a refusal (docs/50).
        const auto why = s::json(p.mcpp({"why", "toolchain", "--format", "json"}).out);
        expect(fatal(why["data"].value("status", std::string{}) == "refused" && why["data"].value("reason", std::string{}) == "other"))
            << "FAIL: the refusal's reason is not `other`: " << why.dump();
        p.write("build.mcpp", "#include <cstdio>\nint main(){ return 0; }\n");
        p.remove("target");
        r = p.mcpp({"build"});
        expect(fatal(!r)) << "FAIL: [toolchain] system with a build.mcpp built";
        expect_refusal(s::text(r), "a project with a build.mcpp");
        expect(fatal(s::lacks(s::text(r), "posix_spawnp"))) << "FAIL: still spawning an empty compiler path instead of refusing";
        expect(fatal(s::no_line(s::text(r), R"(^ *build\.mcpp )"))) << "FAIL: the refusal arrived after the build program had started";
        p.remove("build.mcpp");
        manifest("");
        p.remove("target");
        r = p.mcpp({"build"}, {.env = {{"MCPP_TOOLCHAIN", "system"}}});
        expect(fatal(!r)) << "FAIL: MCPP_TOOLCHAIN=system built; the policy must not have two answers";
        expect_refusal(s::text(r), "MCPP_TOOLCHAIN=system");
        manifest("");
        p.remove("target");
        r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: a project with no [toolchain] at all must still build: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), "toolchains it manages"))) << "FAIL: the refusal fired for a project that did not ask for system";
    };

    "890 a toolchain declared for an unservable target is still installed"_test[s::unix_shell] = [] {
        // On a host that cannot serve a Linux guest, a declared toolchain is installed before the
        // hosted-guest refusal, which then names the graph remedy (mcpp#782).
        const auto home = s::case_home(s::home::fresh);
        const s::options o{.where = s::home::fresh};
        auto r = s::mcpp({"self", "config", "--mirror", s::env("MCPP_E2E_MIRROR").value_or("GLOBAL")}, s::case_dir(), o);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        if (!s::on_macos() && !s::on_windows()) skip("hosted-guest refusal semantics only bite on a non-linux host");
        const auto llvm = s::llvm_version();
        s::project app({{"mcpp.toml", std::format("[package]\nname    = \"declared-cross\"\nversion = \"0.1.0\"\n\n[toolchain]\ndefault = \"llvm@{}\"\n", llvm)},
                        {"src/main.cpp", "int main() { return 0; }\n"}});
        r = app.mcpp({"build", "--target", "x86_64-linux-gnu"}, o);
        const auto out = s::text(r);
        expect(fatal(!r)) << "FAIL: an empty project built a linux-gnu guest with no system supplier";
        expect(fatal(s::has_line(out, std::format("Resolved llvm@{}", llvm)))) << "FAIL: the declared toolchain did not resolve: " << out;
        expect(fatal(fs::exists(home / "registry/data/xpkgs/xim-x-llvm" / llvm / s::exe("bin/clang++")))) << "FAIL: llvm@" << llvm << " not installed — the held diagnosis skipped it: " << out;
        expect(fatal(e2e::contains(out, "cannot be built on this host"))) << "FAIL: expected the hosted-guest refusal once the empty graph cannot supply the system: " << out;
        expect(fatal(s::has_line(out, "depend on a package that implements|implement the target's system"))) << "FAIL: the refusal does not name the graph remedy";
    };

    "899 toolchain flag replacing a declared toolchain says so"_test[s::gcc][s::llvm] = [] {
        // `--toolchain` replacing a toolchain the manifest (or the workspace root) declared warns,
        // naming both and where it was declared; the declared spec itself and an undeclared one
        // do not.
        const s::options o{.where = s::home::inherited};
        const auto tmp = s::case_dir();
        auto pkg = [&](const fs::path& dir, std::string_view extra) {
            const auto name = dir.filename().string();
            return s::project(dir, {{"mcpp.toml", std::format("[package]\nname = \"{0}\"\nversion = \"0.1.0\"\n{1}\n[targets.{0}]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", name, extra)},
                                    {"src/main.cpp", "int main() { return 0; }\n"}});
        };
        auto declared = pkg(tmp / "declared", "[toolchain]\ndefault = \"gcc@16.1.0\"");
        auto r = declared.mcpp({"build", "--toolchain", "llvm@23.1.3", "--strict"}, o);
        expect(fatal(e2e::succeeded(r))) << "FAIL: --strict failed on the override warning: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "warning: --toolchain llvm@23.1.3 replaces gcc@16.1.0 declared at [toolchain].default (mcpp.toml)"))) << "FAIL: no warning for a replaced toolchain: " << s::text(r);
        r = declared.mcpp({"build", "--toolchain", "gcc@16.1.0"}, o);
        expect(fatal(e2e::succeeded(r))) << "FAIL: same: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), "replaces"))) << "FAIL: warned for the declared spec itself";
        auto undeclared = pkg(tmp / "undeclared", "");
        r = undeclared.mcpp({"build", "--toolchain", "llvm@23.1.3"}, o);
        expect(fatal(e2e::succeeded(r))) << "FAIL: undeclared: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), "replaces"))) << "FAIL: warned where the manifest declares no toolchain";
        s::project ws(tmp / "ws", {{"mcpp.toml", "[workspace]\nmembers = [\"app\"]\n\n[workspace.toolchain]\ndefault = \"gcc@16.1.0\"\n"}});
        pkg(ws.at("app"), "");
        r = ws.mcpp({"build", "-p", "app", "--toolchain", "llvm@23.1.3"}, o);
        expect(fatal(e2e::succeeded(r))) << "FAIL: workspace member: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "replaces gcc@16.1.0 declared at [toolchain].default (mcpp.toml)"))) << "FAIL: the warning does not name the workspace root's manifest: " << s::text(r);
    };

    "988 a default toolchain that moved is said once"_test[s::gcc][s::llvm] = [] {
        // A project that states no toolchain follows the machine's default; when the default
        // answers another toolchain than the one the project last built with, the build says so
        // once, with the statement that keeps the old one (SPEC-006 §2.8). `--toolchain` is this
        // build's choice and is neither compared nor remembered. Under --locked a stated partial
        // request the lock has no record of resolves, says so, and leaves the lock as it was.
        const s::options o{.where = s::home::inherited};
        const auto cwd = s::case_dir();
        for (auto spec : {"gcc@16.1.0", "llvm@23.1.3"}) {
            auto r = s::mcpp({"toolchain", "install", spec}, cwd, o);
            expect(fatal(e2e::succeeded(r))) << "install " << spec << ": " << s::text(r);
        }
        expect(fatal(e2e::succeeded(s::mcpp({"toolchain", "default", "gcc@16.1.0"}, cwd, o))));
        s::project p(cwd / "app", {{"mcpp.toml", "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                                   {"src/main.cpp", "int main() { return 0; }\n"}});
        auto build = [&](std::vector<std::string> extra = {}) {
            p.write("src/main.cpp", p.read("src/main.cpp") + "\n");
            std::vector<std::string> args{"build"};
            args.insert(args.end(), extra.begin(), extra.end());
            auto r = p.mcpp(args, o);
            expect(fatal(e2e::succeeded(r))) << s::text(r);
            return s::text(r);
        };
        auto out = build();
        expect(fatal(s::lacks(out, "last built with"))) << "the first build had nothing to compare with: " << out;
        out = build({"--toolchain", "llvm@23.1.3"});
        expect(fatal(s::lacks(out, "last built with"))) << "--toolchain was compared with the default: " << out;
        out = build();
        expect(fatal(s::lacks(out, "last built with"))) << "--toolchain was remembered as the project's toolchain: " << out;
        expect(fatal(e2e::succeeded(s::mcpp({"toolchain", "default", "llvm@23.1.3"}, cwd, o))));
        out = build();
        expect(fatal(e2e::contains(out, "is now llvm@23.1.3; this project last built with gcc@16.1.0"))) << "the moved default was not said: " << out;
        expect(fatal(e2e::contains(out, "[toolchain] default = \"gcc@16.1.0\""))) << "the notice does not show the statement that keeps the old one: " << out;
        out = build();
        expect(fatal(s::lacks(out, "last built with"))) << "the moved default was said twice: " << out;

        p.write("mcpp.toml", p.read("mcpp.toml") + "\n[toolchain]\ndefault = \"gcc@16.1\"\n");
        out = build({"--locked"});
        expect(fatal(e2e::contains(out, "mcpp.lock has no record of the toolchain xim:gcc@16.1; it resolved to 16.1.0"))) << "--locked did not say the record is missing: " << out;
        expect(fatal(!p.exists("mcpp.lock"))) << "--locked wrote a lock: " << p.read("mcpp.lock");
        out = build();
        expect(fatal(p.exists("mcpp.lock") && e2e::contains(p.read("mcpp.lock"), "[toolchain.\"xim:gcc@16.1\"]"))) << "a build without --locked did not record it: " << out;
        out = build({"--locked"});
        expect(fatal(s::lacks(out, "has no record"))) << "the record was not read back: " << out;
    };

    "1132 a host bootstrap tool is said once"_test[s::unix_shell] = [] {
        // `[build] allow_host_tools = ["ninja"]` builds with the ninja on PATH and says so once per
        // project and identity, naming mcpp.toml's statement as a machine's (design 2026-11-21,
        // offline and toolchain defaults §3.3); a later build does not say it again, `mcpp clean`
        // forgets it, and MCPP_ALLOW_HOST_TOOLS=none takes the store's.
        const auto tmp = s::case_dir();
        const auto stores = s::glob(s::shared_home() / "registry/data/xpkgs/xim-x-ninja", "*");
        fs::path storeNinja;
        for (auto const& v : stores)
            if (fs::is_regular_file(v / "ninja")) storeNinja = v / "ninja";
        if (storeNinja.empty()) skip("the home's store holds no ninja");
        s::project bin(tmp / "hostbin", {{"ninja", std::format("#!/bin/sh\nif [ \"$1\" = --version ]; then echo 9.9.9; exit 0; fi\n"
                                                              "echo ran >> {}\nexec {} \"$@\"\n", (tmp / "ran").string(), storeNinja.string())}});
        fs::permissions(tmp / "hostbin/ninja", fs::perms::owner_all, fs::perm_options::add);
        const s::options o{.env = {{"PATH", (tmp / "hostbin").string() + ":" + s::env("PATH").value_or("")}}};
        s::project p(tmp / "app", {{"mcpp.toml", "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n[build]\nallow_host_tools = [\"ninja\"]\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                                   {"src/main.cpp", "int main() { return 0; }\n"}});
        auto build = [&](const s::options& with) {
            p.write("src/main.cpp", p.read("src/main.cpp") + "\n");
            auto r = p.mcpp({"build"}, with);
            expect(fatal(e2e::succeeded(r))) << s::text(r);
            return s::text(r);
        };
        auto out = build(o);
        expect(fatal(e2e::contains(out, "ninja (bootstrap) is this machine's (9.9.9 "))) << "the host ninja was not said: " << out;
        expect(fatal(e2e::contains(out, "a machine's input belongs in config.toml"))) << "mcpp.toml's statement was not named a machine's: " << out;
        expect(fatal(fs::exists(tmp / "ran"))) << "the build did not run the host ninja";
        out = build(o);
        expect(fatal(s::lacks(out, "is this machine's"))) << "the host ninja was said twice: " << out;
        expect(fatal(e2e::succeeded(p.mcpp({"clean"}, o))));
        out = build(o);
        expect(fatal(e2e::contains(out, "ninja (bootstrap) is this machine's"))) << "mcpp clean did not forget the note: " << out;
        fs::remove(tmp / "ran");
        auto none = o;
        none.env.emplace_back("MCPP_ALLOW_HOST_TOOLS", "none");
        out = build(none);
        expect(fatal(!fs::exists(tmp / "ran"))) << "MCPP_ALLOW_HOST_TOOLS=none still ran the host ninja: " << out;
    };

    "1138 a binutils stated beside a gcc older than its payload declares is refused"_test[s::gcc][s::elf] = [] {
        // SPEC-006 §2.9: `companions = { binutils = ... }` in the project's [toolchain] replaces
        // the binutils the gcc payload declares. GCC chose its assembler features by that one,
        // so an older binutils is refused, naming the version the payload declares; the declared
        // version builds, reported as the companion.
        const auto gcc = std::format("gcc@{}", s::gcc_version());
        s::project p({{"src/main.cpp", "int main() { return 0; }\n"}});
        const auto with = [&](std::string_view binutils) {
            p.write("mcpp.toml", std::format("[package]\nname = \"comp\"\nversion = \"0.1.0\"\n\n[toolchain]\n"
                                             "default = {{ package = \"{}\", companions = {{ binutils = \"xim:binutils@{}\" }} }}\n",
                                             gcc, binutils));
            return p.mcpp({"build"});
        };
        auto r = with("2.42");
        expect(fatal(r.started && r.status != 0)) << "a binutils older than the payload declares was taken\n" << s::text(r);
        std::smatch m;
        const auto out = s::text(r);
        expect(fatal(std::regex_search(out, m, std::regex{R"(companions\.binutils = "xim:binutils@2\.42" is older than the binutils .*'s payload declares \(([0-9.]+)\))"})))
            << "the refusal does not name the declared version\n" << out;
        const auto declared = m[1].str();
        r = with(declared);
        expect(fatal(e2e::succeeded(r))) << "the declared binutils stated as the companion did not build\n" << s::text(r);
        expect(fatal(s::has_line(s::text(r), "Companion +binutils " + std::regex_replace(declared, std::regex{R"(\.)"}, R"(\.)"))))
            << "the companion is not reported\n" << s::text(r);
    };
}};

}  // namespace

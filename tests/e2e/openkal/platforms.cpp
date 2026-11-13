// Platform packages: a class of targets a platform states, its version group and its series,
// the default platform release, a target a platform defines, the platform skeleton, and a row a
// platform defines taking nothing of the payload's host (formerly tests/e2e/952, 1020, 1021,
// 1025, 1040, 1049, 1057).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

const s::options inherited{.where = s::home::inherited};

#if defined(__x86_64__) || defined(_M_X64)
constexpr std::string_view host_arch = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
constexpr std::string_view host_arch = "aarch64";
#else
constexpr std::string_view host_arch = "unknown";
#endif

// `text` with its first `from` replaced by `to` (a script's `sed -i 's/.../.../'`).
std::string replaced(std::string text, std::string_view from, std::string_view to) {
    const auto at = text.find(from);
    expect(fatal(at != std::string::npos)) << "the fixture has no '" << from << "'";
    return text.replace(at, from.size(), to);
}

// `grep -A<n> <needle> | grep -q <want>`: a line holding `needle`, or one of the `n` after it,
// holds `want`.
bool within_after(std::string_view text, std::string_view needle, std::size_t n, std::string_view want) {
    std::vector<std::string> lines;
    std::istringstream in{std::string(text)};
    for (std::string l; std::getline(in, l);) lines.push_back(l);
    for (std::size_t i = 0; i < lines.size(); ++i)
        if (lines[i].contains(needle))
            for (std::size_t j = i; j < lines.size() && j <= i + n; ++j)
                if (lines[j].contains(want)) return true;
    return false;
}

// The last `n` bytes of a file (`tail -c <n>`).
std::string tail_bytes(const fs::path& f, std::size_t n) {
    const auto b = s::read_file(f);
    return b.size() <= n ? b : b.substr(b.size() - n);
}

E2E platforms{"platforms", [] {
    "952 a platform package states a class of targets"_test[tag{"requires:unix-shell"}][tag{"requires:python3"}] = [] {
        // A local platform for this machine's triples: A its flag, program name, seal step and
        // runner reach the program; B the report names it and its source; C an override under
        // `cfg(platform = "demo")` takes the field's place, and the JSON says where each field
        // came from; D `--platform none` builds without it; E a package without [platform] is
        // refused naming it; F a flag with `$`, quotes and a space arrives as written; G a
        // program linked again is sealed again, once.
        s::needs({"unix-shell", "python3"});
        const auto tmp = s::case_dir();
        s::project(tmp / "plat", {{"mcpp.toml", R"([package]
name    = "demo-platform"
version = "0.1.0"

[platform]
targets = ["x86_64-linux-gnu", "aarch64-linux-gnu", "aarch64-macos", "x86_64-macos"]
runner  = ["env", "DEMO_PLATFORM_RUNNER=yes", "{}"]

[platform.build]
cxxflags = ["-DDEMO_PLATFORM=1", "-DDEMO_TAG=\"a$b c\""]

[platform.link]
exe  = "{name}.demo"
seal = [["sh", "-c", "printf DEMO-SEALED >> \"$0\"", "{}"]]
)"}});
        s::project app(tmp / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[target.'cfg(any(linux, macos))']\nplatform = { path = \"../plat\" }\n"},
                                     {"src/main.cpp", R"(#include <cstdio>
#include <cstdlib>
int main() {
#ifdef DEMO_PLATFORM
    const char* flag = "flag";
#else
    const char* flag = "no-flag";
#endif
#ifdef DEMO_TAG
    const char* tag = DEMO_TAG;
#else
    const char* tag = "none";
#endif
    const char* runner = std::getenv("DEMO_PLATFORM_RUNNER");
    std::printf("demo: %s, runner %s, tag [%s]\n", flag, runner ? runner : "none", tag);
    return 0;
}
)"}});
        // A
        auto build = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(build))) << "A: build: " << s::text(build);
        auto progs = s::glob(app.at("target"), "*/*/bin/app.demo");
        expect(fatal(!progs.empty())) << "A: no program named by the platform's pattern (app.demo): " << s::text(build);
        expect(fatal(tail_bytes(progs.front(), 11) == "DEMO-SEALED")) << "A: the seal step did not run before publishing: " << s::text(build);
        const auto run = app.mcpp({"run"});
        expect(fatal(e2e::succeeded(run))) << "A: run: " << s::text(run);
        expect(fatal(e2e::contains(s::text(run), "demo: flag, runner yes"))) << "A: the flag or the runner did not reach the program: " << s::text(run);
        // B
        expect(fatal(s::has_line(s::text(build), R"(Platform +demo-platform 0\.1\.0 \(mcpp\.toml)"))) << "B: the report does not name the platform and its source: " << s::text(build);
        // C
        app.write("mcpp.toml", app.read("mcpp.toml") + "\n[target.'cfg(platform = \"demo\")'.link]\nexe = \"{name}.over\"\n");
        auto r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "C: build with the override: " << s::text(r);
        expect(fatal(!s::glob(app.at("target"), "*/*/bin/app.over").empty())) << "C: the override did not take the field's place: " << s::text(r);
        const auto host = std::format("{}-{}", host_arch, s::on_macos() ? "macos" : "linux-gnu");
        r = app.mcpp({"self", "env", "--target", host, "--format", "json"});
        expect(fatal(e2e::succeeded(r))) << "C: self env: " << s::text(r);
        const auto p = s::json(r.out)["data"]["platform"];
        expect(fatal(p.is_object() && p["name"].get<std::string>().ends_with("demo-platform"))) << "C: " << p.dump();
        expect(fatal(p["fields"]["link.exe"]["source"].get<std::string>().starts_with("mcpp.toml"))) << "C: " << p["fields"]["link.exe"].dump();
        expect(fatal(p["fields"]["runner"]["source"] == "platform")) << "C: " << p["fields"]["runner"].dump();
        // D
        r = app.mcpp({"run", "--platform", "none"});
        expect(fatal(e2e::succeeded(r))) << "D: run --platform none: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "demo: no-flag, runner none"))) << "D: --platform none kept a field of the platform: " << s::text(r);
        expect(fatal(!s::glob(app.at("target"), "*/*/bin/app").empty())) << "D: the program is not under its default name: " << s::text(r);
        // E
        s::project(tmp / "notplat", {{"mcpp.toml", "[package]\nname = \"notplat\"\nversion = \"0.1.0\"\n"}});
        r = app.mcpp({"build", "--platform", "path:" + (tmp / "notplat").string()});
        expect(fatal(!e2e::succeeded(r))) << "E: a package without [platform] was accepted as a platform: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "has no [platform] section"))) << "E: the refusal does not say why: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "notplat"))) << "E: the refusal does not name the package: " << s::text(r);
        // F
        expect(fatal(e2e::contains(s::text(run), "tag [a$b c]"))) << "F: the flag with $ did not reach the compile as written: " << s::text(run);
        // G
        r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "G: build: " << s::text(r);
        progs = s::glob(app.at("target"), "*/*/bin/app.over");
        expect(fatal(!progs.empty())) << "G: no program: " << s::text(r);
        const auto prog = progs.front();
        fs::remove_all(prog.parent_path().parent_path() / "obj");
        r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "G: build after the objects were removed: " << s::text(r);
        expect(fatal(tail_bytes(prog, 11) == "DEMO-SEALED")) << "G: the program was not sealed again: " << s::text(r);
        expect(fatal(tail_bytes(prog, 22) != "DEMO-SEALEDDEMO-SEALED")) << "G: the program was sealed twice: " << s::text(r);
        r = app.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "G: run: " << s::text(r);
    };

    "1020 a platform version group chooses the version"_test[tag{"requires:unix-shell"}][tag{"requires:gcc"}][tag{"requires:fresh-sandbox"}] = [] {
        // `[platform.versions]` takes part in resolution: A a range admitting the group's
        // version resolves to it though the index holds a newer one; B a request it does not
        // satisfy is refused before anything is built, naming the request, its requester and
        // the group; C a path dependency is not bound by the group.
        s::needs({"unix-shell", "gcc", "fresh-sandbox"});
        const auto tmp = s::case_dir();
        s::project(tmp / "plat", {{"mcpp.toml", "[package]\nname    = \"group-platform\"\nversion = \"0.1.0\"\n\n[platform]\ntargets = [\"x86_64-linux-gnu\", \"aarch64-linux-gnu\", \"aarch64-macos\", \"x86_64-macos\"]\n\n[platform.versions]\nfam = \"1.1.0\"\n"}});
        s::project proj(tmp / "proj", {{"local-index/pkgs/a/acme.fam.lua", R"(package = {
    spec = "1", namespace = "acme", name = "acme.fam",
    description = "version-group fixture", licenses = {"MIT"}, type = "package",
    xpm = {
        linux  = { ["1.2.0"] = { url = "u", sha256 = "0" }, ["1.1.0"] = { url = "u", sha256 = "0" }, ["1.0.0"] = { url = "u", sha256 = "0" } },
        macosx = { ["1.2.0"] = { url = "u", sha256 = "0" }, ["1.1.0"] = { url = "u", sha256 = "0" }, ["1.0.0"] = { url = "u", sha256 = "0" } },
    },
    mcpp = { language = "c++23", import_std = false, sources = { "src/fam.cppm" },
             targets = { ["fam"] = { kind = "lib" } }, deps = {} },
}
)"},
                                       // Only the group's version has a payload: resolving another one fails to install.
                                       {".mcpp/.xlings/data/xpkgs/acme.fam/1.1.0/src/fam.cppm", "export module fam;\nexport int fam_value() { return 110; }\n"},
                                       {"src/main.cpp", "import fam;\nint main() { return fam_value() == 110 ? 0 : 1; }\n"}});
        const auto use = [&](std::string_view request) {
            proj.write("mcpp.toml", std::format("[package]\nname    = \"proj\"\nversion = \"0.1.0\"\n\n[indices]\nacme = {{ path = \"local-index\" }}\n\n[target.'cfg(any(linux, macos))']\nplatform = {{ path = \"../plat\" }}\n\n[dependencies.acme]\nfam = {}\n\n[targets.proj]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", request));
        };
        // A
        use(R"("^1.0.0")");
        auto r = proj.mcpp({"build"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "A: the group's version was not taken: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "fam =1.1.0 → v1.1.0"))) << "A: the request did not resolve to the group's version: " << s::text(r);
        r = proj.mcpp({"run"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "A: the program: " << s::text(r);
        // B
        use(R"("^1.2.0")");
        fs::remove_all(proj.at("target"));
        r = proj.mcpp({"build"}, inherited);
        expect(fatal(!e2e::succeeded(r))) << "B: a request outside the group was built: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "fam is requested as '^1.2.0' by proj"))) << "B: the refusal does not name the request: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "is a version group") && e2e::contains(s::text(r), "names fam 1.1.0"))) << "B: the refusal does not name the group: " << s::text(r);
        expect(fatal(!fs::is_directory(proj.at("target")) || s::find_files(proj.at("target"), R"(.*\.o)").empty())) << "B: something was compiled before the refusal: " << s::text(r);
        // C
        s::project(tmp / "fam-dev", {{"mcpp.toml", "[package]\nnamespace = \"acme\"\nname = \"fam\"\nversion = \"1.2.0\"\n[targets.fam]\nkind = \"lib\"\n[build]\nsources = [\"src/fam.cppm\"]\n"},
                                     {"src/fam.cppm", "export module fam;\nexport int fam_value() { return 110; }\n"}});
        use(R"({ path = "../fam-dev" })");
        fs::remove_all(proj.at("target"));
        r = proj.mcpp({"build"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "C: a development tree was bound by the group: " << s::text(r);
    };

    "1021 a platform series is locked and skips newer engines"_test[tag{"requires:unix-shell"}][tag{"requires:gcc"}][tag{"requires:fresh-sandbox"}] = [] {
        // A platform selected as a series (`acme.plat@^0`): A it takes its newest release whose
        // `[package] mcpp` floor this mcpp meets, naming the one it skips, and the lock records
        // it; B a newer release published later is not taken while the lock names one; C
        // `mcpp update` drops the record and the next build takes the newer one.
        s::needs({"unix-shell", "gcc", "fresh-sandbox"});
        s::project proj(s::case_dir() / "proj", {{"src/main.cpp", "#include <cstdio>\nint main() { std::printf(\"release %s\\n\", PLAT_RELEASE); }\n"}});
        std::string entries;
        const auto release = [&](std::string_view version, std::string_view floor) {
            proj.write(std::format(".mcpp/.xlings/data/xpkgs/acme.plat/{}/mcpp.toml", version),
                       std::format("[package]\nnamespace = \"acme\"\nname = \"plat\"\nversion = \"{0}\"\n{1}\n[platform]\ntargets = [\"x86_64-linux-gnu\", \"aarch64-linux-gnu\", \"aarch64-macos\", \"x86_64-macos\"]\n\n[platform.build]\ncxxflags = [\"-DPLAT_RELEASE=\\\"{0}\\\"\"]\n",
                                   version, floor.empty() ? "" : std::format("mcpp = \">={}\"\n", floor)));
            entries = std::format("[\"{}\"] = {{ url = \"u\", sha256 = \"0\" }}, {}", version, entries);
            proj.write("local-index/pkgs/a/acme.plat.lua", std::format(R"(package = {{
    spec = "1", namespace = "acme", name = "acme.plat",
    description = "platform series fixture", licenses = {{"MIT"}}, type = "package",
    mcpp = "mcpp.toml",
    xpm = {{ linux = {{ {0} }}, macosx = {{ {0} }} }},
}}
)", entries));
        };
        release("0.1.0", "");
        release("0.2.0", "2099.1.1.1");
        proj.write("mcpp.toml", "[package]\nname    = \"proj\"\nversion = \"0.1.0\"\n\n[indices]\nacme = { path = \"local-index\" }\n\n[target.'cfg(any(linux, macos))']\nplatform = \"acme.plat@^0\"\n");
        // A
        auto r = proj.mcpp({"run"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "A: build: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "plat v0.2.0 needs mcpp >= 2099.1.1.1"))) << "A: the release for a newer mcpp was not named: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "release 0.1.0"))) << "A: the series did not take 0.1.0: " << s::text(r);
        expect(fatal(within_after(proj.read("mcpp.lock"), "[package.\"acme.plat\"]", 4, "version = \"0.1.0\""))) << "A: mcpp.lock does not record 0.1.0: " << proj.read("mcpp.lock");
        // B: the graph is planned again, against the index that now holds 0.1.1
        release("0.1.1", "");
        fs::remove_all(proj.at("target"));
        r = proj.mcpp({"run"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "B: build: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "release 0.1.0"))) << "B: a newer release was taken over the lock: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "(mcpp.lock)"))) << "B: the build does not say the lock chose: " << s::text(r);
        // C
        r = proj.mcpp({"update"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "C: mcpp update: " << s::text(r);
        fs::remove_all(proj.at("target"));
        r = proj.mcpp({"run"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "C: build: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "release 0.1.1"))) << "C: after mcpp update the newer release was not taken: " << s::text(r);
    };

    "1025 the default platform is one release"_test[tag{"requires:llvm"}][tag{"requires:python3"}] = [] {
        // A a project stating no platform builds x86_64-openkal over openkal-platform 0.27.1
        // and locks it; B a lock recording another release does not decide, and is rewritten;
        // C a platform the project states (a series) is taken as it is.
        s::needs({"llvm", "python3"});
        auto app = s::project::scaffold("app");
        const std::regex record{R"rx((\[package\."openkal-platform"\][^\[]*?version\s*=\s*")([^"]+)("))rx"};
        const auto locked = [&] {
            std::smatch m;
            const auto text = app.read("mcpp.lock");
            return std::regex_search(text, m, record) ? m[2].str() : std::string{};
        };
        auto r = app.mcpp({"build", "--target", "x86_64-openkal"});
        expect(fatal(e2e::succeeded(r))) << "A: the openkal build failed: " << s::text(r);
        expect(fatal(s::has_line(s::text(r), "Platform *openkal-platform 0.27.1"))) << "A: the build does not use openkal-platform 0.27.1: " << s::text(r);
        expect(fatal(locked() == "0.27.1")) << "A: mcpp.lock does not record 0.27.1: " << app.read("mcpp.lock");
        // B
        {
            const auto text = app.read("mcpp.lock");
            std::smatch m;
            expect(fatal(std::regex_search(text, m, record) && m[2] == "0.27.1")) << "B: the fixture could not edit mcpp.lock: " << text;
            app.write("mcpp.lock", m.prefix().str() + m[1].str() + "0.24.3" + m[3].str() + m.suffix().str());
        }
        expect(fatal(locked() == "0.24.3")) << "B: the fixture could not edit mcpp.lock: " << app.read("mcpp.lock");
        r = app.mcpp({"build", "--target", "x86_64-openkal"});
        expect(fatal(e2e::succeeded(r))) << "B: the openkal build failed: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "default; mcpp.lock recorded v0.24.3"))) << "B: the build does not say which release it takes over the lock's: " << s::text(r);
        expect(fatal(locked() == "0.27.1")) << "B: mcpp.lock was not rewritten to 0.27.1: " << app.read("mcpp.lock");
        // C
        app.write("mcpp.toml", app.read("mcpp.toml") + "\n[target.'cfg(os = \"openkal\")']\nplatform = \"openkal-platform@^0.24\"\n");
        r = app.mcpp({"build", "--target", "x86_64-openkal"});
        expect(fatal(e2e::succeeded(r))) << "C: the build with a stated series failed: " << s::text(r);
        expect(fatal(s::has_line(s::text(r), "Platform *openkal-platform 0.24"))) << "C: no platform line: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), "'s default"))) << "C: a stated platform was reported as the default: " << s::text(r);
    };

    "1040 a platform defines a target"_test[tag{"requires:elf"}][tag{"requires:llvm"}] = [] {
        // `[[platform.define]]` x86_64-toyos (a static Linux ELF): it builds into
        // target/x86_64-toyos with `cfg(os = "toyos")` applied, and runs (exit 7) directly and by
        // `mcpp run`; a row for another system is not judged by this machine's loader, and is
        // built, not run, without a runner, run with one; the platform's program name stands
        // over a build program's; the row is packed; without the platform it is unknown.
        s::needs({"elf", "llvm"});
        const auto t = s::case_dir();
        s::project toy(t / "toy", {{"mcpp.toml", std::format(R"([package]
name    = "toy-platform"
version = "0.1.0"

[platform]
toolchain = "llvm@{}"

[[platform.define]]
triple        = "x86_64-toyos"
clang-triple  = "x86_64-unknown-linux-gnu"
object-format = "elf"
family        = "unix"
execution     = "linux"

[[platform.provider]]
serves = ["x86_64-toyos"]

[platform.build]
cflags = ["-ffreestanding", "-fno-stack-protector", "-fno-pic"]

[platform.link]
driver = "compiler"

[platform.layers]
"c-abi" = false
"c++-abi" = false
"compiler-runtime" = false
"kernel-abi" = false
)", s::llvm_version())}});
        s::project app(t / "app", {{"mcpp.toml", R"([package]
name    = "toyapp"
version = "0.1.0"

[build]
sources = ["src/*.c"]
ldflags = ["-nostdlib", "-static", "-no-pie"]

[targets.toyapp]
kind = "bin"
main = "src/main.c"

[target.x86_64-toyos]
platform = { path = "../toy" }

[target.'cfg(os = "toyos")'.build]
cflags = ["-DTOY_SYSTEM=1"]
)"},
                                   {"src/main.c", "#ifndef TOY_SYSTEM\n#error \"cfg(os = \\\"toyos\\\") did not apply\"\n#endif\nvoid _start(void) {\n    __asm__ volatile(\"mov $60, %eax\\n mov $7, %edi\\n syscall\");\n}\n"}});
        auto r = app.mcpp({"build", "--target", "x86_64-toyos"});
        expect(fatal(e2e::succeeded(r))) << "the platform's row did not build: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "Target x86_64-toyos → x86_64-unknown-linux-gnu"))) << "the report names no clang triple of the row: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "Platform toy-platform 0.1.0"))) << "the platform is not reported: " << s::text(r);
        std::optional<fs::path> exe;
        for (const auto& f : s::find_files(app.at("target/x86_64-toyos"), "toyapp"))
            if (f.parent_path().filename() == "bin") { exe = f; break; }
        expect(fatal(exe.has_value())) << "no artifact under target/x86_64-toyos";
        expect(fatal(!s::read_header(*exe).elf_interpreter)) << "the artifact names a program interpreter: the host's C library leaked in";
        auto ran = s::run(*exe);
        expect(fatal(ran.started && ran.status == 7)) << "the artifact exited " << ran.status << ", not 7";
        r = app.mcpp({"run", "--target", "x86_64-toyos"});
        expect(fatal(r.started && r.status == 7)) << "mcpp run ended " << r.status << ", not 7: " << s::text(r);

        // A row whose artifacts run on another system: not judged by this machine's loader.
        toy.write("mcpp.toml", replaced(toy.read("mcpp.toml"), "execution     = \"linux\"", "execution     = \"toyos\""));
        r = app.mcpp({"build", "--target", "x86_64-toyos"});
        expect(fatal(e2e::succeeded(r))) << "a row for another system did not build: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "the artifacts are for toyos, not this machine"))) << "the build judged a foreign artifact by this machine: " << s::text(r);
        // Built, not run, without a runner; a runner states how it runs (mcpp#911 G11).
        r = app.mcpp({"run", "--target", "x86_64-toyos"});
        expect(fatal(r.started && r.status != 0)) << "an artifact for toyos was run on this host: " << s::text(r);
        expect(fatal(s::has_line(s::text(r), "x86_64-toyos artifacts run on toyos, not on this .* host: built, not run"))) << "the refusal does not say built, not run: " << s::text(r);
        const auto without_runner = app.read("mcpp.toml");
        app.write("mcpp.toml", replaced(without_runner, "platform = { path = \"../toy\" }\n", "platform = { path = \"../toy\" }\nrunner = [\"env\", \"{}\"]\n"));
        r = app.mcpp({"run", "--target", "x86_64-toyos"});
        expect(fatal(r.started && r.status == 7)) << "the runner did not run it (exit " << r.status << "): " << s::text(r);
        app.write("mcpp.toml", without_runner);

        // The platform names its programs; a build program's name for the host does not change
        // the platform's suffix, and the build says so (mcpp#911 B2).
        toy.write("mcpp.toml", replaced(toy.read("mcpp.toml"), "driver = \"compiler\"\n", "driver = \"compiler\"\nexe    = \"{name}.toy\"\n"));
        app.write("build.mcpp", "import std;\nint main() { std::println(\"mcpp:artifact-name=exe:{{name}}\"); }\n");
        r = app.mcpp({"build", "--target", "x86_64-toyos"});
        expect(fatal(e2e::succeeded(r))) << "the build with the program's name did not succeed: " << s::text(r);
        expect(fatal(s::has_line(s::text(r), R"(the platform toy-platform 0\.1\.0 names it `\{name\}\.toy`: the platform.s name stands)"))) << "the conflict of names is not reported: " << s::text(r);
        bool named = false;
        for (const auto& f : s::find_files(app.at("target/x86_64-toyos"), R"(toyapp\.toy)"))
            if (f.parent_path().filename() == "bin") named = true;
        expect(fatal(named)) << "the program is not named as the platform names it";
        app.remove("build.mcpp");

        // A row a platform defines is packed as a built-in row is (mcpp#911 G10).
        r = app.mcpp({"pack", "--target", "x86_64-toyos"});
        expect(fatal(e2e::succeeded(r))) << "the platform's row was not packed: " << s::text(r);
        const auto listing = s::tar_list(app.at("target/dist/toyapp-0.1.0-x86_64-toyos.tar.gz"));
        expect(fatal(std::ranges::any_of(listing, [](const std::string& e) { return e.ends_with("bin/toyapp.toy"); }))) << "the package holds no program";

        // `sed -i '/^\[target.x86_64-toyos\]/,/^platform/d'`
        app.write("mcpp.toml", replaced(app.read("mcpp.toml"), "[target.x86_64-toyos]\nplatform = { path = \"../toy\" }\n", ""));
        r = app.mcpp({"build", "--target", "x86_64-toyos"});
        expect(fatal(e2e::contains(s::text(r), "unknown target 'x86_64-toyos'"))) << "without the platform the request was not an unknown target: " << s::text(r);
    };

    "1049 a platform skeleton is made and its statement read as the engine reads it"_test[tag{"requires:elf"}][tag{"requires:llvm"}] = [] {
        // `mcpp new <name> --template platform` serves <arch>-<name>, which `why platform` lists
        // with its provider and priority, in the platform and in a project selecting it; a
        // program builds for the row; a provider serving nothing is a warning of the platform's
        // build; a redefined built-in row fails the project's build naming it.
        s::needs({"elf", "llvm"});
        const auto t = s::case_dir();
        const std::string arch(host_arch);
        auto r = s::mcpp({"new", "myos", "--template", "platform"}, t);
        expect(fatal(e2e::succeeded(r))) << "the skeleton was not made: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "mcpp why platform"))) << "the next step is not named: " << s::text(r);
        const s::project myos(t / "myos", {});
        r = myos.mcpp({"why", "platform"});
        expect(fatal(e2e::succeeded(r))) << "the skeleton is not read: " << s::text(r);
        expect(fatal(s::has_line(s::text(r), std::format("serves +{}-myos +myos +priority 100", arch)))) << "the served row is not listed: " << s::text(r);
        s::project app(t / "app", {{"mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/*.c\"]\nldflags = [\"-nostdlib\", \"-static\"]\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.c\"\n\n[target.{}-myos]\nplatform = {{ path = \"../myos\" }}\n", arch)},
                                   {"src/main.c", "void _start(void) { for (;;) {} }\n"}});
        r = app.mcpp({"build", "--target", arch + "-myos"});
        expect(fatal(e2e::succeeded(r))) << "a program did not build for the row: " << s::text(r);
        r = app.mcpp({"why", "platform", "--target", arch + "-myos"});
        expect(fatal(e2e::succeeded(r))) << "the selected platform is not read: " << s::text(r);
        expect(fatal(s::has_line(s::text(r), std::format("serves +{}-myos +myos +priority 100", arch)))) << "the project's platform is not listed: " << s::text(r);
        r = app.mcpp({"why", "platform", "--target", arch + "-myos", "--format", "json"});
        expect(fatal(e2e::succeeded(r))) << "no JSON: " << s::text(r);
        expect(fatal(s::has_line(s::text(r), std::format(R"("identity": *"{}-myos")", arch)))) << "the JSON lacks the served identity: " << s::text(r);
        myos.write("mcpp.toml", myos.read("mcpp.toml") + "\n[[platform.provider]]\nname   = \"nothing\"\nserves = [\"riscv64-nosuch\"]\n");
        r = myos.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "a warning ended the platform's build: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "[[platform.provider]] 'nothing' serves no row"))) << "a provider serving nothing is not said: " << s::text(r);
        myos.write("mcpp.toml", myos.read("mcpp.toml") + "\n[[platform.define]]\ntriple = \"x86_64-linux-gnu\"\n");
        r = app.mcpp({"build", "--target", arch + "-myos"});
        expect(fatal(failed(r))) << "a redefined built-in row did not fail the build: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "'x86_64-linux-gnu' is a built-in row"))) << "the redefinition is not named: " << s::text(r);
    };

    "1057 a platform row takes nothing of the payloads host"_test[tag{"requires:elf"}][tag{"requires:llvm"}] = [] {
        // A row a platform defines (i386-toy32): its `march` and `link-emulation` are the row's
        // words of the compile and the ld.lld link; nothing of the payload's own target side
        // (libc++ headers, `lib/<host triple>`, the gcc runtime) reaches it; its program is
        // ELF32.
        s::needs({"elf", "llvm"});
        const auto t = s::case_dir();
        s::project(t / "plat", {{"mcpp.toml", std::format(R"([package]
name    = "toy32-platform"
version = "0.1.0"

[platform]
toolchain = "llvm@{}"

[[platform.define]]
triple         = "i386-toy32"
clang-triple   = "i386-unknown-none-elf"
object-format  = "elf"
family         = "unix"
execution      = "toy32"
march          = "i386"
link-emulation = "elf_i386"

[[platform.provider]]
serves = ["i386-toy32"]

[platform.build]
cflags = ["-ffreestanding", "-fno-pic"]

[platform.link]
driver = "lld"
flags  = ["-e", "_start"]

[platform.layers]
"kernel-abi"       = false
"c-abi"            = false
"c++-abi"          = false
"compiler-runtime" = false
)", s::llvm_version())}});
        s::project app(t / "app", {{"mcpp.toml", "[package]\nname    = \"toy32\"\nversion = \"0.1.0\"\n\n[targets.toy32]\nkind = \"bin\"\nmain = \"src/main.c\"\n\n[target.i386-toy32]\nplatform = { path = \"../plat\" }\n"},
                                   {"src/main.c", "_Static_assert(sizeof(void*) == 4, \"a 32-bit pointer\");\nvoid _start(void) { for (;;) {} }\n"}});
        auto r = app.mcpp({"build", "--target", "i386-toy32"});
        expect(fatal(e2e::succeeded(r))) << "the row did not build: " << s::text(r);
        r = app.mcpp({"why", "flags", "--target", "i386-toy32"});
        const auto flags = s::text(r);
        expect(fatal(e2e::succeeded(r))) << "why flags failed: " << flags;
        expect(fatal(s::has_line(flags, "^ *row +-march=i386 "))) << "the row's march is not the row's word: " << flags;
        expect(fatal(s::has_line(flags, "^ *row +-m elf_i386 "))) << "the row's emulation is not on the link: " << flags;
        const std::regex host_side{R"(include/c\+\+/v1|/lib/[a-z0-9_]+-unknown-linux-gnu|gcc-runtime)"}, compiler_line{"^ *compiler "};
        std::istringstream in{flags};
        for (std::string line; std::getline(in, line);)
            expect(fatal(!std::regex_search(line, host_side) || std::regex_search(line, compiler_line))) << "the payload's own target side reached a platform row: " << line << "\n" << flags;
        std::optional<fs::path> exe;
        for (const auto& f : s::find_files(app.at("target/i386-toy32"), "toy32"))
            if (f.parent_path().filename() == "bin") { exe = f; break; }
        expect(fatal(exe.has_value())) << "no program";
        const auto h = s::read_header(*exe);
        expect(fatal(h.kind == s::format::elf && !h.wide)) << "the program is not ELF32";
    };
}};

}  // namespace

// Project-level build hooks: lifecycle order, policy, timeout, the directory a hook runs in, and
// the two places nothing may fire; a dependency's install hook (formerly tests/e2e/317, 648).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

// A file's text without CR and without trailing newlines (`$(tr -d '\r' < file)`); "" when absent.
std::string body(const fs::path& file) {
    if (!fs::exists(file)) return {};
    auto t = s::read_file(file);
    std::erase(t, '\r');
    while (!t.empty() && t.back() == '\n') t.pop_back();
    return t;
}

// `expect_log <file> <lines...>`: the file holds exactly these lines.
void expect_log(const fs::path& file, std::vector<std::string_view> lines) {
    std::string want;
    for (auto l : lines) want += std::string(l) + "\n";
    while (!want.empty() && want.back() == '\n') want.pop_back();
    expect(fatal(eq(body(file), want))) << file.filename().string();
}

std::uintmax_t size_of(const fs::path& f) { return fs::exists(f) ? fs::file_size(f) : 0; }

// The file grew, and then stopped growing.
bool frozen(const fs::path& beat) {
    const auto before = size_of(beat);
    std::this_thread::sleep_for(std::chrono::seconds{1});
    return before != 0 && before == size_of(beat);
}

E2E hooks{"hooks", [] {
    "317 project build hooks"_test = [] {
        // [hooks]: start/finished/failed in order, run in the project root; a failing or timed
        // out hook warns and keeps the result; side_effect = true is refused, enabled = false
        // runs nothing, an invalid timeout runs nothing, an unknown key is reported; nothing
        // fires when prepare fails; during_build ends with the build (grandchildren included),
        // loops on request and gives up warning; loop is refused elsewhere; Ctrl-C ends it; a
        // dependency's hooks and a workspace root's own hooks do not fire.
        const auto tmp = temp_dir();
        s::project app{tmp / "app", {{"src/main.cpp", "int main() { return 0; }\n"}}};
        fs::create_directories(app.at("nested"));
        auto manifest = [&](std::string_view hooks, std::string_view rest = "") {
            app.write("mcpp.toml", std::format("[package]\nname = \"hookprobe\"\nversion = \"0.1.0\"\n\n[hooks]\n{}{}", hooks, rest));
        };
        manifest("build_start = \"echo start>>hooks.log\"\nbuild_failed = \"echo failed>>hooks.log\"\nbuild_finished = \"echo finished>>hooks.log\"\ntimeout_seconds = 10\nenabled = true\nside_effect = false\n");
        expect(fatal(e2e::succeeded(app.mcpp_in("nested", {"build"})))) << "hooked build failed";
        expect(fatal(e2e::succeeded(app.mcpp_in("nested", {"build"})))) << "second hooked build failed";
        expect(fatal(!app.exists("nested/hooks.log"))) << "hook ran in the invocation directory, not the project root";
        expect_log(app.at("hooks.log"), {"start", "finished", "start", "finished"});

        app.write("src/main.cpp", "#error \"intentional hook failure path\"\nint main() { return 0; }\n");
        app.write("hooks.log", "");
        expect(fatal(failed(app.mcpp({"build"})))) << "broken source built";
        expect_log(app.at("hooks.log"), {"start", "failed"});
        app.write("src/main.cpp", "int main() { return 0; }\n");

        manifest("build_finished = \"mcpp-hook-command-that-does-not-exist\"\n");
        auto ignored = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(ignored))) << "a failing hook changed the build result";
        expect(fatal(e2e::contains(s::text(ignored), "warning: hook 'build_finished' exited with status"))) << "the hook failure was not reported";
        expect(fatal(s::lacks(s::text(ignored), "error: hook 'build_finished'"))) << "an experimental hook failure was reported as an error";
        manifest("build_finished = \"mcpp-hook-command-that-does-not-exist\"\nside_effect = false\n");
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "side_effect=false was not accepted";
        manifest("build_finished = \"echo done\"\nside_effect = true\n");
        auto side = app.mcpp({"build"});
        expect(fatal(failed(side))) << "side_effect=true was accepted";
        expect(fatal(e2e::contains(s::text(side), "experimental"))) << "the refusal does not say why";
        manifest("build_start = \"echo disabled>>disabled.log\"\nbuild_finished = \"echo disabled>>disabled.log\"\nenabled = false\n");
        app.remove("disabled.log");
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "disabled hook build failed";
        expect(fatal(!app.exists("disabled.log"))) << "enabled=false still ran hooks";

        const bool windows = s::on_windows();
        const std::string slow = windows ? "ping -n 6 127.0.0.1 >NUL" : "sleep 5";
        const std::string pause_2s = windows ? "ping -n 3 127.0.0.1 >NUL" : "sleep 2";
        const std::string pause_5s = windows ? "ping -n 6 127.0.0.1 >NUL" : "sleep 5";
        manifest(std::format("build_finished = \"{}\"\ntimeout_seconds = 1\n", slow));
        auto timeout = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(timeout))) << "a timed-out hook changed the build result";
        expect(fatal(s::has_line(s::text(timeout), R"(^warning: hook 'build_finished' timed out after 1s\r?$)"))) << "'warning: hook 'build_finished' timed out after 1s' is not a line of the output";
        manifest("build_start = \"echo should-not-run>>invalid.log\"\ntimeout_seconds = 0\n");
        app.remove("invalid.log");
        auto invalid = app.mcpp({"build"});
        expect(fatal(failed(invalid))) << "invalid hooks were accepted";
        expect(fatal(!app.exists("invalid.log"))) << "invalid hook ran";
        expect(fatal(e2e::contains(s::text(invalid), "[hooks].timeout_seconds must be a positive integer"))) << "invalid hook diagnostic is missing";
        app.write("hooks.log", "");
        manifest("build_start = \"echo start>>hooks.log\"\nbuild_finsihed = \"echo typo>>hooks.log\"\n");
        auto unknown = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(unknown))) << "unknown hook key was fatal";
        expect(fatal(e2e::contains(s::text(unknown), "[hooks] has unsupported key 'build_finsihed'"))) << "unknown hook key was not reported";
        expect_log(app.at("hooks.log"), {"start"});
        app.write("hooks.log", "");
        manifest("build_start = \"echo start>>hooks.log\"\nbuild_failed = \"echo failed>>hooks.log\"\n", "\n[dependencies]\nabsent = { path = \"no/such/dependency\" }\n");
        expect(fatal(failed(app.mcpp({"build"})))) << "a missing path dependency built";
        expect_log(app.at("hooks.log"), {});

        // during_build, with a grandchild that writes a heartbeat.
        std::string writer;
        if (windows) {
            app.write("writer.cmd", "@echo off\n:loop\necho tick>>beat.log\nping -n 2 127.0.0.1 >NUL\ngoto loop\n");
            writer = "start /b cmd /c writer.cmd";
        } else {
            app.write("writer.sh", "while :; do echo tick>>beat.log; sleep 0.2; done\n");
            writer = "sh writer.sh & wait";
        }
        app.write("hooks.log", "");
        app.remove("beat.log");
        manifest(std::format("build_start = \"{}\"\nbuild_finished = \"echo finished>>hooks.log\"\nduring_build = \"{}\"\n", pause_2s, writer));
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "build with during_build failed";
        expect(fatal(frozen(app.at("beat.log")))) << "during_build kept running after the build (or never ran)";
        expect_log(app.at("hooks.log"), {"finished"});
        auto runs = [&] { return s::count_lines(app.exists("runs.log") ? app.read("runs.log") : "", "run"); };
        app.remove("runs.log");
        manifest(std::format("build_start = \"{}\"\nduring_build = {{ cmd = \"echo run>>runs.log\", loop = true }}\n", pause_2s));
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "looped during_build failed the build";
        expect(fatal(runs() >= 2)) << "loop=true ran " << runs() << " time(s), expected >= 2";
        app.remove("runs.log");
        manifest(std::format("build_start = \"{}\"\nduring_build = \"echo run>>runs.log\"\n", pause_2s));
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "unlooped during_build failed the build";
        expect(fatal(eq(runs(), std::size_t{1}))) << "loop=false ran " << runs() << " time(s), expected 1";
        manifest(std::format("build_start = \"{}\"\nduring_build = {{ cmd = \"mcpp-hook-command-that-does-not-exist\", loop = true }}\n", pause_5s));
        auto giveup = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(giveup))) << "a during_build that never ran changed the build result";
        expect(fatal(e2e::contains(s::text(giveup), "warning: hook 'during_build' failed to stay up"))) << "giving up was not reported";
        manifest("build_start = { cmd = \"echo start\", loop = true }\n");
        auto misplaced = app.mcpp({"build"});
        expect(fatal(failed(misplaced))) << "loop on build_start was accepted";
        expect(fatal(e2e::contains(s::text(misplaced), "during_build"))) << "the diagnostic does not name the event that supports loop";
        if (!windows) {
            // Ctrl-C: the shell runs the build in the background and sends it SIGINT after 3 s.
            app.remove("beat.log");
            manifest(std::format("build_start = \"sleep 20\"\nduring_build = \"{}\"\n", writer));
            const auto sh = s::on_path("sh");
            expect(fatal(sh.has_value())) << "no sh";
            s::run(*sh, {"-c", "\"$0\" build > interrupted.log 2>&1 & pid=$!; sleep 3; kill -INT \"$pid\" 2>/dev/null; wait \"$pid\" 2>/dev/null; true", s::mcpp_path()}, app.dir());
            expect(fatal(frozen(app.at("beat.log")))) << "Ctrl-C left during_build running";
        }

        // A dependency's hooks do not fire.
        s::project dep{tmp / "dep", {{"src/dep.cppm", "export module dep;\nexport int dep_answer() { return 0; }\n"},
                                     {"mcpp.toml", "[package]\nname = \"dep\"\nversion = \"0.1.0\"\n\n[targets.dep]\nkind = \"lib\"\n\n[hooks]\nbuild_start = \"echo dependency>>dependency-hook.log\"\nbuild_finished = \"echo dependency>>dependency-hook.log\"\n"}}};
        app.write("hooks.log", "");
        app.remove("dependency-hook.log");
        manifest("build_start = \"echo start>>hooks.log\"\nbuild_finished = \"echo finished>>hooks.log\"\n", std::format("\n[dependencies]\ndep = {{ path = \"{}\" }}\n", dep.dir().generic_string()));
        app.write("src/main.cpp", "import dep;\nint main() { return dep_answer(); }\n");
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "build with a hooked dependency failed";
        expect_log(app.at("hooks.log"), {"start", "finished"});
        expect(fatal(!dep.exists("dependency-hook.log") && !app.exists("dependency-hook.log"))) << "a dependency's hooks ran";

        // A workspace root's own hooks do not fire; each member's do, in the member.
        s::project ws{tmp / "ws", {{"mcpp.toml", "[workspace]\nmembers = [\"alpha\", \"beta\"]\n\n[hooks]\nbuild_start = \"echo root>>root.log\"\nbuild_finished = \"echo root>>root.log\"\n"}}};
        for (std::string_view m : {"alpha", "beta"}) {
            ws.write(std::format("{}/src/main.cpp", m), "int main() { return 0; }\n");
            ws.write(std::format("{}/mcpp.toml", m), std::format("[package]\nname = \"{0}\"\nversion = \"0.1.0\"\n\n[hooks]\nbuild_start = \"echo {0}-start>>hooks.log\"\nbuild_finished = \"echo {0}-finished>>hooks.log\"\n", m));
        }
        expect(fatal(e2e::succeeded(ws.mcpp({"build"})))) << "workspace build with hooks failed";
        expect_log(ws.at("alpha/hooks.log"), {"alpha-start", "alpha-finished"});
        expect_log(ws.at("beta/hooks.log"), {"beta-start", "beta-finished"});
        expect(!ws.exists("root.log")) << "the workspace root's own hooks fired";
        expect(!ws.exists("hooks.log")) << "a member hook ran in the workspace root";
    };
    "648 an install hook sees the build target"_test[tag{"requires:elf"}] = [] {
        // A dependency's install hook receives the build's target under the names a build
        // program uses, and the two toolchain names present and empty (the toolchain is resolved
        // after the graph), whatever the caller's environment holds.
        s::needs({"elf"});
        s::project p{s::files{{"local-index/pkgs/h/acme.hookprobe.lua", R"cpp(package = {
    spec = "1",
    namespace = "acme",
    name = "hookprobe",
    description = "Compiles in what its install hook saw",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux   = { ["latest"] = { ref = "1.0.0" }, ["1.0.0"] = { } },
        macosx  = { ["latest"] = { ref = "1.0.0" }, ["1.0.0"] = { } },
        windows = { ["latest"] = { ref = "1.0.0" }, ["1.0.0"] = { } },
    },
    mcpp = {
        language = "c++23",
        import_std = false,
        sources = { "src/hookprobe.cppm" },
        targets = { ["hookprobe"] = { kind = "lib" } },
        deps = {},
    },
}

import("xim.libxpkg.pkginfo")

function install()
    local dir = pkginfo.install_dir()
    os.mkdir(path.join(dir, "src"))
    local function value(name) return os.getenv(name) or "" end
    io.writefile(path.join(dir, "src", "hookprobe.cppm"), string.format([[
export module hookprobe;
export const char* hook_compiler()  { return "%s"; }
export const char* hook_stdlib()    { return "%s"; }
export const char* hook_target_os() { return "%s"; }
export const char* hook_target()    { return "%s"; }
]], value("MCPP_COMPILER"), value("MCPP_CXX_STDLIB"), value("MCPP_TARGET_OS"),
    value("MCPP_TARGET")))
    return true
end
)cpp"},
                      {"src/main.cpp", R"cpp(#include <cstdio>
import hookprobe;
int main() {
    std::printf("compiler=%s stdlib=%s os=%s target=%s\n",
                hook_compiler(), hook_stdlib(), hook_target_os(), hook_target());
    return 0;
}
)cpp"},
                      {"mcpp.toml", R"cpp([package]
name    = "consumer"
version = "0.1.0"

[dependencies.acme]
hookprobe = "1.0.0"

[indices]
acme = { path = "local-index" }
)cpp"}}};
        auto r = p.mcpp({"run"}, {.where = s::home::inherited, .env = {{"MCPP_COMPILER", "inherited"}, {"MCPP_CXX_STDLIB", "inherited"}}});
        expect(fatal(e2e::succeeded(r))) << "the build or the run failed";
        expect(s::has_line(s::text(r), "^compiler= stdlib= os=linux target=[a-z0-9_]+-linux")) << "the install hook did not see the build's target with empty toolchain names";
    };
}};

}  // namespace

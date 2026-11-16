// A Windows image as mcpp links it: its subsystem and entry from the target's keys or a build
// program's directive, inert on a target that is not PE; the runtime DLLs a dependency declares
// placed beside the program; a cross link that declares the file it produces and is not redone;
// MSVC refused off Windows (formerly tests/e2e/84, 96, 183, 642, 643, 644 and
// _windows_subsystem_body.sh).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

std::string bytes_of(const fs::path& f) {
    std::ifstream in(f, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

// The project of the subsystem body: gui and winmain state the windows subsystem, tool takes it
// from a build program's directive, cli and wide are console programs (wide enters at wmain).
s::project subsystem_project() {
    return s::project{{"src/probe.cppm", "export module probe;\nexport int probe_value() { return 7; }\n"},
                      {"app/gui.cpp", "#include <cstdio>\nstruct Probe {\n    Probe() {\n        if (auto f = std::fopen(\"order.txt\", \"w\")) { std::fputs(\"ctor\\n\", f); std::fclose(f); }\n    }\n};\n"
                                      "static Probe probe_instance;\nint main() {\n    if (auto f = std::fopen(\"order.txt\", \"a\")) { std::fputs(\"main\\n\", f); std::fclose(f); }\n    return 0;\n}\n"},
                      {"app/cli.cpp", "int main() { return 0; }\n"},
                      {"app/tool.cpp", "int main() { return 0; }\n"},
                      {"app/wide.cpp", "int wmain() { return 0; }\n"},
                      {"app/winmain.cpp", "#include <windows.h>\nint WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) { return 0; }\n"},
                      {"build.mcpp", "import mcpp;\nint main() {\n    mcpp::windows_subsystem(\"tool\", \"windows\");\n    return 0;\n}\n"},
                      {"mcpp.toml", "[package]\nname    = \"subsys\"\nversion = \"0.1.0\"\n\n[modules]\nsources = [\"src/**/*.cppm\"]\n\n"
                                    "[targets.gui]\nkind              = \"bin\"\nmain              = \"app/gui.cpp\"\nwindows_subsystem = \"windows\"\n\n"
                                    "[targets.cli]\nkind = \"bin\"\nmain = \"app/cli.cpp\"\n\n[targets.tool]\nkind = \"bin\"\nmain = \"app/tool.cpp\"\n\n"
                                    "[targets.wide]\nkind          = \"bin\"\nmain          = \"app/wide.cpp\"\nwindows_entry = \"wmain\"\n\n"
                                    "[targets.winmain]\nkind              = \"bin\"\nmain              = \"app/winmain.cpp\"\nwindows_subsystem = \"windows\"\nwindows_entry     = \"WinMain\"\n"}};
}

fs::path exe_of(const s::project& p, std::string_view name) {
    const auto found = s::glob(p.at("target"), std::format("*/*/bin/{}.exe", name));
    expect(fatal(!found.empty())) << "no " << name << ".exe under target/";
    return found.front();
}

// 1 the subsystem bytes (2 the windows subsystem, 3 the console); 3 where this host executes a PE
// image (directly, or through `runner`), a static constructor runs before main and wmain enters.
void subsystems(const s::project& p, std::vector<std::string> build_args, std::optional<fs::path> runner) {
    auto args = std::vector<std::string>{"build"};
    args.insert(args.end(), build_args.begin(), build_args.end());
    auto r = p.mcpp(args);
    expect(fatal(e2e::succeeded(r))) << "the build failed: " << s::text(r);
    for (const auto& [name, want] : std::vector<std::pair<std::string_view, int>>{{"gui", 2}, {"tool", 2}, {"winmain", 2}, {"cli", 3}, {"wide", 3}}) {
        const auto h = s::read_header(exe_of(p, name));
        expect(fatal(h.kind == s::format::pe && h.subsystem == want)) << name << ".exe has Subsystem " << h.subsystem << ", expected " << want;
    }
    if (!runner && !s::on_windows()) return;  // criterion 3 is not measured on a host that cannot execute a PE image
    const auto run = [&](const fs::path& exe) {
        if (runner) return s::run(*runner, {exe.string()}, p.dir(), {{"WINEDEBUG", "-all"}});
        return s::run(exe, {}, p.dir());
    };
    p.remove("order.txt");
    auto ran = run(exe_of(p, "gui"));
    expect(fatal(e2e::succeeded(ran))) << "gui.exe did not exit 0: " << s::text(ran);
    auto order = p.exists("order.txt") ? p.read("order.txt") : std::string{};
    std::erase(order, '\r');
    expect(fatal(order == "ctor\nmain\n")) << "the static constructor did not run before main: " << order;
    ran = run(exe_of(p, "wide"));
    expect(fatal(e2e::succeeded(ran))) << "wide.exe did not exit 0: " << s::text(ran);
}

E2E images{"images", [] {
    "84 runtime dll deploy"_test = [] {
        // A dependency's [runtime] library_dirs: each *.dll is placed beside the program, byte
        // for byte, and nothing else of that directory is.
        const auto tmp = s::case_dir();
        s::project lib(tmp / "blasish", {{"mcpp.toml", "[package]\nname    = \"blasish\"\nversion = \"0.1.0\"\n\n[targets.blasish]\nkind = \"lib\"\n\n[runtime]\nlibrary_dirs = [\"bin\"]\n"},
                                         {"src/blasish.cppm", "export module blasish;\nexport int blasish_anchor() { return 0; }\n"},
                                         {"bin/libdummy.dll", "MZ stub dll payload"},
                                         {"bin/readme.txt", "not a dll"}});
        s::project app(tmp / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\nblasish = { path = \"../blasish\" }\n"},
                                     {"src/main.cpp", "import blasish;\nint main() { return blasish_anchor(); }\n"}});
        auto r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "build errored: " << s::text(r);
        auto bin = s::find_file(app.at("target"), "app");
        if (!bin) bin = s::find_file(app.at("target"), "app.exe");
        expect(fatal(bin.has_value())) << "built binary not found under target/";
        const auto dir = bin->parent_path();
        expect(fatal(fs::is_regular_file(dir / "libdummy.dll"))) << "libdummy.dll was not deployed beside the executable (" << dir.string() << ")";
        expect(fatal(bytes_of(dir / "libdummy.dll") == "MZ stub dll payload")) << "the deployed libdummy.dll differs from its source";
        expect(fatal(!fs::exists(dir / "readme.txt"))) << "the non-DLL readme.txt was deployed (only *.dll should be)";
    };

    "96 msvc unavailable nonwindows"_test[s::unix_shell] = [] {
        // Off Windows, `toolchain default msvc`, `toolchain install msvc` and a build of a
        // manifest that pins msvc@system are refused, saying MSVC is only available on Windows;
        // the listing has no System section.
        const s::options fresh{.where = s::home::fresh};
        for (std::vector<std::string> args : {std::vector<std::string>{"toolchain", "default", "msvc"}, std::vector<std::string>{"toolchain", "install", "msvc"}}) {
            auto r = s::mcpp(args, s::case_dir(), fresh);
            expect(fatal(failed(r) && e2e::contains(s::text(r), "only available on Windows"))) << args[1] << " msvc: " << s::text(r);
        }
        auto r = s::mcpp({"toolchain", "list"}, s::case_dir(), fresh);
        expect(fatal(s::lacks(s::text(r), "System:"))) << "a System section off Windows: " << s::text(r);
        s::project p{{"mcpp.toml", "[package]\nname    = \"msvcpin\"\nversion = \"0.1.0\"\n\n[toolchain]\ndefault = \"msvc@system\"\n"},
                     {"src/main.cpp", "import std;\nint main() { std::println(\"never built\"); return 0; }\n"}};
        r = p.mcpp({"build"}, fresh);
        expect(fatal(failed(r) && e2e::contains(s::text(r), "only available on Windows"))) << "build with an msvc@system manifest: " << s::text(r);
    };

    "183 cross no relink"_test[s::mingw_cross] = [] {
        // A cross link declares in build.ninja the file it produces (the program with its
        // suffix), so an up-to-date rebuild does not link it again.
        const std::string t = "x86_64-windows-gnu";
        s::project p{{"mcpp.toml", "[package]\nname    = \"relinkprobe\"\nversion = \"0.1.0\"\n"}, {"src/main.cpp", "int main() { return 0; }\n"}};
        auto r = p.mcpp({"build", "--target", t});
        expect(fatal(e2e::succeeded(r))) << "the first cross build failed: " << s::text(r);
        const auto art = s::glob(p.at("target") / t, "*/bin/relinkprobe*");
        expect(fatal(!art.empty())) << "no artifact produced under target/" << t;
        const auto ninja = s::find_file(p.at("target") / t, "build.ninja");
        expect(fatal(ninja.has_value())) << "no build.ninja found";
        std::string declared;
        for (const auto& l : s::lines_with(s::read_file(*ninja), "bin/relinkprobe")) {
            std::smatch m;
            if (std::regex_search(l, m, std::regex(R"(^build ([^:]*bin/relinkprobe[^ :]*))"))) { declared = m[1].str(); break; }
        }
        expect(fatal(!declared.empty())) << "no link edge for relinkprobe in build.ninja";
        expect(fatal(fs::is_regular_file(ninja->parent_path() / declared))) << "ninja declares output '" << declared << "' but that file does not exist (produced: "
                                                                            << art.front().filename().string() << "); the link edge reruns on every build";
        const auto t1 = fs::last_write_time(art.front());
        std::this_thread::sleep_for(std::chrono::seconds{1});
        r = p.mcpp({"build", "--target", t});
        expect(fatal(e2e::succeeded(r))) << "the second cross build failed: " << s::text(r);
        expect(fatal(fs::last_write_time(art.front()) == t1)) << "the artifact was linked again on an up-to-date rebuild";
    };

    "642 windows subsystem"_test[s::windows] = [] {
        // The subsystem body on Windows, and `mcpp test`'s programs are console programs.
        auto p = subsystem_project();
        subsystems(p, {}, std::nullopt);
        p.write("tests/smoke.cpp", "int main() { return 0; }\n");
        auto r = p.mcpp({"test"});
        expect(fatal(e2e::succeeded(r))) << "mcpp test failed: " << s::text(r);
        int checked = 0;
        for (const auto& exe : s::find_files(p.at("target"), R"(.*\.exe)")) {
            const auto name = exe.filename().string();
            if (name == "gui.exe" || name == "tool.exe" || name == "winmain.exe") continue;
            expect(fatal(s::read_header(exe).subsystem == 3)) << exe.string() << " has Subsystem " << s::read_header(exe).subsystem << ", expected 3";
            ++checked;
        }
        expect(fatal(checked >= 3)) << "only " << checked << " console executables were found";
    };

    "643 windows subsystem cross"_test[s::mingw_cross] = [] {
        // The subsystem body through x86_64-windows-gnu; its run criterion through wine where
        // wine is installed.
        subsystems(subsystem_project(), {"--target", "x86_64-windows-gnu"}, s::on_path("wine"));
    };

    "644 windows keys are inert elsewhere"_test[s::elf] = [] {
        // On an ELF target the keys and the directive change no byte of the artefact and say
        // nothing; a directive naming no target and a library target with the key are refused,
        // naming them.
        const auto write = [](const s::project& p, std::string_view extra) {
            p.write("mcpp.toml", std::format("[package]\nname    = \"inert\"\nversion = \"0.1.0\"\n\n[targets.inert]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n{}\n", extra));
        };
        s::project p{{"src/main.cpp", "int main() { return 0; }\n"}};
        const auto artefact = [&] {
            const auto f = s::glob(p.at("target"), "*/*/bin/inert");
            expect(fatal(!f.empty())) << "no artefact";
            return bytes_of(f.front());
        };
        write(p, "");
        auto r = p.mcpp({"build", "--release"});
        expect(fatal(e2e::succeeded(r))) << "the plain build failed: " << s::text(r);
        const auto plain = artefact();
        fs::remove_all(p.at("target"));
        write(p, "windows_subsystem = \"windows\"\nwindows_entry     = \"wWinMain\"");
        r = p.mcpp({"build", "--release"});
        expect(fatal(e2e::succeeded(r))) << "the build with the keys failed: " << s::text(r);
        expect(fatal(artefact() == plain)) << "the keys changed the ELF artefact";
        expect(fatal(!s::has_line_nocase(s::text(r), "subsystem|windows_entry|unsupported key"))) << "the keys produced output on a target that is not PE: " << s::text(r);
        fs::remove_all(p.at("target"));
        write(p, "");
        p.write("build.mcpp", "import mcpp;\nint main() {\n    mcpp::windows_subsystem(\"inert\", \"windows\");\n    return 0;\n}\n");
        r = p.mcpp({"build", "--release"});
        expect(fatal(e2e::succeeded(r))) << "the build with the directive failed: " << s::text(r);
        expect(fatal(artefact() == plain)) << "the directive changed the ELF artefact";
        p.write("build.mcpp", "import mcpp;\nint main() {\n    mcpp::windows_subsystem(\"nosuch\", \"windows\");\n    return 0;\n}\n");
        r = p.mcpp({"build", "--release"});
        expect(fatal(failed(r))) << "a directive naming no target was accepted";
        expect(fatal(e2e::contains(s::text(r), "declares no target named `nosuch`"))) << "the directive refusal does not name the target: " << s::text(r);
        s::project lib(s::case_dir() / "lib", {{"src/core.cppm", "export module core;\nexport int core_value() { return 1; }\n"},
                                               {"mcpp.toml", "[package]\nname    = \"lib\"\nversion = \"0.1.0\"\n\n[targets.core]\nkind              = \"lib\"\nwindows_subsystem = \"windows\"\n"}});
        r = lib.mcpp({"build"});
        expect(fatal(failed(r))) << "a library target accepted windows_subsystem";
        expect(fatal(e2e::contains(s::text(r), "targets.core.windows_subsystem applies to an executable"))) << "the refusal does not name the target and the key: " << s::text(r);
    };
}};

}  // namespace

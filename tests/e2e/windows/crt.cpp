// The CRT model is a property of the MSVC ABI, not of the compiler: clang on x86_64-windows-msvc
// receives the model cl.exe does (`-fms-runtime-lib=static`/`=dll`), its default is
// toolchain-coupled (the dynamic CRT, the toolset's DLLs staged beside the artifact), and an
// explicit request is delivered as recorded (#649 E10, #718; formerly tests/e2e/703, 814, 888). Each
// read the llvm row only: a machine whose default toolchain is msvc@system skips them, the cl.exe
// cells being covered by the unit property test and by 180.
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

// The distributable contract of the build's resolution.json.
std::string recorded(const fs::path& project) {
    for (const auto& f : s::find_files(project / "target", "resolution\\.json")) {
        if (f.generic_string().contains("/.build-mcpp/")) continue;
        auto text = s::read_file(f);
        std::erase_if(text, [](char c) { return c == ' ' || c == '\n' || c == '\r'; });
        std::smatch m;
        if (std::regex_search(text, m, std::regex(R"re("distributable":"([a-z-]*)")re"))) return m[1].str();
        return {};
    }
    expect(fatal(false)) << "no resolution.json under " << project.string();
    return {};
}

std::string app_manifest(std::string_view build_lines, bool dev) {
    return std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[build]\n{}{}\n", dev ? "default-profile = \"dev\"\n" : "", build_lines);
}

// The default build of `p`; skips the case when the default toolchain is not the llvm row.
e2e::outcome default_build_on_llvm_row(s::project& p) {
    auto r = p.mcpp({"build"});
    expect(fatal(e2e::succeeded(r))) << "the default build failed: " << s::text(r);
    if (!e2e::contains(s::text(r), "Resolved llvm@")) {
        const auto resolved = s::lines_with(s::text(r), "Resolved");
        skip(std::format("the default toolchain here is not the llvm row ({})", resolved.empty() ? "?" : resolved.front()));
    }
    return r;
}

fs::path program_of(const s::project& p) {
    for (const auto& f : s::find_files(p.at("target"), "app\\.exe"))
        if (f.parent_path().generic_string().contains("/bin")) return f;
    expect(fatal(false)) << "no app.exe produced";
    return {};
}

// The DLL names of an image's import table, read by the llvm payload's llvm-objdump.
std::string imports_of(const fs::path& exe) {
    const auto objdump = s::payload_root("xim-x-llvm", s::llvm_version()) / "bin" / s::exe("llvm-objdump");
    expect(fatal(fs::exists(objdump))) << "no llvm-objdump in the llvm payload (" << objdump.string() << ")";
    std::string names;
    for (const auto& line : s::lines_with(s::run(objdump, {"-p", exe.string()}).out, "DLL Name")) names += line + "\n";
    return names;
}

// The number of files of the shared std cache whose text holds `word`.
std::size_t std_cache_files_with(std::string_view word) {
    std::size_t n = 0;
    std::error_code ec;
    const auto root = s::shared_home() / "build-cache/v1/std";
    for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator{}; it.increment(ec))
        if (it->is_regular_file(ec) && s::read_file(it->path()).contains(word)) ++n;
    return n;
}

E2E crt{"crt", [] {
    "703 the llvm row on the msvc abi records its static crt"_test[s::windows] = [] {
        // What resolution.json records for the llvm row's C++ runtime contract: toolchain-coupled
        // by default, and an explicit host-coupled or self-contained delivered as recorded, with
        // no "is not delivered" message.
        s::project p{{"src/main.cpp", "import std;\nint main() { std::println(\"crt\"); }\n"}, {"mcpp.toml", app_manifest("", false)}};
        default_build_on_llvm_row(p);
        expect(fatal(recorded(p.dir()) == "toolchain-coupled")) << "the llvm row's undeclared default recorded '" << recorded(p.dir()) << "'";
        p.write("mcpp.toml", app_manifest("cxx_runtime = \"host-coupled\"", false));
        fs::remove_all(p.at("target"));
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "the host-coupled build failed: " << s::text(r);
        expect(fatal(!e2e::contains(s::text(r), "is not delivered for clang on the MSVC ABI"))) << "an explicit host-coupled request still prints the old E10 message: " << s::text(r);
        expect(fatal(recorded(p.dir()) == "host-coupled")) << "an explicit host-coupled request recorded '" << recorded(p.dir()) << "'";
        p.write("mcpp.toml", app_manifest("cxx_runtime = \"self-contained\"", false));
        fs::remove_all(p.at("target"));
        r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "the self-contained build failed: " << s::text(r);
        expect(fatal(recorded(p.dir()) == "self-contained")) << "an explicit self-contained request recorded '" << recorded(p.dir()) << "'";
    };

    "814 the llvm row defaults to the dynamic crt"_test[s::windows] = [] {
        // The default program imports vcruntime140.dll, staged beside it, and runs with the
        // toolset off PATH; self-contained restores the static CRT and imports no CRT DLL;
        // switching A, B, A keeps both std BMI identities valid; `mcpp pack` carries the DLL, and
        // `--mode system` resolves the defaulted contract to host-coupled instead of refusing; a
        // dependency's contradicting CRT word is refused by name, an agreeing one accepted
        // silently.
        s::project p{{"src/main.cpp", "import std;\nint main() { std::println(\"crt-abi-ok\"); return 0; }\n"}, {"mcpp.toml", app_manifest("", true)}};
        default_build_on_llvm_row(p);
        auto exe = program_of(p);
        auto imports = imports_of(exe);
        expect(fatal(s::contains_nocase(imports, "vcruntime140"))) << "the default program imports no vcruntime140.dll: " << imports;
        expect(fatal(fs::exists(exe.parent_path() / "vcruntime140.dll"))) << "vcruntime140.dll was not staged beside the exe";
        // Only the staged copy may serve it: the toolset's directories are off PATH.
        const auto windir = std::getenv("SystemRoot") ? std::string(std::getenv("SystemRoot")) : std::string("C:\\Windows");
        auto run = s::run(exe, {}, exe.parent_path(), {{"PATH", windir + "\\System32"}});
        expect(fatal(e2e::succeeded(run) && e2e::contains(run.out, "crt-abi-ok"))) << "the default program did not run with the VS directories off PATH: " << s::text(run);
        expect(fatal(recorded(p.dir()) == "toolchain-coupled")) << "the undeclared default recorded '" << recorded(p.dir()) << "'";
        expect(fatal(std_cache_files_with("-fms-runtime-lib=dll") >= 1)) << "no std BMI command recorded the dynamic CRT word";

        p.write("mcpp.toml", app_manifest("cxx_runtime = \"self-contained\"", true));
        fs::remove_all(p.at("target"));
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "the self-contained build failed: " << s::text(r);
        exe = program_of(p);
        imports = imports_of(exe);
        expect(fatal(!s::contains_nocase(imports, "vcruntime") && !s::contains_nocase(imports, "msvcp"))) << "self-contained still imports a CRT DLL: " << imports;
        expect(fatal(!fs::exists(exe.parent_path() / "vcruntime140.dll"))) << "self-contained staged vcruntime140.dll";
        expect(fatal(recorded(p.dir()) == "self-contained")) << "self-contained recorded '" << recorded(p.dir()) << "'";
        expect(fatal(std_cache_files_with("-fms-runtime-lib=static") >= 1)) << "no std BMI command recorded the static CRT word";

        p.write("mcpp.toml", app_manifest("", true));
        fs::remove_all(p.at("target"));
        r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "the second default build failed: " << s::text(r);
        expect(fatal(s::contains_nocase(imports_of(program_of(p)), "vcruntime140"))) << "switching back to the default lost the dynamic CRT";

        // The default pack carries the DLL; `--mode system` on a defaulted (never declared)
        // toolchain-coupled contract resolves it to host-coupled. A package's names are stored
        // plainly in a zip's directory, so the archive's bytes are read for the name.
        r = p.mcpp({"pack"});
        expect(fatal(e2e::succeeded(r))) << "the default pack failed: " << s::text(r);
        bool carried = false;
        for (auto it = fs::recursive_directory_iterator(p.at("target/dist")); it != fs::recursive_directory_iterator{}; ++it)
            carried = carried || s::contains_nocase(it->path().filename().string(), "vcruntime140.dll") ||
                      (it->is_regular_file() && it->path().extension() == ".zip" && s::contains_nocase(s::read_file(it->path()), "vcruntime140.dll"));
        expect(fatal(carried)) << "the default pack did not carry vcruntime140.dll: " << s::text(r);
        fs::remove_all(p.at("target/dist"));
        r = p.mcpp({"pack", "--mode", "system", "--pkg-format", "dir"});
        expect(fatal(e2e::succeeded(r))) << "--mode system refused a defaulted toolchain-coupled contract: " << s::text(r);
        for (auto it = fs::recursive_directory_iterator(p.at("target/dist")); it != fs::recursive_directory_iterator{}; ++it)
            expect(fatal(!s::contains_nocase(it->path().filename().string(), "vcruntime140.dll"))) << "--mode system still bundled vcruntime140.dll for a defaulted (host-coupled) contract: " << it->path().string();

        // A dependency's [build] cxxflags reach its own units after the graph's flags: a CRT word
        // there that contradicts the root's model is refused, naming the dependency.
        const auto tmp = s::case_dir();
        s::project dep(tmp / "dep", {{"src/dep.cppm", "export module dep;\nexport int dep_value() { return 7; }\n"}});
        const auto write_dep = [&](std::string_view word) {
            dep.write("mcpp.toml", std::format("[package]\nname    = \"dep\"\nversion = \"0.1.0\"\n\n[targets.dep]\nkind = \"lib\"\n\n[build]\ncxxflags = [\"{}\"]\n", word));
        };
        s::project user(tmp / "user", {{"src/main.cpp", "import dep;\nint main() { return dep_value() == 7 ? 0 : 1; }\n"},
                                       {"mcpp.toml", "[package]\nname    = \"user\"\nversion = \"0.1.0\"\n\n[dependencies]\ndep = { path = \"../dep\" }\n"}});
        write_dep("-fms-runtime-lib=static");
        r = user.mcpp({"build"});
        expect(fatal(failed(r))) << "a dependency's contradicting CRT word was not refused";
        expect(fatal(e2e::contains(s::text(r), "dependency 'dep'") && e2e::contains(s::text(r), "-fms-runtime-lib=static"))) << "the refusal does not name the dependency and the word: " << s::text(r);
        write_dep("-fms-runtime-lib=dll");
        r = user.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "a dependency's agreeing CRT word was refused: " << s::text(r);
        expect(fatal(!e2e::contains(s::text(r), "agrees with the CRT model"))) << "a dependency's agreeing CRT word was warned: " << s::text(r);
    };
    "888 the llvm row in a developer environment"_test[s::windows] = [] {
        // vcvars exports WindowsSdkDir with a trailing backslash; rendered as "...\Windows Kits\10\"
        // the closing quote was escaped under the Windows argv rules and the std module's precompile
        // received the rest of the command as inputs. `import std` and `import std.compat` (the MSVC
        // STL) build and run on the llvm row with that variable set (2026.10.5.2).
        if (!fs::is_directory("C:/Program Files (x86)/Windows Kits/10/Include")) skip("no Windows SDK at the conventional root");
        const std::vector<std::pair<std::string, std::optional<std::string>>> env{{"WindowsSdkDir", "C:\\Program Files (x86)\\Windows Kits\\10\\"}};
        s::project std_project(s::case_dir() / "std", {{"mcpp.toml", "[package]\nname = \"s\"\nversion = \"0.1.0\"\nstandard = \"c++23\"\n"},
                                                       {"src/main.cpp", "import std;\nint main() { std::println(\"std\"); }\n"}});
        auto r = std_project.mcpp({"run"}, {.env = env});
        expect(fatal(e2e::succeeded(r))) << "import std failed with WindowsSdkDir ending in a backslash: " << s::text(r);
        if (!e2e::contains(s::text(r), "Resolved llvm@")) {
            const auto resolved = s::lines_with(s::text(r), "Resolved");
            skip(std::format("the default toolchain here is not the llvm row ({})", resolved.empty() ? "?" : resolved.front()));
        }
        expect(fatal(s::has_line(r.out, "^std"))) << "the std program did not run: " << s::text(r);
        s::project compat(s::case_dir() / "compat", {{"mcpp.toml", "[package]\nname = \"c\"\nversion = \"0.1.0\"\nstandard = \"c++23\"\n"},
                                                     {"src/main.cpp", "import std.compat;\nint main() {\n    auto* memory = ::malloc(32);\n    if (!memory) return 1;\n    ::free(memory);\n    std::vector<int> values{1, 2, 3};\n    ::printf(\"compat=%zu\\n\", values.size());\n    return values.size() == 3 ? 0 : 1;\n}\n"}});
        r = compat.mcpp({"run"}, {.env = env});
        expect(fatal(e2e::succeeded(r) && e2e::contains(s::text(r), "compat=3"))) << "import std.compat failed on the llvm row: " << s::text(r);
    };
}};

}  // namespace

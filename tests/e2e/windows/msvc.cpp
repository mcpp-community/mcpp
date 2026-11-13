// The MSVC ABI on a Windows host: `msvc@system` is located, never managed, and builds natively
// (modules, `import std` at C++20, incremental); one toolset is chosen once and every consumer reads
// it; a build program reads the toolset as facts and builds the build program of a cross target;
// objects carry their own debug information; LTO meets export discovery; two programs each bind
// their own module of one name; clang's missing coroutines on the 32-bit ABI are explained
// (formerly tests/e2e/95, 99, 177, 180, 241, 760, 825, 848, 887, 889, 892, 1017, 1023).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

std::string trimmed(std::string text) {
    std::erase(text, '\r');
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
    return text;
}

// The build graph of the project's build (not of its build program's).
std::vector<fs::path> ninja_files(const fs::path& project) {
    std::vector<fs::path> found;
    for (const auto& f : s::find_files(project / "target", "build\\.ninja"))
        if (!f.generic_string().contains("/.build-mcpp/")) found.push_back(f);
    return found;
}
fs::path ninja_of(const fs::path& project) {
    const auto all = ninja_files(project);
    expect(fatal(!all.empty())) << "no build.ninja under " << project.string();
    return all.front();
}
fs::path newest_ninja_of(const fs::path& project) {
    auto all = ninja_files(project);
    expect(fatal(!all.empty())) << "no build.ninja under " << project.string();
    return *std::ranges::max_element(all, {}, [](const fs::path& f) { return fs::last_write_time(f); });
}
fs::path resolution_of(const fs::path& project) {
    for (const auto& f : s::find_files(project / "target", "resolution\\.json"))
        if (!f.generic_string().contains("/.build-mcpp/")) return f;
    expect(fatal(false)) << "no resolution.json under " << project.string();
    return {};
}

// The distinct `-fms-compatibility-version=` words of a build graph.
std::set<std::string> ms_versions(const fs::path& ninja) {
    std::set<std::string> words;
    const auto text = s::read_file(ninja);
    static const std::regex word(R"(-fms-compatibility-version=[0-9.]*)");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), word); it != std::sregex_iterator{}; ++it) words.insert(it->str());
    return words;
}

E2E msvc{"msvc", [] {
    "95 msvc system toolchain"_test[s::msvc] = [] {
        // `toolchain default msvc` locates and identifies the machine's MSVC and persists
        // msvc@system; `toolchain list` shows it under System, starred; a versioned spec is a
        // payload, and an absent one is refused, the default undisturbed; the retired
        // `msvc@<cl-version>` names both replacements; mcpp never removes or installs the
        // machine's Visual Studio; a hello builds and runs. A neutral directory: a project's
        // [toolchain] would shadow the default.
        const s::options home{.where = s::home::registry};
        const auto cwd = s::case_dir();
        auto r = s::mcpp({"toolchain", "default", "msvc"}, cwd, home);
        auto out = s::text(r);
        expect(fatal(e2e::succeeded(r))) << "default msvc: " << out;
        for (std::string_view word : {"Detected", "msvc ", "msvc@system", "cl:"}) expect(fatal(e2e::contains(out, word))) << "1: no '" << word << "': " << out;
        const auto starred = [&] {
            const auto list = s::text(s::mcpp({"toolchain", "list"}, cwd, home));
            return std::pair{list, std::regex_search(list, std::regex(R"(\*\s*msvc)"))};
        };
        auto [list, star] = starred();
        expect(fatal(e2e::contains(list, "System:"))) << "2: no System section: " << list;
        expect(fatal(star)) << "2: the msvc row is not starred as the default: " << list;

        // A toolset that is not installed fails: falling back to the machine's compiler is the
        // silent substitution the version axis exists to prevent.
        r = s::mcpp({"toolchain", "default", "msvc@14.0.99999"}, cwd, home);
        expect(fatal(failed(r))) << "3: an absent toolset resolved: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "not installed"))) << "3: absent-toolset message: " << s::text(r);
        std::tie(list, star) = starred();
        expect(fatal(star)) << "3: a failed pin disturbed the default: " << list;

        std::smatch m;
        const auto again = s::text(s::mcpp({"toolchain", "default", "msvc"}, cwd, home));
        if (std::regex_search(again, m, std::regex(R"(msvc (19\.[0-9]+))"))) {
            r = s::mcpp({"toolchain", "default", "msvc@" + m[1].str()}, cwd, home);
            expect(fatal(failed(r))) << "3b: msvc@" << m[1].str() << " resolved";
            expect(fatal(e2e::contains(s::text(r), "COMPILER version"))) << "3b: no cl-version signpost: " << s::text(r);
            expect(fatal(e2e::contains(s::text(r), "msvc@system"))) << "3b: the signpost omits msvc@system: " << s::text(r);
        }

        r = s::mcpp({"toolchain", "remove", "msvc"}, cwd, home);
        expect(fatal(failed(r) && e2e::contains(s::text(r), "cannot remove it"))) << "4: remove msvc: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "msvc@<toolset>"))) << "4: remove msvc omits the managed route: " << s::text(r);
        (void)s::mcpp({"toolchain", "default", "msvc"}, cwd, home);
        r = s::mcpp({"toolchain", "install", "msvc"}, cwd, home);
        expect(fatal(e2e::succeeded(r))) << "4: install msvc (present) should succeed: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "does not manage it"))) << "4: install msvc message: " << s::text(r);

        r = s::mcpp({"new", "hello_msvc"}, cwd, home);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        r = s::mcpp({"run"}, cwd / "hello_msvc", home);
        expect(fatal(e2e::succeeded(r))) << "5: msvc hello build/run: " << s::text(r);
        expect(fatal(s::contains_nocase(s::text(r), "hello"))) << "5: hello output: " << s::text(r);
    };

    "99 msvc native build"_test[s::msvc] = [] {
        // toolchain default msvc, new, build, run: `import std` through std.ixx, a named module
        // through /interface, .obj and .ifc from the cl.exe pipeline; editing the module rebuilds
        // and the run prints the new value.
        const s::options home{.where = s::home::registry};
        const auto cwd = s::case_dir();
        auto r = s::mcpp({"toolchain", "default", "msvc"}, cwd, home);
        expect(fatal(e2e::succeeded(r))) << "toolchain default msvc: " << s::text(r);
        r = s::mcpp({"new", "hello_cl"}, cwd, home);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        s::project p(cwd / "hello_cl", {{"src/greet.cppm", "export module hello.greet;\nimport std;\nexport namespace hello {\nstd::string greet() { return \"cl-ok\"; }\n}\n"},
                                        {"src/main.cpp", "import std;\nimport hello.greet;\nint main() { std::println(\"{}\", hello::greet()); return 0; }\n"}});
        r = p.mcpp({"build"}, home);
        expect(fatal(e2e::succeeded(r))) << "msvc build: " << s::text(r);
        // A stale default_target once rerouted `toolchain default msvc` to another toolchain.
        expect(fatal(e2e::contains(s::text(r), "msvc"))) << "the build did not resolve msvc: " << s::text(r);
        r = p.mcpp({"run"}, home);
        expect(fatal(e2e::succeeded(r) && e2e::contains(s::text(r), "cl-ok"))) << "msvc run: " << s::text(r);
        expect(fatal(!s::find_files(p.at("target"), ".*\\.obj").empty())) << "no .obj produced";
        bool ifc = false;
        for (const auto& f : s::find_files(p.at("target"), ".*\\.ifc")) ifc = ifc || f.generic_string().contains("ifc.cache");
        expect(fatal(ifc)) << "no .ifc produced under ifc.cache";
        std::this_thread::sleep_for(std::chrono::seconds(1));
        p.write("src/greet.cppm", std::regex_replace(p.read("src/greet.cppm"), std::regex("cl-ok"), "cl-ok2"));
        r = p.mcpp({"run"}, home);
        expect(fatal(e2e::succeeded(r) && e2e::contains(s::text(r), "cl-ok2"))) << "incremental run: " << s::text(r);
    };

    "177 cpp20 msvc"_test[s::msvc] = [] {
        // standard = "c++20" on cl.exe: /std:c++20 reaches the command lines, not /std:c++latest,
        // and `import std` compiles and runs (microsoft/STL#3945, fixed by STL#3977, cl 19.38). If
        // this fails on a 19.38+ cl, msvc::std_module_min_level goes back to 23. The toolset the
        // claim was verified on is printed into the case's log.
        const s::options home{.where = s::home::registry};
        auto r = s::mcpp({"toolchain", "default", "msvc"}, s::case_dir(), home);
        expect(fatal(e2e::succeeded(r))) << "toolchain default msvc: " << s::text(r);
        for (const auto& line : s::lines_with(s::text(s::mcpp({"toolchain", "list"}, s::case_dir(), home)), "msvc")) std::println("{}", line);
        s::project p{{"mcpp.toml", "[package]\nname     = \"cpp20cl\"\nversion  = \"0.1.0\"\nstandard = \"c++20\"\n"},
                     {"src/main.cpp", "import std;\nint main() {\n    std::vector<int> v{1, 2, 3};\n    std::cout << \"cl20 \" << v.size() << \"\\n\";\n    return 0;\n}\n"}};
        const s::options o{.where = s::home::registry, .env = {{"MCPP_COMPILE_COMMANDS", "compile_commands.json"}}};
        r = p.mcpp({"build"}, o);
        expect(fatal(e2e::succeeded(r))) << "msvc c++20 + import std build failed; on a cl >= 19.38 the premise (MSVC answers 20) is wrong: " << s::text(r);
        r = p.mcpp({"run"}, o);
        expect(fatal(e2e::succeeded(r) && e2e::contains(s::text(r), "cl20 3"))) << "run: " << s::text(r);
        const auto cdb = p.read("compile_commands.json");
        expect(fatal(e2e::contains(cdb, "/std:c++20"))) << "compile_commands.json has no /std:c++20";
        expect(fatal(!e2e::contains(cdb, "/std:c++latest"))) << "/std:c++latest reached the command line under standard = c++20";
    };

    "760 msvc toolset is chosen once"_test[s::msvc] = [] {
        // The toolset is the row's sysroot (`[target.<triple>].sysroot`, default msvc@system),
        // resolved once and handed to clang as -Xmicrosoft-* words; recorded in resolution.json;
        // a pin outranks VCToolsInstallDir, which is reported; what is not an MSVC toolset is
        // refused; on the cl.exe row a pinned version the machine has is used in place, and the
        // compiler is its own sysroot. The expected toolset is read independently: vswhere's
        // newest instance and its Microsoft.VCToolsVersion.default.txt.
        const fs::path vswhere = "C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe";
        if (!fs::exists(vswhere)) skip("no vswhere on this machine");
        auto r = s::run(vswhere, {"-latest", "-prerelease", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"});
        const fs::path vsroot = trimmed(r.out);
        expect(fatal(!vsroot.empty())) << "vswhere reports no instance with C++ tools";
        auto expected = s::read_file(vsroot / "VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt");
        std::erase_if(expected, [](char c) { return c == '\r' || c == '\n' || c == ' '; });
        expect(fatal(!expected.empty())) << "no default toolset in " << vsroot.string();
        std::println("runner: {}, default toolset {}", vsroot.string(), expected);
        // No developer prompt: the default selection does not depend on one.
        s::options o{.env = {{"VCToolsInstallDir", std::nullopt}, {"VSINSTALLDIR", std::nullopt}, {"VCINSTALLDIR", std::nullopt}}};
        const auto cwd = s::case_dir();

        // 1. The clang row, no sysroot declared: the machine's default.
        r = s::mcpp({"new", "chosen"}, cwd, o);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        const auto chosen = cwd / "chosen";
        r = s::mcpp({"build"}, chosen, o);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(e2e::contains(s::text(r), std::format("sysroot msvc@system → MSVC {}", expected)))) << "1: no line naming the chosen toolset: " << s::text(r);
        const auto d = s::json_file(resolution_of(chosen));
        expect(fatal(d.contains("msvc_toolset"))) << "resolution.json records no msvc_toolset for the clang row: " << d.dump();
        const auto& t = d["msvc_toolset"];
        expect(fatal(t["version"].get<std::string>() == expected)) << "toolset " << t.dump() << " != " << expected;
        expect(fatal(t["origin"].get<std::string>() == "system")) << t.dump();
        expect(fatal(t["root"].get<std::string>().ends_with("/" + expected))) << t.dump();
        const auto sdk = d.value("windows_sdk", nlohmann::json::object()).value("version", std::string{});
        expect(fatal(!sdk.empty())) << "no Windows SDK recorded: " << d.dump();
        const auto rid = d.value("runtime", nlohmann::json::object()).value("binding", nlohmann::json::object()).value("runtime_id", std::string{});
        expect(fatal(rid == "ucrt@" + sdk)) << "the SDK identity does not reach the runtime contract: " << rid << " vs " << sdk;
        const auto ninja = ninja_of(chosen);
        const auto graph = s::read_file(ninja);
        expect(fatal(e2e::contains(graph, "-Xmicrosoft-visualc-tools-root"))) << "build.ninja does not pass the toolset to clang";
        expect(fatal(e2e::contains(graph, "-Xmicrosoft-windows-sdk-version"))) << "build.ninja does not pass the SDK to clang";
        expect(fatal(e2e::contains(graph, "MSVC\\" + expected) || e2e::contains(graph, "MSVC/" + expected))) << "build.ninja names a toolset other than " << expected;
        // The compiler version travels with the toolset (mcpp#746): one MSVC version on every command.
        const auto versions = ms_versions(ninja);
        expect(fatal(versions.size() == 1 && versions.begin()->starts_with("-fms-compatibility-version=19."))) << "build.ninja does not say one MSVC compiler version" << std::vector<std::string>(versions.begin(), versions.end());
        r = s::mcpp({"run"}, chosen, o);
        expect(fatal(e2e::succeeded(r) && s::contains_nocase(s::text(r), "hello"))) << "run: " << s::text(r);

        // 2. Pinned to an installed toolset; VCToolsInstallDir is not consulted, and is reported.
        fs::create_directories(cwd / "fake/VC/Tools/MSVC/14.99.0/include");
        s::project(chosen, {{"mcpp.toml", s::read_file(chosen / "mcpp.toml") + std::format("\n[target.x86_64-windows-msvc]\nsysroot = \"msvc@{}\"\n", expected)}});
        auto pinned = o;
        pinned.env[0] = {"VCToolsInstallDir", (cwd / "fake/VC/Tools/MSVC/14.99.0").make_preferred().string() + "\\"};
        r = s::mcpp({"build"}, chosen, pinned);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "VCToolsInstallDir (14.99.0) is ignored"))) << "2: the ignored VCToolsInstallDir was not reported: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), std::format("sysroot msvc@{0} → MSVC {0} (system", expected)))) << "2: the pinned toolset was not taken from the machine: " << s::text(r);
        expect(fatal(ms_versions(newest_ninja_of(chosen)) == versions)) << "2: the pinned build says another MSVC compiler version";

        // 3. What is not an MSVC toolset is refused where the manifest is read.
        s::project(chosen, {{"mcpp.toml", std::regex_replace(s::read_file(chosen / "mcpp.toml"), std::regex("sysroot = \"msvc@" + std::regex_replace(expected, std::regex(R"(\.)"), R"(\.)") + "\""), "sysroot = \"xim:glibc@2.39\"")}});
        r = s::mcpp({"build"}, chosen, o);
        expect(fatal(failed(r))) << "3: a C library package was accepted as an MSVC sysroot";
        expect(fatal(e2e::contains(s::text(r), "MSVC toolset"))) << s::text(r);

        // 4. The cl.exe row: a pinned version this machine has is used in place.
        r = s::mcpp({"new", "clrow"}, cwd, o);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        const auto clrow = cwd / "clrow";
        s::project(clrow, {{"mcpp.toml", s::read_file(clrow / "mcpp.toml") + std::format("\n[toolchain]\nwindows = \"msvc@{}\"\n", expected)}});
        r = s::mcpp({"build", "--verbose"}, clrow, o);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        const auto resolved = s::lines_with(s::text(r), "Resolved msvc@" + expected);
        expect(fatal(!resolved.empty() && resolved.front().contains("(installed:"))) << "4: the cl.exe row did not use the installed toolset: " << s::text(r);
        expect(fatal(!resolved.front().contains("xim-x-msvc"))) << "4: resolved to a package: " << resolved.front();

        // 5. On the cl.exe row the compiler is its own sysroot.
        s::project(clrow, {{"mcpp.toml", s::read_file(clrow / "mcpp.toml") + "\n[target.x86_64-windows-msvc]\nsysroot = \"msvc@14.1.0\"\n"}});
        r = s::mcpp({"build"}, clrow, o);
        expect(fatal(failed(r))) << "5: a sysroot naming another toolset was accepted on the cl.exe row";
        expect(fatal(e2e::contains(s::text(r), "names a different toolset than the compiler"))) << s::text(r);
    };

    "825 a build program reads the msvc toolset"_test[s::windows] = [] {
        // W1 abi_tool("cxx") is an existing cl.exe, abi_tool("ld") a link.exe; W2 abi_tool("rc")
        // and ("mt") are existing SDK tools; W3 tool_env() carries INCLUDE and LIB; W4
        // toolset_identity() reads "msvc <version>; sdk <version>"; W5 msvc_crt_linkage() is
        // "dynamic" by default and "static" under cxx_runtime = "self-contained" (#734 E2).
        s::project p{{"src/main.cpp", "int main() { return 0; }\n"}, {"mcpp.toml", "[package]\nname = \"msvc825\"\nversion = \"0.1.0\"\n"},
                     {"build.mcpp", R"(import std;
import mcpp.core;
int main() {
    std::ofstream out(std::string(mcpp::out_dir()) + "/info.txt");
    for (const char* r : {"cxx", "ld", "ar", "rc", "mt"})
        out << "abi." << r << "=" << mcpp::abi_tool(r) << "\n";
    std::string env = mcpp::tool_env();
    out << "include=" << (env.find("INCLUDE=") != std::string::npos ? "yes" : "no") << "\n"
        << "lib=" << (env.find("\nLIB=") != std::string::npos || env.starts_with("LIB=") ? "yes" : "no") << "\n"
        << "identity=" << mcpp::toolset_identity() << "\n"
        << "crt=" << mcpp::msvc_crt_linkage() << "\n";
    return 0;
}
)"}};
        const auto info = [&] {
            const auto f = s::find_file(p.at("target"), "info.txt");
            expect(fatal(f.has_value() && fs::file_size(*f) > 0)) << "the build program wrote no info.txt";
            std::map<std::string, std::string> v;
            for (auto line : s::lines_with(s::read_file(*f), "=")) {
                std::erase(line, '\r');
                const auto eq = line.find('=');
                v.try_emplace(line.substr(0, eq), line.substr(eq + 1));
            }
            return v;
        };
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "the build failed: " << s::text(r);
        auto v = info();
        const auto lower = [](std::string text) {
            std::ranges::transform(text, text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            return text;
        };
        expect(fatal(lower(v["abi.cxx"]).ends_with("cl.exe") && fs::exists(v["abi.cxx"]))) << "W1: abi_tool(cxx) is '" << v["abi.cxx"] << "'";
        expect(fatal(lower(v["abi.ld"]).ends_with("link.exe"))) << "W1: abi_tool(ld) is '" << v["abi.ld"] << "'";
        for (std::string_view t : {"rc", "mt"}) expect(fatal(fs::exists(v[std::format("abi.{}", t)]))) << "W2: abi_tool(" << t << ") does not exist: " << v[std::format("abi.{}", t)];
        expect(fatal(v["include"] == "yes")) << "W3: tool_env() has no INCLUDE";
        expect(fatal(v["lib"] == "yes")) << "W3: tool_env() has no LIB";
        expect(fatal(std::regex_match(v["identity"], std::regex("msvc .*; sdk .*")))) << "W4: identity is '" << v["identity"] << "'";
        expect(fatal(v["crt"] == "dynamic")) << "W5: the default CRT linkage is '" << v["crt"] << "'";
        p.write("mcpp.toml", "[package]\nname = \"msvc825\"\nversion = \"0.1.0\"\n\n[build]\ncxx_runtime = \"self-contained\"\n");
        r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "the self-contained build failed: " << s::text(r);
        v = info();
        expect(fatal(v["crt"] == "static")) << "W5: the self-contained CRT linkage is '" << v["crt"] << "'";
    };

    "848 msvc binds a module name per program"_test[s::msvc] = [] {
        // An app and its `artifacts` updater each provide a different module `boost`; under
        // --toolchain msvc both build, bound with `/reference <name>=<path>`, and each prints its
        // own value (mcpp#732).
        const auto tmp = s::case_dir();
        const auto main_cpp = "#include <cstdio>\nimport boost;\nint main() { std::printf(\"%d\\n\", value()); }\n";
        s::project(tmp / "updater", {{"src/boost.cppm", "export module boost;\nexport int value() { return 2; }\n"}, {"src/main.cpp", main_cpp},
                                     {"mcpp.toml", "[package]\nname    = \"updater\"\nversion = \"0.1.0\"\n\n[targets.updater]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"}});
        s::project app(tmp / "app", {{"src/boost.cppm", "export module boost;\nexport int value() { return 1; }\n"}, {"src/main.cpp", main_cpp},
                                     {"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\nupdater = { path = \"../updater\", artifacts = [\"updater\"] }\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"}});
        auto r = app.mcpp({"build", "--toolchain", "msvc"});
        expect(fatal(e2e::succeeded(r))) << "the build under MSVC was refused: " << s::text(r);
        const auto program = [&](std::string_view name) {
            for (const auto& f : s::find_files(app.at("target"), std::string(name) + "\\.exe"))
                if (f.parent_path().generic_string().contains("/bin")) return f;
            expect(fatal(false)) << name << ".exe is missing: " << s::text(r);
            return fs::path{};
        };
        expect(fatal(trimmed(s::run(program("app")).out) == "1")) << "the app does not print its own module's value";
        expect(fatal(trimmed(s::run(program("updater")).out) == "2")) << "the updater does not print its own module's value";
    };

    "887 msvc lto and export discovery"_test[s::msvc] = [] {
        // A a program: /GL on the compile, /LTCG on the link, `+ lto`. B a DLL whose exports are
        // discovered: its packages compile with /GL-, the build says so, the summary says
        // `+ lto (partial)`, a consumer links. C stating windows_auto_export = true: refused. D
        // stating false with __declspec(dllexport): full /GL, it links. E /GL in the DLL's flags
        // with discovery on: refused. F /bigobj with discovery on: read, the consumer links
        // (2026.10.5.2, #770).
        const s::options o{.where = s::home::registry};
        const auto tmp = s::case_dir();
        s::project a(tmp / "a", {{"src/main.cpp", "int main() { return 0; }\n"},
                                 {"mcpp.toml", "[package]\nname = \"a\"\nversion = \"0.1.0\"\n[toolchain]\nwindows = \"msvc@system\"\n[profile.release]\nlto = true\n"}});
        auto r = a.mcpp({"build", "--release"}, o);
        expect(fatal(e2e::succeeded(r))) << "A: build failed: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "+ lto]"))) << "A: the summary does not say + lto: " << s::text(r);
        const auto ga = s::read_file(ninja_of(a.dir()));
        expect(fatal(e2e::contains(ga, "/GL") && e2e::contains(ga, "/LTCG"))) << "A: /GL or /LTCG is missing from the build";

        const auto plain = "int twice(int x) { return 2 * x; }\n";
        const auto annotated = "__declspec(dllexport) int twice(int x) { return 2 * x; }\n";
        s::project app(tmp / "app", {});
        const auto write_dll = [&](std::string_view target_lines, std::string_view build_lines, std::string_view source, bool lto) {
            s::project(tmp / "dll", {{"src/dll.cpp", source},
                                     {"mcpp.toml", std::format("[package]\nname = \"dll\"\nversion = \"0.1.0\"\n[toolchain]\nwindows = \"msvc@system\"\n[build]\n{}\n[targets.dll]\nkind = \"shared\"\n{}\n", build_lines, target_lines)}});
            app.write("mcpp.toml", std::format("[package]\nname = \"app\"\nversion = \"0.1.0\"\n[toolchain]\nwindows = \"msvc@system\"\n[profile.release]\nlto = {}\n[dependencies]\ndll = {{ path = \"../dll\" }}\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", lto));
            app.write("src/main.cpp", "int twice(int);\nint main() { return twice(21) == 42 ? 0 : 1; }\n");
            fs::remove_all(app.at("target"));
        };

        write_dll("", "", plain, true);
        r = app.mcpp({"run", "--release"}, o);
        expect(fatal(e2e::succeeded(r))) << "B: the consumer of a discovered DLL did not link or run: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "LTO is not applied to the packages linked into the shared libraries"))) << "B: the downgrade was not stated: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "lto (partial)"))) << "B: the summary does not say + lto (partial): " << s::text(r);
        expect(fatal(e2e::contains(s::read_file(ninja_of(app.dir())), "/GL-"))) << "B: the DLL's package was not compiled with /GL-";

        write_dll("windows_auto_export = true", "", plain, true);
        r = app.mcpp({"build", "--release"}, o);
        expect(fatal(failed(r))) << "C: a stated windows_auto_export = true was not refused under LTO";
        expect(fatal(e2e::contains(s::text(r), "states `windows_auto_export = true`"))) << "C: the refusal does not name the key: " << s::text(r);

        write_dll("windows_auto_export = false", "", annotated, true);
        r = app.mcpp({"run", "--release"}, o);
        expect(fatal(e2e::succeeded(r))) << "D: an annotated DLL under full LTO did not link or run: " << s::text(r);
        expect(fatal(!e2e::contains(s::read_file(ninja_of(app.dir())), "/GL-"))) << "D: a DLL that opted out of discovery was compiled with /GL-";

        write_dll("", "cxxflags = [\"/GL\"]", plain, false);
        r = app.mcpp({"build"}, o);
        expect(fatal(failed(r))) << "E: /GL beside export discovery was not refused";
        expect(fatal(e2e::contains(s::text(r), "compiles with /GL"))) << "E: the refusal does not name /GL: " << s::text(r);

        write_dll("", "cxxflags = [\"/bigobj\"]", plain, false);
        r = app.mcpp({"run"}, o);
        expect(fatal(e2e::succeeded(r))) << "F: a /bigobj DLL's discovered exports did not link: " << s::text(r);
    };

    "892 coroutines on the 32 bit msvc abi are explained"_test[s::msvc] = [] {
        // clang 23 does not predefine __cpp_impl_coroutine for i686-pc-windows-msvc, so the MSVC
        // STL's <coroutine> is empty; mcpp follows the compiler and appends a note. A C++23
        // `import std` fails, the note offering C++20 first and llvm 22.1.8 second; B C++20
        // `import std` and `import std.compat` build and run, no note; C C++20 code using
        // coroutines fails, the note offering no C++20. When the compiler predefines the macro, A
        // builds and no note appears (2026.10.8.1, LLVM 23.1.3 Part 3, D1).
        const s::options o{.where = s::home::registry};
        const auto llvm = s::llvm_version();
        const std::string note = "does not support C++20 coroutines";
        const auto make = [&](std::string_view name, std::string_view standard, std::string_view source) {
            return s::project(s::case_dir() / std::string(name), {{"src/main.cpp", source},
                                                                  {"mcpp.toml", std::format("[package]\nname = \"{}\"\nversion = \"0.1.0\"\nstandard = \"{}\"\n\n[target.i686-windows-msvc]\ntoolchain = \"llvm@{}\"\n", name, standard, llvm)}});
        };
        auto cxx23 = make("cxx23", "c++23", "import std;\nint main() { std::println(\"x86 std {}\", sizeof(void*)); }\n");
        auto cxx20 = make("cxx20", "c++20", "import std;\nint main() { std::cout << \"x86 std \" << sizeof(void*) << \"\\n\"; }\n");
        auto compat = make("compat", "c++20", "import std.compat;\nint main() { std::printf(\"x86 compat %zu\\n\", sizeof(void*)); }\n");
        auto coro = make("coro", "c++20", R"(#include <coroutine>
struct task { struct promise_type {
    task get_return_object() { return {}; }
    std::suspend_never initial_suspend() noexcept { return {}; }
    std::suspend_never final_suspend() noexcept { return {}; }
    void return_void() {}
    void unhandled_exception() {} }; };
task f() { co_return; }
int main() { f(); }
)");
        // B first: it installs the toolchain the expectation is read from.
        auto r = cxx20.mcpp({"run", "--target", "i686-windows-msvc"}, o);
        expect(fatal(e2e::succeeded(r))) << "B: C++20 import std failed: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "x86 std 4"))) << "B: the 32-bit program did not run: " << s::text(r);
        expect(fatal(!e2e::contains(s::text(r), note))) << "B: a successful build carries the note: " << s::text(r);
        r = compat.mcpp({"run", "--target", "i686-windows-msvc"}, o);
        expect(fatal(e2e::succeeded(r) && e2e::contains(s::text(r), "x86 compat 4"))) << "B: C++20 import std.compat: " << s::text(r);

        const auto clang = s::payload_root("xim-x-llvm", llvm) / "bin" / s::exe("clang++");
        s::project(s::case_dir(), {{"empty.cpp", ""}});
        const auto macros = s::run(clang, {"--no-default-config", "--target=i686-pc-windows-msvc", "-std=c++23", "-dM", "-E", "-x", "c++", (s::case_dir() / "empty.cpp").string()});
        if (e2e::contains(macros.out, "__cpp_impl_coroutine")) {
            r = cxx23.mcpp({"run", "--target", "i686-windows-msvc"}, o);
            expect(fatal(e2e::succeeded(r))) << "A: llvm@" << llvm << " supports coroutines here, yet C++23 import std failed: " << s::text(r);
            expect(fatal(!e2e::contains(s::text(r), note))) << "A: the note appeared for a compiler that supports coroutines: " << s::text(r);
            return;
        }
        r = cxx23.mcpp({"build", "--target", "i686-windows-msvc"}, o);
        auto out = s::text(r);
        expect(fatal(failed(r))) << "A: C++23 import std built: " << out;
        expect(fatal(e2e::contains(out, note))) << "A: the failure carries no note: " << out;
        expect(fatal(e2e::contains(out, "<generator>"))) << "A: the note does not name <generator>: " << out;
        const auto c20 = out.find("standard = \"c++20\""), l22 = out.find("toolchain = \"llvm@22.1.8\"");
        expect(fatal(c20 != std::string::npos && l22 != std::string::npos && c20 < l22)) << "A: the note does not offer C++20 first and llvm 22.1.8 second: " << out;
        r = coro.mcpp({"build", "--target", "i686-windows-msvc"}, o);
        out = s::text(r);
        expect(fatal(failed(r))) << "C: coroutine code built: " << out;
        expect(fatal(e2e::contains(out, note))) << "C: the failure carries no note: " << out;
        expect(fatal(!e2e::contains(out, "standard = \"c++20\""))) << "C: C++20 was offered for code that uses coroutines: " << out;
        expect(fatal(e2e::contains(out, "toolchain = \"llvm@22.1.8\""))) << "C: llvm 22.1.8 was not offered: " << out;
    };

    "1017 a system msvc builds the build program of a cross target"_test[s::windows][s::msvc] = [] {
        // With msvc@system the machine's default, a build program of a riscv64-none-elf build is
        // compiled by the located Visual Studio, not looked up as a package, and its directive
        // reaches the cross build (2026.11.7.1; design 2026-11-07 §4.1.3).
        const s::options home{.where = s::home::registry};
        auto r = s::mcpp({"toolchain", "default", "msvc"}, s::case_dir(), home);
        expect(fatal(e2e::succeeded(r))) << "msvc@system not selectable: " << s::text(r);
        s::project p{{"mcpp.toml", "[package]\nname    = \"layout\"\nversion = \"0.1.0\"\n"},
                     {"build.mcpp", "#include <cstdio>\nint main() {\n    std::puts(\"mcpp:include-dir=inc\");\n    std::puts(\"mcpp:rerun-if-changed=build.mcpp\");\n    return 0;\n}\n"},
                     {"inc/layout.h", "struct pair_layout { unsigned size; unsigned long long stamp; };\n"},
                     {"src/main.cpp", "#include \"layout.h\"\nstatic_assert(__builtin_offsetof(pair_layout, stamp) == 8);\nextern \"C\" void _start() { for (;;) {} }\n"}};
        r = p.mcpp({"build", "--target", "riscv64-none-elf"}, home);
        const auto out = s::text(r);
        expect(fatal(e2e::succeeded(r))) << "the cross build with a build program failed: " << out;
        expect(fatal(e2e::contains(out, "host toolchain for build.mcpp: msvc@system"))) << "the build program was not compiled by the located msvc@system: " << out;
        expect(fatal(!std::regex_search(out, std::regex("xim:msvc@system|msvc@system.*not found in the", std::regex::icase)))) << "msvc@system was looked up as a package: " << out;
    };

    "1023 msvc objects carry their own debug information"_test[s::msvc] = [] {
        // A dev build writes /Z7 debug information into each object, not /Zi /FS into one PDB
        // through mspdbsrv.exe, which retired under concurrent compiles (C1090); a shared-PDB word
        // in the manifest is refused before anything compiles (2026.11.7.1).
        s::project p{{"mcpp.toml", "[package]\nname    = \"many\"\nversion = \"0.1.0\"\n[toolchain]\nwindows = \"msvc@system\"\n"}};
        std::string decls, body;
        for (int k = 1; k <= 16; ++k) {
            p.write(std::format("src/part{}.cpp", k), std::format("int part{0}(int v) {{ return v + {0}; }}\n", k));
            decls += std::format("int part{}(int);\n", k);
            body += std::format("    s += part{}(0);\n", k);
        }
        p.write("src/main.cpp", decls + "int main() { int s = 0;\n" + body + "    return s == 136 ? 0 : 1; }\n");
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "a dev build of overlapping cl.exe compiles did not build: " << s::text(r);
        const auto graph = s::read_file(ninja_of(p.dir()));
        expect(fatal(e2e::contains(graph, "/Z7"))) << "the dev profile's cl.exe compile does not write /Z7 debug information";
        const std::regex shared_pdb(R"((^| )/(Zi|FS)( |$))");
        for (const auto& line : s::lines_with(graph, "/"))
            expect(fatal(!std::regex_search(line, shared_pdb))) << "a cl.exe compile still writes a shared PDB: " << line;
        r = p.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "the program of the dev build did not run: " << s::text(r);
        p.write("mcpp.toml", p.read("mcpp.toml") + "[build]\ncxxflags = [\"/Zi\"]\n");
        r = p.mcpp({"build"});
        expect(fatal(failed(r))) << "[build] cxxflags = [\"/Zi\"] built on cl.exe: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "mspdbsrv") && e2e::contains(s::text(r), "/Z7"))) << "the refusal does not say why /Zi is refused and what to write: " << s::text(r);
        expect(fatal(!e2e::contains(s::text(r), "Compiling"))) << "the refusal came after compiling began: " << s::text(r);
    };
    "241 windows ucrt runtime identity"_test[s::msvc] = [] {
        // The Windows SDK has an identity, `ucrt@<version>`, in the runtime binding of the cl.exe row
        // (pinned msvc@system; 760 states it of the clang row): it takes part in the contract hash
        // that keys the build cache, and it is not projected into `libc`, which names a private libc
        // payload (ucrtbase.dll is a Windows component).
        auto r = s::mcpp({"new", "ucrtid"}, s::case_dir());
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        const auto dir = s::case_dir() / "ucrtid";
        s::project(dir, {{"mcpp.toml", s::read_file(dir / "mcpp.toml") + "\n[toolchain]\nwindows = \"msvc@system\"\n"}});
        r = s::mcpp({"build"}, dir);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        const auto b = s::json_file(resolution_of(dir))["runtime"]["binding"];
        const auto rid = b.value("runtime_id", std::string{});
        expect(fatal(rid.starts_with("ucrt@"))) << "the Windows runtime identity is not filled in: runtime_id=" << rid;
        const auto version = rid.substr(5);
        expect(fatal(!version.empty() && std::isdigit(static_cast<unsigned char>(version.front())))) << "implausible SDK version in " << rid;
        expect(fatal(!b.value("contract_hash", std::string{}).empty())) << "the runtime binding has no contract hash: " << b.dump();
        expect(fatal(!b.contains("libc") || b["libc"].is_null() || (b["libc"].is_string() && b["libc"].get<std::string>().empty())))
            << "ucrt was projected into `libc`: " << b.dump();
    };

    "889 windows driver target and coff resources"_test[s::msvc] = [] {
        // The driver's link and the COFF resource target the selected machine (mcpp#775): for
        // i686, x86_64 and `x86` (MSVC's spelling of the i686 row, 2026.10.5.3), a C program, a C++
        // program and a DLL link for their machine, a program runs, the resource object is of that
        // machine, its data reaches the image, and an edit rebuilds it.
        const s::options o{.where = s::home::registry};
        const auto u16 = [](const std::string& b, std::size_t at) { return std::uint32_t(std::uint8_t(b[at])) | std::uint32_t(std::uint8_t(b[at + 1])) << 8; };
        const auto pe_machine = [&](const fs::path& f) {
            const auto b = s::read_file(f);
            const auto offset = u16(b, 60) | u16(b, 62) << 16;
            return u16(b, offset + 4);
        };
        const std::string first_data("\x57\x13\x68\x24\x01\xef\xcd\xab", 8), second_data("\x54\x76\x10\x32\x98\xba\xdc\xfe", 8);
        for (std::string_view arch : {"i686", "x86_64", "x86"}) {
            const std::uint32_t machine = arch == "x86_64" ? 34404 : 332;
            for (std::string_view kind : {"c", "cxx", "dll"}) {
                if (arch == "x86" && kind != "cxx") continue;
                const bool dll = kind == "dll";
                const std::string target = std::format("{}-windows-msvc", arch);
                s::project p(s::case_dir() / std::string(arch) / std::string(kind), {});
                if (dll) p.write("src/main.cpp", "extern \"C\" __declspec(dllexport) int answer() { return 42; }\n");
                else if (kind == "c") p.write("src/main.c", "int main(void) { return 0; }\n");
                else p.write("src/main.cpp", "int main() { return 0; }\n");
                const auto manifest = std::format("[package]\nname = \"probe\"\nversion = \"0.1.0\"\n[toolchain]\nwindows = \"llvm@{}\"\n[build]\ncxx_runtime = \"host-coupled\"\n[target.{}]\nsysroot = \"msvc@system\"\n[targets.probe]\nkind = \"{}\"\n{}\nwindows_auto_export = false\n",
                                                  s::llvm_version(), target, dll ? "shared" : "bin", dll ? "" : kind == "c" ? "main = \"src/main.c\"" : "main = \"src/main.cpp\"");
                p.write("mcpp.toml", manifest);
                auto r = p.mcpp({"build", "--target", target}, o);
                expect(fatal(e2e::succeeded(r))) << arch << "/" << kind << ": the driver's link failed: " << s::text(r);
                const auto image = s::find_file(p.at("target"), dll ? "probe.dll" : "probe.exe");
                expect(fatal(image.has_value())) << arch << "/" << kind << ": the image is missing";
                expect(fatal(pe_machine(*image) == machine)) << "wrong PE machine: " << image->string();
                if (!dll) expect(fatal(e2e::succeeded(s::run(*image)))) << "the program failed: " << image->string();
                p.write("mcpp.toml", manifest + "\n[resources]\nfiles = [\"app.rc\"]\n");
                p.write("app.rc", "101 RCDATA { 0x1357, 0x2468, 0xabcdef01L }\n");
                r = p.mcpp({"build", "--target", target}, o);
                expect(fatal(e2e::succeeded(r))) << arch << "/" << kind << ": the resource's link failed: " << s::text(r);
                std::optional<fs::path> object;
                for (const auto& f : s::find_files(p.at("target"), ".*\\.o"))
                    if (f.parent_path().generic_string().contains("/res")) object = f;
                expect(fatal(object.has_value())) << arch << "/" << kind << ": the COFF resource is missing";
                expect(fatal(u16(s::read_file(*object), 0) == machine)) << "wrong COFF machine: " << object->string();
                expect(fatal(s::read_file(*image).contains(first_data))) << "the resource's data is missing";
                p.write("app.rc", "101 RCDATA { 0x7654, 0x3210, 0xfedcba98L }\n");
                r = p.mcpp({"build", "--target", target}, o);
                expect(fatal(e2e::succeeded(r))) << arch << "/" << kind << ": the resource's rebuild failed: " << s::text(r);
                expect(fatal(pe_machine(*image) == machine)) << "the rebuild changed the PE machine";
                expect(fatal(s::read_file(*image).contains(second_data))) << "the resource's data stayed stale";
            }
        }
    };
    "180 msvc build mcpp"_test[s::windows][s::msvc] = [] {
        // build.mcpp under msvc@system, checked by its effect: 1 an #include-based program runs (it
        // writes a file) and its define reaches the build; 2 `mcpp:link-lib` is spelled the MSVC way
        // (advapi32 links); 3 a build program with named modules (`import mcpp`) compiles through
        // cl.exe's module pipeline, whose .ifc for the bundled module is in the cache or the
        // workspace's store (#748).
        const s::options home{.where = s::home::registry};
        auto r = s::mcpp({"toolchain", "default", "msvc"}, s::case_dir(), home);
        expect(fatal(e2e::succeeded(r))) << "msvc@system not selectable: " << s::text(r);
        r = s::mcpp({"new", "msvc_bp"}, s::case_dir(), home);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        s::project p(s::case_dir() / "msvc_bp", {});
        const auto step = [&](std::string_view what, std::string_view program, std::string_view define, std::string_view says) {
            p.write("build.mcpp", program);
            p.write("src/main.cpp", std::format("import std;\nint main() {{\n#ifdef {}\n    std::println(\"{}\");\n    return 0;\n#else\n    std::println(\"define missing\");\n    return 1;\n#endif\n}}\n", define, says));
            auto b = p.mcpp({"build"}, home);
            expect(fatal(e2e::succeeded(b))) << what << ": " << s::text(b);
            auto run = p.mcpp({"run"}, home);
            expect(fatal(e2e::succeeded(run) && e2e::contains(s::text(run), says))) << what << ": run: " << s::text(run);
            return b;
        };
        r = step("1 an #include-based build.mcpp", "#include <cstdio>\nint main() {\n    std::FILE* f = std::fopen(\"helper-ran\", \"w\");\n    if (!f) return 2;\n    std::fputs(\"ok\\n\", f);\n    std::fclose(f);\n    std::puts(\"mcpp:cfg=MSVC_BUILD_PROGRAM_RAN\");\n    std::puts(\"mcpp:rerun-if-changed=build.mcpp\");\n    return 0;\n}\n",
                 "MSVC_BUILD_PROGRAM_RAN", "msvc-build-mcpp-ok");
        expect(fatal(p.exists("helper-ran"))) << "build.mcpp did not run under MSVC: " << s::text(r);
        step("2 link-lib in the MSVC spelling", "#include <cstdio>\nint main() {\n    std::puts(\"mcpp:link-lib=advapi32\");\n    std::puts(\"mcpp:cfg=MSVC_LINK_LIB_OK\");\n    std::puts(\"mcpp:rerun-if-changed=build.mcpp\");\n    return 0;\n}\n",
             "MSVC_LINK_LIB_OK", "msvc-link-lib-ok");
        step("3 named modules in build.mcpp", "import std;\nimport mcpp;\nint main() {\n    std::string tag = std::format(\"MSVC_MODULES_{}\", 1 + 1);\n    mcpp::define(tag.c_str());\n    mcpp::rerun_if_changed(\"build.mcpp\");\n    return 0;\n}\n",
             "MSVC_MODULES_2", "msvc-modules-ok");
        const auto cache = s::lines_with(s::text(p.mcpp({"cache", "dir"}, home)), "");
        bool ifc = !s::find_files(p.at("target/.build-mcpp"), ".*\\.ifc").empty();
        if (!cache.empty()) {
            auto root = cache.front();
            std::erase(root, '\r');
            ifc = ifc || !s::find_files(fs::path(root) / "pkg/_engine", ".*\\.ifc").empty();
        }
        expect(fatal(ifc)) << "no .ifc was produced for the bundled mcpp module";
    };
}};

}  // namespace

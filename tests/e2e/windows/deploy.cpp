// One destination, one writer: a declared `[runtime] deploy` outranks a DLL of the same name in a
// runtime search directory, whether the plan's scan or the post-link placement finds it; the
// build warns, naming the DLL, and a later build keeps the declared file (SPEC-007 R4.2, R4.3,
// R4.5; mcpp#723; formerly tests/e2e/811 and 818).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

constexpr std::string_view declared = "not the real DLL\n";
constexpr std::string_view placed = "is placed by this project's deploy list";

std::string app_manifest(std::string_view name, bool native) {
    return std::format("[package]\nname    = \"{0}\"\nversion = \"0.1.0\"\n{1}[targets.{0}]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n\n"
                       "[runtime]\ndeploy = [ {{ from = \"deploy/libmathkit.dll\", to = \".\" }} ]\n",
                       name, native ? "[toolchain]\nwindows = \"gcc@16.1.0\"\n" : "");
}

// `target` empty builds natively (811, a Windows host); otherwise through the cross toolchain
// (818), which adds the prepare-filled directory whose difference only the placement edge sees.
void declared_deploy_outranks(std::vector<std::string> target) {
    const bool native = target.empty();
    const auto build = [&](s::project& p) {
        std::vector<std::string> args{"build"};
        args.insert(args.end(), target.begin(), target.end());
        return p.mcpp(args);
    };
    const auto tmp = s::case_dir();
    s::project mathkit(tmp / "mathkit", {{"src/mathkit.cppm", "export module mathkit;\nexport namespace mk { int answer(); }\n"},
                                         {"src/impl.cpp", std::format("module mathkit;\nnamespace mk {{ int answer() {{ return 42; }} }}\nextern \"C\" {}int mk_answer_extern() {{ return mk::answer(); }}\n", native ? "__declspec(dllexport) " : "")},
                                         {"mcpp.toml", std::format("[package]\nname    = \"mathkit\"\nversion = \"0.1.0\"\n{}[build]\nsources = [\"src/*.cppm\", \"src/*.cpp\"]\n[targets.mathkit]\nkind = \"shared\"\n", native ? "[toolchain]\nwindows = \"gcc@16.1.0\"\n" : "")}});
    auto r = build(mathkit);
    expect(fatal(e2e::succeeded(r))) << "mathkit build failed: " << s::text(r);
    const auto dll = s::find_file(mathkit.at("target"), "libmathkit.dll");
    const auto imp = s::find_file(mathkit.at("target"), "libmathkit.dll.a");
    expect(fatal(dll.has_value() && imp.has_value())) << "mathkit did not produce a DLL and an import library";
    const auto dll_dir = fs::canonical(dll->parent_path()).generic_string();
    const auto lib_dir = fs::canonical(imp->parent_path()).generic_string();
    const auto main_cpp = "extern \"C\" int mk_answer_extern();\nint main() { return mk_answer_extern() == 42 ? 0 : 1; }\n";

    s::project app(tmp / "app", {{"src/main.cpp", main_cpp}, {"deploy/libmathkit.dll", declared}, {"mcpp.toml", app_manifest("app", native)},
                                 {"build.mcpp", std::format("import mcpp;\nint main() {{\n    mcpp::link_flag(\"-L{}\");\n    mcpp::link_flag(\"-Wl,-Bdynamic\");\n    mcpp::link_lib(\"mathkit\");\n    mcpp::runtime_search_dir(\"{}\");\n    return 0;\n}}\n", lib_dir, dll_dir)}});
    r = build(app);
    expect(fatal(e2e::succeeded(r))) << "1: the build failed; the search directory's DLL met the declared deploy as a second source: " << s::text(r);
    const auto exe = s::find_file(app.at("target"), "app.exe");
    expect(fatal(exe.has_value())) << "no app.exe produced: " << s::text(r);
    const auto beside = exe->parent_path() / "libmathkit.dll";
    expect(fatal(fs::exists(beside) && s::read_file(beside) == declared)) << "2: the file beside app.exe is not the declared deploy's: " << s::text(r);
    const auto out = s::text(r);
    // A warning names the DLL on its line or within the three after it, or a line says the DLL
    // differs.
    bool warned = false;
    const auto lines = s::lines_with(out, "");
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].contains("libmathkit.dll") && lines[i].find("differ", lines[i].find("libmathkit.dll")) != std::string::npos) warned = true;
        if (lines[i].contains("warning:"))
            for (std::size_t j = i; j < lines.size() && j <= i + 3; ++j) warned = warned || lines[j].contains("libmathkit.dll");
    }
    expect(fatal(warned)) << "3: the build does not warn about the difference, naming libmathkit.dll: " << out;
    r = build(app);
    expect(fatal(e2e::succeeded(r))) << "the second build failed: " << s::text(r);
    expect(fatal(s::read_file(beside) == declared)) << "4: a second build replaced the declared file";
    if (native) return;

    // 5-6. The same difference in a directory a `prepare` action fills: at planning time it is
    // empty, so only the placement edge compares the two files, and states it once without -v; a
    // build in which that edge does not run states nothing.
    const auto rt = (tmp / "rt").generic_string();
    const auto dll_abs = fs::canonical(*dll).generic_string();
    s::project app2(tmp / "app2", {{"src/main.cpp", main_cpp}, {"deploy/libmathkit.dll", declared}, {"mcpp.toml", app_manifest("app2", native)},
                                   {"build.mcpp", std::format(R"(import std;
import mcpp;
int main() {{
    mcpp::link_flag("-L{0}");
    mcpp::link_flag("-Wl,-Bdynamic");
    mcpp::link_lib("mathkit");
    mcpp::runtime_search_dir("{1}");
    mcpp::action a;
    a.id          = "fill-rt";
    a.role        = mcpp::roles::prepare;
    a.description = "a directory whose DLL arrives during the build";
    a.arg("${{mcpp.self}}").arg("stage").arg("--output").arg("{1}/libmathkit.dll")
     .arg("{2}")
     .input("{2}")
     .output((std::string(mcpp::out_dir()) + "/fill-rt.stamp").c_str())
     .output_dir("{1}")
     .submit();
    return 0;
}}
)", lib_dir, rt, dll_abs)}});
    r = build(app2);
    expect(fatal(e2e::succeeded(r))) << "5: the build with a prepare-filled search directory failed: " << s::text(r);
    expect(fatal(fs::exists(tmp / "rt" / "libmathkit.dll"))) << "5: the prepare action did not fill its directory";
    const auto said = s::lines_with(s::text(r), placed);
    expect(fatal(said.size() == 1)) << "5: the difference found by the placement edge was stated " << said.size() << " time(s) without -v, not once: " << s::text(r);
    expect(fatal(said.front().contains("libmathkit.dll"))) << "5: the statement does not name libmathkit.dll: " << said.front();
    r = build(app2);
    expect(fatal(e2e::succeeded(r))) << "the second app2 build failed: " << s::text(r);
    expect(fatal(s::lines_with(s::text(r), placed).empty())) << "6: a build in which the placement edge did not run stated its difference again: " << s::text(r);
}

E2E deploy{"deploy", [] {
    "811 place dlls never overwrites a declared deploy"_test[s::windows] = [] {
        declared_deploy_outranks({});
    };

    "818 a declared deploy outranks a search dir dll cross"_test[s::mingw_cross] = [] {
        // 811 through x86_64-windows-gnu: the plan-time scan and the post-link placement run on
        // whichever host builds.
        declared_deploy_outranks({"--target", "x86_64-windows-gnu"});
    };
}};

}  // namespace

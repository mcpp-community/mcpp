// `[build] module_extensions`: the declared extensions build under GCC and Clang, their files are
// inputs of the graph's shape, the default toolchain compiles them, an undeclared extension is
// refused, and the lib root follows the declared extension (formerly tests/e2e/217, 218, 236,
// 260, 263).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

std::optional<fs::path> graph_of(const fs::path& root) {
    auto all = s::find_files(root / "target", R"(build\.ninja)");
    std::ranges::sort(all);
    for (const auto& f : all)
        if (!f.generic_string().contains("/.build-mcpp/")) return f;
    return std::nullopt;
}

std::string graph_text(const fs::path& root) {
    const auto g = graph_of(root);
    expect(fatal(g.has_value())) << "no build.ninja";
    return s::read_file(*g);
}

std::string output_of(const fs::path& program) {
    auto out = s::run(program).out;
    std::erase(out, '\r');
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

// `<key>: <value>` of `mcpp build --print-fingerprint` (`sed -n 's/^<key>: //p' | head -1`), its
// first word.
std::string printed(const s::project& p, std::string_view key) {
    const auto text = s::text(p.mcpp({"build", "--print-fingerprint"}));
    const auto line = s::last_starting(text, std::string(key) + ": ");
    auto value = line.substr(key.size() + 2);
    return value.substr(0, value.find(' '));
}

// Some build.ninja of the project (outside the build programs') written after `mark`.
bool planned_since(const s::project& p, fs::file_time_type mark) {
    for (const auto& f : s::find_files(p.at("target"), R"(build\.ninja)"))
        if (!f.generic_string().contains("/.build-mcpp/") && fs::last_write_time(f) > mark) return true;
    return false;
}

fs::file_time_type mark_then_wait() {
    const auto mark = fs::file_time_type::clock::now();
    std::this_thread::sleep_for(std::chrono::seconds{1});
    return mark;
}

// tests/e2e/217's fixture: four module interfaces, one per extension.
s::project extfix(const fs::path& dir, std::string_view extra) {
    return s::project(dir, {{"mcpp.toml", std::format("[package]\nname    = \"extfix\"\nversion = \"0.1.0\"\n{}\n", extra)},
                            {"src/a.cppm", "export module extfix.a;\nimport std;\nexport auto a() -> void { std::println(\"from .cppm\"); }\n"},
                            {"src/b.ccm", "export module extfix.b;\nimport std;\nexport auto b() -> void { std::println(\"from .ccm\"); }\n"},
                            {"src/c.cxxm", "export module extfix.c;\nimport std;\nexport auto c() -> void { std::println(\"from .cxxm\"); }\n"},
                            {"src/d.ixx", "export module extfix.d;\nimport std;\nexport auto d() -> void { std::println(\"from .ixx\"); }\n"}});
}

E2E extensions{"extensions", [] {
    "217 module extensions"_test[s::gcc] = [] {
        // Undeclared, .ccm/.cxxm/.ixx are not compiled and the default glob is unchanged;
        // declared, they join the glob, take the module rule and their own object names, and
        // link and run, under GCC and (with its payload) Clang.
        auto nocfg = extfix(s::case_dir() / "nocfg", "");
        nocfg.write("src/main.cpp", "import std;\nimport extfix.a;\nint main(){ a(); std::println(\"NOCFG OK\"); }\n");
        auto r = nocfg.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "default build broke\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "sources [src/**/*.{cppm,cpp,cc,c,S,s,asm}]"))) << "default source glob changed\n" << s::text(r);
        const auto nj = graph_text(nocfg.dir());
        for (std::string_view stray : {"b.ccm", "c.cxxm", "d.ixx"})
            expect(fatal(s::lacks(nj, stray))) << stray << " compiled without opt-in";
        expect(fatal(e2e::contains(s::text(nocfg.mcpp({"run"})), "NOCFG OK"))) << "the un-configured program did not run";

        const auto leg = [](std::string_view name, std::string_view extra) {
            auto p = extfix(s::case_dir() / std::format("leg_{}", name),
                            std::format("\n[build]\nmodule_extensions = [\".ccm\", \".cxxm\", \".ixx\"]\n{}", extra));
            p.write("src/main.cpp", "import std;\nimport extfix.a;\nimport extfix.b;\nimport extfix.c;\nimport extfix.d;\nint main(){ a(); b(); c(); d(); std::println(\"ALL OK\"); }\n");
            auto r = p.mcpp({"build"});
            expect(fatal(e2e::succeeded(r))) << "[" << name << "]: build\n" << s::text(r);
            expect(fatal(e2e::contains(s::text(r), "sources [src/**/*.{cppm,ccm,cxxm,ixx,cpp,cc,c,S,s,asm}]"))) << "[" << name << "]: declared extensions missing from default glob\n" << s::text(r);
            const auto out = s::text(p.mcpp({"run"}));
            for (std::string_view want : {"from .cppm", "from .ccm", "from .cxxm", "from .ixx", "ALL OK"})
                expect(fatal(e2e::contains(out, want))) << "[" << name << "]: missing '" << want << "' in: " << out;
            const auto g = graph_text(p.dir());
            for (std::string_view src : {"a.cppm", "b.ccm", "c.cxxm", "d.ixx"})
                expect(fatal(s::has_line(g, std::format(": cxx_module .*{}", src)))) << "[" << name << "]: " << src << " did not use the cxx_module rule";
            std::set<std::string> objs;
            static const std::regex obj{R"(obj/[A-Za-z0-9_.]+\.o)"};
            for (std::sregex_iterator it(g.begin(), g.end(), obj), end; it != end; ++it) objs.insert(it->str());
            for (std::string_view want : {"obj/a.m.o", "obj/b.ccm.o", "obj/c.cxxm.o", "obj/d.ixx.o"})
                expect(fatal(objs.contains(std::string(want)))) << "[" << name << "]: expected object " << want << "; got:" << objs;
        };
        leg("gcc", "");
        const auto llvm = s::llvm_version();
        if (fs::is_directory(s::payload_root("xim-x-llvm", llvm)))
            leg("clang", std::format("\n[toolchain]\ndefault = \"llvm@{}\"", llvm));
        else
            std::println("  skip: no LLVM payload installed — clang leg not run");
    };

    "218 module extensions graph shape"_test[s::gcc] = [] {
        // A no-change build replays; a new import in a .ixx plans again and the edge is real;
        // module_extensions changes the graph in the same directory; a dead entry is reported; a
        // reserved extension is refused naming the key.
        s::project p({{"mcpp.toml", "[package]\nname    = \"gshape\"\nversion = \"0.1.0\"\n\n[build]\nmodule_extensions = [\".ixx\"]\n"},
                      {"src/helper.cppm", "export module gshape.helper;\nimport std;\nexport auto helper() -> int { return 41; }\n"},
                      {"src/face.ixx", "export module gshape.face;\nimport std;\nexport auto face() -> int { return 1; }\n"},
                      {"src/main.cpp", "import std;\nimport gshape.face;\nint main(){ std::println(\"{}\", face()); }\n"}});
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "first build\n" << s::text(r);
        const auto fp_before = printed(p, "Fingerprint");
        expect(fatal(!fp_before.empty())) << "could not read the fingerprint";
        auto mark = mark_then_wait();
        r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "no-change build\n" << s::text(r);
        expect(fatal(!planned_since(p, mark))) << "fast path did not engage — the rest proves nothing\n" << s::text(r);

        mark = mark_then_wait();
        p.write("src/face.ixx", "export module gshape.face;\nimport std;\nimport gshape.helper;\nexport auto face() -> int { return helper() + 1; }\n");
        r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "build after .ixx edit\n" << s::text(r);
        expect(fatal(planned_since(p, mark))) << "editing a .ixx did not invalidate the fast path\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(p.mcpp({"run"})), "42"))) << "expected 42 (41+1)";

        p.write("src/extra.ccm", "export module gshape.extra;\nexport auto extra() -> int { return 2; }\n");
        p.write("mcpp.toml", "[package]\nname    = \"gshape\"\nversion = \"0.1.0\"\n\n[build]\nmodule_extensions = [\".ixx\", \".ccm\"]\n");
        r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "build after key change\n" << s::text(r);
        const auto fp_after = printed(p, "Fingerprint");
        expect(fatal(fp_before == fp_after)) << "module_extensions moved the build directory (" << fp_before << " -> " << fp_after << ")";
        // The build directory is named by the configuration's lineage (`Lineage:`), not by the
        // whole fingerprint (2026.11.13.1).
        const auto lineage = printed(p, "Lineage");
        const auto graphs = s::glob(p.at("target"), std::format("*/{}/build.ninja", lineage));
        expect(fatal(!graphs.empty() && e2e::matches(s::read_file(graphs.front()), R"(gshape\.extra)")))
            << "the .ccm named by module_extensions is not a module interface of the graph (lineage " << lineage << ")";

        p.write("mcpp.toml", "[package]\nname    = \"gshape\"\nversion = \"0.1.0\"\n\n[build]\nmodule_extensions = [\".ixx\", \".nosuchext\"]\n");
        r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "build with a dead entry\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "nosuchext"))) << "a module_extensions entry matching nothing was not reported\n" << s::text(r);
        p.write("mcpp.toml", "[package]\nname    = \"gshape\"\nversion = \"0.1.0\"\n\n[build]\nmodule_extensions = [\".c\"]\n");
        r = p.mcpp({"build"});
        expect(fatal(failed(r))) << "[build] module_extensions = [\".c\"] was accepted";
        expect(fatal(e2e::contains(s::text(r), "module_extensions"))) << "the error does not name the offending key\n" << s::text(r);
    };

    "236 module extensions default toolchain"_test = [] {
        // On this platform's default toolchain a declared .ixx is a module interface: its BMI is
        // written and the program runs.
        const auto llvm = s::llvm_version();
        s::project p({{"mcpp.toml", std::format(R"([package]
name    = "extdefault"
version = "0.1.0"
[toolchain]
default = "gcc@16.1.0"
macos   = "llvm@{0}"
windows = "llvm@{0}"
[build]
module_extensions = [".ixx"]
)", llvm)},
                      {"src/greet.ixx", "export module extdefault.greet;\nimport std;\nexport auto greet() -> std::string { return \"ixx-ok\"; }\n"},
                      {"src/main.cpp", "import std;\nimport extdefault.greet;\nint main() { std::println(\"{}\", greet()); return greet() == \"ixx-ok\" ? 0 : 1; }\n"}});
        auto r = p.mcpp({"build", "--release"});
        expect(fatal(e2e::succeeded(r))) << "build with module_extensions = [\".ixx\"] failed on this platform's default toolchain\n" << s::text(r);
        bool bmi = false;
        for (const auto& f : s::find_files(p.at("target"), R"(.*\.(gcm|pcm|ifc))")) {
            auto name = f.string();
            for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            bmi = bmi || name.contains("greet");
        }
        expect(fatal(bmi)) << "no BMI produced for the .ixx interface — it was not treated as a module interface unit at all";
        std::optional<fs::path> bin;
        for (const auto& f : s::find_files(p.at("target"), "extdefault(\\.exe)?"))
            if (!bin && f.parent_path().filename() == "bin") bin = f;
        expect(fatal(bin.has_value())) << "no binary produced";
        auto run = s::run(*bin);
        expect(fatal(e2e::succeeded(run))) << "the binary failed";
        auto out = run.out;
        std::erase(out, '\r');
        while (!out.empty() && out.back() == '\n') out.pop_back();
        expect(fatal(out == "ixx-ok")) << "binary printed '" << out << "', expected 'ixx-ok'";
    };

    "260 module extension is configuration"_test = [] {
        // An undeclared .ixx is refused naming the extension and the key; declared, it builds and runs.
        s::project p({{"src/mathkit.ixx", "export module mathkit;\nexport namespace mk { int answer() { return 42; } }\n"},
                      {"src/main.cpp", "#include <cstdio>\nimport mathkit;\nint main(){ std::printf(\"ixx-ok=%d\\n\", mk::answer()); return 0; }\n"}});
        const auto manifest = [&](std::string_view extra) {
            p.write("mcpp.toml", std::format("[package]\nname    = \"ixxprobe\"\nversion = \"0.1.0\"\n[build]\nsources = [\"src/*.ixx\", \"src/*.cpp\"]\n{}\n[targets.ixxprobe]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", extra));
        };
        manifest("");
        auto r = p.mcpp({"build"});
        expect(fatal(failed(r))) << "an undeclared '.ixx' was accepted";
        expect(fatal(e2e::contains(s::text(r), "no role for the extension '.ixx'"))) << "refused, but not for the classification reason\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "module_extensions"))) << "the message does not name the key that fixes it\n" << s::text(r);
        manifest("module_extensions = [\".ixx\"]");
        p.remove("target");
        r = p.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "declared .ixx did not build\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "ixx-ok=42"))) << "wrong answer\n" << s::text(r);
    };

    "263 lib root follows the extension"_test = [] {
        // The lib-root convention offers src/<name>.<declared extension>: no warning where it
        // exists, a warning where it does not, and a host-module dependency whose root is .ixx
        // resolves without a dead-entry warning.
        s::project root({{"lib/src/mathkit.ixx", "export module mathkit;\nexport namespace mk { int answer() { return 42; } }\n"},
                         {"lib/mcpp.toml", "[package]\nname    = \"mathkit\"\nversion = \"0.1.0\"\n[build]\nsources = [\"src/*.ixx\"]\nmodule_extensions = [\".ixx\"]\n[targets.mathkit]\nkind = \"lib\"\n"},
                         {"noroot/src/other.ixx", "export module other;\nexport namespace o { int v() { return 1; } }\n"},
                         {"noroot/mcpp.toml", "[package]\nname    = \"mathkit\"\nversion = \"0.1.0\"\n[build]\nsources = [\"src/*.ixx\"]\nmodule_extensions = [\".ixx\"]\n[targets.mathkit]\nkind = \"lib\"\n"},
                         {"rulepkg/src/rulepkg.ixx", "export module rulepkg;\nexport namespace rp { int magic() { return 7; } }\n"},
                         {"rulepkg/mcpp.toml", "[package]\nname    = \"rulepkg\"\nversion = \"0.1.0\"\n[build]\nsources = [\"src/*.ixx\"]\nmodule_extensions = [\".ixx\"]\n[targets.rulepkg]\nkind = \"lib\"\n[modules]\nexports = [\"rulepkg\"]\n"},
                         {"app/src/main.cpp", "#include <cstdio>\n#ifndef RULE_MAGIC\n#error \"the host-module rule did not reach this compile\"\n#endif\nint main(){ std::printf(\"host-mod=%d\\n\", RULE_MAGIC); return 0; }\n"},
                         {"app/build.mcpp", "import std;\nimport rulepkg;\nint main() {\n    std::println(\"mcpp:rerun-if-changed=build.mcpp\");\n    // A real directive, so the assertion is on an EFFECT rather than on a log\n    // line: the rule's return value has to reach the consumer's compile.\n    std::println(\"mcpp:cfg=RULE_MAGIC={}\", rp::magic());\n    return 0;\n}\n"}});
        root.write("app/mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\nstandard = \"c++23\"\n[dependencies]\nrulepkg = {{ path = \"{}\", host-module = true }}\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n",
                                                root.at("rulepkg").generic_string()));
        auto r = root.mcpp_in("lib", {"build"});
        expect(fatal(e2e::succeeded(r))) << "build\n" << s::text(r);
        expect(fatal(s::lacks(s::text(r), "conventional lib root"))) << "the validator looked for src/mathkit.cppm on a project whose interface is src/mathkit.ixx\n" << s::text(r);
        r = root.mcpp_in("noroot", {"build"});
        expect(fatal(e2e::contains(s::text(r), "conventional lib root"))) << "a genuinely missing lib root produced no warning\n" << s::text(r);
        r = root.mcpp_in("app", {"build"});
        expect(fatal(e2e::succeeded(r))) << "a host-module dependency whose interface is .ixx could not be resolved\n" << s::text(r);
        expect(fatal(s::no_line(s::text(r), "warning:.*module_extensions declares"))) << "the rule package was warned about a dead module_extensions entry\n" << s::text(r);
    };
}};

}  // namespace

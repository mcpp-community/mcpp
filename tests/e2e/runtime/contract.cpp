// The C++ runtime contract: a program's static initialisation, `host-coupled` in both roles and
// checked on the artifact; a shared library that couples to the runtime instead of exporting it;
// a shared library over a graph C++ runtime; one runtime for a program over a C++ shared library,
// and the symbol provision that shape no longer reports; one C++ runtime per process and its
// islands (formerly tests/e2e/183, 222, 690, 700, 701, 933).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

std::optional<fs::path> first_named(const fs::path& root, std::string_view name) {
    for (const auto& f : s::find_files(root, name))
        if (!f.generic_string().contains("/.build-mcpp/")) return f;
    return std::nullopt;
}

// `find <root> -path '*<suffix>' -type f | head -1`, the paths sorted.
std::optional<fs::path> ending(const fs::path& root, std::string_view suffix) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return std::nullopt;
    std::vector<fs::path> found;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec) && it->path().generic_string().ends_with(suffix)) found.push_back(it->path());
    if (found.empty()) return std::nullopt;
    std::ranges::sort(found);
    return found.front();
}

std::optional<fs::path> first(const fs::path& root, std::string_view pattern) {
    const auto all = s::glob(root, pattern);
    if (all.empty()) return std::nullopt;
    return all.front();
}

std::string captured(const e2e::outcome& r) {
    auto t = s::text(r);
    while (t.ends_with('\n') || t.ends_with('\r')) t.pop_back();
    return t;
}

// `mcpp why toolchain --format json | jq -r '.data.reason // empty'`.
std::string why_reason(const s::project& p) {
    const auto r = p.mcpp({"why", "toolchain", "--format", "json"});
    const auto doc = s::json(r.out);
    if (!doc.is_object() || !doc.contains("data") || !doc["data"].is_object()) return {};
    const auto& data = doc["data"];
    return data.contains("reason") && data["reason"].is_string() ? data["reason"].get<std::string>() : std::string();
}

// The symbol readings of 222's elf.py: a .dynsym is required, as "exports nothing" and "was not
// measured" would otherwise read alike.
std::vector<s::elf_symbol> dynsyms(const fs::path& file) {
    bool found = false;
    for (const auto& sec : s::elf_sections(file))
        if (sec.type == 11) found = true;  // SHT_DYNSYM
    expect(fatal(found)) << "no .dynsym in " << file.string() << " — a symbol count here would be zero for the wrong reason";
    return s::elf_symbol_table(file, ".dynsym");
}
bool is_std(std::string_view n) {
    for (std::string_view p : {"_ZNSt", "_ZNKSt", "_ZSt", "_ZTVSt", "_ZTISt"})
        if (n.starts_with(p)) return true;
    return false;
}
// GLOBAL standard-library definitions (weak/COMDAT instantiations are the C++ ABI's own).
std::size_t std_exports(const fs::path& f) {
    return static_cast<std::size_t>(std::ranges::count_if(dynsyms(f), [](const auto& e) { return e.defined && e.binding == 1 && is_std(e.name); }));
}
std::size_t std_undefined(const fs::path& f) {
    return static_cast<std::size_t>(std::ranges::count_if(dynsyms(f), [](const auto& e) { return !e.defined && is_std(e.name); }));
}
bool defines(const fs::path& f, std::string_view sym) {
    return std::ranges::any_of(dynsyms(f), [&](const auto& e) { return e.defined && e.name == sym; });
}

E2E contract{"contract", [] {
    "183 cxx runtime contract"_test = [] {
        // A global object touching std::cout during static initialisation runs, as a program
        // and as a test; `host-coupled` (and `static_stdlib = false`) reaches both roles; an
        // invalid contract is refused; self-contained is checked on the artifact.
        const auto tmp = s::case_dir();
        const auto early = R"(#include <iostream>
struct Early {
    Early() { probe = std::cout.rdbuf(); }
    std::streambuf* probe = nullptr;
};
static Early early_user;
int main() {
    std::cout << "static-init-ok\n";
    return early_user.probe == nullptr;
}
)";
        s::project siof(tmp / "siof", {{"mcpp.toml", "[package]\nname    = \"siof\"\nversion = \"0.1.0\"\n"}, {"src/main.cpp", early}, {"tests/early.cpp", early}});
        // 1
        expect(fatal(e2e::succeeded(siof.mcpp({"build"})))) << "FAIL: default build";
        auto r = siof.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: default-profile binary crashed during static initialization: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "static-init-ok"))) << "FAIL: expected static-init-ok, got: " << s::text(r);
        // 2
        r = siof.mcpp({"test"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: test binary crashed during static initialization: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "test result ok"))) << "FAIL: unexpected test output: " << s::text(r);
        if (s::on_macos()) {
            const auto ninja = first_named(siof.at("target"), R"(build\.ninja)");
            expect(fatal(ninja.has_value())) << "FAIL: no build.ninja";
            const auto text = s::read_file(*ninja);
            expect(fatal(e2e::contains(text, "mcpp_ios_init"))) << "FAIL: no initializer-ordering shim in the macOS link";
            // The line is kept before it is matched for its input: a match into a line of the
            // temporary list would outlive the list.
            std::string link_line;
            static const std::regex link{R"(^build .*: cxx_link +([^ ]+).*)"};
            for (const auto& l : s::lines_with(text, ": cxx_link "))
                if (std::regex_match(l, link)) { link_line = l; break; }
            std::smatch m;
            expect(fatal(std::regex_match(link_line, m, link) && m[1].str().contains("mcpp_ios_init"))) << "FAIL: shim object is not the first link input: " << link_line;
        }
        // 3
        siof.write("mcpp.toml", siof.read("mcpp.toml") + "\n[build]\ncxx_runtime = \"host-coupled\"\n");
        expect(fatal(e2e::succeeded(siof.mcpp({"build"})))) << "FAIL: host-coupled build";
        expect(fatal(e2e::succeeded(siof.mcpp({"test"})))) << "FAIL: host-coupled test";
        const std::string mechanism = R"(unit_ldflags.*(-static-libstdc\+\+|-load_hidden|-nostdlib\+\+))";
        for (std::string_view graph : {R"(build\.ninja)", R"(test\.ninja)"}) {
            const auto ninja = first_named(siof.at("target"), graph);
            expect(fatal(ninja.has_value())) << "FAIL: no " << graph;
            expect(fatal(s::no_line(s::read_file(*ninja), mechanism))) << "FAIL: host-coupled still emits a self-contained mechanism in " << ninja->filename().string();
        }
        // 4
        expect(fatal(e2e::succeeded(siof.mcpp({"build"})))) << "build";
        auto manifest = siof.read("mcpp.toml");
        manifest.replace(manifest.find("cxx_runtime = \"host-coupled\""), 28, "static_stdlib = false");
        siof.write("mcpp.toml", manifest);
        expect(fatal(e2e::succeeded(siof.mcpp({"build"})))) << "FAIL: static_stdlib=false build";
        auto ninja = first_named(siof.at("target"), R"(build\.ninja)");
        expect(fatal(ninja.has_value())) << "FAIL: no build.ninja";
        expect(fatal(s::no_line(s::read_file(*ninja), mechanism))) << "FAIL: static_stdlib = false is not an alias of host-coupled";
        // 5
        manifest = siof.read("mcpp.toml");
        manifest.replace(manifest.find("static_stdlib = false"), 21, "cxx_runtime = \"static\"");
        siof.write("mcpp.toml", manifest);
        r = siof.mcpp({"build"});
        expect(fatal(!r)) << "FAIL: invalid cxx_runtime accepted";
        expect(fatal(e2e::contains(s::text(r), "self-contained"))) << "FAIL: the error does not list the valid values: " << s::text(r);
        // 6
        s::project dist(tmp / "dist", {{"mcpp.toml", "[package]\nname    = \"dist\"\nversion = \"0.1.0\"\n"},
                                       {"src/main.cpp", "#include <iostream>\nint main() { std::cout << \"hi\\n\"; }\n"}});
        expect(fatal(e2e::succeeded(dist.mcpp({"build"})))) << "FAIL: dist build";
        if (s::on_macos()) {
            const auto bin = ending(dist.at("target"), "/bin/dist");
            expect(fatal(bin.has_value())) << "FAIL: no binary";
            // otool, as the script ran it.
            auto otool = s::run(s::on_path("otool").value_or("otool"), {"-L", bin->string()});
            std::string deps;
            std::istringstream in(otool.out);
            std::string line;
            std::getline(in, line);
            while (std::getline(in, line)) {
                std::istringstream words(line);
                std::string w;
                words >> w;
                deps += w + "\n";
            }
            expect(fatal(s::lacks(deps, "libc++"))) << "FAIL: self-contained artifact still depends on a libc++ dylib:\n" << deps;
        } else if (!s::on_windows()) {
            const auto bin = ending(dist.at("target"), "/bin/dist");
            expect(fatal(bin.has_value())) << "FAIL: no binary";
            for (const auto& n : s::read_header(*bin).needed)
                expect(fatal(!n.contains("libstdc++") && !n.contains("libc++"))) << "FAIL: self-contained artifact still depends on a C++ runtime: " << n;
        }
    };

    "222 shared library cxx runtime contract"_test[s::gcc][s::elf][s::python3] = [] {
        // A shared library couples to the C++ runtime instead of exporting it; the executable
        // keeps its own contract; the self-contained opt-out stays hidden.
        const std::string crash = "_ZNKSt13runtime_error4whatEv";
        const std::string manifest_tail = "[targets.sharedrtlib]\nkind = \"shared\"\n\n[targets.sharedrt]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n";
        s::project p({{"mcpp.toml", "[package]\nname = \"sharedrt\"\nversion = \"0.1.0\"\n\n" + manifest_tail},
                      {"src/lib.cppm", "export module sharedrt.lib;\nimport std;\nexport std::string sharedrt_greet() {\n    try { throw std::runtime_error(\"hello\"); }\n    catch (const std::exception& e) { return std::string(e.what()); }\n}\n"},
                      {"src/main.cpp", "import std;\nimport sharedrt.lib;\nint main() { return sharedrt_greet() == \"hello\" ? 0 : 1; }\n"}});
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        const auto bin = first(p.at("target"), "*/*/bin/sharedrt");
        auto lib = first(p.at("target"), "*/*/bin/libsharedrtlib.so");
        expect(fatal(bin.has_value())) << "FAIL: no executable";
        expect(fatal(lib.has_value())) << "FAIL: no shared library";
        // 1
        auto exported = std_exports(*lib);
        expect(fatal(exported == 0u)) << "FAIL: the shared library exports " << exported << " GLOBAL standard-library symbols";
        expect(fatal(!defines(*lib, crash))) << "FAIL: the shared library exports " << crash;
        // 2
        expect(fatal(std::ranges::contains(s::read_header(*lib).needed, "libstdc++.so.6"))) << "FAIL: the shared library declares no dependency on libstdc++.so.6";
        // 3
        const auto undef = std_undefined(*bin);
        expect(fatal(undef == 0u)) << "FAIL: the executable has " << undef << " undefined standard-library symbols.";
        expect(fatal(e2e::succeeded(s::run(*bin)))) << "FAIL: the executable did not run";
        // 4
        p.write("mcpp.toml", "[package]\nname = \"sharedrt\"\nversion = \"0.1.0\"\n\n[build]\ncxx_runtime = { shared = \"self-contained\" }\n\n" + manifest_tail);
        p.remove("target");
        r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        lib = first(p.at("target"), "*/*/bin/libsharedrtlib.so");
        expect(fatal(lib.has_value())) << "FAIL: no shared library on the second build";
        expect(fatal(!std::ranges::contains(s::read_header(*lib).needed, "libstdc++.so.6"))) << "FAIL: cxx_runtime = { shared = \"self-contained\" } was ignored";
        exported = std_exports(*lib);
        expect(fatal(exported == 0u)) << "FAIL: an explicitly self-contained shared library exported " << exported << " GLOBAL standard-library symbols";
        expect(fatal(!defines(*lib, crash))) << "FAIL: the embedded runtime is exported — " << crash;
    };

    "690 a shared library over a graph cxx runtime"_test[s::llvm][s::elf] = [] {
        // Over a graph C++ runtime a shared library is refused before compiling (naming the
        // package's own statement, or both remedies for a requested form); the static edge
        // builds; the stated private copy builds with no undefined libc++ reference and runs.
        const auto llvm = s::llvm_version();
        const auto tmp = s::case_dir();
        s::project fw(tmp / "fw", {{"src/fw.cppm", "export module fw;\nimport std;\nexport std::string fw_greet(int n);\nexport void fw_throw_runtime();\n"},
                                   {"src/fw.cpp", "module fw;\nimport std;\nstd::string fw_greet(int n) { return std::format(\"fw-{}\", n); }\nvoid fw_throw_runtime() { throw std::runtime_error(\"from fw\"); }\n"}});
        auto write_fw = [&](std::string_view kind) {
            fw.write("mcpp.toml", std::format("[package]\nname    = \"fw\"\nversion = \"0.1.0\"\n\n[targets.fw]\nkind = \"{}\"\n", kind));
        };
        s::project app(tmp / "app", {{"src/main.cpp", R"(import std;
import fw;
int main() {
    std::cout << fw_greet(3) << std::endl;
    try { fw_throw_runtime(); }
    catch (const std::runtime_error&) { std::cout << "runtime_error caught by its class" << std::endl; }
    catch (...) { std::cout << "runtime_error not matched by its class" << std::endl; }
}
)"}});
        auto write_app = [&](std::string_view edge, std::string_view build) {
            app.write("mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[toolchain]\ndefault = \"llvm@{}\"\n\n[build]\n{}\n\n[dependencies]\n"
                                               "llvm.libcxx = {{ git = \"https://github.com/mcpplibs/libcxx.git\", tag = \"22.1.8.2\" }}\nfw = {}\n", llvm, build, edge));
        };
        // The refusal, before anything compiles: the package constrains the form.
        write_fw("shared");
        write_app("{ path = \"../fw\" }", "");
        auto r = app.mcpp({"build"});
        auto log = s::text(r);
        expect(fatal(!r)) << "a shared library over a graph C++ runtime was not refused: " << log;
        expect(fatal(e2e::contains(log, "'fw' is linked as a shared library"))) << "the refusal does not name the shared library: " << log;
        expect(fatal(e2e::contains(log, "llvm.libcxx@22.1.8.2"))) << "the refusal does not name the provider: " << log;
        expect(fatal(e2e::contains(log, "cxx_runtime = { shared = \"self-contained\" }"))) << "the refusal does not name the private-copy statement: " << log;
        expect(fatal(e2e::contains(log, "not caught by that class"))) << "the refusal does not state the consequence: " << log;
        expect(fatal(e2e::contains(log, "'fw' states its form itself ([targets.fw] kind = \"shared\")"))) << "the refusal does not name the package's own statement: " << log;
        expect(fatal(s::lacks(log, "linkage = \"static\" }"))) << "the refusal offers an edge remedy the package's constraint defeats: " << log;
        if (const auto ninja = first(app.at("target"), "*/*/build.ninja"))
            expect(fatal(s::no_line(s::read_file(*ninja), "^build bin/libfw.so"))) << "a build graph with the shared library was written before the refusal";
        const auto reason = why_reason(app);
        expect(fatal(reason == "shared-library-cxx-runtime")) << "the machine reason was '" << reason << "', expected shared-library-cxx-runtime";
        // The refusal when the consumer asked for the shared form: both remedies.
        write_fw("lib");
        write_app("{ path = \"../fw\", linkage = \"shared\" }", "");
        app.remove("target");
        r = app.mcpp({"build"});
        log = s::text(r);
        expect(fatal(!r)) << "a requested shared library over a graph C++ runtime was not refused: " << log;
        expect(fatal(e2e::contains(log, "linkage = \"static\" }"))) << "the refusal does not offer the static edge for a requested form: " << log;
        expect(fatal(e2e::contains(log, "cxx_runtime = { shared = \"self-contained\" }"))) << "the refusal does not offer the private copy for a requested form: " << log;
        // The static edge: builds and runs, no shared library.
        write_app("{ path = \"../fw\", linkage = \"static\" }", "");
        app.remove("target");
        r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "the static edge did not build: " << s::text(r);
        auto bin = first(app.at("target"), "*/*/bin/app");
        expect(fatal(bin.has_value())) << "no bin/app";
        expect(fatal(!first(app.at("target"), "*/*/bin/libfw.so").has_value())) << "the static edge still produced libfw.so";
        auto out = s::run(*bin);
        expect(fatal(e2e::succeeded(out))) << "the program over the static edge exited non-zero";
        expect(fatal(s::has_line(out.out, "^fw-3$"))) << "expected fw-3 from the static edge, got: " << out.out;
        // The stated private copy: builds, no undefined libc++ reference, runs.
        write_fw("shared");
        write_app("{ path = \"../fw\" }", "cxx_runtime = { shared = \"self-contained\" }");
        app.remove("target");
        r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "the stated private copy did not build: " << s::text(r);
        const auto so = first(app.at("target"), "*/*/bin/libfw.so");
        expect(fatal(so.has_value())) << "no libfw.so under the stated private copy: " << s::text(r);
        // `nm -D --undefined-only | grep -c '__1'`: the dynamic symbols read here.
        const auto undefined = std::ranges::count_if(s::elf_symbol_table(*so, ".dynsym"), [](const auto& e) { return !e.defined && e.name.contains("__1"); });
        expect(fatal(undefined == 0)) << "libfw.so still has " << undefined << " undefined std::__1 references";
        bin = first(app.at("target"), "*/*/bin/app");
        expect(fatal(bin.has_value())) << "no bin/app";
        out = s::run(*bin);
        expect(fatal(e2e::succeeded(out))) << "the program over the private copy exited non-zero";
        expect(fatal(s::has_line(out.out, "^fw-3$"))) << "expected fw-3 under the private copy, got: " << out.out;
        for (const auto& l : s::lines_with(out.out, "runtime_error")) std::println("READING private copy: {}", l);
    };

    "700 a program over a cxx shared library has one cxx runtime"_test[s::llvm][s::elf] = [] {
        // An unstated program contract takes the shared library's: the program runs on
        // libc++.so.1, the record says toolchain-coupled, --strict passes; a stated
        // self-contained program over the coupled library is refused before compiling.
        const auto llvm = s::llvm_version();
        const auto tmp = s::case_dir();
        s::project lib(tmp / "lib", {{"mcpp.toml", "[package]\nname    = \"lib\"\nversion = \"0.1.0\"\n\n[targets.lib]\nkind = \"shared\"\n"},
                                     {"src/lib.cppm", "export module lib;\nimport std;\nexport [[gnu::visibility(\"default\")]] std::string lib_greet(int n);\n"},
                                     {"src/lib.cpp", "module lib;\nimport std;\nstd::string lib_greet(int n) { return std::format(\"lib-{}\", n); }\n"}});
        s::project app(tmp / "app", {{"src/main.cpp", "import std;\nimport lib;\nint main() { std::println(\"{}\", lib_greet(3)); }\n"}});
        auto write_app = [&](std::string_view build) {
            app.write("mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[toolchain]\ndefault = \"llvm@{}\"\n\n[build]\n{}\n\n[dependencies]\nlib = {{ path = \"../lib\" }}\n", llvm, build));
        };
        write_app("");
        auto r = app.mcpp({"build", "--strict"});
        expect(fatal(e2e::succeeded(r))) << "the default shape did not build under --strict: " << s::text(r);
        const auto dirs = s::glob(app.at("target"), "*/*");
        expect(fatal(!dirs.empty())) << "no build directory";
        const auto dir = dirs.front();
        auto out = s::run(dir / "bin/app");
        expect(fatal(e2e::succeeded(out))) << "the program aborted: " << s::text(out);
        expect(fatal(captured(out) == "lib-3")) << "expected lib-3, got: " << captured(out);
        expect(fatal(std::ranges::contains(s::read_header(dir / "bin/app").needed, "libc++.so.1"))) << "the program does not load libc++.so.1, so it carries its own runtime";
        expect(fatal(s::has_line(s::read_file(dir / "resolution.json"), R"("distributable": *"toolchain-coupled")"))) << "resolution.json does not record the program as toolchain-coupled";
        expect(fatal(s::lacks(s::text(r), "also provided by a library it loads"))) << "the symbol-provision check reported the shared shape: " << s::text(r);
        // A stated self-contained program over the coupled library is refused.
        write_app("cxx_runtime = { default = \"self-contained\", shared = \"toolchain-coupled\" }");
        app.remove("target");
        r = app.mcpp({"build"});
        const auto log = s::text(r);
        expect(fatal(!r)) << "a stated self-contained program over a coupled C++ shared library was not refused: " << log;
        expect(fatal(e2e::contains(log, "state a self-contained C++ runtime and load the C++ shared library 'lib'"))) << "the refusal does not name the statement and the library: " << log;
        expect(fatal(e2e::contains(log, "cxx_runtime = { shared = \"self-contained\" }"))) << "the refusal does not name the private-copy remedy: " << log;
        if (const auto ninja = first(app.at("target"), "*/*/build.ninja"))
            expect(fatal(s::no_line(s::read_file(*ninja), "^build bin/liblib.so"))) << "a build graph was written before the refusal";
        const auto reason = why_reason(app);
        expect(fatal(reason == "program-cxx-runtime-split")) << "the machine reason was '" << reason << "', expected program-cxx-runtime-split";
    };

    "701 symbol provision does not report what the build shares by construction"_test[s::elf][s::python3] = [] {
        // A program over a C++ shared library, both importing std, is clean in the record,
        // passes --strict and runs; the check measured it.
        const auto tmp = s::case_dir();
        s::project lib(tmp / "lib", {{"mcpp.toml", "[package]\nname    = \"lib\"\nversion = \"0.1.0\"\n\n[targets.lib]\nkind = \"shared\"\n"},
                                     {"src/lib.cppm", "export module lib;\nimport std;\nexport [[gnu::visibility(\"default\")]] std::string lib_greet(int n);\n"},
                                     {"src/lib.cpp", "module lib;\nimport std;\nstd::string lib_greet(int n) { return std::format(\"lib-{}\", n); }\n"}});
        s::project app(tmp / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\nlib = { path = \"../lib\" }\n"},
                                     {"src/main.cpp", "import std;\nimport lib;\nint main() { std::println(\"{}\", lib_greet(3)); }\n"}});
        auto r = app.mcpp({"build", "--strict"});
        expect(fatal(e2e::succeeded(r))) << "a program over a C++ shared library failed --strict: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), "also provided by a library it loads"))) << "the check reported the shape the build creates by construction: " << s::text(r);
        const auto dirs = s::glob(app.at("target"), "*/*");
        expect(fatal(!dirs.empty())) << "no build directory";
        const auto dir = dirs.front();
        const auto out = s::run(dir / "bin/app");
        expect(fatal(e2e::succeeded(out))) << "the program exited non-zero: " << s::text(out);
        expect(fatal(captured(out) == "lib-3")) << "expected lib-3, got: " << captured(out);
        const auto doc = s::json_file(dir / "resolution.json");
        nlohmann::json entry;
        if (doc.contains("runtime") && doc["runtime"].contains("symbol_provision"))
            for (const auto& e : doc["runtime"]["symbol_provision"])
                if (e.value("path", std::string()).ends_with("bin/app")) { entry = e; break; }
        expect(fatal(entry.is_object())) << "the record has no entry for bin/app";
        const auto status = entry.value("status", std::string());
        const auto exported = entry.value("exported", 0);
        const auto total = entry.value("dynamic_symbols", 0);
        expect(fatal(status == "clean")) << "the record says '" << entry.dump() << "', expected a clean verdict";
        expect(fatal(exported >= 1)) << "the program exports nothing, so the check did not measure the shape";
        expect(fatal(total > exported)) << "implausible denominator: " << entry.dump();
    };

    "933 one cxx runtime per process"_test = [] {
        // An llvm build with libstdc++ on its link line: a typed catch across a static library
        // works and the undeclared island is reported; a C++ shared library beside it is
        // refused; a typed catch of a gcc library's exception is refused; a declared private
        // island is not reported; `private` on another layer is refused.
        if (!s::on_linux()) skip("933 is about ELF");
        const auto llvm = s::llvm_version();
        if (!fs::exists(s::payload_root("xim-x-llvm", llvm) / "bin/clang++")) skip("no llvm payload");
        const auto gcc = s::gcc_version();
        const auto gcc_root = s::payload_root("xim-x-gcc", gcc);
        const auto glib = (gcc_root / "lib64").generic_string();
        if (!fs::exists(gcc_root / "lib64/libstdc++.so.6") || !fs::exists(gcc_root / "bin/g++")) skip("no gcc payload");
        const auto tmp = s::case_dir();
        auto project = [&](std::string_view dir, std::string_view kind, std::string_view extra) {
            return s::project(tmp / dir, {
                {"mcpp.toml", std::format("[package]\nname     = \"xrt\"\nversion  = \"0.1.0\"\nstandard = \"c++23\"\n[toolchain]\ndefault = \"llvm@{}\"\n[lib]\npath = \"src/xrt.cppm\"\n"
                                          "[targets.xrt]\nkind = \"{}\"\n[build]\nldflags = [\"-L{}\", \"-l:libstdc++.so.6\"]\n{}\n", llvm, kind, glib, extra)},
                {"src/xrt.cppm", "export module xrt;\nimport std;\nexport namespace xrt { int checked(int v) { if (v < 0) throw std::runtime_error(\"negative\"); return v * 2; } }\n"},
                {"tests/catch.cpp", R"cpp(import std;
import xrt;
int main() {
    auto ep = std::make_exception_ptr(std::logic_error("x"));   // a libc++abi-only entry point
    try { xrt::checked(-1); }
    catch (const std::exception& e) { std::println("caught as std::exception: {}", e.what()); return 0; }
    catch (...)                     { std::println("std::exception missed -> (...)"); return 1; }
}
)cpp"}});
        };
        // A
        auto a = project("a", "lib", "");
        auto r = a.mcpp({"test"});
        expect(fatal(e2e::succeeded(r))) << "A: the typed catch failed: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "caught as std::exception"))) << "A: no typed catch: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "no package declares it"))) << "A: the undeclared island is not reported: " << s::text(r);
        // B
        auto b = project("b", "shared", "");
        r = b.mcpp({"build"});
        expect(fatal(!r)) << "B: a C++ shared library beside the island was accepted: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "cxx-runtime-island"))) << "B: not refused as an island: " << s::text(r);
        // C
        s::project foo(tmp / "c/foo", {{"mcpp.toml", std::format("[package]\nname     = \"foo\"\nversion  = \"0.1.0\"\nstandard = \"c++23\"\n[toolchain]\ndefault = \"gcc@{}\"\n[build]\nsources = [\"src/*.cpp\"]\n[targets.foo]\nkind = \"shared\"\n", gcc)},
                                       {"src/foo.cpp", "#include <stdexcept>\nstruct FooError : std::runtime_error { FooError() : std::runtime_error(\"foo\") {} };\nvoid foo_throws() { throw FooError(); }\n"}});
        r = foo.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "C: libfoo did not build with gcc: " << s::text(r);
        const auto libfoo = s::find_file(foo.at("target"), "libfoo.so");
        expect(fatal(libfoo.has_value())) << "C: no libfoo.so: " << s::text(r);
        const auto foo_dir = libfoo->parent_path().generic_string();
        s::project capp(tmp / "c/app", {{"mcpp.toml", std::format("[package]\nname     = \"app\"\nversion  = \"0.1.0\"\nstandard = \"c++23\"\n[toolchain]\ndefault = \"llvm@{}\"\n[build]\nldflags = [\"-L{}\", \"-lfoo\", \"-Wl,-rpath,{}\"]\n", llvm, foo_dir, foo_dir)},
                                        {"src/main.cpp", "#include <stdexcept>\nstruct FooError : std::runtime_error { FooError(); };\nvoid foo_throws();\nint main() { try { foo_throws(); } catch (const FooError&) { return 0; } return 1; }\n"}});
        r = capp.mcpp({"build"});
        expect(fatal(!r)) << "C: a typed catch across runtimes was accepted: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "cxx-runtime-collision"))) << "C: not refused as a collision: " << s::text(r);
        // D
        auto d = project("d", "lib", "[target.'cfg(os = \"linux\")']\nrequires = [{ layer = \"mcpp:c++-abi=libstdc++\", visibility = \"private\" }]");
        r = d.mcpp({"test"});
        expect(fatal(e2e::succeeded(r))) << "D: the declared island failed: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), "no package declares it"))) << "D: a declared island is reported as undeclared: " << s::text(r);
        // E
        s::project e(tmp / "e", {{"mcpp.toml", "[package]\nname = \"e\"\nversion = \"0.1.0\"\nrequires = [{ layer = \"mcpp:c-abi=glibc\", visibility = \"private\" }]\n"},
                                 {"src/main.cpp", "int main() { return 0; }\n"}});
        r = e.mcpp({"build"});
        expect(fatal(!r && e2e::contains(s::text(r), "requires-private-layer"))) << "E: private on another layer was accepted: " << s::text(r);
    };
}};

}  // namespace

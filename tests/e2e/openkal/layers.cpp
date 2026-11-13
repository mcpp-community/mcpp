// The target side's layers as packages state them: the include directories a package is built
// from and those it publishes, the kernel-abi interfaces an implementation provides, a kernel-abi
// package in a shared image, the build's contract naming the target side, the interfaces a
// build program reads, and a base package's layers beside its C library (formerly tests/e2e/305,
// 743, 950, 994, 1056, 1059).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

// The words of a compile database entry: its `arguments`, else its `command` split at spaces.
std::vector<std::string> words_of(const nlohmann::json& entry) {
    std::vector<std::string> out;
    if (entry.contains("arguments")) {
        for (const auto& a : entry["arguments"]) out.push_back(a.get<std::string>());
        return out;
    }
    const auto command = entry.value("command", std::string{});
    for (auto part : std::views::split(command, ' ')) out.emplace_back(part.begin(), part.end());
    return out;
}

E2E layers{"layers", [] {
    "305 built from is not published"_test[s::unix_shell][s::jq] = [] {
        // `private_include_dirs` (a subset of `include_dirs`, globs expanded before the filter)
        // build the provider and do not reach the consumer: the provider's units carry its
        // private and public directories, the consumer's the public ones only, and a consumer
        // naming `hidden` as an identifier compiles.
        s::project p({{"lib/mcpp.toml", "[package]\nname    = \"vendored\"\nversion = \"0.1.0\"\n\n[build]\ninclude_dirs         = [\"internal\", \"gen/*\", \"pub\"]\nprivate_include_dirs = [\"internal\", \"gen/*\"]\n"},
                      {"lib/src/vendored.cppm", "export module vendored;\nexport int vendored_v() { return 7; }\n"},
                      {"lib/pub/pub.h", "#pragma once\n#define VENDORED_PUBLIC 1\n"},
                      {"lib/internal/overlay.h", "#pragma once\n#define hidden __attribute__((visibility(\"hidden\")))\n"},
                      {"lib/gen/one/one.h", "#pragma once\n"},
                      {"lib/gen/two/two.h", "#pragma once\n"},
                      {"mcpp.toml", "[package]\nname    = \"boundaryprobe\"\nversion = \"0.1.0\"\n\n[dependencies]\nvendored = { path = \"lib\" }\n"},
                      {"src/main.cpp", "#include <cstdio>\n#include <pub.h>\nimport vendored;\nint main() {\n    int hidden = 1;\n    std::printf(\"%d %d\\n\", vendored_v() + hidden, VENDORED_PUBLIC);\n}\n"}});
        // The project-root copy of the compile database is opt-in ([build] compile_commands).
        auto r = p.mcpp({"build"}, {.env = {{"MCPP_COMPILE_COMMANDS", "compile_commands.json"}}});
        expect(fatal(e2e::succeeded(r))) << "the consumer did not build — the private overlay is still reaching it: " << s::text(r);
        expect(fatal(p.exists("compile_commands.json") && !p.read("compile_commands.json").empty())) << "no compile_commands.json";
        const auto cdb = s::json_file(p.at("compile_commands.json"));
        const auto rows = [&](std::string_view file) {
            std::vector<nlohmann::json> out;
            for (const auto& e : cdb)
                if (e.value("file", std::string{}).contains(file)) out.push_back(e);
            return out;
        };
        const auto prov = rows("/lib/src/").size(), cons = rows("/src/main").size();
        expect(fatal(prov >= 1 && cons >= 1)) << "CDB has provider=" << prov << " consumer=" << cons << " rows — nothing was checked";
        const auto has_dir = [&](std::string_view file, std::string_view dir) {
            return std::ranges::any_of(rows(file), [&](const nlohmann::json& e) {
                return std::ranges::any_of(words_of(e), [&](const std::string& w) { return w.contains(dir); });
            });
        };
        expect(has_dir("/lib/src/", "/lib/internal")) << "the provider lost its own private directory";
        expect(has_dir("/lib/src/", "/lib/pub")) << "the provider lost its own public directory";
        expect(has_dir("/src/main", "/lib/pub")) << "the public directory did not reach the consumer";
        expect(!has_dir("/src/main", "/lib/internal")) << "the private directory leaked to the consumer";
        for (std::string_view g : {"one", "two"}) {
            expect(has_dir("/lib/src/", std::format("/lib/gen/{}", g))) << "the provider lost 'gen/" << g << "', which its own glob names";
            expect(!has_dir("/src/main", std::format("/lib/gen/{}", g))) << "'gen/" << g << "' leaked to the consumer — the glob was compared unexpanded";
        }
    };

    "743 kernel abi interfaces are resolved not preprocessed"_test[s::gcc] = [] {
        // A: every required interface provided builds. B: one not provided is refused at
        // resolution naming the interface, the requirer and the resolved implementation, with
        // its code, nothing compiled. C: a provider stating no list builds and is named, with the
        // count of unchecked requirements. D: a provider stating its list draws no such note.
        s::project p({{"impl/src/lib.c", "int fake_kernel_marker(void) { return 0; }\n"}, {"src/main.c", "int main(void) { return 0; }\n"}});
        const auto write_impl = [&](std::string_view list) {
            p.write("impl/mcpp.toml", std::format("[package]\nname     = \"fakekernel\"\nversion  = \"0.1.0\"\nprovides = [\"mcpp:kernel-abi=openkal\"]\n\n[targets.fakekernel]\nkind    = \"lib\"\nsources = [\"src/*.c\"]\n{}",
                                                  list.empty() ? "" : std::format("\n[kernel-abi]\nprovides-interfaces = [{}]\n", list)));
        };
        const auto write_root = [&](std::string_view list) {
            p.write("mcpp.toml", std::format("[package]\nname    = \"iface-probe\"\nversion = \"0.1.0\"\n\n[dependencies]\nfakekernel = {{ path = \"impl\" }}\n\n[build]\nallow_host_libs = true\n{}",
                                             list.empty() ? "" : std::format("\n[kernel-abi]\nrequires-interfaces = [{}]\n", list)));
        };
        // A
        write_impl(R"("openkal.abort", "openkal.stream", "openkal.memory", "openkal.fs")");
        write_root(R"("openkal.fs", "openkal.stream")");
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "A: a graph whose implementation provides every required interface must build: " << s::text(r);
        // B
        fs::remove_all(p.at("target"));
        write_root(R"("openkal.fs", "openkal.net")");
        r = p.mcpp({"build"});
        auto out = s::text(r);
        expect(fatal(failed(r))) << "B: a requirement the implementation does not provide must be refused at resolution: " << out;
        expect(fatal(e2e::contains(out, "openkal.net"))) << "B: the refusal must name the interface that is missing: " << out;
        expect(fatal(e2e::contains(out, "iface-probe"))) << "B: the refusal must name the package that required it: " << out;
        expect(fatal(e2e::contains(out, "fakekernel"))) << "B: the refusal must name the implementation that did not provide it: " << out;
        expect(fatal(e2e::contains(out, "the resolved implementation is fakekernel"))) << "B: the line naming the implementation must say that it is the one that was resolved: " << out;
        expect(fatal(!s::has_line_nocase(out, "provided by *fakekernel"))) << "B: 'provided by fakekernel' under the missing interface states the opposite of this refusal: " << out;
        expect(fatal(e2e::contains(out, "[interface-not-provided]"))) << "B: the refusal must carry its code: " << out;
        expect(fatal(!fs::is_directory(p.at("target")) || s::find_files(p.at("target"), R"(.*\.o)").empty())) << "B: the refusal must arrive before anything is compiled";
        // C
        fs::remove_all(p.at("target"));
        write_impl("");
        write_root(R"("openkal.fs", "openkal.net")");
        r = p.mcpp({"build"});
        out = s::text(r);
        expect(fatal(e2e::succeeded(r))) << "C: a provider that predates this key must keep building: " << out;
        expect(fatal(e2e::contains(out, "kernel-abi interfaces"))) << "C: a build in which no provider stated a list must say so: " << out;
        expect(fatal(e2e::contains(out, "fakekernel"))) << "C: the note must name whose silence this is: " << out;
        expect(fatal(e2e::contains(out, "2 requirements unchecked"))) << "C: the note must report how many requirements went unchecked: " << out;
        // D
        fs::remove_all(p.at("target"));
        write_impl(R"("openkal.abort", "openkal.stream", "openkal.memory", "openkal.fs")");
        write_root(R"("openkal.fs", "openkal.stream")");
        r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "D: leg D's graph is leg A's and must build: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), "kernel-abi interfaces"))) << "D: the note belongs to the case where nothing was checked: " << s::text(r);
    };

    "950 a kernel abi package over the system c library is in the shared image"_test[s::elf] = [] {
        // A: a program reaching a kernel-abi package only through a dependency's shared image:
        // the image defines the package's symbols (as its ordinary static dependency's) and
        // imports none of them. B: the program reaching the package itself links and runs.
        s::project p({{"kernel/src/kernel.c", "int kal_fake_answer(void) { return 42; }\n"},
                      {"kernel/mcpp.toml", "[package]\nname     = \"fakekernel\"\nversion  = \"0.1.0\"\nprovides = [\"mcpp:kernel-abi=openkal\"]\n\n[targets.fakekernel]\nkind    = \"lib\"\nsources = [\"src/*.c\"]\n"},
                      {"sys/src/sys.c", "extern int kal_fake_answer(void);\nint sys_answer(void) { return kal_fake_answer(); }\n"},
                      {"sys/mcpp.toml", "[package]\nname    = \"fakesys\"\nversion = \"0.1.0\"\n\n[targets.fakesys]\nkind    = \"lib\"\nsources = [\"src/*.c\"]\n\n[dependencies]\nfakekernel = { path = \"../kernel\" }\n"},
                      {"ext/src/ext.c", "extern int sys_answer(void);\nint ext_answer(void) { return sys_answer(); }\n"},
                      {"ext/mcpp.toml", "[package]\nname    = \"ext\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/*.c\"]\n\n[targets.ext]\nkind = \"shared\"\n\n[dependencies]\nfakesys = { path = \"../sys\" }\n"}});
        const auto write_app = [&](std::string_view dep, std::string_view check) {
            p.write("app/src/main.c", std::format("extern int ext_answer(void);\nextern int kal_fake_answer(void);\nint main(void) {{ return {} ? 0 : 1; }}\n", check));
            p.write("app/mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/*.c\"]\nallow_host_libs = true\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.c\"\n\n[dependencies]\next = {{ path = \"../ext\" }}\n{}\n", dep));
        };
        const auto build_app = [&](std::string_view leg) {
            fs::remove_all(p.at("app/target"));
            auto r = p.mcpp_in("app", {"build"});
            expect(fatal(e2e::succeeded(r))) << leg << ": the program must build: " << s::text(r);
            const auto so = s::find_file(p.at("app/target"), "libext.so");
            std::optional<fs::path> bin;
            for (const auto& f : s::find_files(p.at("app/target"), "app"))
                if (f.parent_path().filename() == "bin") { bin = f; break; }
            expect(fatal(so.has_value() && bin.has_value())) << leg << ": libext.so or bin/app was not produced: " << s::text(r);
            auto ran = s::run(*bin);
            expect(fatal(e2e::succeeded(ran))) << leg << ": the program ran and answered " << ran.status;
            return *so;
        };
        write_app("", "ext_answer() == 42");
        const auto so = build_app("A");
        const auto defined = s::elf_symbols(so, ".symtab", true);
        expect(fatal(std::ranges::contains(defined, std::string("sys_answer")))) << "A: the shared image does not define sys_answer (its static dependency)";
        const auto dynamic_defined = s::elf_symbols(so, ".dynsym", true);
        for (const auto& n : s::elf_symbols(so, ".dynsym"))
            expect(fatal(n != "kal_fake_answer" || std::ranges::contains(dynamic_defined, n)))
                << "A: the shared image imports kal_fake_answer from the program; a host that did not link fakekernel cannot load it";
        expect(fatal(std::ranges::contains(defined, std::string("kal_fake_answer")))) << "A: the shared image does not define kal_fake_answer";
        write_app("fakekernel = { path = \"../kernel\" }", "ext_answer() == 42 && kal_fake_answer() == 42");
        build_app("B");
    };

    "994 the contract names the target side"_test[s::llvm] = [] {
        // For an ok image the contract's C++ words name `-D__OPENKAL__` and an `-isystem`
        // directory holding the C library's <stdio.h>; a C++ unit (<vector>, <cstdio>) and a C
        // unit (<stdio.h>) compile with the contract's program and words alone.
#if defined(__x86_64__) || defined(_M_X64)
        const std::string arch = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
        const std::string arch = "aarch64";
#else
        skip("no ok-image row for this machine");
        const std::string arch;
#endif
        const auto tmp = s::case_dir();
        s::project app(tmp / "app", {{"mcpp.toml", "[package]\nname    = \"ct\"\nversion = \"0.1.0\"\n"}, {"src/main.cpp", "int main() { return 0; }\n"}});
        auto r = app.mcpp({"build", "--target", arch + "-openkal"});
        expect(fatal(e2e::succeeded(r))) << "the build: " << s::text(r);
        // `find <root> -maxdepth 2 -name mcpp-contract.json | head -1`
        const auto root = app.at("target/" + arch + "-openkal");
        auto found = s::glob(root, "mcpp-contract.json");
        for (const auto& f : s::glob(root, "*/mcpp-contract.json")) found.push_back(f);
        expect(fatal(!found.empty())) << "no contract: " << s::text(r);
        const auto contract = s::json_file(found.front());
        std::vector<std::string> cxx;
        for (const auto& w : contract["compile"]["cxx"]["args"]) cxx.push_back(w.get<std::string>());
        expect(fatal(std::ranges::contains(cxx, std::string("-D__OPENKAL__")))) << "A: the target side is not in the contract: " << contract.dump(2);
        bool stdio = false;
        for (std::size_t i = 0; i + 1 < cxx.size(); ++i)
            if (cxx[i] == "-isystem" && fs::is_regular_file(fs::path(cxx[i + 1]) / "stdio.h")) stdio = true;
        expect(fatal(stdio)) << "A: no -isystem directory of the contract holds stdio.h: " << contract.dump(2);

        s::project units(tmp, {{"u.cpp", "#include <vector>\n#include <cstdio>\nint f() { std::vector<int> v{1, 2}; std::printf(\"%zu\", v.size()); return 0; }\n"},
                               {"u.c", "#include <stdio.h>\nint g(void) { return puts(\"x\"); }\n"}});
        for (const auto& [lang, unit, standard] : {std::tuple{"cxx", "u.cpp", "-std=c++20"}, std::tuple{"c", "u.c", "-std=c11"}}) {
            const auto& t = contract["compile"][lang];
            e2e::command c{.argv = {t["program"].get<std::string>()}, .cwd = tmp};
            for (const auto& w : t["args"]) c.argv.push_back(w.get<std::string>());
            for (std::string w : {std::string(standard), std::string("-c"), std::string(unit), std::string("-o"), std::string(unit) + ".o"}) c.argv.push_back(w);
            auto u = e2e::run(c);
            expect(fatal(e2e::succeeded(u))) << "B/C: " << lang << " did not compile with the contract alone: " << s::text(u);
        }
    };

    "1056 a runner reads the interfaces its implementation provides"_test[s::gcc] = [] {
        // The build program of a package that requires the kernel-abi layer reads the provider's
        // `provides-interfaces` (`mcpp::kernel_abi_interfaces()`, ';'-joined); a package that
        // does not require it reads nothing; a provider stating no list gives nothing.
        const auto program = [](std::string_view name, std::string_view requires_line) -> std::vector<std::pair<std::string, std::string>> {
            return {{std::format("{}/mcpp.toml", name), std::format("[package]\nname     = \"{0}\"\nversion  = \"0.1.0\"\n{1}\n\n[targets.{0}]\nkind = \"lib\"\n", name, requires_line)},
                    {std::format("{0}/src/{0}.c", name), std::format("int {}_fn(void) {{ return 2; }}\n", name)},
                    {std::format("{}/build.mcpp", name), std::format("import std;\nimport mcpp;\nint main() {{ std::println(\"mcpp:warning={} sees [{{}}]\", mcpp::kernel_abi_interfaces()); }}\n", name)}};
        };
        const auto impl = "[package]\nname     = \"impl\"\nversion  = \"0.1.0\"\nprovides = [\"mcpp:kernel-abi=toykernel\"]\n\n[kernel-abi]\nprovides-interfaces = [\"openkal.abort\", \"openkal.fs\"]\n\n[targets.impl]\nkind = \"lib\"\n";
        s::project p({{"impl/mcpp.toml", impl},
                      {"impl/src/k.c", "int toy_kernel(void) { return 1; }\n"},
                      {"app/mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\nimpl   = { path = \"../impl\" }\nrunner = { path = \"../runner\" }\nother  = { path = \"../other\" }\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.c\"\n"},
                      {"app/src/main.c", "int main(void) { return 0; }\n"}});
        for (const auto& [file, content] : program("runner", "requires = [\"mcpp:kernel-abi=toykernel\"]")) p.write(file, content);
        for (const auto& [file, content] : program("other", "")) p.write(file, content);
        auto out = s::text(p.mcpp_in("app", {"build"}));
        expect(fatal(e2e::contains(out, "runner sees [openkal.abort;openkal.fs]"))) << "the requiring package did not read the list: " << out;
        expect(fatal(e2e::contains(out, "other sees []"))) << "a package that does not require the layer read something: " << out;
        // `sed -i '/^\[kernel-abi\]$/,/^provides-interfaces/d'`
        p.write("impl/mcpp.toml", "[package]\nname     = \"impl\"\nversion  = \"0.1.0\"\nprovides = [\"mcpp:kernel-abi=toykernel\"]\n\n\n[targets.impl]\nkind = \"lib\"\n");
        out = s::text(p.mcpp_in("app", {"build"}));
        expect(fatal(e2e::contains(out, "runner sees []"))) << "an unstated list was not empty: " << out;
    };

    "1059 a base package states the layers beside its c library"_test[s::llvm] = [] {
        // A sysroot carrying a manifest is a base package: its `provides` answers the c++-abi
        // and compiler-runtime layers, its [build] keys give a unit the base's C++ headers (not
        // the payload's) and the link its libraries with -nostdlib++; each architecture takes
        // the base its provider names; a row the platform does not serve is refused.
        const auto t = s::case_dir();
        for (std::string_view arch : {"x86_64", "aarch64"}) {
            fs::create_directories(t / std::format("{}-toy-sysroot", arch) / "usr/lib");
            s::project(t / std::format("{}-toy-sysroot", arch), {
                {"usr/include/c++/v1/toy_vector", "#pragma once\n"},
                {"usr/share/libc++/v1/std.cppm", "export module std;\n"},
                {"mcpp.toml", std::format("[package]\nname     = \"{}-toy-sysroot\"\nversion  = \"1.0\"\nprovides = [\"mcpp:c-abi=toy-libc\", \"mcpp:c++-abi=libc++\", \"mcpp:compiler-runtime=libgcc\",\n            \"hosted-standard-library\"]\n\n[build]\ninclude_dirs     = [\"usr/include/c++/v1\"]\nldflags          = [\"-lc++\", \"-lcxxrt\"]\nstd-module       = \"usr/share/libc++/v1/std.cppm\"\nstd-module-flags = [\"-DTOY_MODULE=1\"]\n", arch)}});
        }
        s::project(t / "plat", {{"mcpp.toml", std::format(R"([package]
name    = "toy-freebsd-platform"
version = "0.1.0"

[platform]
toolchain = "llvm@{}"

[[platform.provider]]
serves  = ["x86_64-freebsd"]
sysroot = {{ path = "../x86_64-toy-sysroot" }}
hosted  = true

[[platform.provider]]
serves  = ["aarch64-freebsd"]
sysroot = {{ path = "../aarch64-toy-sysroot" }}
hosted  = true

[platform.link]
driver = "compiler"

[platform.layers]
"kernel-abi"       = false
"c-abi"            = false
"c++-abi"          = false
"compiler-runtime" = false
)", s::llvm_version())}});
        s::project app(t / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n\n[target.'cfg(os = \"freebsd\")']\nplatform = { path = \"../plat\" }\n"},
                                   {"src/main.cpp", "int main() { return 0; }\n"}});
        auto r = app.mcpp({"why", "flags", "--target", "x86_64-freebsd"});
        auto out = s::text(r);
        expect(fatal(e2e::succeeded(r))) << "the plan over the base package failed: " << out;
        expect(fatal(s::has_line(out, R"(c\+\+-abi +libc\+\+ +\(x86_64-toy-sysroot@1\.0, prebuilt)"))) << "the C++ layer is not the base's: " << out;
        expect(fatal(s::has_line(out, R"(compiler-runtime +libgcc +\(x86_64-toy-sysroot@1\.0, prebuilt)"))) << "the compiler run-time is not the base's: " << out;
        expect(fatal(e2e::contains(out, std::format("-isystem{}", (t / "x86_64-toy-sysroot/usr/include/c++/v1").string())))) << "a unit lacks the base's C++ headers: " << out;
        expect(fatal(s::no_line(out, R"(xim-x-llvm/[^ ]*/include/c\+\+/v1)"))) << "a unit carries the payload's C++ headers: " << out;
        expect(fatal(e2e::contains(out, std::format("--sysroot={}", (t / "x86_64-toy-sysroot").string())))) << "the sysroot is not the base's root: " << out;
        expect(fatal(s::has_line(out, R"((^| )-nostdlib\+\+( |$))") && s::has_line(out, R"((^| )-lc\+\+( |$))") && s::has_line(out, R"((^| )-lcxxrt( |$))")))
            << "the link does not name the base's libraries: " << out;
        r = app.mcpp({"why", "flags", "--target", "aarch64-freebsd"});
        out = s::text(r);
        expect(fatal(e2e::succeeded(r))) << "the aarch64 plan failed: " << out;
        expect(fatal(e2e::contains(out, std::format("--sysroot={}", (t / "aarch64-toy-sysroot").string())))) << "aarch64 does not take its own base: " << out;
        expect(fatal(s::has_line(out, R"(c\+\+-abi +libc\+\+ +\(aarch64-toy-sysroot@1\.0, prebuilt)"))) << "aarch64's C++ layer is not its base's: " << out;
        r = app.mcpp({"why", "flags", "--target", "x86_64-freebsd-musl"});
        out = s::text(r);
        expect(fatal(!e2e::succeeded(r))) << "the musl row was served by a platform that does not serve it: " << out;
        expect(fatal(e2e::contains(out, "serves x86_64-freebsd, aarch64-freebsd and not x86_64-freebsd-musl"))) << "the refusal does not name what the platform serves: " << out;
    };
}};

}  // namespace

// A target the dependency graph supplies: `linkage = "dynamic"` is refused only when the C library
// is the graph's, a graph-supplied target closes the host's own header search, and a graph link
// searches no host directory (formerly tests/e2e/291, 738, 778).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

// The text contains `a`, and `b` after it (a shell `case` pattern `*a*b*`).
bool in_order(std::string_view text, std::string_view a, std::string_view b) {
    const auto at = text.find(a);
    return at != std::string_view::npos && text.find(b, at + a.size()) != std::string_view::npos;
}

// The directories a compiler's `-v` lists between "search starts here" and "End of search list".
std::vector<std::string> search_list(std::string_view err) {
    std::vector<std::string> out;
    bool capture = false;
    for (auto part : std::views::split(err, '\n')) {
        std::string_view l(part.begin(), part.end());
        if (l.contains("search starts here")) { capture = true; continue; }
        if (l.contains("End of search list")) { capture = false; continue; }
        if (!capture) continue;
        const auto first = l.find_first_not_of(" \t\r");
        const auto last = l.find_last_not_of(" \t\r");
        if (first != std::string_view::npos) out.emplace_back(l.substr(first, last - first + 1));
    }
    return out;
}

bool unreachable(std::string_view out) {
    return out.contains("not found in the synced index") || out.contains("install_packages failed");
}

E2E graph{"graph", [] {
    "291 dynamic linkage is refused only when the c library is the graphs"_test[s::gcc][s::llvm][s::elf][s::unix_shell] = [] {
        // `linkage = "dynamic"` is honoured without a word when the C library is the payload's
        // (the artifact has DT_NEEDED entries), and reported as having no effect when the graph
        // supplies the C library.
        const std::string target = "x86_64-linux-gnu";
        const auto tmp = s::case_dir();
        auto make_project = [&](std::string_view dir, std::string_view tc, std::string_view deps) {
            return s::project(tmp / dir, {{"mcpp.toml", std::format("[package]\nname    = \"linkprobe\"\nversion = \"0.1.0\"\n\n[toolchain]\ndefault = \"{}\"\n\n[target.{}]\nlinkage = \"dynamic\"\n\n[dependencies]\n{}\n", tc, target, deps)},
                                          {"src/main.cpp", "#include <cstdio>\nint main() { std::printf(\"ok\\n\"); }\n"}});
        };
        auto onplatform = make_project("onplatform", "gcc@16.1.0", "openkal-linux = \"0.5.4\"");
        auto r = onplatform.mcpp({"build", "--target", target});
        const auto out = s::text(r);
        if (!r) skip("the on-platform project did not build here");
        if (!in_order(out, "kernel-abi", "graph")) skip("the kernel interface did not come from the graph here");
        std::optional<fs::path> bin;
        for (const auto& f : s::find_files(onplatform.at("target"), "linkprobe"))
            if ((fs::status(f).permissions() & fs::perms::owner_exec) != fs::perms::none) { bin = f; break; }
        expect(fatal(bin.has_value())) << "FAIL: no artifact was produced";
        expect(fatal(s::lacks(out, "has no effect"))) << "FAIL: 'dynamic' was reported as ineffective while the payload's C library was in use";
        const auto needed = s::read_header(*bin).needed.size();
        expect(fatal(needed > 0u)) << "FAIL: the artifact is static, so the warning would have been right";
        std::println("  ok  'dynamic' was honoured — {} DT_NEEDED entries", needed);
        auto fullgraph = make_project("fullgraph", "llvm@" + s::llvm_version(), "openkal-musl = \"0.3.5\"\nopenkal-llvm-runtime = \"0.1.3\"");
        const auto out2 = s::text(fullgraph.mcpp({"build", "--target", target}));
        if (!in_order(out2, "c-abi", "graph")) skip("the C library did not come from the graph here");
        expect(fatal(e2e::contains(out2, "has no effect"))) << "FAIL: 'dynamic' cannot be honoured here and nothing said so: " << out2;
    };

    "738 a graph supplied target closes the hosts own search"_test[s::llvm][s::mingw_host_headers][s::python3] = [] {
        // Over the openkal windows-gnu stack, every unit's header search names only directories
        // of the store, though an unrestricted clang finds the host's separate MinGW headers.
        const auto llvm = s::llvm_version();
        s::options o{.env = {{"MCPP_COMPILE_COMMANDS", "compile_commands.json"}}};
        if (auto root_env = s::env("MCPP_E2E_HOST_MINGW_ROOT"); root_env && !root_env->empty()) {
            o.env.emplace_back("PATH", (fs::path(*root_env) / "bin").string() + ":" + s::env("PATH").value_or(""));
            // The control: an unprotected clang finds the separate host MinGW headers.
            const auto root = fs::weakly_canonical(*root_env);
            const auto home = fs::weakly_canonical(s::shared_home());
            const auto rel = root.lexically_relative(home);
            expect(fatal(rel.empty() || *rel.begin() == "..")) << "FAIL: the host MinGW fixture must be outside MCPP_HOME";
            const auto include = fs::weakly_canonical(root / "x86_64-w64-mingw32/include");
            expect(fatal(fs::is_regular_file(include / "io.h"))) << "FAIL: the separate host MinGW headers are absent";
            const auto clang = s::payload_root("xim-x-llvm", llvm) / "bin/clang";
            auto r = e2e::run({.argv = {clang.string(), "--no-default-config", "--target=x86_64-w64-windows-gnu", "-E", "-v", "-x", "c", "-"}, .input = "#include <io.h>\n"});
            bool found = false;
            for (const auto& d : search_list(r.err))
                if (fs::weakly_canonical(d) == include) found = true;
            expect(fatal(e2e::succeeded(r) && found)) << "FAIL: unrestricted Clang did not find the host MinGW fixture: " << r.err;
        }
        s::project p({{"mcpp.toml", std::format("[package]\nname    = \"hdriso\"\nversion = \"0.1.0\"\n\n[toolchain]\ndefault = \"llvm@{}\"\n\n[dependencies]\nopenkal-musl = \"0.3.5\"\nopenkal-windows = \"0.1.5\"\nopenkal-llvm-runtime = \"0.1.3\"\n", llvm)},
                      {"src/main.cpp", "#include <cstdio>\nint main() { std::puts(\"hi\"); return 0; }\n"},
                      {"src/probe.c", "int probe(void) { return 0; }\n"}});
        auto r = p.mcpp({"build", "--target", "x86_64-windows-gnu", "--cache", "off"}, o);
        const auto out = s::text(r);
        if (!r && unreachable(out)) skip("the openkal packages are not reachable from here");
        expect(fatal(e2e::succeeded(r))) << "FAIL: the openkal windows-gnu stack did not build: " << out;
        fs::path cdb = p.at("compile_commands.json");
        if (!fs::is_regular_file(cdb)) {
            cdb.clear();
            for (const auto& f : s::find_files(p.at("target"), R"(compile_commands\.json)"))
                if (!f.generic_string().contains("/.build-mcpp/")) { cdb = f; break; }
        }
        expect(fatal(!cdb.empty() && fs::is_regular_file(cdb))) << "FAIL: no compile_commands.json produced";
        const auto home = fs::weakly_canonical(s::shared_home()).string();
        const auto entries = s::json_file(cdb);
        std::vector<nlohmann::json> units;
        std::vector<std::string> files;
        for (const auto& e : entries) {
            const auto f = e.value("file", std::string());
            files.push_back(f);
            if (f.ends_with("main.cpp") || f.ends_with("probe.c")) units.push_back(e);
        }
        expect(fatal(units.size() == 2u)) << "FAIL: expected 2 units (main.cpp, probe.c), found " << units.size() << ": " << nlohmann::json(files).dump(1);
        for (const auto& e : units) {
            std::vector<std::string> args;
            if (e.contains("arguments") && e["arguments"].is_array() && !e["arguments"].empty()) {
                for (const auto& a : e["arguments"]) args.push_back(a.get<std::string>());
            } else {
                const auto command = e.value("command", std::string());
                for (auto part : std::views::split(std::string_view(command), ' ')) args.emplace_back(part.begin(), part.end());
            }
            args.push_back("-v");
            args.push_back("-fsyntax-only");
            auto run = e2e::run({.argv = args, .cwd = e.value("directory", std::string())});
            const auto searched = search_list(run.err);
            const auto file = e.value("file", std::string());
            expect(!searched.empty()) << "FAIL: " << file << ": clang printed no header search list (the assertion below would be vacuous): " << run.err;
            std::vector<std::string> outside;
            for (const auto& d : searched) {
                const auto real = fs::weakly_canonical(d).string();
                if (real.starts_with("/") && !real.starts_with(home)) outside.push_back(d);
            }
            std::println("{}: {} directories searched, {} outside the store", file, searched.size(), outside.size());
            expect(outside.empty()) << "FAIL: the search list names a directory outside the store: " << nlohmann::json(outside).dump();
        }
    };

    "778 a graph link searches no host directory"_test[s::llvm][s::elf][s::unix_shell] = [] {
        // A graph link's -lm is answered by openkal-musl's own archive (and runs, on x86_64 and
        // under an aarch64 emulator when there is one); a release that does not answer it fails
        // and names the one that does; a host directory in ldflags is refused by the hermetic
        // check, by name.
        const std::string target = "x86_64-linux-musl";
        const auto tmp = s::case_dir();
        const auto llvm = s::llvm_version();
        auto make_project = [&](std::string_view dir, std::string_view musl, std::string_view runtime, std::string_view ldflags) {
            return s::project(tmp / dir, {{"mcpp.toml", std::format("[package]\nname    = \"lmprobe\"\nversion = \"0.1.0\"\n\n[toolchain]\ndefault = \"llvm@{}\"\n\n[build]\nldflags = [{}]\n\n[targets.lmprobe]\nkind = \"bin\"\nmain = \"src/main.c\"\n\n[dependencies]\nopenkal-musl         = \"{}\"\nopenkal-llvm-runtime = \"{}\"\n", llvm, ldflags, musl, runtime)},
                                          {"src/main.c", "#include <math.h>\n#include <stdio.h>\nint main(void) {\n    volatile double a = 1.0, b = 2.0;\n    printf(\"%g\\n\", fmax(a, b));\n    return fmax(a, b) == 2.0 ? 0 : 1;\n}\n"}});
        };
        auto a = make_project("a", "0.19.2", "0.15.2", "\"-lm\"");
        auto r = a.mcpp({"build", "--target", target, "--verbose"});
        if (!r && unreachable(s::text(r))) skip("the openkal packages are not reachable from here");
        expect(fatal(e2e::succeeded(r))) << "FAIL: with openkal-musl 0.19.2, -lm must be answered by the graph: " << s::text(r);
        // Read from the graph the build wrote, not from its narration: a cold build prints some
        // thousand compile lines before the link, and the program running mcpp keeps 1 MiB of it.
        std::string graph;
        for (const auto& g : s::find_files(a.at("target"), R"(build\.ninja)"))
            if (!g.generic_string().contains("/.build-mcpp/")) graph += s::read_file(g);
        expect(fatal(!graph.empty())) << "FAIL: the build wrote no build.ninja under " << a.at("target");
        expect(fatal(s::has_line(graph, "--sysroot=[^ ]*graph-sysroot"))) << "FAIL: the graph link does not carry the empty sysroot";
        r = a.mcpp({"run", "--target", target});
        expect(fatal(e2e::succeeded(r))) << "FAIL: the program did not run: " << s::text(r);
        expect(fatal(s::has_line(s::text(r), "^2$"))) << "FAIL: unexpected output: " << s::text(r);
        auto b = make_project("b", "0.19.1", "0.15.1", "\"-lm\"");
        r = b.mcpp({"build", "--target", target});
        expect(fatal(!r)) << "FAIL: -lm was answered although nothing in the graph provides it: the host's library directories are still searched: " << s::text(r);
        if (unreachable(s::text(r))) skip("the openkal packages are not reachable from here");
        expect(fatal(e2e::contains(s::text(r), "unable to find library -lm"))) << "FAIL: the failure is not the unanswered -lm: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "openkal-musl 0.19.2"))) << "FAIL: the note does not name the release that answers -lm: " << s::text(r);
        auto c = make_project("c", "0.19.2", "0.15.2", "\"-lm\"");
        r = c.mcpp({"build", "--target", "aarch64-linux-musl"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: with openkal-musl 0.19.2, -lm must link on aarch64-linux-musl: " << s::text(r);
        std::optional<fs::path> bin;
        for (const auto& f : s::find_files(c.at("target"), "lmprobe"))
            if (f.generic_string().contains("aarch64")) { bin = f; break; }
        expect(fatal(bin.has_value())) << "FAIL: no aarch64 lmprobe was produced: " << s::text(r);
        auto runner = s::on_path("qemu-aarch64");
        if (!runner) runner = s::on_path("qemu-aarch64-static");
        if (!runner) {
            std::println("  SKIP  no aarch64 emulator here; the link was checked, not the run");
        } else {
            auto ran = s::run(*runner, {bin->string()});
            auto text = s::text(ran);
            while (text.ends_with('\n')) text.pop_back();
            expect(fatal(e2e::succeeded(ran))) << "FAIL: non-zero exit under " << runner->filename().string() << ": " << text;
            expect(fatal(text == "2")) << "FAIL: unexpected output under emulation: " << text;
        }
        auto d = make_project("d", "0.19.2", "0.15.2", "\"-L/usr/lib\", \"-lm\"");
        r = d.mcpp({"build", "--target", target});
        expect(fatal(!r)) << "FAIL: a graph link was allowed to search /usr/lib: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "hermetic link check failed"))) << "FAIL: the refusal is not the hermetic check's: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "/usr/lib"))) << "FAIL: the refusal does not name the directory: " << s::text(r);
    };
}};

}  // namespace

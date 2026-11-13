// The LLVM toolchain: a C/C++ package, import std, several modules, a per-file incremental rebuild,
// the BMI cache of a dependency, std.compat, a 16-byte atomic and libatomic, the hermetic link, and
// `standard = "c++fly"` on Clang (formerly tests/e2e/36, 37, 38, 39, 40, 41, 47, 86, 101).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

const s::options inherited{.where = s::home::inherited};

// `find <root> -type f -path '*/bin/<name>' | head -1`.
std::optional<fs::path> program_under(const fs::path& root, std::string_view name) {
    std::vector<fs::path> found;
    for (const auto& f : s::find_files(root, ".*"))
        if (f.parent_path().filename() == "bin" && f.filename() == name) found.push_back(f);
    if (found.empty()) return std::nullopt;
    return found.front();
}

bool executable(const fs::path& f) {
    std::error_code ec;
    return (fs::status(f, ec).permissions() & fs::perms::owner_exec) != fs::perms::none;
}

// `$(program)`: its standard output without the trailing newlines.
std::string output_of(const fs::path& program) {
    auto out = s::run(program).out;
    while (out.ends_with('\n') || out.ends_with('\r')) out.pop_back();
    return out;
}

// `find target -name build.ninja -not -path '*/.build-mcpp/*' | head -1`.
std::optional<fs::path> build_ninja(const fs::path& target) {
    for (const auto& f : s::find_files(target, R"(build\.ninja)"))
        if (!f.generic_string().contains("/.build-mcpp/")) return f;
    return std::nullopt;
}

std::vector<std::string> lines_of(std::string_view text) {
    std::vector<std::string> out;
    for (auto part : std::views::split(text, '\n')) out.emplace_back(part.begin(), part.end());
    return out;
}

fs::path llvm_root() { return s::payload_root("xim-x-llvm", s::llvm_version()); }

// The checks the scripts made before they began: an llvm payload with clang++ and, for import
// std, its libc++ module source.
void needs_llvm_payload(std::string_view module_source = "") {
    const auto root = llvm_root();
    if (!executable(root / s::exe("bin/clang++"))) skip(std::format("xlings llvm@{} is not installed", s::llvm_version()));
    if (!module_source.empty() && !fs::is_regular_file(root / "share/libc++/v1" / module_source))
        skip(std::format("xlings llvm@{} has no libc++ {}", s::llvm_version(), module_source));
}

s::project llvm_project(std::string_view name, s::files sources, std::string_view extra = "") {
    s::project p;
    p.write("mcpp.toml", std::format("[package]\nname    = \"{}\"\nversion = \"0.1.0\"\n\n[toolchain]\nlinux = \"llvm@{}\"\n{}", name, s::llvm_version(), extra));
    for (const auto& [path, text] : sources) p.write(path, text);
    return p;
}

E2E llvm{"llvm", [] {
    "36 llvm toolchain"_test = [] {
        // A non-module C/C++ package built with the LLVM payload: the list names it, the build
        // finishes, and the program runs.
        needs_llvm_payload();
        const auto llvm = s::llvm_version();
        const auto key = s::on_windows() ? "windows" : "linux";
        s::project p({{"mcpp.toml", std::format("[package]\nname    = \"hello_llvm\"\nversion = \"0.1.0\"\n\n[language]\nimport_std = false\n\n[toolchain]\n{} = \"llvm@{}\"\n", key, llvm)},
                      {"src/main.cpp", "#include <iostream>\n\nextern \"C\" int answer(void);\n\nint main() {\n    std::cout << \"llvm \" << answer() << \"\\n\";\n    return 0;\n}\n"},
                      {"src/answer.c", "int answer(void) {\n    return 42;\n}\n"}});
        auto r = p.mcpp({"toolchain", "list"}, inherited);
        expect(fatal(e2e::contains(s::text(r), "llvm " + llvm))) << "FAIL: toolchain list did not show installed llvm " << llvm << ": " << s::text(r);
        r = p.mcpp({"build", "--no-cache"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "FAIL: llvm build failed: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "Finished"))) << "FAIL: build did not finish: " << s::text(r);
        const auto binary = program_under(p.at("target"), s::exe("hello_llvm"));
        expect(fatal(binary.has_value() && executable(*binary))) << "FAIL: hello_llvm binary missing";
        const auto out = output_of(*binary);
        expect(fatal(out == "llvm 42")) << "FAIL: wrong runtime output: " << out;
    };

    "37 llvm import std"_test[tag{"requires:import-std-libcxx"}] = [] {
        // An import-std package built with the LLVM payload and its libc++.
        s::needs({"import-std-libcxx"});
        if (s::on_windows()) skip("libc++ std.cppm not available on Windows");
        needs_llvm_payload("std.cppm");
        auto p = llvm_project("hello_llvm_std", {{"src/main.cpp", "import std;\n\nint main() {\n    std::println(\"llvm std {}\", 23);\n    return 0;\n}\n"}});
        auto r = p.mcpp({"build", "--no-cache"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "FAIL: llvm import-std build failed: " << s::text(r);
        const auto binary = program_under(p.at("target"), "hello_llvm_std");
        expect(fatal(binary.has_value() && executable(*binary))) << "FAIL: hello_llvm_std binary missing";
        const auto out = output_of(*binary);
        expect(fatal(out == "llvm std 23")) << "FAIL: wrong runtime output: " << out;
    };

    "38 llvm modules"_test[tag{"requires:import-std-libcxx"}] = [] {
        // A multi-module project with Clang: its BMIs are in pcm.cache, never gcm.cache.
        s::needs({"import-std-libcxx"});
        if (s::on_windows()) skip("libc++ std.cppm not available on Windows");
        needs_llvm_payload("std.cppm");
        auto p = llvm_project("llvm_modules", {{"src/greet.cppm", "export module llvm_modules.greet;\nimport std;\n\nexport std::string greet(std::string_view name) {\n    return std::format(\"Hello, {}!\", name);\n}\n"},
                                               {"src/main.cpp", "import std;\nimport llvm_modules.greet;\n\nint main() {\n    std::println(\"{}\", greet(\"clang\"));\n    return 0;\n}\n"}});
        auto r = p.mcpp({"build", "--no-cache"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "FAIL: llvm multi-module build failed: " << s::text(r);
        const auto binary = program_under(p.at("target"), "llvm_modules");
        expect(fatal(binary.has_value() && executable(*binary))) << "FAIL: llvm_modules binary missing";
        const auto out = output_of(*binary);
        expect(fatal(out == "Hello, clang!")) << "FAIL: wrong runtime output: " << out;
        bool pcm = false, gcm = false;
        for (const auto& e : fs::recursive_directory_iterator(p.at("target"))) {
            if (!e.is_directory()) continue;
            if (e.path().filename() == "pcm.cache") pcm = true;
            if (e.path().filename() == "gcm.cache") gcm = true;
        }
        expect(fatal(pcm)) << "FAIL: pcm.cache/ directory not found (Clang should use pcm.cache)";
        expect(fatal(!gcm)) << "FAIL: gcm.cache/ directory found (Clang should NOT use gcm.cache)";
    };

    "39 llvm incremental"_test[tag{"requires:import-std-libcxx"}][tag{"requires:scan-deps"}] = [] {
        // A change of one module interface rebuilds it, and not the unit that only imports it
        // (clang-scan-deps dyndep).
        s::needs({"import-std-libcxx", "scan-deps"});
        needs_llvm_payload("std.cppm");
        auto p = llvm_project("llvm_inc", {{"src/greet.cppm", "export module llvm_inc.greet;\nimport std;\nexport std::string greet() { return \"hello\"; }\n"},
                                           {"src/main.cpp", "import std;\nimport llvm_inc.greet;\nint main() { std::println(\"{}\", greet()); }\n"}});
        auto r = p.mcpp({"build", "--no-cache"}, inherited);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        r = p.mcpp({"build"}, inherited);
        expect(fatal(e2e::succeeded(r) && e2e::contains(s::text(r), "Finished"))) << "FAIL: no-op rebuild failed: " << s::text(r);
        std::this_thread::sleep_for(std::chrono::seconds{1});
        fs::last_write_time(p.at("src/greet.cppm"), fs::file_time_type::clock::now());
        r = p.mcpp({"build", "--verbose"}, inherited);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        const auto out = s::text(r);
        expect(fatal(e2e::contains(out, "greet.cppm"))) << "FAIL: greet.cppm not rebuilt: " << out;
        if (e2e::contains(out, "MOD obj/main.o") || e2e::contains(out, "OBJ obj/main.o"))
            expect(fatal(s::no_line(out, "-c.*main.cpp"))) << "FAIL: main.cpp was recompiled when only greet.cppm changed";
    };

    "40 llvm bmi cache"_test[tag{"requires:import-std-libcxx"}] = [] {
        // A dependency's BMIs are cached: the second cold build stages them, has no compile edge
        // for the dependency, and sequences the staged artifacts before compilation.
        s::needs({"import-std-libcxx"});
        if (s::on_windows()) skip("libc++ std.cppm not available on Windows");
        needs_llvm_payload("std.cppm");
        const auto home = s::case_home(s::home::inherited);
        const auto user_index = fs::path(s::env("HOME").value_or("")) / ".mcpp/indices/mcpplibs";
        if (fs::is_directory(user_index) && !fs::exists(fs::symlink_status(home / "indices/mcpplibs"))) {
            fs::create_directories(home / "indices");
            std::error_code ec;
            fs::create_directory_symlink(user_index, home / "indices/mcpplibs", ec);
            if (ec) fs::copy(user_index, home / "indices/mcpplibs", fs::copy_options::recursive);
        }
        auto p = llvm_project("llvm_cache", {{"src/main.cpp", "import std;\nimport mcpplibs.cmdline;\n\nint main() {\n    std::println(\"cache test ok\");\n    return 0;\n}\n"}},
                              "\n[dependencies]\n\"mcpplibs.cmdline\" = \"0.0.1\"\n");
        auto r = p.mcpp({"build", "-v"}, inherited);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(s::has_line(s::text(r), "Compiling.*mcpplibs.cmdline"))) << "FAIL: mcpplibs.cmdline not compiled in the first (cold) build";
        p.remove("target");
        r = p.mcpp({"build", "-v"}, inherited);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(s::has_line(s::text(r), R"(Cached mcpplibs\.cmdline v[0-9.]+ \([0-9]+ units?\))"))) << "FAIL: mcpplibs.cmdline not cached on second build";
        const auto ninja = build_ninja(p.at("target"));
        expect(fatal(ninja.has_value())) << "FAIL: no build.ninja";
        const auto text = s::read_file(*ninja);
        expect(fatal(s::no_line(text, ": (cxx_module|cxx_object|cxx_scan) .*mcpplibs"))) << "FAIL: cached dependency still has compile edges";
        expect(fatal(e2e::contains(text, "_mcpp_staged_cache"))) << "FAIL: staged artifacts are not sequenced before compilation";
        r = p.mcpp({"run"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "FAIL: run: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "cache test ok"))) << "FAIL: run output: " << s::text(r);
    };

    "41 llvm std compat"_test[tag{"requires:import-std-libcxx"}] = [] {
        // `import std.compat` with Clang gives the C library's functions.
        s::needs({"import-std-libcxx"});
        if (s::on_windows()) skip("libc++ std.compat.cppm not available on Windows");
        needs_llvm_payload("std.compat.cppm");
        auto p = llvm_project("compat_test", {{"src/main.cpp", "import std.compat;\n\nint main() {\n    // std.compat provides C stdlib functions like printf\n    printf(\"compat %d\\n\", 42);\n    return 0;\n}\n"}});
        auto r = p.mcpp({"build", "--no-cache"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "FAIL: std.compat build failed: " << s::text(r);
        const auto binary = program_under(p.at("target"), "compat_test");
        expect(fatal(binary.has_value() && executable(*binary))) << "FAIL: compat_test binary missing";
        const auto out = output_of(*binary);
        expect(fatal(out == "compat 42")) << "FAIL: wrong output: " << out;
    };

    "47 llvm atomic link"_test[tag{"requires:import-std-libcxx"}] = [] {
        // A 16-byte std::atomic lowers to __atomic_* libcalls that libatomic answers: the program
        // links and records DT_NEEDED libatomic.so; one without such atomics does not (--as-needed).
        s::needs({"import-std-libcxx"});
        if (s::on_windows()) skip("libatomic/-latomic path is Linux-only");
        if (s::on_macos()) skip("libatomic is a GNU/Linux runtime; not applicable on macOS");
        needs_llvm_payload();
        const auto lib = llvm_root() / "lib/x86_64-unknown-linux-gnu";
        if (!fs::exists(lib / "libatomic.so") && !fs::exists(lib / "libatomic.a"))
            skip(std::format("llvm@{} package ships no link-resolvable libatomic", s::llvm_version()));
        auto p = llvm_project("atomic_link", {{"src/main.cpp", R"(#include <atomic>
#include <cstdio>
struct alignas(16) Big { long long a, b; };
std::atomic<Big> g{{0, 0}};
int main() {
    g.store(Big{1, 2});
    Big r = g.load();
    Big e{1, 2}, d{9, 9};
    bool ok = g.compare_exchange_strong(e, d);
    std::printf("atomic %lld %lld %d\n", r.a, r.b, (int)ok);
    return 0;
}
)"}});
        auto r = p.mcpp({"build", "--no-cache"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "FAIL: 16-byte atomic build failed (libatomic not linked?): " << s::text(r);
        auto binary = program_under(p.at("target"), "atomic_link");
        expect(fatal(binary.has_value() && executable(*binary))) << "FAIL: atomic_link binary missing";
        const auto out = output_of(*binary);
        expect(fatal(out == "atomic 1 2 1")) << "FAIL: wrong runtime output: " << out;
        auto needs_libatomic = [](const fs::path& f) {
            return std::ranges::any_of(s::read_header(f).needed, [](const std::string& n) { return n.contains("libatomic.so"); });
        };
        expect(fatal(needs_libatomic(*binary))) << "FAIL: atomic binary missing libatomic NEEDED";
        p.write("src/main.cpp", "#include <cstdio>\nint main() { std::printf(\"noatomic\\n\"); return 0; }\n");
        r = p.mcpp({"build", "--no-cache"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "FAIL: non-atomic rebuild failed: " << s::text(r);
        binary = program_under(p.at("target"), "atomic_link");
        expect(fatal(binary.has_value())) << "FAIL: atomic_link binary missing after the rebuild";
        expect(fatal(!needs_libatomic(*binary))) << "FAIL: non-atomic binary should not depend on libatomic (--as-needed)";
    };

    "86 llvm hermetic link"_test[tag{"requires:import-std-libcxx"}] = [] {
        // The link flags carry the CRT discovery prefix and the build records a hermetic verdict;
        // the driver, given those flags, resolves every CRT object and the dynamic linker inside
        // the payload store (#195).
        s::needs({"import-std-libcxx"});
        if (!s::on_linux()) skip("hermetic CRT/loader model is Linux-only");
        needs_llvm_payload("std.cppm");
        auto p = llvm_project("hermetic_hello", {{"src/main.cpp", "import std;\n\nint main() {\n    std::println(\"hermetic {}\", 195);\n    return 0;\n}\n"}});
        auto r = p.mcpp({"build", "--no-cache"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "FAIL: llvm build failed: " << s::text(r);
        const auto ninja = build_ninja(p.at("target"));
        expect(fatal(ninja.has_value())) << "no build.ninja";
        // `grep '^ldflags' | sed 's/^ldflags *= *//'`: every such line, its key removed.
        std::string ldflags;
        static const std::regex key{"^ldflags *= *"};
        for (const auto& l : lines_of(s::read_file(*ninja)))
            if (l.starts_with("ldflags")) ldflags += (ldflags.empty() ? "" : "\n") + std::regex_replace(l, key, "");
        expect(fatal(e2e::contains(ldflags, " -B"))) << "FAIL: link flags carry no -B (CRT discovery prefix): " << ldflags;
        expect(fatal(fs::exists(ninja->parent_path() / ".mcpp-hermetic-ok"))) << "FAIL: build did not record a hermetic-link verdict";
        // The words of the flags, ninja's `$x` read as `x`, given to the payload's driver.
        std::vector<std::string> argv{(llvm_root() / "bin/clang++").string()};
        static const std::regex escaped{R"(\$(.))"};
        std::istringstream words(std::regex_replace(ldflags, escaped, "$1"));
        for (std::string w; words >> w;) argv.push_back(w);
        for (std::string w : {"-###", "-x", "c++", "/dev/null", "-o", "/dev/null"}) argv.push_back(w);
        const auto dry = s::text(e2e::run({.argv = argv}));
        std::vector<std::string> tokens;
        for (auto part : std::views::split(std::string_view(dry), ' ')) {
            std::string t(part.begin(), part.end());
            std::erase(t, '"');
            for (auto line : std::views::split(std::string_view(t), '\n')) tokens.emplace_back(line.begin(), line.end());
        }
        static const std::regex crt{R"((^|/)(S|g|r|M)?crt[1in]\.o$)"};
        std::vector<std::string> crt_tokens;
        for (const auto& t : tokens)
            if (std::regex_search(t, crt) && !t.contains("clang_rt")) crt_tokens.push_back(t);
        expect(fatal(!crt_tokens.empty())) << "FAIL: -### output contains no CRT objects to check";
        for (const auto& t : crt_tokens) {
            if (t.contains("/xpkgs/")) continue;
            expect(fatal(false)) << (t.starts_with("/") ? "FAIL: CRT resolves outside the sandbox: " : "FAIL: bare CRT name reached the linker: ") << t;
        }
        std::string loader;
        for (const auto& t : tokens)
            if (const auto at = t.find("--dynamic-linker="); at != std::string::npos) {
                const auto value = t.substr(at + std::string_view("--dynamic-linker=").size());
                loader = value.substr(0, value.find('='));
            }
        if (loader.empty())
            for (std::size_t i = 0; i + 1 < tokens.size(); ++i)
                if (tokens[i] == "-dynamic-linker") loader = tokens[i + 1];
        expect(fatal(loader.contains("/xpkgs/"))) << "FAIL: effective dynamic linker outside the sandbox: '" << loader << "'";
        const auto binary = program_under(p.at("target"), "hermetic_hello");
        expect(fatal(binary.has_value())) << "no hermetic_hello";
        const auto out = output_of(*binary);
        expect(fatal(out == "hermetic 195")) << "FAIL: wrong runtime output: " << out;
    };

    "101 cppfly llvm soft"_test = [] {
        // `standard = "c++fly"` on Clang builds at the latest level and reports the gates it
        // skipped; -fexperimental-library is on the command line exactly when the summary says it
        // is enabled.
        if (!fs::is_directory(llvm_root())) skip(std::format("xlings llvm@{} is not installed", s::llvm_version()));
        s::project p({{"mcpp.toml", std::format("[package]\nname     = \"flysoft\"\nversion  = \"0.1.0\"\nstandard = \"c++fly\"\n\n[toolchain]\ndefault = \"llvm@{}\"\n", s::llvm_version())},
                      {"src/main.cpp", "import std;\nint main() { std::println(\"fly soft ok\"); }\n"}});
        auto r = p.mcpp({"build", "--no-cache"}, inherited);
        const auto log = s::text(r);
        expect(fatal(e2e::succeeded(r))) << "FAIL: c++fly soft-path build failed on llvm: " << log;
        expect(fatal(e2e::contains(log, "c++fly on clang"))) << "FAIL: build log missing c++fly summary line: " << log;
        expect(fatal(s::has_line(log, "skipped: .*reflection"))) << "FAIL: summary does not report reflection as skipped on clang";
        const auto ninja = s::read_file(build_ninja(p.at("target")).value_or(fs::path()));
        expect(fatal(s::has_line(ninja, R"(^cxxflags  = -std=c\+\+2c)"))) << "FAIL: clang c++fly did not resolve to -std=c++2c";
        if (e2e::contains(log, "experimental-library (-fexperimental-library)")) {
            expect(fatal(e2e::contains(ninja, "-fexperimental-library"))) << "FAIL: summary claims experimental-library but build.ninja lacks the flag";
        } else {
            expect(fatal(s::has_line(log, "skipped: .*experimental-library"))) << "FAIL: experimental-library neither enabled nor reported skipped: " << log;
            expect(fatal(s::lacks(ninja, "-fexperimental-library"))) << "FAIL: -fexperimental-library emitted for a non-libc++ stdlib";
        }
        auto binary = program_under(p.at("target"), "flysoft");
        if (!binary) binary = program_under(p.at("target"), "flysoft.exe");
        expect(fatal(binary.has_value())) << "no flysoft";
        const auto out = output_of(*binary);
        expect(fatal(e2e::contains(out, "fly soft ok"))) << "FAIL: runtime output: " << out;
    };
}};

}  // namespace

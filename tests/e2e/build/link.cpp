// Links: response files one input per line, a link past the command-line ceiling, the C
// driver for C-only units, link flags as written, a relink never seen half-written, a prebuilt
// archive as a link input, a response file as one, link flags a compiler link does not use
// (formerly tests/e2e/190, 191, 238, 795, 938, 991, 1006, 1062).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

// The first build.ninja of the project's target, outside the build program's tree.
std::optional<fs::path> graph_of(const s::project& p) {
    for (const auto& f : s::find_files(p.at("target"), R"(build\.ninja)"))
        if (f.generic_string().find("/.build-mcpp/") == std::string::npos) return f;
    return std::nullopt;
}

// The text of the project's build.ninja (the case fails when there is none).
std::string graph_text(const s::project& p) {
    const auto g = graph_of(p);
    expect(fatal(g.has_value())) << "no build.ninja generated";
    return s::read_file(*g);
}

// The entries of `dir`, sorted (`ls -d <dir>/*`).
std::vector<fs::path> entries(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (fs::is_directory(dir, ec))
        for (const auto& e : fs::directory_iterator(dir)) out.push_back(e.path());
    std::ranges::sort(out);
    return out;
}

// `[ -x file ]`.
bool executable(const fs::path& f) {
    std::error_code ec;
    return fs::is_regular_file(f, ec) && (fs::status(f, ec).permissions() & fs::perms::owner_exec) != fs::perms::none;
}

// A program's standard output without its trailing newline (`$(program)`).
std::string output_of(const fs::path& program) {
    auto out = s::run(program).out;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

// The lines of `text` (`while read line`).
std::vector<std::string> lines_of(std::string_view text) {
    std::vector<std::string> out;
    std::istringstream in{std::string(text)};
    for (std::string l; std::getline(in, l);) out.push_back(l);
    return out;
}

// A file's modification time moved past its present one (`sleep 1; touch file`).
void touch(const fs::path& f) { fs::last_write_time(f, fs::last_write_time(f) + std::chrono::seconds(2)); }

// The first file under `root` whose path ends with `tail` (`find <root> -path '*<tail>'`).
std::optional<fs::path> path_ending(const fs::path& root, std::string_view tail) {
    for (const auto& f : s::find_files(root, ".*"))
        if (f.generic_string().ends_with(tail)) return f;
    return std::nullopt;
}

// `rm bin/<name>; ninja -d keeprsp bin/<name>` in a build directory: the response file it kept.
fs::path relink_keeping_rsp(const fs::path& bdir, std::string_view name) {
    std::string bin;
    for (const auto& e : entries(bdir / "bin"))
        if (bin.empty() && (e.filename() == name || e.filename() == std::string(name) + ".exe")) bin = "bin/" + e.filename().string();
    expect(fatal(!bin.empty())) << "nothing was linked";
    fs::remove(bdir / bin);
    auto r = e2e::run({.argv = {"ninja", "-d", "keeprsp", bin}, .cwd = bdir});
    expect(fatal(e2e::succeeded(r))) << "relink under -d keeprsp\n" << s::text(r);
    const auto rsp = s::find_files(bdir, R"(.*\.rsp)");
    expect(fatal(!rsp.empty())) << "-d keeprsp left no response file";
    return rsp.front();
}

// The length of the longest line of a file.
std::size_t longest_line(const fs::path& f) {
    std::size_t longest = 0;
    for (const auto& l : lines_of(s::read_file(f))) longest = std::max(longest, l.size());
    return longest;
}

E2E link{"link", [] {
    "190 link rspfile newlines"_test = [] {
        // Link rules write their response file one input per line ($in_newline), and a relink
        // kept with -d keeprsp holds the objects one per line.
        s::project p{s::files{{"mcpp.toml", "[package]\nname    = \"multi\"\nversion = \"0.1.0\"\n"}, {"src/main.cpp", "int main() { return 0; }\n"}}};
        for (int i = 1; i <= 24; ++i) p.write(std::format("src/f{}.cpp", i), std::format("int f{}() {{ return {}; }}\n", i, i));
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "build\n" << s::text(r);
        const auto ninja = graph_of(p);
        expect(fatal(ninja.has_value())) << "no build.ninja";
        const auto g = s::read_file(*ninja);
        expect(fatal(s::no_line(g, "^[[:space:]]*rspfile_content = (\xef\xbb\xbf)?\\$in[[:space:]]*$"))) << "a link rule still writes its response file on ONE line ($in)";
        expect(fatal(s::has_line(g, "^[[:space:]]*rspfile_content = (\xef\xbb\xbf)?\\$in_newline[[:space:]]*$"))) << "no link rule uses $in_newline";
        const auto rsp = relink_keeping_rsp(ninja->parent_path(), "multi");
        const auto lines = s::count_matching(s::read_file(rsp), "") - 1;
        expect(fatal(lines >= 20u)) << "response file has " << lines << " newline(s): objects are not one-per-line — the LNK1170 shape is back";
        expect(longest_line(rsp) < 4096u) << "longest response-file line is " << longest_line(rsp) << " chars";
    };

    "191 link scale"_test = [] {
        // A link of 1401 objects whose list passes the 128 KiB command-line ceiling links through
        // a response file of one object per line, and the program runs.
        constexpr std::size_t ceiling = 128 * 1024;
        constexpr int n = 1400;
        const std::string pad(88, 'x');
        s::project p{s::files{{"mcpp.toml", "[package]\nname    = \"scale\"\nversion = \"0.1.0\"\n\n[build]\nc_standard = \"c11\"\n"}}};
        for (int i = 1; i <= n; ++i) p.write(std::format("src/f{}_{}.c", i, pad), std::format("int f{:04}(void) {{ return {}; }}\n", i, i));
        p.write("src/main.cpp", std::format("extern \"C\" int f0001(void);\nextern \"C\" int f{0:04}(void);\nint main() {{ return (f0001() == 1 && f{0:04}() == {0}) ? 0 : 1; }}\n", n));
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "build " << n << " objects\n" << s::text(r);
        const auto ninja = graph_of(p);
        expect(fatal(ninja.has_value())) << "no build.ninja";
        const auto rsp = relink_keeping_rsp(ninja->parent_path(), "scale");
        const auto bytes = fs::file_size(rsp);
        expect(fatal(bytes > ceiling)) << "response file is " << bytes << " bytes; the ceiling is " << ceiling << ": this test no longer reaches the regime it exists to cover";
        expect(fatal(longest_line(rsp) < 4096u)) << "longest response-file line is " << longest_line(rsp) << " chars — objects are not one per line";
        expect(e2e::succeeded(s::run(ninja->parent_path() / "bin" / s::exe("scale")))) << "linked binary did not run cleanly";
    };

    "238 c only unit links with c driver"_test[s::elf] = [] {
        // A link unit of C objects only links with the C driver (c_shared: NEEDED libc and not
        // libstdc++, libm or libgcc_s, its symbol exported); one C++ object makes it a C++ link
        // (cxx_shared); a C++ program links with cxx_link and runs; c_ldflags is defined.
        const auto tmp = s::case_dir();
        const auto llvm = s::llvm_version();
        const auto tc = std::format("[toolchain]\ndefault = \"gcc@16.1.0\"\nmacos   = \"llvm@{0}\"\nwindows = \"llvm@{0}\"\n", llvm);
        s::project purec{tmp / "purec", {{"mcpp.toml", "[package]\nname = \"purec\"\nversion = \"0.1.0\"\n\n" + tc + "\n[targets.purec]\nkind = \"shared\"\n"}, {"src/purec.c", R"cpp(int purec_add(int a, int b) { return a + b; }
)cpp"}}};
        auto r = purec.mcpp({"build", "--release"});
        expect(fatal(e2e::succeeded(r))) << "the pure-C project did not build\n" << s::text(r);
        auto first_graph = [](const s::project& p) { const auto g = s::find_files(p.at("target"), R"(build\.ninja)"); expect(fatal(!g.empty())) << "no build.ninja"; return s::read_file(g.front()); };
        expect(fatal(s::has_line(first_graph(purec), R"(^build [^:]*libpurec\.so[^:]*: c_shared )"))) << "the pure-C shared library is not linked by the C rule";
        const auto so = s::find_files(purec.at("target"), R"(libpurec\.so.*)");
        expect(fatal(!so.empty())) << "no libpurec.so produced";
        const auto needed = s::read_header(so.front()).needed;
        for (std::string_view lib : {"libstdc++.so.6", "libm.so.6", "libgcc_s.so.1"})
            expect(fatal(!std::ranges::contains(needed, std::string(lib)))) << "the pure-C library still depends on " << lib;
        expect(fatal(std::ranges::contains(needed, std::string("libc.so.6")))) << "the pure-C library lost its dependency on libc itself — the C driver was not given the link flags it needs";
        expect(fatal(std::ranges::contains(s::elf_symbols(so.front(), ".dynsym", true), std::string("purec_add")))) << "purec_add is no longer exported";
        s::project mixed{tmp / "mixed", {{"mcpp.toml", "[package]\nname = \"mixed\"\nversion = \"0.1.0\"\n\n" + tc + "\n[targets.mixed]\nkind = \"shared\"\n"}, {"src/mixed.c", R"cpp(int mixed_c(int a) { return a * 2; }
)cpp"}, {"src/mixed_cxx.cpp", R"cpp(#include <string>
extern "C" int mixed_cxx_len(const char* s) {
    return static_cast<int>(std::string(s).size());
}
)cpp"}}};
        r = mixed.mcpp({"build", "--release"});
        expect(fatal(e2e::succeeded(r))) << "the mixed project did not build\n" << s::text(r);
        expect(fatal(s::has_line(first_graph(mixed), R"(^build [^:]*libmixed\.so[^:]*: cxx_shared )"))) << "a unit containing a C++ translation unit was linked by the C driver";
        s::project cxx{tmp / "cxx", {{"mcpp.toml", "[package]\nname = \"cxxbin\"\nversion = \"0.1.0\"\n\n" + tc}, {"src/main.cpp", R"cpp(import std;
int main() { std::println("cdriver-ok"); return 0; }
)cpp"}}};
        r = cxx.mcpp({"build", "--release"});
        expect(fatal(e2e::succeeded(r))) << "the C++ project did not build\n" << s::text(r);
        const auto g = first_graph(cxx);
        expect(fatal(s::has_line(g, R"(^build [^:]*bin/cxxbin[^:]*: cxx_link )"))) << "the C++ binary is no longer linked by the C++ rule";
        const auto bin = path_ending(cxx.at("target"), "/bin/cxxbin");
        expect(fatal(bin.has_value())) << "no bin/cxxbin";
        auto run = s::run(*bin);
        expect(fatal(e2e::succeeded(run))) << "the C++ binary exited " << run.status;
        expect(fatal(output_of(*bin) == "cdriver-ok")) << "binary printed '" << output_of(*bin) << "'";
        expect(s::has_line(g, "^c_ldflags ")) << "c_ldflags is not defined, but the C rules reference it";
    };
    "795 a link flag reaches the linker as written"_test[s::elf][s::unix_shell] = [] {
        // $ORIGIN in [build] ldflags, in mcpp::link_flag and in a dependency's ldflags reaches the
        // run path unexpanded; a search directory with a space is one argument; an element
        // escaped by hand reads as written and the first plan (only) says what the linker gets.
        const auto tmp = s::case_dir();
        auto program = [](const s::project& p, std::string_view name) {
            for (const auto& f : s::find_files(p.at("target"), std::string(name)))
                if (f.parent_path().filename() == "bin" || f.generic_string().find("/bin/") != std::string::npos) return f;
            expect(fatal(false)) << "no program was produced";
            return fs::path{};
        };
        auto expect_origin = [](const fs::path& bin, std::string_view rest) {
            std::vector<std::string> entries;
            for (auto part : std::views::split(s::read_header(bin).runpath, ':')) entries.emplace_back(part.begin(), part.end());
            expect(fatal(std::ranges::contains(entries, "$ORIGIN/" + std::string(rest)))) << "$ORIGIN/" << rest << " is not in the run path: " << s::read_header(bin).runpath;
            expect(fatal(!std::ranges::contains(entries, "/" + std::string(rest)))) << "the run path holds /" << rest << ": $ORIGIN was expanded by the shell";
        };
        const std::string main_cpp = R"cpp(#include <cstdio>
int main() { std::printf("LINK_FLAG_OK\n"); }
)cpp";
        s::project a{tmp / "a", {{"src/main.cpp", main_cpp}, {"mcpp.toml", R"cpp([package]
name    = "ldorigin"
version = "0.1.0"

[build]
ldflags = ["-Wl,-rpath,$ORIGIN/../lib"]
)cpp"}}};
        auto r = a.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "the build failed\n" << s::text(r);
        expect_origin(program(a, "ldorigin"), "../lib");
        s::project b{tmp / "b", {{"src/main.cpp", main_cpp}, {"mcpp.toml", R"cpp([package]
name    = "linkflagorigin"
version = "0.1.0"
)cpp"}, {"build.mcpp", R"cpp(import mcpp;
int main() {
    mcpp::link_flag("-Wl,-rpath,$ORIGIN/../plugins");
}
)cpp"}}};
        r = b.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "the build failed\n" << s::text(r);
        expect_origin(program(b, "linkflagorigin"), "../plugins");
        s::project{tmp / "c/dep", {{"mcpp.toml", R"cpp([package]
name    = "originlib"
version = "0.1.0"

[build]
ldflags = ["-Wl,-rpath,$ORIGIN/../dep"]

[targets.originlib]
kind = "lib"
)cpp"}, {"src/originlib.cppm", R"cpp(export module originlib;
export int originlib_value() { return 7; }
)cpp"}}};
        s::project c{tmp / "c/app", {{"src/main.cpp", R"cpp(#include <cstdio>
import originlib;
int main() { std::printf("LINK_FLAG_OK %d\n", originlib_value()); }
)cpp"}, {"mcpp.toml", R"cpp([package]
name    = "originapp"
version = "0.1.0"

[dependencies]
originlib = { path = "../dep" }
)cpp"}}};
        r = c.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "the build failed\n" << s::text(r);
        expect_origin(program(c, "originapp"), "../dep");
        s::project d{tmp / "d", {{"src/main.cpp", main_cpp}, {"mcpp.toml", R"cpp([package]
name    = "spacedsearch"
version = "0.1.0"
)cpp"}, {"build.mcpp", R"cpp(import mcpp;
int main() {
    mcpp::link_search("my libs");
}
)cpp"}}};
        fs::create_directories(d.at("my libs"));
        r = d.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "a search directory whose path holds a space broke the link\n" << s::text(r);
        r = d.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "the program did not run: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "LINK_FLAG_OK"))) << "unexpected output: " << s::text(r);
        s::project e{tmp / "e", {{"src/main.cpp", main_cpp}, {"mcpp.toml", R"cpp([package]
name    = "handescaped"
version = "0.1.0"

[build]
ldflags = ["-Wl,-rpath='$$ORIGIN/../x'"]
)cpp"}}};
        r = e.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "the build failed\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "reaches the linker as ['-Wl,-rpath=$$ORIGIN/../x']"))) << "the first plan does not say what the linker receives now\n" << s::text(r);
        r = e.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "the second build failed\n" << s::text(r);
        expect(s::lacks(s::text(r), "reaches the linker")) << "a build that repeats the plan repeated the note";
    };
    "938 a relink never exposes a partial file"_test = [] {
        // The link rule publishes through a temporary name, so a program started over and over
        // while it is relinked twelve times never meets a partially written file (POSIX).
        if (!s::on_linux() && !s::on_macos()) skip("POSIX publication");
        auto p = s::project::scaffold("app");
        p.write("src/main.cpp", "int main() { return 0; }\n");
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "the build failed\n" << s::text(r);
        const auto ninjas = s::find_files(p.at("target"), R"(build\.ninja)");
        expect(fatal(!ninjas.empty())) << "no build.ninja";
        const auto lines = lines_of(s::read_file(ninjas.front()));
        bool published = false;
        for (std::size_t i = 0; i + 1 < lines.size(); ++i)
            if (lines[i].starts_with("rule cxx_link"))
                published = published || std::regex_search(lines[i + 1], std::regex{R"(\$out\.tmp.*&& mv -f \$out\.tmp \$out)"});
        expect(fatal(published)) << "A: the link rule does not publish through a temporary name";
        const auto exe = path_ending(p.at("target"), "/bin/app");
        expect(fatal(exe.has_value())) << "no bin/app";
        std::atomic<bool> stop{false};
        std::mutex m;
        std::string errors;
        std::jthread starter([&] {
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            while (!stop && std::chrono::steady_clock::now() < end) {
                auto x = s::run(*exe);
                std::scoped_lock lock{m};
                errors += x.err;
                if (!e2e::succeeded(x)) errors += std::format("exec failed: {}\n", x.status);
            }
        });
        for (int i = 1; i <= 12; ++i) {
            p.write("src/main.cpp", std::format("int main() {{ volatile int x = {0}; return x - {0}; }}\n", i));
            r = p.mcpp({"build"});
            if (!e2e::succeeded(r)) { stop = true; starter.join(); expect(fatal(false)) << "B: relink " << i << " failed\n" << s::text(r); }
        }
        stop = true;
        starter.join();
        expect(errors.empty()) << "B: a start met a partially written program\n" << errors;
    };

    "991 a prebuilt archive in ldflags is a link input"_test[s::gcc][s::unix_shell] = [] {
        // An archive named in [build] ldflags is a link input: replacing it relinks the program,
        // and a build with nothing changed links nothing.
        const auto work = s::case_dir();
        s::project val{work / "val", {{"mcpp.toml", R"cpp([package]
name    = "val"
version = "0.1.0"

[targets.val]
kind = "lib"
)cpp"}}};
        fs::create_directories(work / "prebuilt");
        auto make_archive = [&](int value) {
            val.write("src/val.cpp", std::format("extern \"C\" int val() {{ return {}; }}\n", value));
            auto b = val.mcpp({"build"});
            expect(fatal(e2e::succeeded(b))) << "the library did not build\n" << s::text(b);
            const auto a = s::find_files(val.at("target"), R"(libval.*\.a)");
            expect(fatal(!a.empty())) << "no archive of the library";
            fs::copy_file(a.front(), work / "prebuilt/libval.a", fs::copy_options::overwrite_existing);
        };
        make_archive(41);
        const auto work_s = work.string();
        s::project app{work / "app", {{"mcpp.toml", std::format(R"cpp([package]
name    = "app"
version = "0.1.0"

[build]
ldflags = ["{0}/prebuilt/libval.a"]

[targets.app]
kind = "bin"
main = "src/main.cpp"
)cpp", work_s)}, {"src/main.cpp", R"cpp(import std;
extern "C" int val();
int main() { std::println("val {}", val()); }
)cpp"}}};
        auto r = app.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "the first run\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "val 41"))) << "the first run did not print val 41\n" << s::text(r);
        make_archive(42);
        r = app.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "the second run\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "val 42"))) << "a replaced archive named in [build] ldflags did not relink the program\n" << s::text(r);
        r = app.mcpp({"build", "-v"});
        expect(fatal(e2e::succeeded(r))) << "the third build\n" << s::text(r);
        expect(s::no_line(s::text(r), R"(^\[[0-9]+/[0-9]+\] .*bin/app)")) << "a build with nothing changed linked the program again\n" << s::text(r);
    };
    "1006 a response file and what it names are link inputs"_test[s::gcc][s::unix_shell] = [] {
        // A response file in [build] ldflags and the archive it names are link inputs:
        // replacing the archive relinks the program, and a build with nothing changed links nothing.
        const auto work = s::case_dir();
        s::project val{work / "val", {{"mcpp.toml", R"cpp([package]
name    = "val"
version = "0.1.0"

[targets.val]
kind = "lib"
)cpp"}}};
        fs::create_directories(work / "prebuilt");
        auto make_archive = [&](int value) {
            val.write("src/val.cpp", std::format("extern \"C\" int val() {{ return {}; }}\n", value));
            auto b = val.mcpp({"build"});
            expect(fatal(e2e::succeeded(b))) << "the library did not build\n" << s::text(b);
            const auto a = s::find_files(val.at("target"), R"(libval.*\.a)");
            expect(fatal(!a.empty())) << "no archive of the library";
            fs::copy_file(a.front(), work / "prebuilt/libval.a", fs::copy_options::overwrite_existing);
        };
        make_archive(41);
        s::project{work / "prebuilt", {{"libs.rsp", std::format("\"{}\"\n", (work / "prebuilt/libval.a").string())}}};
        const auto work_s = work.string();
        s::project app{work / "app", {{"mcpp.toml", std::format(R"cpp([package]
name    = "app"
version = "0.1.0"

[build]
ldflags = ["@{0}/prebuilt/libs.rsp"]

[targets.app]
kind = "bin"
main = "src/main.cpp"
)cpp", work_s)}, {"src/main.cpp", R"cpp(import std;
extern "C" int val();
int main() { std::println("val {}", val()); }
)cpp"}}};
        auto r = app.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "the first run\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "val 41"))) << "the first run did not print val 41\n" << s::text(r);
        make_archive(42);
        r = app.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "the second run\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "val 42"))) << "a replaced archive a response file names did not relink the program\n" << s::text(r);
        r = app.mcpp({"build", "-v"});
        expect(fatal(e2e::succeeded(r))) << "the third build\n" << s::text(r);
        expect(s::no_line(s::text(r), R"(^\[[0-9]+/[0-9]+\] .*bin/app)")) << "a build with nothing changed linked the program again\n" << s::text(r);
    };
    "1062 link flags on a compiler link are named not dropped"_test[s::elf][s::llvm] = [] {
        // [platform.link] flags a compiler-driven link does not use are named with the remedy
        // ([platform.build] ldflags); an ld.lld link takes them without a word.
        const auto t = s::case_dir();
        const auto LLVM_VERSION = s::llvm_version();
        s::project plat{t / "plat", {{"mcpp.toml", std::format(R"cpp([package]
name    = "words-platform"
version = "0.1.0"

[platform]
toolchain = "llvm@{0}"

[[platform.define]]
triple        = "x86_64-wordsos"
clang-triple  = "x86_64-unknown-linux-gnu"
object-format = "elf"
family        = "unix"
execution     = "wordsos"

[[platform.provider]]
serves = ["x86_64-wordsos"]

[platform.build]
cflags  = ["-ffreestanding", "-fno-stack-protector"]
ldflags = ["-nostdlib", "-static"]

[platform.link]
driver = "compiler"
flags  = ["-e", "_start"]

[platform.layers]
"kernel-abi"       = false
"c-abi"            = false
"c++-abi"          = false
"compiler-runtime" = false
)cpp", LLVM_VERSION)}}};
        s::project app{t / "app", {{"mcpp.toml", R"cpp([package]
name    = "words"
version = "0.1.0"

[targets.words]
kind = "bin"
main = "src/main.c"

[target.x86_64-wordsos]
platform = { path = "../plat" }
)cpp"}, {"src/main.c", "void _start(void) { for (;;) {} }\n"}}};
        auto r = app.mcpp({"build", "--target", "x86_64-wordsos"});
        expect(fatal(e2e::succeeded(r))) << "the build failed\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "states `[platform.link] flags`"))) << "the dropped words were not named\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "`[platform.build] ldflags`"))) << "the remedy was not named\n" << s::text(r);
        std::string m;
        for (const auto& l : lines_of(plat.read("mcpp.toml"))) {
            if (l == "driver = \"compiler\"") m += "driver = \"lld\"\n";
            else if (l.starts_with("ldflags = ")) m += "\n";
            else m += l + "\n";
        }
        plat.write("mcpp.toml", m);
        app.remove("target");
        r = app.mcpp({"build", "--target", "x86_64-wordsos"});
        expect(fatal(e2e::succeeded(r))) << "the lld build failed\n" << s::text(r);
        expect(s::lacks(s::text(r), "platform.link] flags")) << "an lld link was warned\n" << s::text(r);
    };
}};

}  // namespace

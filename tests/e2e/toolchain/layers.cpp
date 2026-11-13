// A toolchain over the libraries beneath it: a gcc artifact carries only the RUNPATH it asked for,
// a graph's libc++ over the payload's C library, a C++-layer provider keeps its own standard, gcc
// over a graph C library is degraded and not refused, gcc is accepted where the [c-abi]
// realisation is empty, a host header is written and not searched, llvm over a graph-supplied
// libstdc++, and gcc over the host's payload links from the payload (formerly tests/e2e/201, 663,
// 696, 740, 742, 891, 934, 1080).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

std::optional<fs::path> program_under(const fs::path& root, std::string_view name) {
    for (const auto& f : s::find_files(root, ".*"))
        if (f.parent_path().filename() == "bin" && f.filename() == name) return f;
    return std::nullopt;
}

std::string output_of(const fs::path& program) {
    auto out = s::run(program).out;
    while (out.ends_with('\n') || out.ends_with('\r')) out.pop_back();
    return out;
}

std::vector<std::string> lines_of(std::string_view text) {
    std::vector<std::string> out;
    for (auto part : std::views::split(text, '\n')) out.emplace_back(part.begin(), part.end());
    return out;
}

std::optional<fs::path> first(const fs::path& root, std::string_view pattern) {
    const auto all = s::glob(root, pattern);
    if (all.empty()) return std::nullopt;
    return all.front();
}

// `ldd <file> | grep -E '^\s*[^/\s]+ => +(/lib|/usr/lib|/lib64|/usr/lib64)/'`: the libraries the
// loader finds in the host's own directories (nothing when there is no ldd).
std::vector<std::string> host_lib_lines(const fs::path& file) {
    std::vector<std::string> out;
    const auto ldd = s::on_path("ldd");
    if (!ldd) return out;
    static const std::regex host{R"(^\s*[^/\s]+ => +(/lib|/usr/lib|/lib64|/usr/lib64)/)"};
    for (const auto& l : lines_of(s::run(*ldd, {file.string()}).out))
        if (std::regex_search(l, host)) out.push_back(l);
    return out;
}

std::string ldd_of(const fs::path& file) {
    const auto ldd = s::on_path("ldd");
    return ldd ? s::text(s::run(*ldd, {file.string()})) : std::string();
}

// What `readelf -d | grep -q 'libc++'` finds: a dynamic entry naming libc++.
bool names_libcxx(const fs::path& file) {
    const auto h = s::read_header(file);
    return std::ranges::any_of(h.needed, [](const std::string& n) { return n.contains("libc++"); }) || h.runpath.contains("libc++") || h.soname.contains("libc++");
}

// awk's std_tokens over an emitted build database: the -std words of the arguments of the first
// translation unit whose source ends with `want`, each after a space; "NOT-FOUND" when none does.
std::optional<std::string> std_tokens_in(const nlohmann::json& j, std::string_view want) {
    if (j.is_object()) {
        if (j.contains("source") && j["source"].is_string() && j["source"].get<std::string>().ends_with(want) && j.contains("arguments")) {
            std::string out;
            for (const auto& a : j["arguments"])
                if (a.is_string() && a.get<std::string>().starts_with("-std=")) out += " " + a.get<std::string>();
            return out;
        }
        for (auto it = j.begin(); it != j.end(); ++it)
            if (auto found = std_tokens_in(it.value(), want)) return found;
    } else if (j.is_array()) {
        for (const auto& e : j)
            if (auto found = std_tokens_in(e, want)) return found;
    }
    return std::nullopt;
}

E2E layers{"layers", [] {
    "201 gcc no specs pollution"_test[tag{"requires:gcc"}][tag{"requires:elf"}] = [] {
        // A gcc artifact carries only the RUNPATH the build asked for (no other home's temporary
        // directory, no dead entry), names the payload's glibc as its interpreter, and loads
        // nothing from the host, in the sysroot mode and in the payload-first one.
        s::needs({"gcc", "elf"});
        auto p = s::project::scaffold("prog");
        p.write("mcpp.toml", p.read("mcpp.toml") + "\n[toolchain]\nlinux = \"gcc@16.1.0\"\n");
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "build failed: " << s::text(r);
        r = p.mcpp({"build", "--no-cache"});
        expect(fatal(e2e::succeeded(r))) << "cold build failed: " << s::text(r);
        const auto bin = program_under(p.at("target"), "prog");
        expect(fatal(bin.has_value())) << "no binary produced";
        const auto h = s::read_header(*bin);
        std::vector<std::string> tmp_entries, dead;
        for (auto part : std::views::split(std::string_view(h.runpath), ':')) {
            std::string d(part.begin(), part.end());
            if (d.empty()) continue;
            if (d.starts_with("/tmp/")) tmp_entries.push_back(d);
            if (d.starts_with("$ORIGIN")) continue;
            if (!fs::is_directory(d)) dead.push_back(d);
        }
        expect(fatal(tmp_entries.empty())) << "the artifact carries RUNPATH entries from other homes' temp dirs (from the payload's specs, which mcpp must no longer write): " << nlohmann::json(tmp_entries).dump();
        expect(fatal(dead.empty())) << dead.size() << " dead RUNPATH entries: " << nlohmann::json(dead).dump();
        expect(fatal(h.interpreter.contains("xim-x-glibc"))) << "PT_INTERP is not the payload's glibc: " << h.interpreter;
        const auto ran = s::run(*bin);
        expect(fatal(e2e::succeeded(ran))) << "binary did not run: " << s::text(ran);
        expect(fatal(e2e::contains(s::text(ran), "Hello"))) << "unexpected output: " << s::text(ran);
        const auto host_libs = host_lib_lines(*bin);
        expect(fatal(host_libs.empty())) << "the artifact loads libraries from the host (a directory is missing from RUNPATH): " << nlohmann::json(host_libs).dump();
        // The payload-first mode: a home of the payloads alone.
        const s::options o{.where = s::home::payloads};
        const auto parent = s::case_dir() / "pf-new";
        fs::create_directories(parent);
        r = s::mcpp({"new", "pf"}, parent, o);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        s::project pf(parent / "pf", {});
        pf.write("mcpp.toml", pf.read("mcpp.toml") + "\n[toolchain]\nlinux = \"gcc@16.1.0\"\n");
        r = pf.mcpp({"build"}, o);
        expect(fatal(e2e::succeeded(r))) << "payload-first build failed: " << s::text(r);
        const auto pfbin = program_under(pf.at("target"), "pf");
        expect(fatal(pfbin.has_value())) << "no payload-first binary produced";
        const auto pf_host = host_lib_lines(*pfbin);
        expect(fatal(pf_host.empty())) << "the payload-first artifact loads libraries from the host: " << nlohmann::json(pf_host).dump() << "\nRUNPATH was: " << s::read_header(*pfbin).runpath;
        expect(fatal(e2e::succeeded(s::run(*pfbin)))) << "payload-first binary did not run";
    };

    "663 a graph libcxx over the payloads c library"_test[tag{"requires:llvm"}] = [] {
        // llvm.libcxx supplies the C++ layer over the payload's C library: the payload's libc++
        // headers and archives leave the command lines, the package's std module names the
        // payload's glibc, and the program runs without a libc++ shared object.
        s::needs({"llvm"});
        auto p = s::project::scaffold("app");
        p.write("src/main.cpp", "import std;\nint main() {\n    std::unordered_map<std::string, int> m;\n    m[\"one\"] = 1;\n    std::atomic<int> a{2};\n    a.notify_all();\n    std::print(\"{}-{}-3\\n\", m[\"one\"], a.load());\n}\n");
        p.write("mcpp.toml", p.read("mcpp.toml") + std::format("\n[toolchain]\ndefault = \"llvm@{}\"\n", s::llvm_version()));
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: the baseline build failed: " << s::text(r);
        expect(fatal(s::no_line(s::text(r), R"(c\+\+-abi.*graph\))"))) << "FAIL: the baseline report names a graph C++ layer";
        const auto base = s::read_file(first(p.at("target"), "*/*/build.ninja").value_or(fs::path()));
        expect(fatal(s::has_line(base, R"(-isystem.*include/c\+\+/v1)"))) << "FAIL: the baseline compile line carries no payload libc++ -isystem";
        expect(fatal(s::has_line(base, R"(xim-x-llvm[^ ]*libc\+\+\.a)"))) << "FAIL: the baseline link line does not name the payload's libc++.a";
        p.write("mcpp.toml", p.read("mcpp.toml") + "\n[dependencies]\nllvm.libcxx = { git = \"https://github.com/mcpplibs/libcxx.git\", tag = \"22.1.8.1\" }\n");
        p.remove("target");
        r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: the build over llvm.libcxx failed: " << s::text(r);
        expect(fatal(s::has_line(s::text(r), R"(c\+\+-abi +libc\+\+ +\(libcxx@22\.1\.8\.1, graph\))"))) << "FAIL: the report does not name llvm.libcxx as the C++ layer (graph)";
        const auto ninja = s::read_file(first(p.at("target"), "*/*/build.ninja").value_or(fs::path()));
        expect(fatal(s::no_line(ninja, R"(-isystem[^ ]*xim-x-llvm[^ ]*include/c\+\+/v1)"))) << "FAIL: the payload's libc++ headers are still on a compile line";
        expect(fatal(e2e::contains(ninja, "-nostdinc++"))) << "FAIL: the compile lines carry no -nostdinc++";
        expect(fatal(e2e::contains(ninja, "-nostdlib++"))) << "FAIL: the link line carries no -nostdlib++";
        expect(fatal(s::no_line(ninja, R"(xim-x-llvm[^ ]*libc\+\+(abi)?\.a)"))) << "FAIL: the link line still names the payload's libc++ archives";
        std::optional<fs::path> stdjson;
        for (const auto& f : s::glob(s::shared_home() / "build-cache/v1/std", "*/std-module.json"))
            if (e2e::contains(s::read_file(f), "libcxx") && (!stdjson || fs::last_write_time(f) > fs::last_write_time(*stdjson))) stdjson = f;
        expect(fatal(stdjson.has_value())) << "FAIL: no std-module.json records the package's module under " << (s::shared_home() / "build-cache/v1/std").string();
        expect(fatal(e2e::contains(s::read_file(*stdjson), "xim-x-glibc"))) << "FAIL: the package std module's command does not name the payload's glibc: " << s::read_file(*stdjson);
        const auto bin = first(p.at("target"), "*/*/bin/app");
        expect(fatal(bin.has_value())) << "no app";
        expect(fatal(s::lacks(ldd_of(*bin), "libc++"))) << "FAIL: the artefact still links a libc++ shared object";
        const auto ran = s::run(*bin);
        expect(fatal(e2e::succeeded(ran))) << "FAIL: the program exited non-zero";
        expect(fatal(output_of(*bin) == "1-2-3")) << "FAIL: expected 1-2-3, got: " << output_of(*bin);
        std::println("ok: llvm.libcxx supplies the C++ layer over the payload's C library, and the program runs");
    };

    "696 a cxx layer provider keeps its own standard"_test[tag{"requires:llvm"}] = [] {
        // A C++-layer provider that states a standard compiles its implementation units at it
        // under a lower or a higher graph level, its std module at the graph's; one that states
        // none keeps the graph's; an ordinary dependency keeps the one-standard rule and its
        // warning.
        s::needs({"llvm"});
        const auto tmp = s::case_dir();
        const auto git = s::on_path("git");
        expect(fatal(git.has_value())) << "FAIL: cannot clone mcpplibs/libcxx 22.1.8.2 (no git)";
        auto clone = s::run(*git, {"clone", "-q", "--depth", "1", "--branch", "22.1.8.2", "https://github.com/mcpplibs/libcxx.git", "upstream"}, tmp);
        expect(fatal(e2e::succeeded(clone))) << "FAIL: cannot clone mcpplibs/libcxx 22.1.8.2: " << s::text(clone);
        fs::remove_all(tmp / "upstream/.git");
        const auto upstream_manifest = s::read_file(tmp / "upstream/mcpp.toml");
        expect(fatal(s::has_line(upstream_manifest, R"(^cxx_standard = "c\+\+23"$)"))) << "FAIL: the fixture no longer has the line this test restates: " << upstream_manifest;
        fs::copy(tmp / "upstream", tmp / "stated", fs::copy_options::recursive);
        std::string stated;
        for (const auto& l : lines_of(upstream_manifest)) {
            if (l == "cxx_standard = \"c++23\"") continue;
            stated += l + "\n";
            if (l == "version     = \"22.1.8.2\"") stated += "standard    = \"c++23\"\n";
        }
        if (stated.ends_with("\n\n") && !upstream_manifest.ends_with("\n\n")) stated.pop_back();
        s::project(tmp / "stated", {{"mcpp.toml", stated}});
        expect(fatal(s::has_line(stated, R"(^standard    = "c\+\+23"$)"))) << "FAIL: the restated manifest has no standard: " << stated;
        auto make_root = [&](std::string_view dir, std::string_view standard, std::string_view libcxx) {
            return s::project(tmp / dir, {{"mcpp.toml", std::format("[package]\nname     = \"app\"\nversion  = \"0.1.0\"\nstandard = \"{}\"\n\n[toolchain]\ndefault = \"llvm@{}\"\n\n[dependencies.llvm.libcxx]\npath = \"{}\"\n", standard, s::llvm_version(), libcxx)},
                                          {"src/main.cpp", "import std;\nint main() {\n    std::unordered_map<std::string, int> m;\n    m[\"one\"] = 1;\n    try { throw std::runtime_error(\"x\"); } catch (const std::runtime_error&) { m[\"two\"] = 2; }\n    std::cout << std::format(\"{}-{}\", m[\"one\"], m[\"two\"]) << std::endl;\n}\n"}});
        };
        auto emit = [&](const s::project& p, std::string_view label) {
            auto r = p.mcpp({"emit", "build-database"});
            expect(fatal(e2e::succeeded(r))) << "FAIL: " << label << ": emit build-database failed: " << r.err;
            return std::pair{s::json(r.out), r.err};
        };
        auto tokens = [](const nlohmann::json& db, std::string_view want) { return std_tokens_in(db, want).value_or("NOT-FOUND"); };
        auto a = make_root("a", "c++20", "../stated");
        auto [adb, aerr] = emit(a, "A");
        expect(fatal(tokens(adb, "llvm/libcxx/src/new.cpp") == " -std=c++20 -std=c++23")) << "FAIL: A: new.cpp std tokens were '" << tokens(adb, "llvm/libcxx/src/new.cpp") << "', expected ' -std=c++20 -std=c++23'\n" << aerr;
        expect(fatal(tokens(adb, "a/src/main.cpp") == " -std=c++20")) << "FAIL: A: main.cpp std tokens were '" << tokens(adb, "a/src/main.cpp") << "', expected ' -std=c++20'";
        expect(fatal(tokens(adb, "stated/generated/std.cppm") == " -std=c++20")) << "FAIL: A: the std module's std tokens were '" << tokens(adb, "stated/generated/std.cppm") << "', expected ' -std=c++20'";
        expect(fatal(s::lacks(aerr, "declares standard"))) << "FAIL: A: the provider is reported as not applied";
        auto r = a.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: A: the c++20 build over the provider failed: " << s::text(r);
        auto app = first(a.at("target"), "*/*/bin/app");
        expect(fatal(app.has_value() && e2e::succeeded(s::run(*app)))) << "FAIL: A: the program exited non-zero";
        expect(fatal(output_of(*app) == "1-2")) << "FAIL: A: expected 1-2, got: " << output_of(*app);
        auto b = make_root("b", "c++20", "../upstream");
        auto [bdb, berr] = emit(b, "B");
        expect(fatal(tokens(bdb, "llvm/libcxx/src/new.cpp") == " -std=c++20")) << "FAIL: B: without a statement new.cpp std tokens were '" << tokens(bdb, "llvm/libcxx/src/new.cpp") << "', expected ' -std=c++20'";
        auto c = make_root("c", "c++26", "../stated");
        auto [cdb, cerr] = emit(c, "C");
        expect(fatal(tokens(cdb, "llvm/libcxx/src/new.cpp") == " -std=c++26 -std=c++23")) << "FAIL: C: new.cpp std tokens were '" << tokens(cdb, "llvm/libcxx/src/new.cpp") << "', expected ' -std=c++26 -std=c++23'";
        expect(fatal(tokens(cdb, "c/src/main.cpp") == " -std=c++26")) << "FAIL: C: main.cpp std tokens were '" << tokens(cdb, "c/src/main.cpp") << "', expected ' -std=c++26'";
        r = c.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: C: the c++26 build over the provider failed: " << s::text(r);
        app = first(c.at("target"), "*/*/bin/app");
        expect(fatal(app.has_value() && e2e::succeeded(s::run(*app)))) << "FAIL: C: the program exited non-zero";
        expect(fatal(output_of(*app) == "1-2")) << "FAIL: C: expected 1-2, got: " << output_of(*app);
        s::project(tmp / "plain", {{"mcpp.toml", "[package]\nname     = \"plain\"\nversion  = \"0.1.0\"\nstandard = \"c++23\"\n\n[targets.plain]\nkind = \"lib\"\n"},
                                   {"src/plain.cpp", "int plain_answer() { return 42; }\n"}});
        s::project d(tmp / "d", {{"mcpp.toml", "[package]\nname     = \"d\"\nversion  = \"0.1.0\"\nstandard = \"c++20\"\n\n[dependencies]\nplain = { path = \"../plain\" }\n"},
                                 {"src/main.cpp", "int plain_answer();\nint main() { return plain_answer() == 42 ? 0 : 1; }\n"}});
        auto [ddb, derr] = emit(d, "D");
        expect(fatal(tokens(ddb, "plain/src/plain.cpp") == " -std=c++20")) << "FAIL: D: an ordinary dependency's std tokens were '" << tokens(ddb, "plain/src/plain.cpp") << "', expected ' -std=c++20'";
        expect(fatal(e2e::contains(derr, "dependency `plain` declares standard = \"c++23\""))) << "FAIL: D: the ordinary dependency's higher standard is no longer reported: " << derr;
        std::println("PASS: a C++-layer provider compiles its implementation units at its own standard");
    };

    "740 gcc over a graph c library is degraded not refused"_test[tag{"requires:gcc"}] = [] {
        // GCC over a graph-supplied C library cannot isolate it: a warning names the C library,
        // the compiler and the toolchain that does isolate, and the target side still reports; a
        // Clang-family toolchain over the same graph gets no such note.
        s::needs({"gcc"});
        s::project p({{"libc/mcpp.toml", "[package]\nname     = \"fake-libc\"\nversion  = \"0.1.0\"\nprovides = [\"mcpp:c-abi=musl\"]\n\n[build]\nsources  = []\n"},
                      {"mcpp.toml", "[package]\nname    = \"clibdeg\"\nversion = \"0.1.0\"\n\n[dependencies]\nfake-libc = { path = \"libc\" }\n"}});
        auto r = p.mcpp({"why", "toolchain"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        const auto out = s::text(r);
        expect(fatal(s::has_line(out, "^warning: "))) << "FAIL: no warning at all on a GCC toolchain over a graph-supplied C library";
        expect(fatal(e2e::contains(out, "C library ('musl'"))) << "FAIL: the warning does not name the C library: " << out;
        expect(fatal(e2e::contains(out, "resolved compiler ('gcc')"))) << "FAIL: the warning does not name the compiler that cannot isolate: " << out;
        expect(fatal(s::contains_nocase(out, "clang"))) << "FAIL: the warning does not point at the toolchain that does isolate: " << out;
        expect(fatal(s::has_line(out, "c-abi *musl"))) << "FAIL: the target-side report did not print at all";
        std::set<std::string> llvms;
        const auto list = s::json(p.mcpp({"toolchain", "list", "--format", "json"}).out);
        if (list.is_object() && list.contains("data") && list["data"].contains("toolchains"))
            for (const auto& t : list["data"]["toolchains"])
                if (t.value("family", std::string()) == "llvm") llvms.insert("llvm@" + t.value("version", std::string()));
        if (llvms.empty()) {
            std::println("SKIP: no llvm installed here to run the control against");
            return;
        }
        p.write("mcpp.toml", std::format("[package]\nname    = \"clibdeg\"\nversion = \"0.1.0\"\n\n[toolchain]\ndefault = \"{}\"\n\n[dependencies]\nfake-libc = {{ path = \"libc\" }}\n", *llvms.begin()));
        const auto out2 = s::text(p.mcpp({"why", "toolchain"}));
        expect(fatal(s::lacks(out2, "has no way to stop"))) << "FAIL: the same degradation fired on a Clang-family toolchain, which DOES isolate the graph's C library";
    };

    "742 gcc accepted when the realisation is empty"_test[tag{"requires:gcc"}][tag{"requires:mingw-cross"}] = [] {
        // GCC is refused for a declared [c-abi] block only where the realisation is non-empty:
        // accepted on Linux (empty), refused for mingw on Windows (the Cygwin substitution).
        s::needs({"gcc", "mingw-cross"});
        s::project p({{"fakemusl/src/lib.c", "int fakemusl_marker(void) { return 0; }\n"},
                      {"src/main.c", "int main(void) { return 0; }\n"},
                      {"mcpp.toml", "[package]\nname    = \"gcc-cabi-probe\"\nversion = \"0.1.0\"\n\n[dependencies]\nfakemusl = { path = \"fakemusl\" }\n\n[build]\nallow_host_libs = true\n"},
                      {"fakemusl/mcpp.toml", "[package]\nname     = \"fakemusl\"\nversion  = \"0.15.0\"\nprovides = [\"mcpp:c-abi=musl\"]\n\n[targets.fakemusl]\nkind    = \"lib\"\nsources = [\"src/*.c\"]\n\n[c-abi]\npresents   = \"posix\"\ndata-model = \"arch-default\"\nwchar      = 32\nbuiltins   = \"iso\"\n"}});
        auto r = p.mcpp({"build", "--target", "x86_64-linux-gnu"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: GCC must be accepted when the [c-abi] realisation is empty: " << s::text(r);
        expect(fatal(!s::has_line_nocase(s::text(r), "is not one mcpp can realise|requires Clang-specific"))) << "FAIL: GCC/Linux was refused for a substitution nothing asked it to perform: " << s::text(r);
        p.remove("target");
        r = p.mcpp({"build", "--target", "x86_64-windows-gnu"});
        expect(fatal(!r)) << "FAIL: GCC (mingw) on Windows must still be refused -- the Cygwin substitution there is non-empty: " << s::text(r);
        expect(fatal(s::contains_nocase(s::text(r), "requires Clang-specific"))) << "FAIL: the refusal must still name the Clang-specific requirement: " << s::text(r);
    };

    "891 a host header is written not searched"_test[tag{"requires:llvm"}][tag{"requires:elf"}] = [] {
        // The host's /usr/include is not searched (a payload C library is not described as the
        // graph's, on the fast path either); a project that needs it writes it down.
        s::needs({"llvm", "elf"});
        std::string header;
        for (std::string h : {"zlib.h", "X11/Xlib.h", "curl/curl.h", "openssl/ssl.h"})
            if (fs::is_regular_file("/usr/include/" + h)) { header = h; break; }
        if (header.empty()) skip("891 needs a header that only the host's /usr/include has");
        s::project p({{"src/main.cpp", std::format("#include <{}>\n#include <cstdio>\nint main() {{ std::puts(\"host header seen\"); }}\n", header)}});
        auto write_manifest = [&](std::string_view extra) {
            p.write("mcpp.toml", std::format("[package]\nname = \"hosthdr\"\nversion = \"0.1.0\"\n\n[toolchain]\ndefault = \"llvm@{}\"\n{}", s::llvm_version(), extra));
        };
        write_manifest("");
        auto r = p.mcpp({"build"});
        expect(fatal(!r)) << "FAIL: A: <" << header << "> was found in the host's /usr/include";
        expect(fatal(e2e::contains(s::text(r), "file not found"))) << "FAIL: A: the failure is not the missing header: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), "comes from the dependency graph"))) << "FAIL: A: a payload C library was described as the graph's";
        r = p.mcpp({"build"});
        expect(fatal(!r)) << "FAIL: A: the second build succeeded";
        expect(fatal(s::lacks(s::text(r), "comes from the dependency graph"))) << "FAIL: A: the fast path described a payload C library as the graph's";
        write_manifest("\n[build]\ncxxflags = [\"-idirafter\", \"/usr/include\"]\n");
        r = p.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: B: an explicit -idirafter /usr/include did not build: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "host header seen"))) << "FAIL: B: the program did not run: " << s::text(r);
    };

    "934 llvm with a graph supplied libstdcxx"_test = [] {
        // An llvm build whose C++ layer is libstdc++ from a package of the graph: a typed catch
        // works across a shared library, neither image needs libc++, and a requirement of the
        // layer without its supplier is refused.
        if (!s::on_linux()) skip("934 uses the Linux gcc payload");
        const auto llvm_root = s::payload_root("xim-x-llvm", s::llvm_version());
        if (!fs::exists(llvm_root / "bin/clang++")) skip("no llvm payload");
        const auto gcc = s::gcc_version();
        const auto G = s::payload_root("xim-x-gcc", gcc);
        if (!fs::exists(G / "include/c++" / gcc / "bits/std.cc") || !fs::exists(G / "lib64/libstdc++.so.6")) skip("no gcc payload with bits/std.cc");
        const auto tmp = s::case_dir();
        const auto P = tmp / "libstdcxx";
        fs::create_directories(P / "lib");
        fs::create_directory_symlink(G / "include/c++" / gcc, P / "cxx");
        for (std::string f : {"libstdc++.so", "libstdc++.so.6", "libstdc++.a", "libgcc_s.so", "libgcc_s.so.1"})
            if (fs::exists(G / "lib64" / f)) fs::create_symlink(G / "lib64" / f, P / "lib" / f);
        s::project(P, {{"mcpp.toml", "[package]\nnamespace = \"gcc\"\nname      = \"libstdcxx\"\nversion   = \"16.1.0.1\"\nprovides  = [\"mcpp:c++-abi=libstdc++\"]\nrequires  = [\"mcpp:compiler=llvm\", \"mcpp:c-abi=glibc\"]\n\n[build]\nsources      = []\ninclude_dirs = [\"cxx\", \"cxx/x86_64-linux-gnu\", \"cxx/backward\"]\nstd-module   = \"cxx/bits/std.cc\"\n\n[runtime]\nlibrary_dirs = [\"lib\"]\n"}});
        s::project(tmp / "needs", {{"mcpp.toml", "[package]\nname     = \"needs\"\nversion  = \"0.1.0\"\nrequires = [\"mcpp:c++-abi=libstdc++\"]\n\n[build]\nsources = []\n"}});
        fs::create_directories(tmp / "needs/src");
        s::project app(tmp / "app", {{"mcpp.toml", std::format("[package]\nname     = \"app\"\nversion  = \"0.1.0\"\nstandard = \"c++23\"\n\n[toolchain]\ndefault = \"llvm@{}\"\n\n[lib]\npath = \"src/xrt.cppm\"\n\n[targets.xrt]\nkind = \"shared\"\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n\n[dependencies]\nlibstdcxx = {{ path = \"../libstdcxx\" }}\nneeds     = {{ path = \"../needs\" }}\n", s::llvm_version())},
                                     {"src/xrt.cppm", "export module xrt;\nimport std;\nexport namespace xrt { int checked(int v) { if (v < 0) throw std::runtime_error(\"negative\"); return v * 2; } }\n"},
                                     {"src/main.cpp", "import std;\nimport xrt;\nint main() {\n    std::vector<int> v{3, 1, 2};\n    std::ranges::sort(v);\n    try { xrt::checked(-1); }\n    catch (const std::exception& e) { std::println(\"{} caught {}\", v, e.what()); return 0; }\n    catch (...) { std::println(\"missed\"); return 1; }\n}\n"}});
        fs::create_directories(tmp / "app/tests");
        auto r = app.mcpp({"run", "app"});
        expect(fatal(e2e::succeeded(r))) << "FAIL: A/B/C: the build or the run failed: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "caught negative"))) << "FAIL: B: no typed catch across the shared library: " << s::text(r);
        const auto exe = program_under(app.at("target"), "app");
        expect(fatal(exe.has_value())) << "no app";
        expect(fatal(!names_libcxx(*exe))) << "FAIL: B: the image needs libc++";
        std::optional<fs::path> so;
        for (const auto& f : s::find_files(app.at("target"), R"(libxrt\.so.*)")) { so = f; break; }
        expect(fatal(so.has_value())) << "FAIL: B: no libxrt.so";
        expect(fatal(!names_libcxx(*so))) << "FAIL: B: the shared library needs libc++";
        std::string manifest;
        for (const auto& l : lines_of(app.read("mcpp.toml")))
            if (!l.starts_with("libstdcxx = ")) manifest += l + "\n";
        if (manifest.ends_with("\n\n")) manifest.pop_back();
        app.write("mcpp.toml", manifest);
        r = app.mcpp({"build"});
        expect(fatal(!r)) << "FAIL: D: the requirement was not refused";
        expect(fatal(s::has_line(s::text(r), R"(requires the c\+\+-abi to be .libstdc\+\+.)"))) << "FAIL: D: the refusal does not name the requirement: " << s::text(r);
    };

    "1080 gcc over the hosts payload links from the payload"_test[tag{"requires:gcc"}][tag{"requires:elf"}] = [] {
        // gcc over the host's C library payload names the payload on its link (crt prefix,
        // library directory, loader), not a sysroot, and the program runs.
        s::needs({"gcc", "elf"});
        auto p = s::project::scaffold("app");
        auto r = p.mcpp({"build", "--toolchain", "gcc@15.1.0"});
        expect(fatal(e2e::succeeded(r))) << "the project did not build with gcc@15.1.0: " << s::text(r);
        const auto ninja = s::find_file(p.at("target"), "build.ninja");
        expect(fatal(ninja.has_value())) << "no build.ninja";
        std::string line;
        static const std::regex key{"^ *ldflags *="};
        for (const auto& l : lines_of(s::read_file(*ninja)))
            if (std::regex_search(l, key)) { line = l; break; }
        expect(fatal(!line.empty())) << "no link flags in build.ninja";
        expect(fatal(s::has_line(line, "-B[^ ]*xim-x-glibc"))) << "the link names no crt search prefix in the payload: " << line;
        expect(fatal(s::has_line(line, "-L[^ ]*xim-x-glibc"))) << "the link names no library directory in the payload: " << line;
        expect(fatal(s::has_line(line, "--dynamic-linker=[^ ]*xim-x-glibc"))) << "the link names no loader in the payload: " << line;
        expect(fatal(s::lacks(line, "--sysroot="))) << "this gcc was linked through a sysroot: the script holds the payload-first form";
        r = p.mcpp({"run", "--toolchain", "gcc@15.1.0"});
        expect(fatal(e2e::succeeded(r))) << "the program did not run: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "Hello"))) << "the program's output did not arrive: " << s::text(r);
    };
}};

}  // namespace

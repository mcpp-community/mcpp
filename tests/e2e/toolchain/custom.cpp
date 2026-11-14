// A toolchain mcpp does not manage: one named by path, one a build program's toolchain phase
// states, and an external toolchain that finds mcpp's own ninja (formerly tests/e2e/875, 876,
// 1004).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

// `text` with the characters an ECMAScript expression reads specially escaped.
std::string literal(std::string_view text) {
    static constexpr std::string_view special = R"(\^$.|?*+()[]{})";
    std::string out;
    for (char c : text) {
        if (special.contains(c)) out += '\\';
        out += c;
    }
    return out;
}

// A tree of the llvm payload's programs and directories, by links: `bin/` holds a link to each of
// its programs but the `.cfg` files, and include, lib, share and libexec are links.
fs::path llvm_tree(const fs::path& at) {
    const auto root = s::payload_root("xim-x-llvm", s::llvm_version());
    fs::create_directories(at / "bin");
    for (const auto& f : fs::directory_iterator(root / "bin"))
        if (f.path().extension() != ".cfg") fs::create_symlink(f.path(), at / "bin" / f.path().filename());
    for (std::string d : {"include", "lib", "share", "libexec"})
        if (fs::exists(root / d)) fs::create_directory_symlink(root / d, at / d);
    return root;
}

E2E custom{"custom", [] {
    "875 toolchain by path"_test[s::llvm][s::unix_shell][s::symlink] = [] {
        // `[toolchain] default = { path = ... }` (mcpp#755): a toolchain tree named by path builds
        // with its launcher and its stated linker, is reported as custom, writes nothing into the
        // tree, keeps the fast path until one of its programs changes, is named by MCPP_TOOLCHAIN
        // too, and is identified (a contradicting family, a tree with no driver, and a gcc tree
        // that states ld are refused).
        const auto llvm_root = s::payload_root("xim-x-llvm", s::llvm_version());
        if (!fs::exists(llvm_root / "bin/clang++")) skip(std::format("no llvm payload installed ({})", llvm_root.string()));
        const auto work = s::case_dir();
        const auto T = work / "llvm";
        llvm_tree(T);
        const auto wrapper = work / "ld-wrapper";
        s::write_executable(wrapper, std::format("#!/bin/sh\nexec \"{}/bin/ld.lld\" \"$@\"\n", llvm_root.string()));
        const s::options o{.env = {{"NO_COLOR", "1"}}};
        s::project app(work / "app", {{"src/main.cpp", "#include <cstdio>\nint main() { std::puts(\"built by a toolchain named by path\"); return 0; }\n"}});
        auto write_manifest = [&](const fs::path& root, std::string_view extra) {
            app.write("mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[toolchain]\ndefault = {{ path = \"{}\"{} }}\n", root.string(), extra));
        };
        write_manifest(T, std::format(", launcher = \"/usr/bin/env\", tools = {{ ld = \"{}\" }}", wrapper.string()));
        app.remove("target");
        auto r = app.mcpp({"build"}, o);
        auto out = s::text(r);
        expect(fatal(e2e::succeeded(r))) << "FAIL: the build failed: " << out;
        expect(fatal(s::has_line(out, std::format(R"(Using toolchain clang .* ← {}  \[custom · mcpp\.toml:[0-9]*\])", literal(T.string()))))) << "FAIL: no Using line for the toolchain: " << out;
        expect(fatal(s::has_line(out, "Finished .* · custom: toolchain"))) << "FAIL: Finished does not summarise the source: " << out;
        const auto bins = s::glob(app.at("target"), "*/*/bin/app");
        expect(fatal(!bins.empty())) << "no app";
        auto run = s::run(bins.front()).out;
        while (run.ends_with('\n')) run.pop_back();
        expect(fatal(run == "built by a toolchain named by path")) << "FAIL: the program did not run: " << run;
        std::string ninja;
        for (const auto& n : s::glob(app.at("target"), "*/*/build.ninja")) ninja += s::read_file(n);
        expect(fatal(s::has_line(ninja, std::format(R"(^cxx *= /usr/bin/env {}/bin/clang\+\+)", literal(T.string()))))) << "FAIL: the launcher does not prefix the compiler";
        expect(fatal(e2e::contains(ninja, "--ld-path=" + wrapper.string()))) << "FAIL: the stated linker is not used";
        bool cfg = false;
        for (const auto& e : fs::directory_iterator(T)) {
            if (e.path().extension() == ".cfg") cfg = true;
            if (e.is_directory() && !e.is_symlink())
                for (const auto& f : fs::directory_iterator(e.path()))
                    if (f.path().extension() == ".cfg") cfg = true;
        }
        expect(fatal(!cfg)) << "FAIL: mcpp wrote a cfg into the tree";
        out = s::text(app.mcpp({"build", "-v"}, o));
        expect(fatal(s::lacks(out, "fast-path: build declined"))) << "FAIL: an unchanged build declined the fast path: " << out;
        fs::last_write_time(wrapper, fs::file_time_type::clock::now() + std::chrono::minutes{1});
        out = s::text(app.mcpp({"build", "-v"}, o));
        expect(fatal(e2e::contains(out, "a program of the toolchain named by path changed"))) << "FAIL: a changed linker was not noticed: " << out;
        app.write("mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n");
        app.remove("target");
        s::options env_named = o;
        env_named.env.emplace_back("MCPP_TOOLCHAIN", "path:" + T.string());
        r = app.mcpp({"build"}, env_named);
        out = s::text(r);
        expect(fatal(e2e::succeeded(r))) << "FAIL: MCPP_TOOLCHAIN=path: failed: " << out;
        expect(fatal(e2e::contains(out, "[custom · env MCPP_TOOLCHAIN]"))) << "FAIL: the env-named toolchain is not reported: " << out;
        write_manifest(T, std::format(", family = \"gcc\", tools = {{ cxx = \"{}\" }}", (T / "bin/clang++").string()));
        app.remove("target");
        out = s::text(app.mcpp({"build"}, o));
        expect(fatal(e2e::contains(out, "stated as family \"gcc\""))) << "FAIL: a contradicting family was accepted: " << out;
        fs::create_directories(work / "empty/bin");
        write_manifest(work / "empty", "");
        out = s::text(app.mcpp({"build"}, o));
        expect(fatal(e2e::contains(out, "no C++ driver in bin/"))) << "FAIL: a tree without a driver was accepted: " << out;
        // The newest numeric gcc of ~/.mcpp, when it has a g++.
        const auto gcc_base = fs::path(s::env("HOME").value_or("")) / ".mcpp/registry/data/xpkgs/xim-x-gcc";
        std::string gcc_ver;
        static const std::regex numeric{R"([0-9]+(\.[0-9]+)*)"};
        std::vector<std::string> versions;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(gcc_base, ec))
            if (std::regex_match(e.path().filename().string(), numeric)) versions.push_back(e.path().filename().string());
        std::ranges::sort(versions, [](const std::string& a, const std::string& b) {
            std::istringstream x(a), y(b);
            for (std::string p, q;;) {
                const bool more_a = static_cast<bool>(std::getline(x, p, '.')), more_b = static_cast<bool>(std::getline(y, q, '.'));
                if (!more_a || !more_b) return !more_a && more_b;
                if (std::stoull(p) != std::stoull(q)) return std::stoull(p) < std::stoull(q);
            }
        });
        if (!versions.empty()) gcc_ver = versions.back();
        if (!gcc_ver.empty() && fs::exists(gcc_base / gcc_ver / "bin/g++")) {
            write_manifest(gcc_base / gcc_ver, std::format(", tools = {{ ld = \"{}\" }}", wrapper.string()));
            app.remove("target");
            out = s::text(app.mcpp({"build"}, o));
            expect(fatal(e2e::contains(out, "this is a gcc toolchain"))) << "FAIL: a gcc toolchain that states ld was accepted: " << out;
        } else {
            std::println("note: no gcc payload installed, so the gcc refusal is not exercised here");
        }
    };

    "876 toolchain phase"_test[s::llvm][s::unix_shell][s::symlink] = [] {
        // `[toolchain] default = { configure = "build.mcpp" }` (mcpp#755): the build program's
        // toolchain phase states the toolchain (a path, reported with its origin; a managed spec,
        // not announced as custom), must state one, and states nothing else.
        const auto llvm = s::llvm_version();
        const auto llvm_root = s::payload_root("xim-x-llvm", llvm);
        if (!fs::exists(llvm_root / "bin/clang++")) skip(std::format("no llvm payload installed ({})", llvm_root.string()));
        const auto work = s::case_dir();
        const auto T = work / "llvm";
        llvm_tree(T);
        const s::options o{.env = {{"NO_COLOR", "1"}}};
        s::project app(work / "app", {{"mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[toolchain]\ndefault   = {{ configure = \"build.mcpp\" }}\nbootstrap = \"llvm@{}\"\n", llvm)},
                                      {"src/main.cpp", "int main() { return 0; }\n"}});
        auto write_program = [&](std::string_view statement) {
            app.write("build.mcpp", std::format(R"(import std;
import mcpp;
int main() {{
    if (std::string_view(mcpp::phase()) == "toolchain") {{
        {}
        return 0;
    }}
    mcpp::warning((std::string("build phase, compiler ") + mcpp::compiler()).c_str());
    return 0;
}}
)", statement));
        };
        write_program(std::format("mcpp::toolchain(\"path\", \"{}\"); mcpp::toolchain(\"origin\", \"build.mcpp:6\");", T.string()));
        app.remove("target");
        auto r = app.mcpp({"build"}, o);
        auto out = s::text(r);
        expect(fatal(e2e::succeeded(r))) << "FAIL: the build failed: " << out;
        expect(fatal(s::has_line(out, std::format(R"(Using toolchain clang .* ← {}  \[program · build\.mcpp:6\])", literal(T.string()))))) << "FAIL: no Using line naming the build program: " << out;
        expect(fatal(e2e::contains(out, "Bootstrap llvm@" + llvm))) << "FAIL: no Bootstrap line: " << out;
        expect(fatal(e2e::contains(out, "build phase, compiler clang"))) << "FAIL: the build phase did not run: " << out;
        expect(fatal(s::has_line(out, "Finished .* · program: toolchain"))) << "FAIL: Finished does not summarise the source: " << out;
        expect(fatal(s::count_lines(out, "Resolving toolchain") <= 1u)) << "FAIL: the first pass narrated: " << out;
        write_program(std::format("mcpp::toolchain(\"spec\", \"llvm@{}\");", llvm));
        app.remove("target");
        r = app.mcpp({"build"}, o);
        out = s::text(r);
        expect(fatal(e2e::succeeded(r))) << "FAIL: a managed spec from the phase failed: " << out;
        expect(fatal(s::lacks(out, "Using toolchain"))) << "FAIL: a managed spec was announced as a custom source";
        write_program("");
        app.remove("target");
        out = s::text(app.mcpp({"build"}, o));
        expect(fatal(e2e::contains(out, "stated no"))) << "FAIL: a phase without a statement was accepted: " << out;
        write_program(std::format("mcpp::cxxflag(\"-DX\"); mcpp::toolchain(\"path\", \"{}\");", T.string()));
        app.remove("target");
        out = s::text(app.mcpp({"build"}, o));
        expect(fatal(e2e::contains(out, "stated `mcpp:cxxflag=` in its toolchain phase"))) << "FAIL: a flag in the toolchain phase was accepted: " << out;
    };

    "1004 an external toolchain finds mcpps ninja"_test[s::gcc] = [] {
        // A toolchain named by a path outside every provider finds the ninja of mcpp's home, with
        // no ninja on PATH; `self env` names it absolutely.
        const auto home = s::case_home(s::home::inherited);
        const auto work = s::case_dir();
        const auto gxxs = s::glob(home / "registry/data/xpkgs", "xim-x-gcc/*/bin/g++");
        if (gxxs.empty()) skip("no gcc payload to reach from outside the store");
        if (s::glob(home / "registry/data/xpkgs", "xim-x-ninja/*/ninja").empty()) skip("mcpp's home holds no ninja");
        fs::create_directory_symlink(gxxs.front().parent_path().parent_path(), work / "external-gcc");
        std::string vendored = s::env("MCPP_VENDORED_XLINGS").value_or("");
        if (vendored.empty())
            if (auto x = s::on_path("xlings")) vendored = x->string();
        if (vendored.empty() && !fs::exists(home / "registry/bin/xlings")) skip("no xlings to name");
        // A PATH of the shell's tools and the payload's binutils, and no ninja.
        const auto bin = work / "bin";
        fs::create_directories(bin);
        for (std::string t : {"sh", "bash", "env", "cat", "sed", "grep", "mkdir", "rm", "ls", "cp", "mv", "ln", "dirname", "basename", "uname", "tr", "head", "tail", "awk"})
            if (auto p = s::on_path(t)) {
                std::error_code ec;
                fs::remove(bin / t, ec);
                fs::create_symlink(*p, bin / t);
            }
        const auto binutils = s::glob(home / "registry/data/xpkgs", "xim-x-binutils/*/bin");
        for (std::string t : {"as", "ld", "ar", "ranlib", "nm", "objcopy", "strip"})
            if (!binutils.empty() && fs::exists(binutils.front() / t)) {
                std::error_code ec;
                fs::remove(bin / t, ec);
                fs::create_symlink(binutils.front() / t, bin / t);
            }
        expect(fatal(!fs::exists(bin / "ninja"))) << "FAIL: the restricted PATH holds a ninja";
        s::project app(work / "app", {{"mcpp.toml", "[package]\nname = \"app\"\nversion = \"0.1.0\"\n"},
                                      {"src/main.cpp", "import std;\nint main() { std::println(\"built\"); }\n"}});
        const s::options o{.where = s::home::inherited,
                           .env = {{"MCPP_VENDORED_XLINGS", vendored}, {"PATH", bin.string()}, {"MCPP_TOOLCHAIN", "path:" + (work / "external-gcc").string()}}};
        auto r = app.mcpp({"build"}, o);
        expect(fatal(e2e::succeeded(r))) << "FAIL: the build with an external toolchain and no ninja on PATH failed: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), "cannot start 'ninja'"))) << "FAIL: the bare name reached PATH";
        const auto env = s::text(app.mcpp({"self", "env"}, o));
        const auto ninja_lines = s::lines_with(env, "MCPP_NINJA");
        if (!ninja_lines.empty())
            expect(fatal(std::ranges::any_of(ninja_lines, [](const std::string& l) { return l.contains("/ninja"); }))) << "FAIL: MCPP_NINJA is not an absolute ninja: " << env;
    };
}};

}  // namespace

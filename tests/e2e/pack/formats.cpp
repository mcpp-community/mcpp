// Formats and what a pack states: --pkg-format dispatch to a package's format, --features on
// pack, stripping what the graph built, the JSON report and the profile shorthands, a summary
// of many outputs, the build a pack states and its job count, the three axes of a package,
// the compression a provided format is told (formerly tests/e2e/638, 689, 722, 723, 841, 871,
// 954, 1008).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

// The entries of `dir`, sorted (`find <dir> -maxdepth 1`).
std::vector<fs::path> entries(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (fs::is_directory(dir, ec))
        for (const auto& e : fs::directory_iterator(dir)) out.push_back(e.path());
    std::ranges::sort(out);
    return out;
}

// The first file of `dir` whose name ends with `suffix` (`ls dir/*<suffix> | head -1`).
std::optional<fs::path> first_ending(const fs::path& dir, std::string_view suffix) {
    for (const auto& e : entries(dir))
        if (fs::is_regular_file(e) && e.filename().string().ends_with(suffix)) return e;
    return std::nullopt;
}

// The first build.ninja of the project's target, outside the build program's tree.
std::optional<fs::path> graph_of(const s::project& p) {
    for (const auto& f : s::find_files(p.at("target"), R"(build\.ninja)"))
        if (f.generic_string().find("/.build-mcpp/") == std::string::npos) return f;
    return std::nullopt;
}

std::vector<std::string> lines_of(std::string_view text) {
    std::vector<std::string> out;
    std::istringstream in{std::string(text)};
    for (std::string l; std::getline(in, l);) out.push_back(l);
    return out;
}

bool has_exact_line(std::string_view text, std::string_view line) { return std::ranges::contains(lines_of(text), std::string(line)); }

// The 1-based number of the first line of `text` holding `needle`, or 0 (`grep -n | head -1`).
std::size_t first_line_with(std::string_view text, std::string_view needle) {
    const auto lines = lines_of(text);
    for (std::size_t i = 0; i < lines.size(); ++i)
        if (lines[i].find(needle) != std::string::npos) return i + 1;
    return 0;
}

std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

E2E formats{"formats", [] {
    "638 pack format dispatch"_test[tag{"requires:gcc"}] = [] {
        // `--pkg-format <name>` dispatches to a format the resolved graph provides; a plain
        // build's graph is dist=none and makes no distributable; an unknown format is refused
        // before a compile, on stderr, with exit 2, naming what is available; the format's
        // action is told the package version and the platform floor and runs over the staged
        // tree; an ungated stage_dir, a non-artifact role and an unclaimed format are refused;
        // an ungated artifact action is not the package; a provider reading no staged tree
        // gains no stage dependency.
        s::needs({"gcc"});
        const auto tmp = temp_dir();
        s::project app{tmp / "app", {{"mcpp.toml", R"cpp([package]
name        = "app"
version     = "2.5.0"
description = "a program that ships"
license     = "Apache-2.0"
authors     = ["Ada <ada@example.org>"]
repo        = "https://example.org/app"

[targets.app]
kind = "bin"
main = "src/main.cpp"
)cpp"}, {"src/main.cpp", R"cpp(#include <cstdio>
int main() { std::puts("app"); return 0; }
)cpp"}, {"build.mcpp", R"cpp(import mcpp;
#include <cstdio>
#include <string>
#include <string_view>
int main() {
    // Half one: unconditional. This is what makes the set knowable on a build
    // that asked for nothing.
    mcpp::provides_pack_format("zap");

    // Half two: conditional. A plain build must have no such edge.
    if (std::string_view(mcpp::pack_format()) != "zap") return 0;

    const std::string root = mcpp::manifest_dir();
    const std::string out  = std::string(mcpp::out_dir()) + "/app.zap";
    mcpp::action a;
    a.id          = "zap";
    a.role        = "artifact";
    a.description = "zap";
    a.arg((root + "/dist.sh").c_str())
     // The VERSION comes from `[package]`, not from an option the project
     // restates: a second copy drifts with nothing able to detect it.
     .arg(mcpp::package_version())
     .arg("${mcpp.stage_dir}")
     .arg(out.c_str())
     // #622 A11: the platform floor for this triple, empty on Linux. Read
     // here rather than restated, exactly as the version above is.
     .arg(mcpp::min_platform_version())
     .input("${mcpp.target_file:app}")
     .output(out.c_str())
     .submit();
    return 0;
}
)cpp"}}};
        s::write_executable(app.at("dist.sh"), R"cpp(#!/usr/bin/env bash
set -e
version="$1"; stage="$2"; out="$3"; minplat="$4"
{
  echo "version=$version"
  # #622 A11: MCPP_TARGET_MIN_PLATFORM_VERSION, unset for every other build
  # program below (they pass 3 args), so this line reads "min_platform_version="
  # for them too -- harmless, since the assertion below matches it by exact
  # line rather than by file content as a whole.
  echo "min_platform_version=$minplat"
  echo "staged:"
  ls -1 "$stage" | sort
} > "$out"
)cpp");
        auto graph_dist = [](const s::project& p) {
            const auto g = graph_of(p);
            expect(fatal(g.has_value())) << "no build.ninja";
            const auto lines = lines_of(s::read_file(*g));
            std::string l = lines.size() >= 2 ? lines[1] : "";
            if (const auto at = l.rfind(";dist="); at != std::string::npos) l = l.substr(at + 6);
            if (const auto semi = l.find(';'); semi != std::string::npos) l = l.substr(0, semi);
            return l;
        };
        auto r = app.mcpp({"build", "--release"});
        expect(fatal(e2e::succeeded(r))) << "plain build failed\n" << s::text(r);
        expect(fatal(graph_dist(app) == "none")) << "a plain build's graph is not dist=none";
        expect(fatal(!s::find_file(app.at("target"), "app.zap").has_value())) << "a plain build produced a distributable";
        r = app.mcpp({"pack", "--pkg-format", "bogus"});
        expect(fatal(failed(r))) << "--format bogus was accepted\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "unknown --pkg-format 'bogus'"))) << "the refusal does not name the value\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "available in this build: tar, zip, dir, single-file, polyglot, zap"))) << "the refusal does not name what is available\n" << s::text(r);
        expect(fatal(s::lacks(s::text(r), "Compiling app"))) << "the refusal arrived after a compile";
        r = app.mcpp({"pack", "--pkg-format", "bogus"});
        expect(fatal(r.out.empty())) << "the refusal wrote to stdout: " << r.out;
        expect(fatal(!r.err.empty())) << "the refusal said nothing on stderr";
        expect(fatal(r.status == 2)) << "the refusal exited " << r.status << ", expected 2";
        r = app.mcpp({"pack", "--pkg-format", "zap"});
        expect(fatal(e2e::succeeded(r))) << "pack --pkg-format zap failed\n" << s::text(r);
        const auto z = s::find_file(app.at("target"), "app.zap");
        expect(fatal(z.has_value())) << "--format zap produced nothing\n" << s::text(r);
        const auto zt = s::read_file(*z);
        expect(fatal(has_exact_line(zt, "version=2.5.0"))) << "the action was not told the package version\n" << zt;
        expect(fatal(has_exact_line(zt, "min_platform_version="))) << "MCPP_TARGET_MIN_PLATFORM_VERSION was not empty on Linux\n" << zt;
        expect(fatal(has_exact_line(zt, "bin"))) << "the staged tree had no bin/ when the action ran\n" << zt;
        expect(fatal(e2e::contains(s::text(r), "Packed"))) << "the produced file was not reported\n" << s::text(r);
        expect(fatal(std::ranges::any_of(entries(app.at("target/dist")), [](const fs::path& e) { return e.filename().string().ends_with(".stage-manifest"); })))
            << "no stage manifest beside the staged tree";
        bool deeper = false;
        for (const auto& e : entries(app.at("target/dist")))
            if (fs::is_directory(e)) deeper = deeper || !s::find_files(e, R"(.*\.stage-manifest)").empty();
        expect(fatal(!deeper)) << "the stage manifest landed inside the staged tree";
        fs::remove(*z);
        r = app.mcpp({"build", "--release"});
        expect(fatal(e2e::succeeded(r))) << "build after pack failed\n" << s::text(r);
        expect(fatal(graph_dist(app) == "none")) << "the graph still says dist= after a plain build";
        expect(fatal(!fs::exists(*z))) << "a plain build regenerated the distributable";

        auto variant = [&](std::string_view name, std::string_view build_mcpp) {
            fs::copy(app.dir(), tmp / name, fs::copy_options::recursive | fs::copy_options::copy_symlinks);
            s::project p{tmp / name, {{"build.mcpp", std::string(build_mcpp)}}};
            return p;
        };
        auto ungated = variant("ungated", R"cpp(import mcpp;
#include <string>
int main() {
    const std::string root = mcpp::manifest_dir();
    const std::string out  = std::string(mcpp::out_dir()) + "/app.zap";
    mcpp::action a;
    a.id = "zap"; a.role = "artifact";
    a.arg((root + "/dist.sh").c_str()).arg("x").arg("${mcpp.stage_dir}").arg(out.c_str())
     .input("${mcpp.target_file:app}").output(out.c_str()).submit();
    return 0;
}
)cpp");
        r = ungated.mcpp({"build", "--release"});
        expect(fatal(failed(r))) << "an ungated stage_dir built\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "this build is not packaging"))) << "the refusal does not say why\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "provides_pack_format"))) << "the refusal does not say what to do instead\n" << s::text(r);
        auto wrongrole = variant("wrongrole", R"cpp(import mcpp;
#include <string>
#include <string_view>
int main() {
    mcpp::provides_pack_format("zap");
    if (std::string_view(mcpp::pack_format()) != "zap") return 0;
    const std::string root = mcpp::manifest_dir();
    const std::string out  = std::string(mcpp::out_dir()) + "/app.zap";
    mcpp::action a;
    a.id = "zap"; a.role = "source";
    a.arg((root + "/dist.sh").c_str()).arg("x").arg("${mcpp.stage_dir}").arg(out.c_str())
     .output(out.c_str()).submit();
    return 0;
}
)cpp");
        r = wrongrole.mcpp({"pack", "--pkg-format", "zap"});
        expect(fatal(failed(r))) << "a source action reached the staged tree\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), R"(other than "artifact")"))) << "the role refusal does not name the role\n" << s::text(r);
        auto silent = variant("silent", R"cpp(import mcpp;
int main() { mcpp::provides_pack_format("zap"); return 0; }
)cpp");
        r = silent.mcpp({"pack", "--pkg-format", "zap"});
        expect(fatal(failed(r))) << "a format nothing claimed reported success\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "no action claimed --pkg-format 'zap'"))) << "the refusal does not name the unclaimed format\n" << s::text(r);
        auto twoshapes = variant("twoshapes", R"cpp(import mcpp;
#include <string>
#include <string_view>
int main() {
    mcpp::provides_pack_format("zap");
    const std::string root = mcpp::manifest_dir();

    // Ungated: present in both passes, so it is not the distributable.
    const std::string stamp = std::string(mcpp::out_dir()) + "/size.stamp";
    mcpp::action s;
    s.id = "size-budget"; s.role = "artifact";
    s.arg((root + "/dist.sh").c_str()).arg("stamp").arg(root.c_str()).arg(stamp.c_str())
     .input("${mcpp.target_file:app}").output(stamp.c_str());
    s.submit();

    if (std::string_view(mcpp::pack_format()) != "zap") return 0;
    // Gated, and it names NO staged tree: the program arrives through
    // ${mcpp.target_file:...} exactly as an MSI's one File row does.
    const std::string out = std::string(mcpp::out_dir()) + "/app.zap";
    mcpp::action a;
    a.id = "zap"; a.role = "artifact";
    a.arg((root + "/dist.sh").c_str()).arg("named").arg(root.c_str()).arg(out.c_str())
     .input("${mcpp.target_file:app}").output(out.c_str());
    a.submit();
    return 0;
}
)cpp");
        r = twoshapes.mcpp({"pack", "--pkg-format", "zap"});
        expect(fatal(e2e::succeeded(r))) << "a member that reads no staged tree was refused\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "app.zap"))) << "the gated action was not reported as the package\n" << s::text(r);
        expect(fatal(s::lacks(s::text(r), "size.stamp"))) << "an action predating the request was reported as the package";
        expect(fatal(s::find_file(twoshapes.at("target"), "size.stamp").has_value())) << "the ungated artifact action did not run at all";
        auto nostage = variant("nostage", R"cpp(import mcpp;
#include <string>
#include <string_view>
int main() {
    mcpp::provides_pack_format("zap");
    if (std::string_view(mcpp::pack_format()) != "zap") return 0;
    // Reads NOTHING from the staged tree: the program arrives through
    // ${mcpp.target_file:...}, which is what an `.msi` of one program does.
    const std::string root = mcpp::manifest_dir();
    const std::string out  = std::string(mcpp::out_dir()) + "/app.zap";
    mcpp::action a;
    a.id = "zap"; a.role = "artifact";
    a.arg((root + "/dist.sh").c_str()).arg("named").arg(root.c_str()).arg(out.c_str())
     .input("${mcpp.target_file:app}").output(out.c_str());
    a.submit();
    return 0;
}
)cpp");
        r = nostage.mcpp({"pack", "--pkg-format", "zap"});
        expect(fatal(e2e::succeeded(r))) << "a provider that reads no staged tree was refused\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "app.zap"))) << "the provider was not reported as the package\n" << s::text(r);
        expect(s::lacks(s::text(r), "stage-manifest")) << "a provider that names no tree gained a stage dependency";
    };
    "689 pack takes features"_test[tag{"requires:gcc"}] = [] {
        // `mcpp pack --features` activates root-package features for the pack's build passes
        // (#641): without it the feature-gated host tool is neither built nor mentioned; with it
        // the tool is built and its output reaches the packed program; a plain build without the
        // feature builds no host tool.
        s::needs({"gcc"});
        const auto tmp = temp_dir();
        const s::options linked{.where = s::home::linked};
        s::project{tmp / "toolpkg", {{"mcpp.toml", R"cpp([package]
name    = "toolpkg"
version = "0.1.0"

[targets.gen]
kind = "bin"
main = "src/gen.cpp"
)cpp"}, {"src/gen.cpp", R"cpp(#include <cstdio>
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    FILE* f = std::fopen(argv[1], "w");
    if (!f) return 3;
    std::fprintf(f, "int generated_answer() { return 41; }\n");
    std::fclose(f);
    return 0;
}
)cpp"}}};
        s::project app{tmp / "app", {{"mcpp.toml", R"cpp([package]
name    = "app"
version = "0.1.0"

[targets.app]
kind = "bin"
main = "src/main.cpp"

[features]
installer = {}

[feature-deps.installer]
toolpkg = { path = "../toolpkg", tools = ["gen"] }
)cpp"}, {"src/main.cpp", R"cpp(#include <cstdio>
int generated_answer();
int main() { std::printf("ANSWER=%d\n", generated_answer()); }
)cpp"}, {"build.mcpp", R"cpp(#include <cstdio>
#include <cstdlib>
#include <string>
import mcpp;
int main() {
    std::string out = std::string(mcpp::out_dir()) + "/gen.cpp";
    if (mcpp::has_feature("installer")) {
        const char* tool = mcpp::dep_bin("toolpkg", "gen");
        if (!tool || !*tool) { std::fprintf(stderr, "no tool path\n"); return 1; }
        std::string cmd = std::string("\"") + tool + "\" \"" + out + "\"";
        if (std::system(cmd.c_str()) != 0) { std::fprintf(stderr, "tool failed\n"); return 1; }
    } else {
        FILE* f = std::fopen(out.c_str(), "w");
        if (!f) return 1;
        std::fprintf(f, "int generated_answer() { return 0; }\n");
        std::fclose(f);
    }
    mcpp::generated(out.c_str());
}
)cpp"}}};
        auto packed_answer = [&] {
            std::optional<fs::path> exe;
            for (const auto& f : s::find_files(app.at("target/dist"), "app"))
                if (!exe && (fs::status(f).permissions() & fs::perms::owner_exec) != fs::perms::none) exe = f;
            expect(fatal(exe.has_value())) << "the pack staged no program";
            return trimmed(s::run(*exe).out);
        };
        auto tool_entries = [] {
            std::size_t n = 0;
            for (const auto& f : s::find_files(s::case_home(s::home::linked) / "build-cache", "gen.*"))
                if (f.generic_string().find("/tool/") != std::string::npos) ++n;
            return n;
        };
        auto r = app.mcpp({"pack", "--pkg-format", "dir"}, linked);
        expect(fatal(e2e::succeeded(r))) << "pack without the feature failed\n" << s::text(r);
        expect(fatal(packed_answer() == "ANSWER=0")) << "pack without the feature: expected 0\n" << s::text(r);
        expect(fatal(tool_entries() == 0u)) << "a pack without the feature built the host tool\n" << s::text(r);
        expect(fatal(s::lacks(s::text(r), "host tool"))) << "a pack without the feature mentioned the host tool";
        app.remove("target");
        r = app.mcpp({"pack", "--pkg-format", "dir", "--features", "installer"}, linked);
        expect(fatal(e2e::succeeded(r))) << "mcpp pack --features installer failed\n" << s::text(r);
        expect(fatal(s::lacks(s::text(r), "unknown option"))) << "pack did not accept --features";
        expect(fatal(s::has_line(s::text(r), "Building.*host tool"))) << "the pack with the feature did not build the tool\n" << s::text(r);
        expect(fatal(packed_answer() == "ANSWER=41")) << "pack with the feature: expected 41\n" << s::text(r);
        app.remove("target");
        fs::remove_all(s::case_home(s::home::linked) / "build-cache");
        r = app.mcpp({"build"}, linked);
        expect(fatal(e2e::succeeded(r))) << "the plain build failed\n" << s::text(r);
        expect(tool_entries() == 0u) << "a plain build without the feature built the host tool\n" << s::text(r);
    };
    "722 pack strips what the graph built"_test[tag{"requires:pack"}][tag{"requires:python3"}] = [] {
        // `mcpp pack` strips what the graph built on every row that strips, and says so only
        // when it does (#649 E5): the program and the graph-built shared library lose their
        // symbol and debug tables and keep their exports; --no-strip reaches the library;
        // --debug-symbols separates its debug information; a dispatched format's program reads
        // the strip decision; on the Android row (where the NDK is installed) the program, the
        // graph's library and the NDK runtime are stripped.
        s::needs({"pack", "python3"});
        const auto tmp = temp_dir();
        struct facts { int symtab = 0, debug = 0; bool debuglink = false; std::vector<std::string> dynsym; };
        auto elf_facts = [](const fs::path& f) {
            facts out;
            const auto h = s::read_header(f);
            expect(fatal(h.kind == s::format::elf && h.wide)) << "not ELF64: " << f.string();
            for (const auto& sec : s::elf_sections(f)) {
                out.symtab += sec.name == ".symtab";
                out.debug += sec.name.starts_with(".debug_");
                out.debuglink = out.debuglink || sec.name == ".gnu_debuglink";
            }
            out.dynsym = s::elf_symbols(f, ".dynsym");
            return out;
        };
        auto exports = [](const facts& f, std::string_view name) {
            return std::ranges::any_of(f.dynsym, [&](const std::string& n) { return n.find(name) != std::string::npos; });
        };
        auto only = [](const fs::path& x, std::string_view tail) {
            for (const auto& top : entries(x))
                if (fs::exists(top / tail)) return top / tail;
            return x / "*" / tail;
        };
        s::project{tmp / "dep", {{"mcpp.toml", R"cpp([package]
name      = "dep"
namespace = "spike"
version   = "0.1.0"
standard  = "c++20"

[targets.dep]
kind = "shared"

[build]
sources = ["src/*.cpp"]
)cpp"}, {"src/dep.cpp", R"cpp(#include <string>
[[gnu::visibility("default")]] int dep_answer() { return static_cast<int>(std::to_string(42).size()) + 40; }
)cpp"}}};
        s::project host{tmp / "hostapp", {{"mcpp.toml", R"cpp([package]
name     = "hostapp"
version  = "0.1.0"
standard = "c++20"

[dependencies]
spike.dep = { path = "../dep" }

[targets.hostapp]
kind = "bin"
main = "src/main.cpp"
)cpp"}, {"src/main.cpp", "int dep_answer();\nint main() { return dep_answer() == 42 ? 0 : 1; }\n"}}};
        const auto tarball = host.at("target/dist/hostapp-0.1.0-x86_64-linux-gnu.tar.gz");
        auto unpack = [&](std::string_view into) {
            host.remove(into);
            s::tar_extract(tarball, host.at(into));
            return host.at(into);
        };
        auto r = host.mcpp({"pack", "--pkg-format", "tar"});
        expect(fatal(e2e::succeeded(r))) << "A: pack failed\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "Packing hostapp v0.1.0 (vendored, stripped)"))) << "A: the Packing line\n" << s::text(r);
        auto xa = unpack("xa");
        const auto fa = elf_facts(only(xa, "lib/libdep.so"));
        const auto fp = elf_facts(only(xa, "bin/hostapp"));
        expect(fatal(fa.symtab == 0 && fa.debug == 0)) << "A: lib/libdep.so is not stripped: symtab=" << fa.symtab << " debug=" << fa.debug;
        expect(fatal(exports(fa, "dep_answer"))) << "A: lib/libdep.so lost its export";
        expect(fatal(fp.symtab == 0 && fp.debug == 0)) << "A: bin/hostapp is not stripped: symtab=" << fp.symtab << " debug=" << fp.debug;
        auto run = s::run(only(xa, "hostapp"));
        expect(fatal(e2e::succeeded(run))) << "A: the unpacked program does not run\n" << s::text(run);

        r = host.mcpp({"pack", "--pkg-format", "tar", "--no-strip"});
        expect(fatal(e2e::succeeded(r))) << "B: pack failed\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "Packing hostapp v0.1.0 (vendored)"))) << "B: the Packing line\n" << s::text(r);
        const auto fb = elf_facts(only(unpack("xb"), "lib/libdep.so"));
        expect(fatal(fb.symtab == 1)) << "B: --no-strip did not reach lib/libdep.so: symtab=" << fb.symtab;

        r = host.mcpp({"pack", "--pkg-format", "tar", "--debug-symbols", (tmp / "dbg").string()});
        expect(fatal(e2e::succeeded(r))) << "C: pack failed\n" << s::text(r);
        expect(fatal(fs::exists(tmp / "dbg/libdep.so.debug") && fs::file_size(tmp / "dbg/libdep.so.debug") > 0)) << "C: no libdep.so.debug\n" << s::text(r);
        expect(fatal(elf_facts(only(unpack("xc"), "lib/libdep.so")).debuglink)) << "C: lib/libdep.so names no debug file";

        host.write("build.mcpp", R"cpp(import mcpp;
#include <cstdio>
#include <string>
#include <string_view>
int main() {
    mcpp::provides_pack_format("probe");
    if (std::string_view(mcpp::pack_format()) != "probe") return 0;
    const std::string out = std::string(mcpp::out_dir()) + "/probe.txt";
    const std::string text = std::string("strip=") + mcpp::pack_strip()
                           + " debug=" + mcpp::pack_debug_symbols_dir();
    mcpp::action a;
    a.id = "probe";
    a.role = "artifact";
    a.description = "probe";
    a.arg("/bin/sh").arg("-c").arg(("printf '%s' '" + text + "' > " + out).c_str())
     .input("${mcpp.target_file:hostapp}")
     .output(out.c_str())
     .submit();
    return 0;
}
)cpp");
        r = host.mcpp({"pack", "--pkg-format", "probe", "--debug-symbols", (tmp / "dbg2").string()});
        expect(fatal(e2e::succeeded(r))) << "D: pack failed\n" << s::text(r);
        const auto probe = s::find_file(host.at("target"), "probe.txt");
        expect(fatal(probe.has_value())) << "D: no probe.txt\n" << s::text(r);
        expect(fatal(s::read_file(*probe) == "strip=1 debug=" + (tmp / "dbg2").string())) << "D: the program read '" << s::read_file(*probe) << "'";
        r = host.mcpp({"pack", "--pkg-format", "probe", "--no-strip"});
        expect(fatal(e2e::succeeded(r))) << "D: pack --no-strip failed\n" << s::text(r);
        expect(fatal(s::read_file(*probe) == "strip=0 debug=")) << "D: under --no-strip the program read '" << s::read_file(*probe) << "'";

        if (s::glob(s::shared_home() / "registry/data/xpkgs", "xim-x-android-ndk/*").empty()) return;  // E: no NDK here
        s::project app{tmp / "app", {{"mcpp.toml", R"cpp([package]
name     = "app"
version  = "0.1.0"
standard = "c++20"

[dependencies]
spike.dep = { path = "../dep" }

[targets.app]
kind = "app"
main = "src/main.cpp"

[target.x86_64-linux-android]
min_api_level = 23
)cpp"}, {"src/main.cpp", R"cpp(#include <string>
int dep_answer();
[[gnu::visibility("default")]] int app_entry() { return dep_answer() + static_cast<int>(std::to_string(1).size()); }
int main() { return app_entry() == 43 ? 0 : 1; }
)cpp"}}};
        r = app.mcpp({"pack", "--target", "x86_64-linux-android", "--pkg-format", "tar"});
        expect(fatal(e2e::succeeded(r))) << "E: pack failed\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "Packing app v0.1.0 (vendored, stripped)"))) << "E: the Packing line\n" << s::text(r);
        s::tar_extract(app.at("target/dist/app-0.1.0-x86_64-linux-android.tar.gz"), app.at("xe"));
        for (std::string_view f : {"libapp.so", "libdep.so", "libc++_shared.so"}) {
            const auto fe = elf_facts(only(app.at("xe"), std::format("lib/{}", f)));
            expect(fatal(fe.symtab == 0 && fe.debug == 0)) << "E: lib/" << f << " is not stripped";
        }
        expect(fatal(exports(elf_facts(only(app.at("xe"), "lib/libapp.so")), "app_entry"))) << "E: libapp.so lost app_entry";
        expect(exports(elf_facts(only(app.at("xe"), "lib/libdep.so")), "dep_answer")) << "E: libdep.so lost dep_answer";
    };
    "723 pack reports what it produced"_test[tag{"requires:pack"}][tag{"requires:python3"}] = [] {
        // `mcpp pack --message-format json` reports what it produced in one envelope on stdout
        // (the archive, its legs, the staged tree; a directory as a directory); `pack` takes the
        // profile shorthands with --profile winning, as run and build do; an unknown message
        // format is refused with exit 2; the protocol document declares the kind and command.
        s::needs({"pack", "python3"});
        const auto tmp = temp_dir();
        s::project app{tmp / "app", {{"mcpp.toml", R"cpp([package]
name    = "app"
version = "0.3.0"

[targets.app]
kind = "bin"
main = "src/main.cpp"
)cpp"}, {"src/main.cpp", "#include <cstdio>\nint main() { std::puts(\"app ran\"); return 0; }\n"}}};
        auto r = app.mcpp({"pack", "--pkg-format", "tar", "--message-format", "json"});
        expect(fatal(e2e::succeeded(r))) << "A: pack failed\n" << s::text(r);
        nlohmann::json e;
        try { e = nlohmann::json::parse(r.out); } catch (const std::exception& x) { expect(fatal(false)) << "A: the whole of stdout is not one document: " << x.what() << "\n" << r.out; }
        expect(fatal(e["kind"] == "mcpp.pack" && e["kindVersion"] == 1)) << "A: the envelope\n" << r.out;
        expect(fatal(std::ranges::contains(e["effects"], nlohmann::json("write-project")))) << "A: effects\n" << r.out;
        auto& arts = e["data"]["artifacts"];
        expect(fatal(arts.size() == 1u)) << "A: artifacts\n" << r.out;
        auto& a = arts[0];
        const std::string path = a["path"];
        expect(fatal(fs::path(path).is_absolute() && fs::is_regular_file(path))) << "A: the artifact path\n" << r.out;
        expect(fatal(path.ends_with(".tar.gz") && a["type"] == "file" && a["format"] == "tar")) << "A: the artifact\n" << r.out;
        expect(fatal(a["targets"].size() == 1u && a["targets"][0].get<std::string>().ends_with("linux-gnu"))) << "A: the targets\n" << r.out;
        auto& st = e["data"]["stage"];
        expect(fatal(fs::is_directory(st["dir"].get<std::string>()) && fs::is_regular_file(st["manifest"].get<std::string>()))) << "A: the stage\n" << r.out;
        expect(fatal(st["closure"] == "walked")) << "A: the closure\n" << r.out;
        expect(fatal(e["diagnostics"].empty())) << "A: diagnostics\n" << r.out;
        expect(fatal(e2e::contains(r.err, "Packed"))) << "A: the human lines did not go to stderr\n" << r.err;
        r = app.mcpp({"pack", "--pkg-format", "dir", "--message-format", "json"});
        expect(fatal(e2e::succeeded(r))) << "B: pack failed\n" << s::text(r);
        auto b = nlohmann::json::parse(r.out)["data"]["artifacts"][0];
        expect(fatal(b["type"] == "directory" && fs::is_directory(b["path"].get<std::string>()) && b["format"] == "dir")) << "B: the envelope\n" << r.out;
        auto built_dev = [&] {
            const auto g = s::glob(app.at("target"), "*-linux-gnu/*/build.ninja");
            expect(fatal(!g.empty())) << "no target/*-linux-gnu/*/build.ninja";
            return e2e::contains(s::read_file(g.front()), " -O0");
        };
        app.remove("target");
        r = app.mcpp({"pack", "--release", "--pkg-format", "tar"});
        expect(fatal(e2e::succeeded(r))) << "C: pack --release was refused\n" << s::text(r);
        expect(fatal(!built_dev())) << "C: pack --release built dev";
        app.remove("target");
        r = app.mcpp({"pack", "--dev", "--pkg-format", "tar"});
        expect(fatal(e2e::succeeded(r))) << "C: pack --dev was refused\n" << s::text(r);
        expect(fatal(built_dev())) << "C: pack --dev did not build dev";
        app.remove("target");
        r = app.mcpp({"pack", "--profile", "release", "--dev", "--pkg-format", "tar"});
        expect(fatal(e2e::succeeded(r))) << "C: pack --profile --dev failed\n" << s::text(r);
        expect(fatal(!built_dev())) << "C: --dev won over --profile release on pack";
        r = app.mcpp({"run", "--profile", "dev", "--release"});
        expect(fatal(e2e::succeeded(r))) << "D: run failed\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "Finished dev"))) << "D: run --profile dev --release did not build dev\n" << s::text(r);
        r = app.mcpp({"build", "--profile", "dev", "--release"});
        expect(fatal(e2e::succeeded(r))) << "D: build failed\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "Finished dev"))) << "D: build --profile dev --release did not build dev\n" << s::text(r);
        r = app.mcpp({"pack", "--pkg-format", "tar", "--message-format", "yaml"});
        expect(fatal(r.status == 2)) << "E: --message-format yaml exited " << r.status << "\n" << r.err;
        expect(fatal(e2e::contains(r.err, "unknown --message-format 'yaml'"))) << "E: the refusal does not name the value\n" << r.err;
        r = app.mcpp({"--protocol-version"});
        auto p = nlohmann::json::parse(r.out);
        expect(fatal(p["kinds"]["mcpp.pack"] == 1)) << "F: the protocol document\n" << r.out;
        auto& fx = p["commands"]["pack"]["effects"];
        expect(std::ranges::contains(fx, nlohmann::json("write-project")) && std::ranges::contains(fx, nlohmann::json("exec-build-script")))
            << "F: the protocol document\n" << r.out;
    };
    "841 pack summarizes many outputs"_test[tag{"requires:python3"}] = [] {
        // A format that submits one output per file is reported one `Packed` line per
        // directory with its file count (and a single file by name); --verbose names every
        // file; the JSON report lists every artifact.
        s::needs({"python3"});
        const auto tmp = temp_dir();
        s::project app{tmp / "app", {{"mcpp.toml", R"cpp([package]
name = "app"
version = "1.0.0"

[targets.app]
kind = "bin"
main = "src/main.cpp"
)cpp"}, {"src/main.cpp", "int main() { return 0; }\n"}, {"build.mcpp", R"cpp(import mcpp;
#include <string>
#include <string_view>
int main() {
    mcpp::provides_pack_format("tree");
    if (std::string_view(mcpp::pack_format()) != "tree") return 0;
    const std::string out = std::string(mcpp::out_dir()) + "/tree";
    auto copy = [&](const std::string& id, const std::string& dst) {
        mcpp::action a;
        a.id = id.c_str();
        a.role = "artifact";
        a.arg("${mcpp.self}").arg("stage").arg("--verify").arg("content")
         .arg("--output").arg(dst.c_str()).arg("${mcpp.target_file:app}")
         .input("${mcpp.target_file:app}")
         .output(dst.c_str())
         .submit();
    };
    for (int i = 0; i < 6; ++i) {
        copy("a" + std::to_string(i), out + "/bin/f" + std::to_string(i));
        copy("b" + std::to_string(i), out + "/data/f" + std::to_string(i));
    }
    copy("m", out + "/manifest");
    return 0;
}
)cpp"}}};
        auto r = app.mcpp({"pack", "--pkg-format", "tree"});
        expect(fatal(e2e::succeeded(r))) << "pack failed\n" << s::text(r);
        expect(fatal(s::count_lines(s::text(r), "Packed") == 3u)) << "P1: " << s::count_lines(s::text(r), "Packed") << " Packed lines, expected 3\n" << s::text(r);
        expect(fatal(s::has_line(s::text(r), R"(Packed .*tree[/\\]bin \(6 files\))"))) << "P1: bin/ is not summarised\n" << s::text(r);
        expect(fatal(s::has_line(s::text(r), R"(Packed .*tree[/\\]data \(6 files\))"))) << "P1: data/ is not summarised\n" << s::text(r);
        expect(fatal(s::has_line(s::text(r), R"(Packed .*tree[/\\]manifest$)"))) << "P1: the single file is not named\n" << s::text(r);
        r = app.mcpp({"pack", "--pkg-format", "tree", "--verbose"});
        expect(fatal(e2e::succeeded(r))) << "pack --verbose failed\n" << s::text(r);
        expect(fatal(s::count_lines(s::text(r), "Packed") == 13u)) << "P2: " << s::count_lines(s::text(r), "Packed") << " Packed lines under --verbose, expected 13\n" << s::text(r);
        r = app.mcpp({"pack", "--pkg-format", "tree", "--message-format", "json"});
        expect(fatal(e2e::succeeded(r))) << "pack json failed\n" << r.err;
        auto a = nlohmann::json::parse(r.out)["data"]["artifacts"];
        expect(a.size() == 13u) << "P3: " << a.size() << " artifacts";
    };
    "871 a pack states its build and every drive takes the job count"_test[tag{"requires:pack"}][tag{"requires:python3"}] = [] {
        // `mcpp pack` states the build it performs (Compiling, then Finished before Packing),
        // its JSON output is one document, and build, test and pack each run at most
        // `[build] jobs` (1) compiles at once, read from the build's .ninja_log (mcpp#753).
        s::needs({"pack", "python3"});
        const auto tmp = temp_dir();
        s::project app{tmp / "app", {{"mcpp.toml", R"cpp([package]
name    = "app"
version = "0.1.0"

[build]
jobs = 1

[targets.app]
kind = "bin"
main = "src/main.cpp"
)cpp"}, {"src/main.cpp", R"cpp(#include <cstdio>
unsigned long long value1(); unsigned long long value2();
unsigned long long value3(); unsigned long long value4();
int main() { std::printf("%llu\n", value1() ^ value2() ^ value3() ^ value4()); }
)cpp"}, {"tests/t.cpp", R"cpp(int main() { return 0; }
)cpp"}}};
        for (int i = 1; i <= 4; ++i) app.write(std::format("src/u{}.cpp", i), std::format(R"cpp(constexpr unsigned long long spin{0}() {{
    unsigned long long x = {0};
    for (unsigned k = 0; k < 600; ++k)
        for (unsigned j = 0; j < 600; ++j) x = x * 6364136223846793005ull + 1442695040888963407ull;
    return x;
}}
unsigned long long value{0}() {{ constexpr auto v = spin{0}(); return v; }}
)cpp", i));
        auto no_overlap = [&](std::string_view label) {
            std::optional<fs::path> log;
            for (const auto& f : s::find_files(app.at("target"), R"(\.ninja_log)"))
                if (!log && f.generic_string().find("/.build-mcpp/") == std::string::npos) log = f;
            expect(fatal(log.has_value())) << label << ": no .ninja_log was written";
            // Each edge on the log's absolute clock: it ended when its output was written (the
            // third column, in nanoseconds) and began its duration (end - start, ms) before. A
            // command may run ninja twice (mcpp test: the program, then the tests), each run
            // with its own relative clock, so the relative columns alone do not compare across runs.
            struct edge { long long start, end; std::string out; };
            std::vector<edge> edges;
            const auto lines = lines_of(s::read_file(*log));
            for (std::size_t i = 1; i < lines.size(); ++i) {
                std::vector<std::string> f;
                for (auto part : std::views::split(lines[i], '\t')) f.emplace_back(part.begin(), part.end());
                if (f.size() >= 4 && std::regex_search(f[3], std::regex{R"(\.o(bj)?$)"})) {
                    const long long end = std::stoll(f[2]) / 1000000, duration = std::stoll(f[1]) - std::stoll(f[0]);
                    edges.push_back({end - duration, end, f[3]});
                }
            }
            expect(fatal(edges.size() >= 4u)) << label << ": expected at least four compiles in .ninja_log, found " << edges.size();
            // The output's write precedes its process's exit by a few milliseconds, which the
            // duration counts, so two compiles run one after the other may read as touching
            // within that margin; two run at once overlap by most of a compile (~300 ms here).
            constexpr long long margin = 50;
            for (std::size_t i = 0; i < edges.size(); ++i)
                for (std::size_t j = i + 1; j < edges.size(); ++j)
                    expect(fatal(!(edges[i].start + margin < edges[j].end && edges[j].start + margin < edges[i].end)))
                        << label << ": " << edges[i].out << " [" << edges[i].start << "," << edges[i].end << "] overlaps "
                        << edges[j].out << " [" << edges[j].start << "," << edges[j].end << "]";
        };
        auto r = app.mcpp({"pack", "--pkg-format", "tar"});
        expect(fatal(e2e::succeeded(r))) << "A: pack failed\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "Compiling app v0.1.0"))) << "A: the pack's build wrote no Compiling line\n" << s::text(r);
        const auto fin = first_line_with(s::text(r), "Finished"), pk = first_line_with(s::text(r), "Packing");
        expect(fatal(fin != 0 && pk != 0 && fin < pk)) << "A: Finished (line " << fin << ") does not precede Packing (line " << pk << ")\n" << s::text(r);
        app.remove("target");
        r = app.mcpp({"pack", "--pkg-format", "tar", "--message-format", "json"});
        expect(fatal(e2e::succeeded(r))) << "B: pack failed\n" << r.err;
        expect(fatal(nlohmann::json::accept(r.out))) << "B: standard output is not one JSON document\n" << r.out;
        app.remove("target");
        r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "C: build failed\n" << s::text(r);
        no_overlap("C build");
        app.remove("target");
        r = app.mcpp({"test"});
        expect(fatal(e2e::succeeded(r))) << "C: test failed\n" << s::text(r);
        no_overlap("C test");
        app.remove("target");
        r = app.mcpp({"pack", "--pkg-format", "tar"});
        expect(fatal(e2e::succeeded(r))) << "C: pack failed\n" << s::text(r);
        no_overlap("C pack");
    };
    "954 pack has three axes"_test[tag{"requires:unix-shell"}][tag{"requires:python3"}] = [] {
        // `mcpp pack` has three axes (SPEC-014 §1): tar compressions and zip each hold one top
        // directory (zstd a zstd frame, zip deflated entries); mcpp-package.json describes every
        // file with its digest; the JSON envelope states the axes and the digest; [pack] format
        // and compress are defaults; impossible combinations are refused before a build;
        // --format is the earlier spelling; [pack] include/exclude stage resources; [pack] sign
        // signs each program and the digests are the signed files'; -o with dots names a directory.
        s::needs({"unix-shell", "python3"});
        const auto tmp = temp_dir();
        s::project app{tmp / "app", {{"mcpp.toml", "[package]\nname    = \"axes\"\nversion = \"0.1.0\"\n"}, {"src/main.cpp", "int main() { return 0; }\n"}}};
        const auto dist = app.at("target/dist");
        for (std::string_view c : {"zstd", "xz", "none"}) {
            auto r = app.mcpp({"pack", "--compress", std::string(c)});
            expect(fatal(e2e::succeeded(r))) << "A: --compress " << c << "\n" << s::text(r);
        }
        auto r = app.mcpp({"pack", "--pkg-format", "zip"});
        expect(fatal(e2e::succeeded(r))) << "A: --pkg-format zip\n" << s::text(r);
        auto one_top = [](const std::vector<std::string>& names) {
            std::set<std::string> tops;
            for (const auto& n : names) tops.insert(n.substr(0, n.find('/')));
            return tops.size() == 1u;
        };
        const auto xz = first_ending(dist, ".tar.xz");
        expect(fatal(xz && one_top(s::tar_list(*xz)))) << "A: the .tar.xz";
        const auto plain = first_ending(dist, ".tar");
        expect(fatal(plain && one_top(s::tar_list(*plain)))) << "A: the .tar";
        const auto zst = first_ending(dist, ".tar.zst");
        expect(fatal(zst && s::read_file(*zst).starts_with(std::string("\x28\xb5\x2f\xfd", 4)))) << "A: not a zstd frame";
        const auto zip = first_ending(dist, ".zip");
        expect(fatal(zip.has_value())) << "A: no zip";
        const auto ze = s::zip_entries(*zip);
        std::vector<std::string> znames;
        for (const auto& z : ze) znames.push_back(z.name);
        expect(fatal(one_top(znames))) << "A: the zip holds more than one top directory";
        expect(fatal(std::ranges::any_of(ze, [](const s::zip_entry& z) { return z.method == 8; }))) << "A: nothing deflated";

        std::optional<fs::path> root;
        for (const auto& e : entries(dist))
            if (!root && fs::is_directory(e)) root = e;
        expect(fatal(root.has_value())) << "B: no package directory";
        auto m = s::json_file(*root / "mcpp-package.json");
        expect(fatal(m["schema"] == 1 && m["name"] == "axes" && m["version"] == "0.1.0")) << "B: " << m.dump();
        expect(fatal(m["mode"] == "vendored" && m["platform"].is_null() && m["legs"].size() == 1u)) << "B: " << m.dump();
        std::set<std::string> listed, on_disk;
        for (auto& f : m["files"]) listed.insert(f["path"].get<std::string>());
        for (const auto& e : fs::recursive_directory_iterator(*root))
            if (e.is_regular_file() && !e.is_symlink()) {
                const auto rel = fs::relative(e.path(), *root).generic_string();
                if (rel != "mcpp-package.json") on_disk.insert(rel);
            }
        expect(fatal(on_disk == listed)) << "B: the files listed are not the files on disk";
        for (auto& f : m["files"])
            expect(fatal(f["sha256"] == s::sha256(s::read_file(*root / f["path"].get<std::string>())))) << "B: the digest of " << f["path"].get<std::string>();
        expect(fatal(std::ranges::any_of(m["files"], [](const nlohmann::json& f) { return f.value("role", std::string{}) == "program"; }))) << "B: no program";

        r = app.mcpp({"pack", "--compress", "zstd", "--message-format", "json"});
        expect(fatal(e2e::succeeded(r))) << "C: json\n" << r.err;
        auto env = nlohmann::json::parse(r.out)["data"];
        expect(fatal(env["pkgFormat"] == "tar" && env["mode"] == "vendored" && !env["linkage"].is_null() && env["linkage"] != "" && env["linkage"] != false))
            << "C: the envelope\n" << r.out;
        expect(fatal(env["legs"].size() == 1u)) << "C: the legs\n" << r.out;
        auto a = env["artifacts"][0];
        expect(fatal(a["sha256"] == s::sha256(s::read_file(a["path"].get<std::string>())))) << "C: the digest\n" << r.out;

        app.write("mcpp.toml", app.read("mcpp.toml") + R"cpp(
[pack]
format   = "tar"
compress = "xz:9"
)cpp");
        app.remove("target/dist");
        r = app.mcpp({"pack"});
        expect(fatal(e2e::succeeded(r))) << "D: pack with [pack] defaults\n" << s::text(r);
        expect(fatal(first_ending(dist, ".tar.xz").has_value())) << "D: [pack] compress was not the default\n" << s::text(r);

        auto refuse = [&](std::string_view label, std::string_view want, std::vector<std::string> args) {
            args.insert(args.begin(), "pack");
            auto rr = app.mcpp(args);
            expect(fatal(failed(rr))) << "E: " << label << " was accepted\n" << s::text(rr);
            expect(fatal(e2e::contains(s::text(rr), want))) << "E: " << label << ": the refusal does not say '" << want << "'\n" << s::text(rr);
            expect(fatal(s::lacks(s::text(rr), "Compiling"))) << "E: " << label << " was refused after compiling";
        };
        refuse("single-file with --mode system", "single-file", {"--pkg-format", "single-file", "--mode", "system"});
        refuse("--compress on a directory", "writes none", {"--pkg-format", "dir", "--compress", "zstd"});
        refuse("zstd in a zip", "deflated", {"--pkg-format", "zip", "--compress", "zstd"});
        refuse("--systems without loaders", "systems", {"--systems", "linux"});

        r = app.mcpp({"pack", "--format", "dir"});
        expect(fatal(e2e::succeeded(r))) << "F: --format dir\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "earlier spelling of --pkg-format"))) << "F: no warning for the earlier spelling\n" << s::text(r);

        app.write("share/data/keep.txt", "kept\n");
        app.write("share/data/drop.tmp", "dropped\n");
        app.write("mcpp.toml", app.read("mcpp.toml") + R"cpp(include = ["share/**"]
exclude = ["share/**/*.tmp"]
)cpp");
        r = app.mcpp({"pack", "--pkg-format", "dir", "-o", "inc"});
        expect(fatal(e2e::succeeded(r))) << "G: pack with [pack] include\n" << s::text(r);
        expect(fatal(fs::is_regular_file(dist / "inc/share/data/keep.txt"))) << "G: the included file is not staged\n" << s::text(r);
        expect(fatal(!fs::exists(dist / "inc/share/data/drop.tmp"))) << "G: the excluded file was staged";
        std::map<std::string, std::string> roles;
        for (auto& f : s::json_file(dist / "inc/mcpp-package.json")["files"]) roles[f["path"].get<std::string>()] = f["role"].get<std::string>();
        expect(fatal(roles["share/data/keep.txt"] == "resource")) << "G: the metadata";

        app.write("mcpp.toml", app.read("mcpp.toml") + R"cpp(sign = ["sh", "-c", "printf SIGNED >> \"$0\"", "{}"]
)cpp");
        r = app.mcpp({"pack", "--pkg-format", "dir", "-o", "signed"});
        expect(fatal(e2e::succeeded(r))) << "H: pack with [pack] sign\n" << s::text(r);
        const auto signed_root = dist / "signed";
        auto sm = s::json_file(signed_root / "mcpp-package.json");
        bool programs = false;
        for (auto& f : sm["files"]) {
            const auto path = f["path"].get<std::string>();
            const auto data = s::read_file(signed_root / path);
            if (f["role"] == "program") {
                programs = true;
                expect(fatal(data.ends_with("SIGNED"))) << "H: " << path << " was not signed";
                expect(fatal(f["sha256"] == s::sha256(data))) << "H: " << path << ": the digest is the unsigned file's";
            }
            if (f["role"] == "resource" || f["role"] == "metadata")
                expect(fatal(!data.ends_with("SIGNED"))) << "H: " << path << " was signed";
        }
        expect(fatal(programs)) << "H: no program in the signed tree";
        r = app.mcpp({"pack", "--pkg-format", "dir", "-o", "axes-0.1.0-named"});
        expect(fatal(e2e::succeeded(r))) << "H: pack -o with dots\n" << s::text(r);
        expect(fs::is_directory(dist / "axes-0.1.0-named")) << "H: -o axes-0.1.0-named did not name the directory";
    };
    "1008 a provided format is told the compression"_test[tag{"requires:pack"}] = [] {
        // A format a package provides is told the compression the command or `[pack] compress`
        // states (mcpp::pack_compress()), nothing when none is stated; a directory writes no
        // container and --compress on it is refused.
        s::needs({"pack"});
        const auto tmp = temp_dir();
        s::project app{tmp / "app", {{"mcpp.toml", R"cpp([package]
name     = "app"
version  = "0.1.0"
standard = "c++20"

[targets.app]
kind = "bin"
main = "src/main.cpp"
)cpp"}, {"src/main.cpp", "int main() { return 0; }\n"}, {"build.mcpp", R"cpp(import mcpp;
#include <string>
#include <string_view>
int main() {
    mcpp::provides_pack_format("probe");
    if (std::string_view(mcpp::pack_format()) != "probe") return 0;
    const std::string out = std::string(mcpp::out_dir()) + "/probe.txt";
    const std::string text = std::string("compress=") + mcpp::pack_compress();
    mcpp::action a;
    a.id = "probe";
    a.role = "artifact";
    a.description = "probe";
    a.arg("/bin/sh").arg("-c").arg(("printf '%s' '" + text + "' > " + out).c_str())
     .input("${mcpp.target_file:app}")
     .output(out.c_str())
     .submit();
    return 0;
}
)cpp"}}};
        auto r = app.mcpp({"pack", "--pkg-format", "probe", "--compress", "gzip:9"});
        expect(fatal(e2e::succeeded(r))) << "A: pack failed\n" << s::text(r);
        const auto probe = s::find_file(app.at("target"), "probe.txt");
        expect(fatal(probe.has_value())) << "A: no probe.txt\n" << s::text(r);
        expect(fatal(s::read_file(*probe) == "compress=gzip:9")) << "A: the program read '" << s::read_file(*probe) << "'";
        r = app.mcpp({"pack", "--pkg-format", "probe"});
        expect(fatal(e2e::succeeded(r))) << "B: pack failed\n" << s::text(r);
        expect(fatal(s::read_file(*probe) == "compress=")) << "B: with none stated the program read '" << s::read_file(*probe) << "'";
        app.write("mcpp.toml", app.read("mcpp.toml") + "\n[pack]\ncompress = \"zstd\"\n");
        r = app.mcpp({"pack", "--pkg-format", "probe"});
        expect(fatal(e2e::succeeded(r))) << "C: pack failed\n" << s::text(r);
        expect(fatal(s::read_file(*probe) == "compress=zstd")) << "C: under [pack] compress the program read '" << s::read_file(*probe) << "'";
        r = app.mcpp({"pack", "--pkg-format", "dir", "--compress", "gzip"});
        expect(fatal(failed(r))) << "D: a directory was compressed\n" << s::text(r);
        expect(e2e::contains(s::text(r), "writes none")) << "D: the refusal\n" << s::text(r);
    };
}};

}  // namespace

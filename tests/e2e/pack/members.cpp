// Packs over workspace members and targets: a workspace root, a named target, -p, a
// dependency's program shipped with the consumer, and a pack over several members (one plan,
// the refusals before a compile, the report, per-member resolution and failure) (formerly
// tests/e2e/249, 250, 801, 829, 867, 868, 869, 870).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

// The entries of `dir`, sorted (`find <dir> -maxdepth 1`, `ls -d <dir>/*`).
std::vector<fs::path> entries(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (fs::is_directory(dir, ec))
        for (const auto& e : fs::directory_iterator(dir)) out.push_back(e.path());
    std::ranges::sort(out);
    return out;
}

// The first entry of `dir` named `<prefix>*<suffix>` (a directory when `dir_only`).
std::optional<fs::path> first_named(const fs::path& dir, std::string_view prefix, std::string_view suffix = "", bool dir_only = false) {
    for (const auto& e : entries(dir)) {
        const auto n = e.filename().string();
        if (n.starts_with(prefix) && n.ends_with(suffix) && (!dir_only || fs::is_directory(e))) return e;
    }
    return std::nullopt;
}

// Every file under `root` whose path ends with `tail` (`find <root> -path '*<tail>'`), sorted.
std::vector<fs::path> paths_ending(const fs::path& root, std::string_view tail) {
    std::vector<fs::path> out;
    for (const auto& f : s::find_files(root, ".*"))
        if (f.generic_string().ends_with(tail)) out.push_back(f);
    return out;
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

// `diff -r a b` finds nothing: the same paths, and each file the same bytes.
bool same_tree(const fs::path& a, const fs::path& b) {
    if (s::listing(a) != s::listing(b)) return false;
    for (const auto& rel : s::listing(a))
        if (fs::is_regular_file(a / rel) && s::read_file(a / rel) != s::read_file(b / rel)) return false;
    return true;
}

// `cp -R from to`.
void copy_tree(const fs::path& from, const fs::path& to) {
    fs::create_directories(to.parent_path());
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::copy_symlinks);
}

// No build of `dir` compiled anything: no .ninja_log outside the host build programs' trees.
bool compiled_nothing(const fs::path& dir) {
    for (const auto& f : s::find_files(dir, R"(\.ninja_log)"))
        if (f.generic_string().find("/.build-mcpp/host/") == std::string::npos) return false;
    return true;
}

E2E members{"members", [] {
    "249 pack workspace root unchanged"_test = [] {
        // `mcpp pack` in a virtual workspace root still packs the member's program.
        s::project ws{s::files{{"mcpp.toml", R"cpp([workspace]
members = ["libs/mathcore", "apps/hello"]
)cpp"}, {"libs/mathcore/mcpp.toml", R"cpp([package]
name    = "mathcore"
version = "0.1.0"
)cpp"}, {"libs/mathcore/src/mathcore.cppm", R"cpp(export module mathcore;
export int core_answer() { return 42; }
)cpp"},
                               {"apps/hello/mcpp.toml", R"cpp([package]
name    = "hello"
version = "0.1.0"
[dependencies]
mathcore = { path = "../../libs/mathcore" }
)cpp"}, {"apps/hello/src/main.cpp", R"cpp(#include <cstdio>
import mathcore;
int main() { std::printf("ok=%d\n", core_answer()); return 0; }
)cpp"}}};
        auto r = ws.mcpp({"pack", "--mode", "system"});
        expect(fatal(e2e::succeeded(r))) << "packing from a virtual workspace root stopped working\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "Packed"))) << "no archive reported\n" << s::text(r);
        expect(!s::find_files(ws.dir(), R"(hello-0\.1\.0-.*\.(tar\.gz|zip))").empty()) << "the member's archive was not produced\n" << s::text(r);
    };
    "250 pack names the target"_test = [] {
        // `mcpp pack <name>` packs the target it was given, and refuses a name it cannot pack,
        // listing the available targets.
        s::project two{s::files{{"src/alpha.cpp", R"cpp(#include <cstdio>
int main() { std::printf("alpha\n"); return 0; }
)cpp"}, {"src/beta.cpp", R"cpp(#include <cstdio>
int main() { std::printf("beta\n"); return 0; }
)cpp"}, {"mcpp.toml", R"cpp([package]
name    = "two"
version = "0.1.0"

[targets.alpha]
kind = "bin"
main = "src/alpha.cpp"

[targets.beta]
kind = "bin"
main = "src/beta.cpp"
)cpp"}}};
        auto bundled = [&](std::string_view name) {
            const auto staged = first_named(two.at("target/dist"), "two-0.1.0", "", true);
            expect(fatal(staged.has_value())) << "no staging dir";
            return !s::find_files(*staged, std::format("{}.*", name)).empty();
        };
        auto r = two.mcpp({"pack", "beta", "--mode", "system"});
        expect(fatal(e2e::succeeded(r))) << "pack beta failed\n" << s::text(r);
        expect(fatal(bundled("beta"))) << "'mcpp pack beta' did not bundle beta";
        expect(fatal(!bundled("alpha"))) << "'mcpp pack beta' bundled alpha instead";
        two.remove("target/dist");
        r = two.mcpp({"pack", "alpha", "--mode", "system"});
        expect(fatal(e2e::succeeded(r))) << "pack alpha failed\n" << s::text(r);
        expect(fatal(bundled("alpha"))) << "'mcpp pack alpha' did not bundle alpha";
        r = two.mcpp({"pack", "nosuch", "--mode", "system"});
        expect(fatal(failed(r))) << "an unknown target name was accepted\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "no target named 'nosuch'"))) << "wrong refusal\n" << s::text(r);
        expect(e2e::contains(s::text(r), "alpha") && e2e::contains(s::text(r), "beta")) << "the refusal did not list the available targets\n" << s::text(r);
    };
    "829 pack takes a member"_test[s::unix_shell] = [] {
        // `mcpp pack -p <member>` at the workspace root packs the member as a pack inside it
        // does (the same archive name and contents); a relative -o lands where it was typed;
        // an unknown member is refused by name (#734 E3).
        s::project ws{s::files{{"mcpp.toml", R"cpp([workspace]
members = ["lib", "app"]
)cpp"}, {"lib/mcpp.toml", R"cpp([package]
namespace = "probe"
name      = "packlib"
version   = "0.1.0"

[targets.packlib]
kind = "lib"
)cpp"},
                               {"lib/src/packlib.cppm", "export module probe.packlib;\nexport int one() { return 1; }\n"},
                               {"app/mcpp.toml", "[package]\nname = \"app829\"\nversion = \"0.1.0\"\n"},
                               {"app/src/main.cpp", "int main() { return 0; }\n"}}};
        auto r = ws.mcpp_in("lib", {"pack"});
        expect(fatal(e2e::succeeded(r))) << "P1: pack inside the member failed\n" << s::text(r);
        const auto a = first_named(ws.at("lib/target/dist"), "", ".tar.gz");
        expect(fatal(a.has_value())) << "P1: no archive inside the member";
        const auto a_name = a->filename().string();
        auto inside = s::tar_list(*a);
        std::ranges::sort(inside);
        ws.remove("lib/target/dist");
        r = ws.mcpp({"pack", "-p", "lib"});
        expect(fatal(e2e::succeeded(r))) << "P1: pack -p lib at the root failed\n" << s::text(r);
        const auto b = first_named(ws.at("lib/target/dist"), "", ".tar.gz");
        expect(fatal(b.has_value())) << "P1: pack -p did not write the member's archive\n" << s::text(r);
        expect(fatal(a_name == b->filename().string())) << "P1: the archives are named differently: " << a_name << " vs " << b->filename().string();
        auto root = s::tar_list(*b);
        std::ranges::sort(root);
        expect(fatal(inside == root)) << "P1: the archives differ";
        r = ws.mcpp({"pack", "-p", "lib", "-o", "out829.tar.gz"});
        expect(fatal(e2e::succeeded(r))) << "P2: pack -p with -o failed\n" << s::text(r);
        expect(fatal(fs::is_regular_file(ws.at("out829.tar.gz")))) << "P2: a relative -o did not land in the directory it was typed in\n" << s::text(r);
        r = ws.mcpp({"pack", "-p", "nosuch"});
        expect(fatal(failed(r))) << "P3: an unknown member was accepted\n" << s::text(r);
        expect(s::contains_nocase(s::text(r), "nosuch")) << "P3: the refusal does not name the value\n" << s::text(r);
    };
    "801 a dependency program is shipped with the consumer"_test[s::gcc][s::elf] = [] {
        // `artifacts = ["updater"]` builds the dependency's program for the consumer's target
        // and profile beside the consumer's programs (mcpp#711): it runs, `mcpp run` still
        // chooses the package's program, none of its code is linked into the consumer,
        // ${mcpp.artifact:} reaches an action, the pack ships it, and a musl build (where a
        // musl toolchain is installed) builds it for musl.
        const auto tmp = s::case_dir();
        s::project{tmp / "updater", {{"mcpp.toml", R"cpp([package]
name    = "updater"
version = "0.1.0"

[targets.updater]
kind = "bin"
main = "src/main.cpp"
)cpp"}, {"src/helper.cpp", R"cpp(int updater_only_helper() { return 7; }
)cpp"}, {"src/main.cpp", R"cpp(#include <cstdio>
int updater_only_helper();
int main() { std::printf("UPDATER=%d\n", updater_only_helper()); }
)cpp"}}};
        s::project app{tmp / "app", {{"mcpp.toml", R"cpp([package]
name    = "app"
version = "0.1.0"

[dependencies]
updater = { path = "../updater", artifacts = ["updater"] }

[targets.app]
kind = "bin"
main = "src/main.cpp"
)cpp"}, {"src/main.cpp", R"cpp(#include <cstdio>
int main() { std::printf("APP_OK\n"); }
)cpp"}, {"build.mcpp", R"cpp(#include <string>
import mcpp;
int main() {
    const std::string out = std::string(mcpp::out_dir()) + "/updater.copy";
    mcpp::action a;
    a.id   = "copy-updater";
    a.role = mcpp::roles::artifact;
    a.arg("cp").arg("${mcpp.artifact:updater/updater}").arg(out.c_str())
     .input("${mcpp.artifact:updater/updater}")
     .output(out.c_str())
     .submit();
}
)cpp"}}};
        auto r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "build failed\n" << s::text(r);
        const auto upd = paths_ending(app.at("target"), "/bin/updater");
        expect(fatal(!upd.empty())) << "1: no bin/updater\n" << s::text(r);
        expect(fatal(trimmed(s::run(upd.front()).out) == "UPDATER=7")) << "1: the artifact does not run";
        r = app.mcpp({"run"});
        expect(fatal(s::last_line(r.out) == "APP_OK")) << "1: mcpp run chose '" << s::last_line(r.out) << "', not the package's program";
        const auto exe = upd.front().parent_path() / "app";
        expect(fatal(std::ranges::none_of(s::elf_symbols(exe), [](const std::string& n) { return n.find("updater_only_helper") != std::string::npos; })))
            << "2: the dependency's code was linked into the consumer";
        const auto copy = s::find_file(app.at("target"), "updater.copy");
        expect(fatal(copy && s::read_file(*copy) == s::read_file(upd.front()))) << "3: ${mcpp.artifact:} did not reach the action";
        r = app.mcpp({"pack"});
        expect(fatal(e2e::succeeded(r))) << "4: pack failed\n" << s::text(r);
        const auto tarball = s::find_files(app.at("target/dist"), R"(.*\.tar\.gz)");
        expect(fatal(!tarball.empty())) << "4: no archive\n" << s::text(r);
        const auto listing = s::tar_list(tarball.front());
        expect(fatal(std::ranges::any_of(listing, [](const std::string& l) { return l.ends_with("/updater"); }))) << "4: the artifact is not in the archive";
        const bool musl = s::on_path("x86_64-linux-musl-g++").has_value()
                          || !s::glob(s::shared_home() / "registry/data/xpkgs", "xim-x-musl-gcc/*/bin/x86_64-linux-musl-g++").empty();
        if (!s::on_x86_64() || !musl) return;  // 5: no musl toolchain on this machine
        r = app.mcpp({"build", "--target", "x86_64-linux-musl"});
        expect(fatal(e2e::succeeded(r))) << "5: cross build failed\n" << s::text(r);
        const auto cu = paths_ending(app.at("target/x86_64-linux-musl"), "/bin/updater");
        expect(fatal(!cu.empty())) << "5: no cross-built updater";
        expect(!s::read_header(cu.front()).elf_interpreter) << "5: the artifact was not built for the musl target";
    };
    "867 a pack over several members plans builds and runs programs once"_test[s::unix_shell] = [] {
        // `mcpp pack` over several members (mcpp#749) runs the shared member's build program
        // once and each provider's once per pass, told its format and stage on the second;
        // each member's program reads its own staged tree; each member's tree and
        // distributable are those `mcpp pack -p <member>` makes; after a build of the
        // workspace the pack compiles nothing and uses the same build directory.
        const s::options o{.where = s::home::inherited};
        s::project ws{s::files{{"mcpp.toml", R"cpp([workspace]
members = ["core", "cli", "gui"]
)cpp"}, {"core/mcpp.toml", R"cpp([package]
name    = "core"
version = "0.1.0"

[targets.core]
kind = "lib"
)cpp"}, {"core/src/core.cppm", R"cpp(export module zcore;
export int core_answer() { return 42; }
)cpp"}, {"core/build.mcpp", R"cpp(import mcpp;
#include <fstream>
#include <string>
int main() {
    std::ofstream log(std::string(mcpp::out_dir()) + "/runs.log", std::ios::app);
    log << "format=" << mcpp::pack_format() << " stage=" << mcpp::pack_stage_dir() << "\n";
    return 0;
}
)cpp"}}};
        s::write_executable(ws.at("dist.sh"), R"cpp(#!/usr/bin/env bash
set -e
stage="$1"; out="$2"; member="$3"
{ echo "member=$member"; ( cd "$stage" && find . -type f | sort ); } > "$out"
)cpp");
        for (std::string_view m : {"cli", "gui"}) {
            ws.write(std::format("{}/mcpp.toml", m), std::format(R"cpp([package]
name    = "{0}"
version = "0.1.0"

[dependencies]
core = {{ path = "../core" }}

[targets.{0}]
kind = "bin"
main = "src/main.cpp"
)cpp", m));
            ws.write(std::format("{}/src/main.cpp", m), std::format(R"cpp(#include <cstdio>
import zcore;
int main() {{ std::printf("{0} %d\n", core_answer()); return 0; }}
)cpp", m));
            ws.write(std::format("{}/build.mcpp", m), R"cpp(import mcpp;
#include <fstream>
#include <string>
#include <string_view>
int main() {
    const std::string out = mcpp::out_dir();
    // One line per run: the request it answered and the tree it was told.
    {
        std::ofstream log(out + "/runs.log", std::ios::app);
        log << "format=" << mcpp::pack_format() << " stage=" << mcpp::pack_stage_dir() << "\n";
    }
    // Half one, unconditional; half two, only when the format is asked for.
    mcpp::provides_pack_format("zap");
    if (std::string_view(mcpp::pack_format()) != "zap") return 0;
    {
        std::ofstream seen(out + "/seen-stage.txt", std::ios::trunc);
        seen << mcpp::pack_stage_dir() << "\n";
    }
    const std::string root = std::string(mcpp::manifest_dir()) + "/..";
    const std::string dist = out + "/" + mcpp::package_name() + ".zap";
    mcpp::action a;
    a.id   = "zap";
    a.role = "artifact";
    a.arg((root + "/dist.sh").c_str())
     .arg("${mcpp.stage_dir}")
     .arg(dist.c_str())
     .arg(mcpp::package_name())
     .input((std::string("${mcpp.target_file:") + mcpp::package_name() + "}").c_str())
     .output(dist.c_str())
     .submit();
    return 0;
}
)cpp");
        }
        auto outdir = [&](std::string_view m) {
            for (const auto& e : entries(ws.at("target/.build-mcpp/out")))
                if (fs::is_directory(e / m)) return e / m;
            return ws.at("target/.build-mcpp/out/none") / m;
        };
        auto runs = [&](std::string_view m) { return fs::exists(outdir(m) / "runs.log") ? s::read_file(outdir(m) / "runs.log") : std::string{}; };
        // `grep -c <pattern>`: the lines of runs.log the pattern matches (every line for "").
        auto count = [&](std::string_view m, std::string_view pattern) {
            const auto lines = lines_of(runs(m));
            const std::regex re{std::string(pattern)};
            return static_cast<std::size_t>(std::ranges::count_if(lines, [&](const std::string& l) { return std::regex_search(l, re); }));
        };
        auto stage_of = [&](std::string_view m) {
            for (const auto& e : entries(ws.at(std::format("{}/target/dist", m)))) {
                const auto n = e.filename().string();
                if (n.starts_with(std::format("{}-0.1.0-", m)) && !n.ends_with(".stage-manifest") && !n.ends_with(".tar.gz")) return e;
            }
            return fs::path{};
        };
        auto objects = [](const fs::path& build_dir) {
            std::size_t n = 0;
            for (const auto& out : s::ninja_log_outputs(build_dir)) n += out.ends_with(".o");
            return n;
        };
        auto r = ws.mcpp({"pack", "--workspace", "--pkg-format", "zap"}, o);
        expect(fatal(e2e::succeeded(r))) << "A: the pack failed\n" << s::text(r);
        expect(fatal(count("core", "") == 1u)) << "A: core's program ran " << count("core", "") << " times, not once\n" << runs("core");
        for (std::string_view m : {"cli", "gui"}) {
            expect(fatal(count(m, "") == 2u)) << "A: " << m << "'s program ran " << count(m, "") << " times, not once for each pass\n" << runs(m);
            expect(fatal(count(m, "^format= stage=$") == 1u)) << "A: " << m << "'s first pass was told a request\n" << runs(m);
            expect(fatal(count(m, "^format=zap stage=/") == 1u)) << "A: " << m << "'s second pass was not told its format and stage\n" << runs(m);
        }
        expect(fatal(s::has_line(s::text(r), "Packed .*cli.zap"))) << "A: cli's distributable was not reported\n" << s::text(r);
        expect(fatal(s::has_line(s::text(r), "Packed .*gui.zap"))) << "A: gui's distributable was not reported\n" << s::text(r);
        for (std::string_view m : {"cli", "gui"}) {
            const auto want = fs::canonical(stage_of(m));
            const auto saw = trimmed(s::read_file(outdir(m) / "seen-stage.txt"));
            expect(fatal(fs::is_directory(saw) && fs::canonical(saw) == want)) << "D: " << m << "'s program read '" << saw << "', and its tree is '" << want.string() << "'";
            const auto zap = s::read_file(outdir(m) / std::format("{}.zap", m));
            expect(fatal(has_exact_line(zap, std::format("member={}", m)))) << "D: " << m << "'s distributable was made for another member\n" << zap;
            expect(fatal(has_exact_line(zap, std::format("./bin/{}", m)))) << "D: " << m << "'s tree did not hold " << m << "'s program\n" << zap;
        }
        expect(fatal(s::read_file(outdir("cli") / "seen-stage.txt") != s::read_file(outdir("gui") / "seen-stage.txt"))) << "D: two members were told one staged tree";
        expect(fatal(s::lacks(s::read_file(outdir("cli") / "cli.zap"), "gui"))) << "D: cli's tree holds gui's program";
        for (std::string_view m : {"cli", "gui"}) {
            auto keep = [&](std::string_view into) {
                const auto dir = ws.at(std::format("{}/{}", into, m));
                copy_tree(stage_of(m), dir / "tree");
                fs::copy_file(stage_of(m).string() + ".stage-manifest", dir / "stage-manifest");
                fs::copy_file(outdir(m) / std::format("{}.zap", m), dir / "dist");
            };
            keep("multi");
            auto p = ws.mcpp({"pack", "-p", std::string(m), "--pkg-format", "zap"}, o);
            expect(fatal(e2e::succeeded(p))) << "B: mcpp pack -p " << m << " failed\n" << s::text(p);
            keep("single");
            expect(fatal(same_tree(ws.at(std::format("multi/{}", m)), ws.at(std::format("single/{}", m))))) << "B: " << m << "'s packed tree differs from the one mcpp pack -p " << m << " makes";
        }
        for (std::string_view t : {"target", "core/target", "cli/target", "gui/target"}) ws.remove(t);
        r = ws.mcpp({"build", "--workspace", "--release"}, o);
        expect(fatal(e2e::succeeded(r))) << "C: the workspace did not build\n" << s::text(r);
        auto build_dirs = [&] {
            std::vector<fs::path> dirs;
            for (const auto& f : s::find_files(ws.at("target"), R"(build\.ninja)"))
                if (f.generic_string().find("/.build-mcpp/") == std::string::npos) dirs.push_back(f.parent_path());
            return dirs;
        };
        const auto dirs = build_dirs();
        expect(fatal(dirs.size() == 1u)) << "C: expected one build directory, got " << dirs.size();
        const auto before = objects(dirs.front());
        expect(fatal(before > 0u)) << "C: the build compiled nothing";
        r = ws.mcpp({"pack", "--workspace", "--pkg-format", "zap"}, o);
        expect(fatal(e2e::succeeded(r))) << "C: the pack failed\n" << s::text(r);
        expect(fatal(build_dirs().size() == 1u)) << "C: the pack used another build directory";
        const auto after = objects(dirs.front());
        expect(after == before) << "C: the pack compiled " << (after - before) << " object(s) after the build";
    };
    "870 a pack over several members resolves and fails per member"_test[s::unix_shell] = [] {
        // A pack of several members resolves a target name for the member an action is for
        // (a and b both name the target `tool`), lists the stages in [workspace] members order
        // across configurations, and lets a member whose distribution step fails fail alone.
        const s::options o{.where = s::home::inherited};
        s::project ws{s::files{{"mcpp.toml", R"cpp([workspace]
members = ["a", "c", "b"]
)cpp"}}};
        s::write_executable(ws.at("dist.sh"), R"cpp(#!/usr/bin/env bash
set -e
here="$(cd "$(dirname "$0")" && pwd)"
[ -e "$here/fail-$3" ] && { echo "dist.sh: $3 is asked to fail" >&2; exit 1; }
cp "$1" "$2"
)cpp");
        auto member = [&](std::string_view m, std::string_view t, std::string_view standard = "") {
            std::string manifest = std::format("[package]\nname    = \"{}\"\nversion = \"0.1.0\"\n", m);
            if (!standard.empty()) manifest += std::format("standard = \"{}\"\n", standard);
            manifest += std::format("\n[targets.{}]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", t);
            ws.write(std::format("{}/mcpp.toml", m), manifest);
            ws.write(std::format("{}/src/main.cpp", m), std::format("#include <cstdio>\nint main() {{ std::puts(\"{}\"); return 0; }}\n", m));
            ws.write(std::format("{}/build.mcpp", m), std::format(R"cpp(import mcpp;
#include <string>
#include <string_view>
int main() {{
    mcpp::provides_pack_format("zap");
    if (std::string_view(mcpp::pack_format()) != "zap") return 0;
    const std::string out  = mcpp::out_dir();
    const std::string root = std::string(mcpp::manifest_dir()) + "/..";
    const std::string dist = out + "/" + mcpp::package_name() + ".zap";
    mcpp::action a;
    a.id   = "zap";
    a.role = "artifact";
    a.arg((root + "/dist.sh").c_str())
     .arg("${{mcpp.target_file:{0}}}")
     .arg(dist.c_str())
     .arg(mcpp::package_name())
     .input("${{mcpp.target_file:{0}}}")
     .output(dist.c_str())
     .submit();
    return 0;
}}
)cpp", t));
        };
        member("a", "tool");
        member("b", "tool");
        member("c", "ctool", "c++20");
        auto zap = [&](std::string_view m) {
            for (const auto& e : entries(ws.at("target/.build-mcpp/out")))
                if (fs::exists(e / m / std::format("{}.zap", m))) return e / m / std::format("{}.zap", m);
            return fs::path{};
        };
        auto zap_runs = [&](std::string_view m) {
            const auto z = zap(m);
            if (z.empty() || (fs::status(z).permissions() & fs::perms::owner_exec) == fs::perms::none) return std::string{"(none)"};
            return trimmed(s::run(z).out);
        };
        auto r = ws.mcpp({"pack", "--workspace", "--pkg-format", "zap"}, o);
        expect(fatal(e2e::succeeded(r))) << "A: the pack failed\n" << s::text(r);
        for (std::string_view m : {"a", "b", "c"})
            expect(fatal(zap_runs(m) == m)) << "A: " << m << "'s distributable runs '" << zap_runs(m) << "', not " << m << "'s program";
        r = ws.mcpp({"pack", "--workspace", "--pkg-format", "zap", "--message-format", "json"}, o);
        expect(fatal(e2e::succeeded(r))) << "B: the JSON pack failed\n" << s::text(r);
        auto envelope = nlohmann::json::parse(r.out);
        expect(fatal(envelope["data"].contains("stages"))) << "B: the envelope has no stages\n" << r.out;
        std::string order;
        for (auto& st : envelope["data"]["stages"]) order += (order.empty() ? "" : " ") + st["member"].get<std::string>();
        expect(fatal(order == "a c b")) << "B: the stages are listed as '" << order << "', not in [workspace] members order (a c b)";
        ws.write("fail-b", "");
        r = ws.mcpp({"pack", "--workspace", "--pkg-format", "zap"}, o);
        expect(fatal(failed(r))) << "C: the pack exited 0 although b's step failed\n" << s::text(r);
        for (std::string_view m : {"a", "c"}) {
            expect(fatal(zap_runs(m) == m)) << "C: " << m << " was not packed beside b's failure\n" << s::text(r);
            expect(fatal(s::has_line(s::text(r), std::format(R"(Packed .*{}\.zap)", m)))) << "C: " << m << "'s distributable was not reported\n" << s::text(r);
        }
        expect(fatal(zap("b").empty())) << "C: b's distributable of the previous pack is still in place";
        expect(s::has_line(s::text(r), "member 'b'|'b'")) << "C: the failure does not name b\n" << s::text(r);
    };
    "868 a pack over several members is refused before anything is compiled"_test[s::unix_shell] = [] {
        // What a pack over several members refuses, before anything is compiled and naming what
        // it refused (mcpp#749): a member without the format (E); a target name (F); an
        // --output naming a file (G); a member with no program by -p, which --workspace skips
        // (H); two members writing one archive (I); members served only by a shared provider,
        // which serves one member alone (J); a member that claimed nothing, or a configuration
        // that cannot be planned, fails alone while the others are packed (K).
        const auto tmp = s::case_dir();
        const s::options o{.where = s::home::inherited};
        auto program_member = [](const s::project& ws, std::string_view m, std::string_view deps) {
            std::string manifest = std::format("[package]\nname    = \"{}\"\nversion = \"0.1.0\"\n", m);
            if (!deps.empty()) manifest += std::format("\n[dependencies]\n{}\n", deps);
            manifest += std::format("\n[targets.{}]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", m);
            ws.write(std::format("{}/mcpp.toml", m), manifest);
            ws.write(std::format("{}/src/main.cpp", m), std::format("#include <cstdio>\nint main() {{ std::puts(\"{}\"); return 0; }}\n", m));
        };
        auto write_dist = [](const fs::path& dir) { s::write_executable(dir / "dist.sh", R"cpp(#!/usr/bin/env bash
set -e
stage="$1"; out="$2"
{ ( cd "$stage" && find . -type f | sort ); } > "$out"
)cpp"); };
        auto provider = [](const s::project& ws, std::string_view m, bool submit) {
            const std::string gate = submit ? R"(if (std::string_view(mcpp::pack_format()) != "zap") return 0;)" : "return 0;";
            ws.write(std::format("{}/build.mcpp", m), std::format(R"cpp(import mcpp;
#include <string>
#include <string_view>
int main() {{
    mcpp::provides_pack_format("zap");
    {0}
    const std::string root = std::string(mcpp::manifest_dir()) + "/..";
    const std::string dist = std::string(mcpp::out_dir()) + "/" + mcpp::package_name() + ".zap";
    mcpp::action a;
    a.id   = "zap";
    a.role = "artifact";
    a.arg((root + "/dist.sh").c_str()).arg("${{mcpp.stage_dir}}").arg(dist.c_str())
     .input((std::string("${{mcpp.target_file:") + mcpp::package_name() + "}}").c_str())
     .output(dist.c_str())
     .submit();
    return 0;
}}
)cpp", gate));
        };
        auto any_named = [](const fs::path& root, std::string_view pattern) { return !s::find_files(root, pattern).empty(); };

        s::project ws{tmp / "ws", {{"mcpp.toml", R"cpp([workspace]
members = ["cli", "plain", "lib"]
)cpp"}}};
        write_dist(ws.dir());
        program_member(ws, "cli", "");
        provider(ws, "cli", true);
        program_member(ws, "plain", "");
        ws.write("lib/mcpp.toml", R"cpp([package]
name    = "lib"
version = "0.1.0"

[targets.lib]
kind = "lib"
)cpp");
        ws.write("lib/src/lib.cppm", "export module zlib;\nexport int f() { return 1; }\n");
        auto r = ws.mcpp({"pack", "--mode", "system", "--workspace", "--pkg-format", "zap"}, o);
        expect(fatal(failed(r))) << "E: a member without the format was packed\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "plain"))) << "E: the refusal does not name the member\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "zap"))) << "E: the refusal does not name the format\n" << s::text(r);
        expect(fatal(s::lacks(s::text(r), "member 'cli'"))) << "E: the refusal names a member that provides the format";
        expect(fatal(compiled_nothing(ws.dir()))) << "E: the refusal arrived after a compile";
        r = ws.mcpp({"pack", "--mode", "system", "-p", "cli", "-p", "plain", "cli"}, o);
        expect(fatal(failed(r) && e2e::contains(s::text(r), "target name") && e2e::contains(s::text(r), "'cli'"))) << "F: a target name with several members was not refused, naming it\n" << s::text(r);
        expect(fatal(compiled_nothing(ws.dir()))) << "F: a refusal arrived after a compile";
        ws.write("a-file", "");
        r = ws.mcpp({"pack", "--mode", "system", "-p", "cli", "-p", "plain", "-o", "a-file"}, o);
        expect(fatal(failed(r) && e2e::contains(s::text(r), "--output names a file"))) << "G: --output naming a file was not refused with several members\n" << s::text(r);
        expect(fatal(compiled_nothing(ws.dir()))) << "G: the refusal arrived after a compile";
        r = ws.mcpp({"pack", "--mode", "system", "-p", "cli", "-p", "lib"}, o);
        expect(fatal(failed(r) && e2e::contains(s::text(r), "member 'lib' has no program target"))) << "H: a member with no program was not refused by name\n" << s::text(r);
        expect(fatal(compiled_nothing(ws.dir()))) << "H: the refusal arrived after a compile";
        r = ws.mcpp({"pack", "--mode", "system", "--workspace"}, o);
        expect(fatal(e2e::succeeded(r))) << "H: --workspace failed over a member with no program\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "Skipping lib"))) << "H: --workspace did not say it skipped lib\n" << s::text(r);
        expect(fatal(any_named(ws.at("cli"), R"((cli|plain)-0\.1\.0-.*\.tar\.gz)") || any_named(ws.at("plain"), R"((cli|plain)-0\.1\.0-.*\.tar\.gz)")))
            << "H: the programs were not packed\n" << s::text(r);
        expect(fatal(!any_named(ws.at("lib"), R"(.*\.tar\.gz)"))) << "H: the library was packed as a program";

        s::project ns{tmp / "ns", {{"mcpp.toml", R"cpp([workspace]
members = ["a/tool", "b/tool"]
)cpp"}}};
        for (std::string_view space : {"a", "b"}) {
            ns.write(std::format("{}/tool/mcpp.toml", space), std::format(R"cpp([package]
name      = "tool"
namespace = "{0}"
version   = "0.1.0"

[targets.tool]
kind = "bin"
main = "src/main.cpp"
)cpp", space));
            ns.write(std::format("{}/tool/src/main.cpp", space), "int main() { return 0; }\n");
        }
        r = ns.mcpp({"pack", "--mode", "system", "--workspace", "-o", "out"}, o);
        expect(fatal(failed(r) && e2e::contains(s::text(r), "would both write") && e2e::contains(s::text(r), "a.tool") && e2e::contains(s::text(r), "b.tool")))
            << "I: two members writing one archive were not refused, naming both\n" << s::text(r);
        expect(fatal(compiled_nothing(ns.dir()))) << "I: the refusal arrived after a compile";
        r = ns.mcpp({"pack", "--mode", "system", "--workspace"}, o);
        expect(fatal(e2e::succeeded(r))) << "I: two members of one name were not packed below their own directories\n" << s::text(r);
        expect(fatal(any_named(ns.at("a/tool/target/dist"), R"(tool-0\.1\.0-.*\.tar\.gz)") && any_named(ns.at("b/tool/target/dist"), R"(tool-0\.1\.0-.*\.tar\.gz)")))
            << "I: a member of the two was not packed\n" << s::text(r);

        const auto PROV_HOST = (tmp / "shared/prov").generic_string();
        const auto prov_dep = std::format("prov = {{ path = \"{}\" }}", PROV_HOST);
        s::project shared{tmp / "shared", {{"mcpp.toml", R"cpp([workspace]
members = ["cli2", "gui2"]
)cpp"}, {"prov/mcpp.toml", R"cpp([package]
name    = "prov"
version = "0.1.0"

[targets.prov]
kind = "lib"
)cpp"},
                                           {"prov/src/prov.cppm", "export module zprov;\nexport int p() { return 1; }\n"}, {"prov/build.mcpp", R"cpp(import mcpp;
#include <string>
#include <string_view>
int main() {
    mcpp::provides_pack_format("zap");
    if (std::string_view(mcpp::pack_format()) != "zap") return 0;
    const std::string dist = std::string(mcpp::out_dir()) + "/prov.zap";
    mcpp::action a;
    a.id   = "zap";
    a.role = "artifact";
    a.arg((std::string(mcpp::manifest_dir()) + "/../dist.sh").c_str()).arg("${mcpp.stage_dir}").arg(dist.c_str())
     .output(dist.c_str())
     .submit();
    return 0;
}
)cpp"}}};
        write_dist(shared.dir());
        for (std::string_view m : {"cli2", "gui2"}) program_member(shared, m, prov_dep);
        r = shared.mcpp({"pack", "--mode", "system", "--workspace", "--pkg-format", "zap"}, o);
        expect(fatal(failed(r) && e2e::contains(s::text(r), "member 'cli2'") && e2e::contains(s::text(r), "member 'gui2'") && e2e::contains(s::text(r), "prov")))
            << "J: members served only by a shared provider were not refused by name\n" << s::text(r);
        expect(fatal(compiled_nothing(shared.dir()))) << "J: the refusal arrived after a compile";
        r = shared.mcpp({"pack", "--mode", "system", "-p", "cli2", "--pkg-format", "zap"}, o);
        expect(fatal(e2e::succeeded(r))) << "J: one member over the same provider was not packed\n" << s::text(r);
        auto provided = s::find_file(shared.dir(), "prov.zap");
        expect(fatal(provided && has_exact_line(s::read_file(*provided), "./bin/cli2"))) << "J: the provider was not given the member's tree\n" << s::text(r);

        s::project mixed{tmp / "mixed", {{"mcpp.toml", R"cpp([workspace]
members = ["cli3", "gui3"]
)cpp"}}};
        write_dist(mixed.dir());
        program_member(mixed, "cli3", prov_dep);
        program_member(mixed, "gui3", "");
        provider(mixed, "gui3", true);
        r = mixed.mcpp({"pack", "--mode", "system", "--workspace", "--pkg-format", "zap"}, o);
        expect(fatal(e2e::succeeded(r))) << "J: a provider one member reaches did not serve it beside another member's own\n" << s::text(r);
        provided = s::find_file(mixed.dir(), "prov.zap");
        expect(fatal(provided && has_exact_line(s::read_file(*provided), "./bin/cli3") && s::lacks(s::read_file(*provided), "gui3")))
            << "J: the provider was not given cli3's tree alone\n" << s::text(r);
        const auto own = s::find_file(mixed.dir(), "gui3.zap");
        expect(fatal(own && has_exact_line(s::read_file(*own), "./bin/gui3"))) << "J: gui3's own provider was not given gui3's tree";
        expect(fatal(s::has_line(s::text(r), "Packed .*prov.zap") && s::has_line(s::text(r), "Packed .*gui3.zap"))) << "J: a distributable was not reported for each member\n" << s::text(r);

        s::project partial{tmp / "partial", {{"mcpp.toml", R"cpp([workspace]
members = ["good", "mute"]
)cpp"}}};
        write_dist(partial.dir());
        program_member(partial, "good", "");
        provider(partial, "good", true);
        program_member(partial, "mute", "");
        provider(partial, "mute", false);
        r = partial.mcpp({"pack", "--mode", "system", "--workspace", "--pkg-format", "zap"}, o);
        expect(fatal(failed(r))) << "K: a member that claimed nothing was reported as packed\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "member 'mute': no action claimed --pkg-format 'zap'"))) << "K: the failure does not name the member\n" << s::text(r);
        expect(fatal(!paths_ending(partial.at("target/.build-mcpp/out"), "/good/good.zap").empty())) << "K: the member that claimed the format was not packed\n" << s::text(r);
        expect(fatal(s::has_line(s::text(r), "Packed .*good.zap"))) << "K: the member that was packed was not reported\n" << s::text(r);

        s::project planfail{tmp / "planfail", {{"mcpp.toml", R"cpp([workspace]
members = ["fine", "broken"]
)cpp"}, {"broken/mcpp.toml", R"cpp([package]
name     = "broken"
version  = "0.1.0"
standard = "c++26"

[targets.broken]
kind = "bin"
main = "src/main.cpp"
)cpp"}, {"broken/src/main.cpp", "int main() { return 0; }\n"},
                                               {"broken/build.mcpp", "this is not C++\n"}}};
        program_member(planfail, "fine", "");
        r = planfail.mcpp({"pack", "--mode", "system", "--workspace"}, o);
        expect(fatal(failed(r))) << "K: a configuration that could not be planned was reported as packed\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "broken"))) << "K: the failure does not name the member\n" << s::text(r);
        expect(any_named(planfail.at("fine"), R"(fine-0\.1\.0-.*\.tar\.gz)")) << "K: the member of the other configuration was not packed\n" << s::text(r);
    };
    "869 a pack over several members names each member in its report"_test[s::unix_shell][s::python3] = [] {
        // What a pack over several members writes and reports for the built-in formats
        // (mcpp#749): each member's archive where it would be written alone, the same archive,
        // in [workspace] members order (L); below a directory --output names (M); less the
        // members --exclude names (N); the JSON envelope names each member's artifacts and
        // stages and keeps the single-member shape for one (O); members of two configurations
        // are each packed from their own build (P).
        const auto tmp = s::case_dir();
        const s::options o{.where = s::home::inherited};
        s::project ws{tmp / "ws", {{"mcpp.toml", R"cpp([workspace]
members = ["core", "gui", "cli"]
)cpp"}, {"core/mcpp.toml", R"cpp([package]
name    = "core"
version = "0.1.0"

[targets.core]
kind = "lib"
)cpp"},
                                   {"core/src/core.cppm", "export module zcore;\nexport int core_answer() { return 42; }\n"}}};
        for (std::string_view m : {"cli", "gui"}) {
            ws.write(std::format("{}/mcpp.toml", m), std::format(R"cpp([package]
name    = "{0}"
version = "0.1.0"

[dependencies]
core = {{ path = "../core" }}

[targets.{0}]
kind = "bin"
main = "src/main.cpp"
)cpp", m));
            ws.write(std::format("{}/src/main.cpp", m), std::format("#include <cstdio>\nimport zcore;\nint main() {{ std::printf(\"{} %d\\n\", core_answer()); return 0; }}\n", m));
        }
        auto archive_of = [&](std::string_view m) {
            return first_named(ws.at(std::format("{}/target/dist", m)), std::format("{}-0.1.0-", m), ".tar.gz");
        };
        auto members_in = [](nlohmann::json& d, std::string_view list) {
            std::string out;
            for (auto& a : d[std::string(list)]) out += (out.empty() ? "" : " ") + a.value("member", std::string("-"));
            return out;
        };
        auto pack = [&](std::vector<std::string> args, std::string_view what) {
            args.insert(args.begin(), {"pack", "--mode", "system"});
            auto r = ws.mcpp(args, o);
            expect(fatal(e2e::succeeded(r))) << what << "\n" << s::text(r);
            return r;
        };
        auto r = pack({"--workspace"}, "L: --workspace did not pack");
        for (std::string_view m : {"cli", "gui"})
            expect(fatal(archive_of(m).has_value())) << "L: " << m << " was not packed below its own target/dist\n" << s::text(r);
        expect(fatal(s::find_files(ws.at("core"), R"(.*\.tar\.gz)").empty())) << "L: the library was packed as a program";
        const auto gui_at = first_line_with(s::text(r), "Packing gui"), cli_at = first_line_with(s::text(r), "Packing cli");
        expect(fatal(gui_at != 0 && cli_at != 0 && gui_at < cli_at)) << "L: the members are not reported in [workspace] members order\n" << s::text(r);
        for (std::string_view m : {"cli", "gui"}) {
            fs::create_directories(ws.at("together"));
            fs::copy_file(*archive_of(m), ws.at(std::format("together/{}.tar.gz", m)));
        }
        ws.remove("cli/target/dist");
        ws.remove("gui/target/dist");
        r = pack({"-p", "cli", "-p", "gui"}, "L: -p cli -p gui did not pack");
        for (std::string_view m : {"cli", "gui"})
            expect(fatal(archive_of(m).has_value())) << "L: -p cli -p gui did not pack " << m << "\n" << s::text(r);
        for (std::string_view m : {"cli", "gui"}) {
            ws.remove(std::format("{}/target/dist", m));
            pack({"-p", std::string(m)}, std::format("L: mcpp pack -p {} failed", m));
            fs::create_directories(ws.at("alone"));
            fs::copy_file(*archive_of(m), ws.at(std::format("alone/{}.tar.gz", m)));
            s::tar_extract(ws.at(std::format("together/{}.tar.gz", m)), ws.at(std::format("together/x-{}", m)));
            s::tar_extract(ws.at(std::format("alone/{}.tar.gz", m)), ws.at(std::format("alone/x-{}", m)));
            expect(fatal(same_tree(ws.at(std::format("together/x-{}", m)), ws.at(std::format("alone/x-{}", m)))))
                << "L: " << m << "'s archive from the pack of several differs from the one packed alone";
        }
        ws.remove("cli/target/dist");
        ws.remove("gui/target/dist");
        r = pack({}, "L: a bare mcpp pack at the workspace root failed");
        expect(fatal(archive_of("gui").has_value() && !archive_of("cli").has_value())) << "L: a bare mcpp pack at the workspace root did not pack the first member with a program alone\n" << s::text(r);

        r = pack({"--workspace", "-o", "out"}, "M: --output <dir> did not pack");
        for (std::string_view m : {"cli", "gui"})
            expect(fatal(first_named(ws.at("out"), std::format("{}-0.1.0-", m), ".tar.gz").has_value())) << "M: " << m << "'s archive is not below the directory --output names\n" << s::text(r);
        std::size_t archives = 0;
        for (const auto& e : entries(ws.at("out"))) archives += e.filename().string().ends_with(".tar.gz");
        expect(fatal(archives == 2u)) << "M: the directory holds more than the two archives";
        r = pack({"--workspace", "--pkg-format", "dir", "-o", "out-trees"}, "M: --pkg-format dir with --output <dir> did not pack");
        for (std::string_view m : {"cli", "gui"}) {
            const auto t = first_named(ws.at("out-trees"), std::format("{}-0.1.0-", m), "", true);
            expect(fatal(t && fs::is_regular_file(*t / "bin" / m))) << "M: " << m << "'s tree is not below the directory --output names\n" << s::text(r);
        }

        ws.remove("cli/target/dist");
        ws.remove("gui/target/dist");
        r = pack({"--workspace", "--exclude", "gui"}, "N: --exclude did not pack");
        expect(fatal(archive_of("cli").has_value())) << "N: cli was not packed\n" << s::text(r);
        expect(fatal(!archive_of("gui").has_value())) << "N: the member --exclude names was packed";

        r = pack({"--workspace", "--message-format", "json"}, "O: --message-format json failed");
        expect(fatal(nlohmann::json::accept(r.out))) << "O: standard output is not one JSON document\n" << r.out;
        auto e = nlohmann::json::parse(r.out);
        expect(fatal(e["kind"] == "mcpp.pack")) << "O: the envelope kind changed\n" << r.out;
        auto& data = e["data"];
        expect(fatal(data["stage"].is_null())) << "O: data.stage is not null for several members\n" << r.out;
        expect(fatal(members_in(data, "artifacts") == "gui cli")) << "O: data.artifacts names '" << members_in(data, "artifacts") << "', not gui and cli in member order";
        expect(fatal(members_in(data, "stages") == "gui cli")) << "O: data.stages does not name one tree per member\n" << r.out;
        auto keys = [](nlohmann::json& j) {
            std::set<std::string> k;
            for (auto it = j.begin(); it != j.end(); ++it) k.insert(it.key());
            return k;
        };
        for (auto& st : data["stages"]) {
            expect(fatal(keys(st) == std::set<std::string>{"member", "dir", "manifest", "closure"})) << "O: a stage record lacks a field: " << st.dump();
            expect(fatal(st["closure"] == "walked" || st["closure"] == "not-walked")) << "O: " << st.dump();
        }
        for (auto& a : data["artifacts"])
            expect(fatal(keys(a) == std::set<std::string>{"path", "type", "format", "targets", "member", "sha256"})) << "O: an artifact record: " << a.dump();
        r = pack({"-p", "cli", "--message-format", "json"}, "O: a pack of one member failed");
        auto one = nlohmann::json::parse(r.out)["data"];
        expect(fatal(!one.contains("stages"))) << "O: the envelope of one member changed\n" << r.out;
        expect(fatal(one["stage"].is_object() && keys(one["stage"]) == std::set<std::string>{"dir", "manifest", "closure"})) << "O: the envelope of one member changed\n" << r.out;
        expect(fatal(one["artifacts"].size() == 1u && keys(one["artifacts"][0]) == std::set<std::string>{"path", "type", "format", "targets", "sha256"}))
            << "O: the envelope of one member changed\n" << r.out;

        s::project cfg{tmp / "cfg", {{"mcpp.toml", R"cpp([workspace]
members = ["app23", "app26"]
)cpp"}}};
        for (std::string_view a : {"app23", "app26"}) {
            std::string manifest = std::format("[package]\nname    = \"{}\"\nversion = \"0.1.0\"\n", a);
            if (a == "app26") manifest += "standard = \"c++26\"\n";
            manifest += std::format("\n[targets.{}]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", a);
            cfg.write(std::format("{}/mcpp.toml", a), manifest);
            cfg.write(std::format("{}/src/main.cpp", a), "#include <cstdio>\nint main() { std::printf(\"%ld\\n\", (long)__cplusplus); return 0; }\n");
        }
        r = cfg.mcpp({"pack", "--mode", "system", "--workspace", "--message-format", "json"}, o);
        expect(fatal(e2e::succeeded(r))) << "P: members of two configurations were not packed\n" << r.err;
        std::size_t graphs = 0;
        for (const auto& f : s::find_files(cfg.at("target"), R"(build\.ninja)"))
            graphs += f.generic_string().find("/.build-mcpp/") == std::string::npos;
        expect(fatal(graphs == 2u)) << "P: expected one build directory for each configuration, found " << graphs;
        auto pdata = nlohmann::json::parse(r.out)["data"];
        expect(fatal(members_in(pdata, "artifacts") == "app23 app26")) << "P: data.artifacts does not name the two members in order\n" << r.out;
        for (std::string_view a : {"app23", "app26"})
            expect(first_named(cfg.at(std::format("{}/target/dist", a)), std::format("{}-0.1.0-", a), ".tar.gz").has_value()) << "P: " << a << " was not packed";
    };
}};

}  // namespace

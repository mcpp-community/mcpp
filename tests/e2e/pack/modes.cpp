// Programs packed: the three modes and their closures, build-machine paths, host
// capabilities a self-contained bundle refuses, runtime files, `kind = "app"`, files staged
// before the closure, the closure an ELF host pack states, several programs and a dotted
// name, an artifact for another system (formerly tests/e2e/30, 215, 216, 649, 652, 662, 669,
// 1043, 1061).
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

// The first directory of the project's target/dist named `<prefix>*`.
std::optional<fs::path> dist_dir(const s::project& p, std::string_view prefix) {
    for (const auto& e : entries(p.at("target/dist")))
        if (fs::is_directory(e) && e.filename().string().starts_with(prefix)) return e;
    return std::nullopt;
}

// The first file of the project's target/dist named `<prefix>*<suffix>`.
std::optional<fs::path> dist_file(const s::project& p, std::string_view prefix, std::string_view suffix = "") {
    for (const auto& e : entries(p.at("target/dist"))) {
        const auto n = e.filename().string();
        if (fs::is_regular_file(e) && n.starts_with(prefix) && n.ends_with(suffix)) return e;
    }
    return std::nullopt;
}

// `[ -x file ]`.
bool executable(const fs::path& f) {
    std::error_code ec;
    return fs::is_regular_file(f, ec) && (fs::status(f, ec).permissions() & fs::perms::owner_exec) != fs::perms::none;
}

// The regular files under `root`, not the links (`find <root> -type f`), sorted.
std::vector<fs::path> plain_files(const fs::path& root) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (fs::is_directory(root, ec))
        for (const auto& e : fs::recursive_directory_iterator(root))
            if (!e.is_symlink() && e.is_regular_file()) out.push_back(e.path());
    std::ranges::sort(out);
    return out;
}

// The first file under `root` whose path ends with `tail` (`find <root> -path '*<tail>'`).
std::optional<fs::path> path_ending(const fs::path& root, std::string_view tail) {
    const auto name = fs::path(std::string(tail)).filename().string();
    for (const auto& f : s::find_files(root, std::regex_replace(name, std::regex{R"([.^$|()\[\]{}*+?\\])"}, R"(\$&)")))
        if (f.generic_string().ends_with(tail)) return f;
    return std::nullopt;
}

// The lines of a file (`sed -n 'Np'` reads the Nth).
std::vector<std::string> file_lines(const fs::path& f) {
    std::vector<std::string> out;
    std::istringstream in{s::read_file(f)};
    for (std::string l; std::getline(in, l);) out.push_back(l);
    return out;
}

bool has_exact_line(const fs::path& f, std::string_view line) { return std::ranges::contains(file_lines(f), std::string(line)); }

// A program's standard output and error, run with no injected environment.
std::string output_of(const fs::path& program) { return s::text(s::run(program)); }

E2E modes{"modes", [] {
    "30 pack modes"_test[s::pack][s::patchelf][s::elf] = [] {
        // `mcpp pack` in each mode, read from the extracted archive: the default (vendored)
        // repoints PT_INTERP and bundles nothing for a dependency-free program; static (where
        // a musl-gcc payload is installed) is statically linked; bundle-all carries the loader
        // and libc with a run.sh exporting MCPP_BUNDLE_DIR; system bundles nothing; the
        // bundle-project alias still packs. Every mode ships a top-level <name> entry.
        const auto tmp = s::case_dir();
        auto app = s::project::scaffold("myapp");
        app.write("mcpp.toml", app.read("mcpp.toml") + "\n[toolchain]\nlinux = \"gcc@16.1.0\"\n");
        auto pack = [&](std::vector<std::string> args, std::string_view what) {
            args.insert(args.begin(), "pack");
            auto r = app.mcpp(args);
            expect(fatal(e2e::succeeded(r))) << what << " pack failed\n" << s::text(r);
        };
        auto extract = [&](std::string_view tarball, std::string_view into, std::string_view what) {
            expect(fatal(fs::is_regular_file(app.at(tarball)))) << what << " tarball not produced at " << tarball;
            s::tar_extract(app.at(tarball), tmp / into);
        };
        auto says_hello = [&](const fs::path& program, std::string_view what) {
            auto r = s::run(program);
            expect(fatal(e2e::succeeded(r))) << what << " failed to run\n" << s::text(r);
            expect(fatal(e2e::contains(s::text(r), "Hello"))) << what << " output missing\n" << s::text(r);
        };
        auto lib_empty = [](const fs::path& lib) { return !fs::is_directory(lib) || entries(lib).empty(); };

        pack({}, "Mode C");
        extract("target/dist/myapp-0.1.0-x86_64-linux-gnu.tar.gz", "c", "Mode C");
        const auto c = tmp / "c/myapp-0.1.0-x86_64-linux-gnu";
        expect(fatal(executable(c / "bin/myapp"))) << "missing bin/myapp";
        expect(fatal(s::read_header(c / "bin/myapp").interpreter == "/lib64/ld-linux-x86-64.so.2")) << "Mode C: PT_INTERP not repointed to /lib64/ld-linux-x86-64.so.2: " << s::read_header(c / "bin/myapp").interpreter;
        expect(fatal(lib_empty(c / "lib"))) << "Mode C: project has no third-party deps but lib/ has entries";
        says_hello(c / "bin/myapp", "Mode C extracted binary");
        expect(fatal(executable(c / "myapp"))) << "Mode C: missing top-level <name> entry";
        says_hello(c / "myapp", "Mode C: top-level entry");

        const auto musl_gcc = s::payload_root("xim-x-musl-gcc", s::musl_gcc_version()) / "bin/x86_64-linux-musl-g++";
        if (executable(musl_gcc)) {
            pack({"--mode", "static"}, "Mode A");
            extract("target/dist/myapp-0.1.0-x86_64-linux-musl-static.tar.gz", "a", "Mode A");
            const auto a = tmp / "a/myapp-0.1.0-x86_64-linux-musl-static";
            expect(fatal(executable(a / "bin/myapp"))) << "Mode A: missing bin/myapp";
            const auto h = s::read_header(a / "bin/myapp");
            expect(fatal(!h.elf_interpreter && h.needed.empty())) << "Mode A: not statically linked";
            says_hello(a / "bin/myapp", "Mode A extracted binary");
            expect(fatal(executable(a / "myapp"))) << "Mode A: missing top-level <name> entry";
            says_hello(a / "myapp", "Mode A: top-level entry");
        }

        pack({"--mode", "bundle-all"}, "Mode B");
        extract("target/dist/myapp-0.1.0-x86_64-linux-gnu-bundle-all.tar.gz", "b", "Mode B");
        const auto b = tmp / "b/myapp-0.1.0-x86_64-linux-gnu-bundle-all";
        expect(fatal(executable(b / "bin/myapp"))) << "Mode B: missing bin/myapp";
        expect(fatal(executable(b / "run.sh"))) << "Mode B: missing run.sh wrapper";
        expect(fatal(fs::is_directory(b / "lib"))) << "Mode B: missing lib/";
        bool loader = false, libc = false;
        for (const auto& e : entries(b / "lib")) {
            const auto n = e.filename().string();
            loader = loader || n.find("ld-linux") != std::string::npos;
            libc = libc || n.starts_with("libc.so");
        }
        expect(fatal(loader)) << "Mode B: ld-linux not bundled";
        expect(fatal(libc)) << "Mode B: libc.so not bundled";
        expect(fatal(executable(b / "myapp"))) << "Mode B: missing top-level <name> entry alongside run.sh";
        says_hello(b / "run.sh", "Mode B run.sh");
        says_hello(b / "myapp", "Mode B <name> entry");
        for (std::string_view entry : {"run.sh", "myapp"})
            expect(fatal(e2e::contains(s::read_file(b / entry), "MCPP_BUNDLE_DIR"))) << "Mode B: " << entry << " does not export MCPP_BUNDLE_DIR — an application cannot locate its own resources under the bundled loader";
        expect(fatal(e2e::contains(s::read_file(b / "run.sh"), R"(MCPP_BUNDLE_DIR="$here")"))) << "Mode B: MCPP_BUNDLE_DIR is not derived from the bundle root ($here)";

        pack({"--mode", "system"}, "system");
        extract("target/dist/myapp-0.1.0-x86_64-linux-gnu-system.tar.gz", "sys", "system");
        const auto sys = tmp / "sys/myapp-0.1.0-x86_64-linux-gnu-system";
        expect(fatal(executable(sys / "bin/myapp"))) << "system: missing bin/myapp";
        expect(fatal(lib_empty(sys / "lib"))) << "system: lib/ should be empty";
        expect(fatal(s::read_header(sys / "bin/myapp").interpreter == "/lib64/ld-linux-x86-64.so.2")) << "system: PT_INTERP not repointed to LSB loader";
        says_hello(sys / "bin/myapp", "system: extracted binary");
        pack({"--mode", "bundle-project"}, "alias bundle-project");
    };
    "215 pack has no build machine paths"_test[s::pack][s::patchelf][s::elf][s::python3] = [] {
        // A bundle does not depend on the machine that built it: no bundled object's search
        // path names the build machine's store; the executable carries DT_RPATH and each
        // bundled library DT_RUNPATH; a program with no host capabilities gets no
        // HOST-REQUIREMENTS file; --mode system's artifacts name no build-machine path either.
        const auto tmp = s::case_dir();
        auto app = s::project::scaffold("bundled");
        app.write("mcpp.toml", app.read("mcpp.toml") + "\n[pack.bundle-project]\nforce_bundle = [\"libgcc_s.so.1\"]\n");
        auto r = app.mcpp({"pack"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        const auto tarball = dist_file(app, "", ".tar.gz");
        expect(fatal(tarball.has_value())) << "no tarball\n" << s::text(r);
        s::tar_extract(*tarball, tmp / "x");
        auto store = s::shared_home().generic_string();
        while (store.size() > 1 && store.ends_with('/')) store.pop_back();
        if (!fs::is_directory(s::shared_home())) store.clear();
        bool found_exe = false, found_lib = false;
        for (const auto& obj : plain_files(tmp / "x")) {
            const auto h = s::read_header(obj);
            if (h.kind != s::format::elf || !h.wide) continue;
            const auto rel = fs::relative(obj, tmp / "x").generic_string();
            expect(store.empty() || s::lacks(h.runpath, store)) << "bundled object still points at the BUILD MACHINE's store: " << rel << " " << h.runpath;
            if (h.elf_interpreter) {
                found_exe = true;
                expect(!h.elf_runpath) << "bundled executable " << rel << " carries DT_RUNPATH, contract requires DT_RPATH: a bundled vendor library cannot be reached through DT_RUNPATH when the dlopen is performed by something else";
            } else {
                found_lib = true;
                expect(!h.elf_rpath) << "bundled library " << rel << " carries DT_RPATH, contract requires DT_RUNPATH";
            }
        }
        expect(fatal(found_exe)) << "swept no executable — the test proved nothing";
        expect(fatal(found_lib)) << "the bundle contains no shared library, so the defect this test targets (bundled .so keeping build-machine paths) was not exercised";
        expect(fatal(!s::find_file(tmp / "x", "HOST-REQUIREMENTS").has_value())) << "a program with no host capabilities got a HOST-REQUIREMENTS file";
        r = app.mcpp({"pack", "--mode", "system"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        const auto systar = dist_file(app, "", "-system.tar.gz");
        expect(fatal(systar.has_value())) << "no system-mode tarball";
        s::tar_extract(*systar, tmp / "xs");
        bool sysfound = false;
        for (const auto& obj : plain_files(tmp / "xs")) {
            const auto h = s::read_header(obj);
            if (h.kind != s::format::elf || !h.wide) continue;
            sysfound = true;
            expect(store.empty() || s::lacks(h.runpath, store)) << "--mode system artifact points at the BUILD MACHINE: " << obj.string() << " " << h.runpath;
        }
        expect(sysfound) << "--mode system swept no ELF";
    };
    "216 selfcontained refuses host capability"_test[s::pack][s::patchelf][s::elf] = [] {
        // A bundle that carries its own libc cannot consume a capability the host must
        // satisfy: self-contained and static refuse, naming the capability and `vendored`;
        // vendored packs and states the capability and its discovery in HOST-REQUIREMENTS.
        const auto tmp = s::case_dir();
        const auto index_dir = tmp / "local-index";
        s::project{index_dir, {{"pkgs/g/gfx-runtime.lua", R"cpp(package = {
    spec = "1",
    name = "gfx-runtime",
    description = "A dependency that needs the host to provide a driver",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux = {
            ["1.0.0"] = {
                url = "https://example.invalid/gfx-runtime-1.0.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
    },
    mcpp = {
        language = "c++23",
        import_std = true,
        sources = { "src/**/*.cppm" },
        targets = { ["gfx-runtime"] = { kind = "lib" } },
        deps = {},
        runtime = {
            requirements = {
                { kind = "capability", value = "opengl.glx.driver", phase = "run",
                  required = true, discovery = "rpath-of-dispatch" },
            },
        },
    },
}
)cpp"}}};
        const auto index_dir_host = index_dir.generic_string();
        s::project app{tmp / "gfxapp", {{".mcpp/.xlings/data/xpkgs/local-dev.gfx-runtime/1.0.0/src/lib.cppm", R"cpp(export module gfx.runtime;
export int gfx_ready() { return 1; }
)cpp"},
                                        {"src/main.cpp", R"cpp(import gfx.runtime;
int main() { return gfx_ready() == 1 ? 0 : 1; }
)cpp"}, {"mcpp.toml", std::format(R"cpp([package]
name = "gfxapp"
version = "0.1.0"

[indices]
local-dev = {{ path = "{0}" }}

[dependencies]
"local-dev.gfx-runtime" = "1.0.0"

[targets.gfxapp]
kind = "bin"
main = "src/main.cpp"
)cpp", index_dir_host)}}};
        auto r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        r = app.mcpp({"why", "runtime"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "capability:opengl.glx.driver"))) << "the dependency's capability never reached the resolved graph\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "gfx-runtime"))) << "the requirement is not attributed to the dependency\n" << s::text(r);
        for (std::string_view mode : {"self-contained", "static"}) {
            r = app.mcpp({"pack", "--mode", std::string(mode)});
            expect(fatal(failed(r))) << "--mode " << mode << " packed a program that needs a host capability\n" << s::text(r);
            expect(fatal(e2e::contains(s::text(r), "opengl.glx.driver"))) << "--mode " << mode << " refused without naming the capability\n" << s::text(r);
            expect(fatal(e2e::contains(s::text(r), "vendored"))) << "--mode " << mode << " refused without naming a way forward\n" << s::text(r);
        }
        r = app.mcpp({"pack", "--mode", "vendored"});
        expect(fatal(e2e::succeeded(r))) << "--mode vendored refused a program it can package\n" << s::text(r);
        const auto tarball = dist_file(app, "", ".tar.gz");
        expect(fatal(tarball.has_value())) << "no tarball";
        s::tar_extract(*tarball, tmp / "x");
        const auto req = s::find_file(tmp / "x", "HOST-REQUIREMENTS");
        expect(fatal(req.has_value())) << "vendored bundle does not state what the host must provide";
        const auto text = s::read_file(*req);
        expect(fatal(e2e::contains(text, "capability=opengl.glx.driver"))) << "HOST-REQUIREMENTS does not list the capability\n" << text;
        expect(e2e::contains(text, "discovery=rpath-of-dispatch")) << "HOST-REQUIREMENTS states the capability but not how it is found\n" << text;
    };
    "649 pack carries runtime deploy files"_test[s::pack] = [] {
        // `mcpp pack` stages runtime.deploy_files and runtime.deploy (#615) at the same
        // relative path beside the packed executable.
        const auto tmp = s::case_dir();
        auto app = s::project::scaffold("app");
        app.write("share/vulkan/icd.d/lvp_icd.json", "icd manifest\n");
        app.write("share/notes.txt", "notes\n");
        app.write("mcpp.toml", app.read("mcpp.toml") + R"cpp(
[toolchain]
linux = "gcc@16.1.0"

[runtime]
deploy_files = ["share/notes.txt"]
deploy = [ { from = "share/vulkan/icd.d/lvp_icd.json", to = "vulkan/icd.d" } ]
)cpp");
        auto r = app.mcpp({"pack"});
        expect(fatal(e2e::succeeded(r))) << "mcpp pack failed\n" << s::text(r);
        const auto tarball = dist_file(app, "app-0.1.0-", ".tar.gz");
        expect(fatal(tarball.has_value())) << "no tarball under target/dist\n" << s::text(r);
        s::tar_extract(*tarball, tmp / "x");
        std::optional<fs::path> root;
        for (const auto& e : entries(tmp / "x"))
            if (!root && fs::is_directory(e) && e.filename().string().starts_with("app-0.1.0-")) root = e;
        expect(fatal(root && executable(*root / "bin/app"))) << "the bundle has no bin/app\n" << s::text(r);
        const auto icd = *root / "bin/vulkan/icd.d/lvp_icd.json";
        expect(fatal(fs::exists(icd) && e2e::contains(s::read_file(icd), "icd manifest"))) << "runtime.deploy did not reach bin/vulkan/icd.d in the bundle\n" << s::text(r);
        const auto notes = *root / "bin/notes.txt";
        expect(fs::exists(notes) && e2e::contains(s::read_file(notes), "notes")) << "runtime.deploy_files did not reach bin/ in the bundle\n" << s::text(r);
    };
    "652 an application is a kind"_test[s::elf][s::gcc] = [] {
        // `kind = "app"` (#622 A3): an unrecognized kind is refused listing every kind; on an
        // ELF host an app links as an executable beside a bin; `mcpp run` runs either and still
        // refuses an unknown target generically; `mcpp pack` routes an app as a program.
        s::project p{s::files{{"mcpp.toml", R"cpp([package]
name    = "myapp"
version = "0.1.0"

[targets.myapp]
kind = "app"
main = "src/main.cpp"

[targets.tool]
kind = "bin"
main = "src/tool.cpp"
)cpp"}, {"src/main.cpp", R"cpp(#include <cstdio>
int main() { std::puts("APP-RUN"); return 0; }
)cpp"}, {"src/tool.cpp", R"cpp(#include <cstdio>
int main() { std::puts("TOOL-RUN"); return 0; }
)cpp"}, {"bad.toml", R"cpp([package]
name    = "p"
version = "0.1.0"

[targets.t]
kind = "framework"
)cpp"},
                              {"bad/mcpp.toml", R"cpp([package]
name    = "p"
version = "0.1.0"

[targets.t]
kind = "framework"
)cpp"}, {"bad/src/main.cpp", "int main(){}\n"}}};
        auto r = p.mcpp_in("bad", {"build"});
        expect(fatal(failed(r))) << "an unrecognized kind must be refused\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "must be 'bin', 'app', 'lib', 'shared' or 'entry'"))) << "the refusal does not list every kind\n" << s::text(r);
        r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "the host build failed\n" << s::text(r);
        std::optional<fs::path> newest;
        for (const auto& n : s::glob(p.at("target"), "*/*/build.ninja"))
            if (!newest || fs::last_write_time(n) > fs::last_write_time(*newest)) newest = n;
        expect(fatal(newest.has_value())) << "no target/*/*/build.ninja\n" << s::text(r);
        const auto outdir = newest->parent_path();
        expect(fatal(executable(outdir / "bin/myapp"))) << "bin/myapp was not linked\n" << s::text(r);
        expect(fatal(executable(outdir / "bin/tool"))) << "bin/tool was not linked\n" << s::text(r);
        const auto h = s::read_header(outdir / "bin/myapp");
        expect(fatal(h.kind == s::format::elf && (h.elf_type == 2 || h.elf_interpreter))) << "bin/myapp is not an executable (an app's host form must be one)";
        r = p.mcpp({"run", "myapp"});
        expect(fatal(e2e::succeeded(r))) << "mcpp run myapp failed on the host\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "APP-RUN"))) << "mcpp run myapp did not print its marker\n" << s::text(r);
        r = p.mcpp({"run", "nosuchtarget"});
        expect(fatal(failed(r))) << "mcpp run of a nonexistent target must fail\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "no binary target 'nosuchtarget' found"))) << "the generic no-binary-target refusal changed wording\n" << s::text(r);
        r = p.mcpp({"run", "tool"});
        expect(fatal(e2e::succeeded(r))) << "mcpp run tool failed\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "TOOL-RUN"))) << "mcpp run tool did not print its marker\n" << s::text(r);
        r = p.mcpp({"pack", "--pkg-format", "dir"});
        expect(fatal(e2e::succeeded(r))) << "mcpp pack failed\n" << s::text(r);
        const auto staged = dist_dir(p, "myapp-0.1.0-");
        expect(fatal(staged.has_value())) << "no staged tree under target/dist\n" << s::text(r);
        expect(executable(*staged / "bin/myapp")) << "the staged tree has no bin/myapp";
    };
    "662 pack stages declared files before the closure"_test[s::gcc] = [] {
        // `mcpp pack` stages the program and its declared runtime files before it asks whether
        // the closure can be walked, and records the outcome on the stage manifest; a
        // dispatched format's action sees the deployed file and `closure = walked`.
        auto app = s::project::scaffold("app");
        app.write("share/notes.txt", "notes\n");
        app.write("mcpp.toml", app.read("mcpp.toml") + R"cpp(
[toolchain]
linux = "gcc@16.1.0"

[runtime]
deploy_files = ["share/notes.txt"]
)cpp");
        auto r = app.mcpp({"pack", "--pkg-format", "dir"});
        expect(fatal(e2e::succeeded(r))) << "mcpp pack --pkg-format dir failed\n" << s::text(r);
        const auto tree = dist_dir(app, "app-0.1.0-");
        expect(fatal(tree.has_value())) << "no staged directory under target/dist\n" << s::text(r);
        expect(fatal(executable(*tree / "bin/app"))) << "the staged tree has no executable bin/app";
        expect(fatal(fs::is_regular_file(*tree / "bin/notes.txt"))) << "the declared runtime file did not reach bin/";
        expect(fatal(e2e::contains(s::read_file(*tree / "bin/notes.txt"), "notes"))) << "the deployed file's content is wrong";
        expect(fatal(!fs::is_directory(*tree / "lib") || entries(*tree / "lib").empty())) << "lib/ has entries but nothing was declared";
        auto run = s::run(*tree / "bin/app");
        expect(fatal(e2e::succeeded(run))) << "the staged program does not run\n" << s::text(run);
        const auto manifest = tree->parent_path() / (tree->filename().string() + ".stage-manifest");
        expect(fatal(fs::is_regular_file(manifest))) << "no stage manifest beside the staged tree";
        const auto lines = file_lines(manifest);
        expect(fatal(!lines.empty() && lines.front() == "closure = walked")) << "the manifest's first line is not 'closure = walked'\n" << s::read_file(manifest);
        expect(fatal(s::no_line(s::read_file(manifest), "^reason = "))) << "a walked closure must carry no reason line";
        expect(fatal(s::has_line(s::read_file(manifest), " bin/notes.txt$"))) << "the deploy file is not in the manifest\n" << s::read_file(manifest);

        auto zap = s::project::scaffold("zapapp");
        zap.write("share/notes.txt", "notes\n");
        zap.write("mcpp.toml", zap.read("mcpp.toml") + R"cpp(
[toolchain]
linux = "gcc@16.1.0"

[runtime]
deploy_files = ["share/notes.txt"]
)cpp");
        s::write_executable(zap.at("dist.sh"), R"cpp(#!/usr/bin/env bash
set -e
stage="$1"; manifest="$2"; out="$3"
{
  echo "staged:"
  ls -1 "$stage/bin" | sort
  echo "manifest-first-line:"
  sed -n '1p' "$manifest"
} > "$out"
)cpp");
        zap.write("build.mcpp", R"cpp(import mcpp;
#include <string>
#include <string_view>
int main() {
    // Unconditional half: this build knows a "zap" format exists whether or
    // not one was requested (638 states why).
    mcpp::provides_pack_format("zap");
    if (std::string_view(mcpp::pack_format()) != "zap") return 0;

    const std::string root  = mcpp::manifest_dir();
    const std::string stage = std::string("${mcpp.stage_dir}");
    const std::string out   = std::string(mcpp::out_dir()) + "/app.zap";
    mcpp::action a;
    a.id          = "zap";
    a.role        = "artifact";
    a.description = "zap";
    a.arg((root + "/dist.sh").c_str())
     .arg(stage.c_str())
     .arg((stage + ".stage-manifest").c_str())
     .arg(out.c_str())
     .input("${mcpp.target_file:zapapp}")
     .output(out.c_str())
     .submit();
    return 0;
}
)cpp");
        r = zap.mcpp({"pack", "--pkg-format", "zap"});
        expect(fatal(e2e::succeeded(r))) << "mcpp pack --pkg-format zap failed\n" << s::text(r);
        const auto z = s::find_file(zap.at("target"), "app.zap");
        expect(fatal(z.has_value())) << "--format zap produced nothing\n" << s::text(r);
        expect(fatal(has_exact_line(*z, "notes.txt"))) << "the action's own view of the staged tree lacks the deploy file\n" << s::read_file(*z);
        expect(has_exact_line(*z, "closure = walked")) << "the manifest the action read does not say closure = walked\n" << s::read_file(*z);
    };
    "669 an elf host pack states its closure"_test[s::elf][s::gcc] = [] {
        // #634 A3/A4 on the ELF host row: a shared library without a declared soname records its
        // file name as DT_SONAME and a declared one keeps it; `--pkg-format dir` states a needs
        // line for each bundled and platform library before the file lines, and the tree runs;
        // a prebuilt library the loader can no longer find is refused by name.
        const auto tmp = s::case_dir();
        s::project{tmp / "fw", {{"mcpp.toml", R"cpp([package]
namespace = "demo"
name      = "fw"
version   = "0.1.0"
[targets.fw]
kind = "lib"
)cpp"}, {"src/fw.cppm", "export module fw;\nexport int fw_anchor() { return 41; }\n"}}};
        s::project{tmp / "named", {{"mcpp.toml", R"cpp([package]
namespace = "demo"
name      = "named"
version   = "0.1.0"
[targets.named]
kind   = "shared"
soname = "libnamed.so.1"
)cpp"}, {"src/named.cppm", "export module named;\nexport int named_anchor() { return 1; }\n"}}};
        s::project app{tmp / "app", {{"mcpp.toml", R"cpp([package]
name    = "app"
version = "0.1.0"
[toolchain]
linux = "gcc@16.1.0"
[dependencies]
demo.fw    = { path = "../fw", linkage = "shared" }
demo.named = { path = "../named" }
)cpp"}, {"src/main.cpp", R"cpp(#include <cstdio>
import fw;
import named;
int main() { std::puts("1-2-3"); return fw_anchor() == 41 && named_anchor() == 1 ? 0 : 1; }
)cpp"}}};
        auto r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "mcpp build failed\n" << s::text(r);
        std::optional<fs::path> exe;
        for (const auto& f : s::find_files(app.at("target"), "app"))
            if (!exe && f.generic_string().ends_with("/bin/app")) exe = f;
        expect(fatal(exe.has_value())) << "no bin/app\n" << s::text(r);
        const auto bin = exe->parent_path();
        expect(fatal(fs::is_regular_file(bin / "libfw.so"))) << "no bin/libfw.so\n" << s::text(r);
        expect(fatal(s::read_header(bin / "libfw.so").soname == "libfw.so")) << "libfw.so carries SONAME '" << s::read_header(bin / "libfw.so").soname << "', not its file name";
        expect(fatal(std::ranges::contains(s::read_header(*exe).needed, std::string("libfw.so")))) << "the consumer no longer needs libfw.so by its file name";
        expect(fatal(fs::is_regular_file(bin / "libnamed.so"))) << "no bin/libnamed.so";
        expect(fatal(s::read_header(bin / "libnamed.so").soname == "libnamed.so.1")) << "the declared soname became '" << s::read_header(bin / "libnamed.so").soname << "'";

        r = app.mcpp({"pack", "--pkg-format", "dir"});
        expect(fatal(e2e::succeeded(r))) << "mcpp pack --pkg-format dir failed\n" << s::text(r);
        const auto tree = dist_dir(app, "app-0.1.0-");
        expect(fatal(tree.has_value())) << "no staged tree\n" << s::text(r);
        const fs::path manifest = tree->string() + ".stage-manifest";
        const auto lines = file_lines(manifest);
        expect(fatal(!lines.empty() && lines.front() == "closure = walked")) << "the manifest's first line is not 'closure = walked'\n" << s::read_file(manifest);
        expect(fatal(fs::is_regular_file(*tree / "lib/libfw.so"))) << "lib/libfw.so is not bundled";
        expect(fatal(has_exact_line(manifest, "needs\tlibfw.so\tlib/libfw.so"))) << "the manifest does not state libfw.so at lib/libfw.so\n" << s::read_file(manifest);
        expect(fatal(has_exact_line(manifest, "needs\tlibc.so.6\tplatform"))) << "the manifest does not state libc.so.6 as the platform's\n" << s::read_file(manifest);
        bool files = false, bad = false;
        for (std::size_t i = 1; i < lines.size(); ++i) {
            if (lines[i].starts_with("needs\t")) bad = bad || files;
            else files = true;
        }
        expect(fatal(!bad)) << "a needs line follows a file line\n" << s::read_file(manifest);
        auto run = s::run(*tree / "bin/app");
        expect(fatal(e2e::succeeded(run))) << "the staged program does not run\n" << s::text(run);
        expect(fatal(s::has_line(s::text(run), "^1-2-3$"))) << "the staged program did not print 1-2-3\n" << s::text(run);

        s::project ext{tmp / "ext", {{"ext.cpp", "int ext_value() { return 1; }\n"}}};
        fs::create_directories(ext.at("lib"));
        auto gxx = e2e::run({.argv = {"g++", "-shared", "-fPIC", "-Wl,-soname,libext.so", "-o", ext.at("lib/libext.so").string(), ext.at("ext.cpp").string()}});
        expect(fatal(e2e::succeeded(gxx))) << "g++ failed\n" << s::text(gxx);
        const auto TMP = tmp.generic_string();
        s::project extapp{tmp / "extapp", {{"mcpp.toml", std::format(R"cpp([package]
name    = "extapp"
version = "0.1.0"
[toolchain]
linux = "gcc@16.1.0"
[build]
ldflags = ["-L{0}/ext/lib", "-lext", "-Wl,-rpath,{0}/ext/lib"]
)cpp", TMP)}, {"src/main.cpp", "int ext_value();\nint main() { return ext_value() == 1 ? 0 : 1; }\n"}}};
        r = extapp.mcpp({"pack", "--pkg-format", "dir"});
        expect(fatal(e2e::succeeded(r))) << "the pack with a prebuilt library failed\n" << s::text(r);
        const auto etree = dist_dir(extapp, "extapp-0.1.0-");
        expect(fatal(etree.has_value())) << "no staged tree\n" << s::text(r);
        const fs::path emanifest = etree->string() + ".stage-manifest";
        expect(fatal(has_exact_line(emanifest, "needs\tlibext.so\tlib/libext.so"))) << "libext.so is not stated as bundled\n" << s::read_file(emanifest);
        fs::remove(ext.at("lib/libext.so"));
        r = extapp.mcpp({"pack", "--pkg-format", "dir"});
        expect(fatal(failed(r))) << "--format dir succeeded with libext.so gone\n" << s::text(r);
        expect(e2e::contains(s::text(r), "libext.so: the loader finds no file for it")) << "the refusal does not name libext.so\n" << s::text(r);
    };
    "1043 several programs and a dotted name are packed as stated"_test[s::elf][s::unix_shell] = [] {
        // Several programs, none named after the package: the first by name is packed and the
        // others and the ways to choose are said; --all-programs ships every one; a program
        // named with a dot is packed into one file that runs; the program named after the
        // package is packed and the ones left out are said.
        s::project p{s::files{{"mcpp.toml", R"cpp([package]
name    = "multi"
version = "0.1.0"

[targets.alpha]
kind = "bin"
main = "src/alpha.cpp"

[targets."COMMAND.COM"]
kind = "bin"
main = "src/command.cpp"
)cpp"}, {"src/alpha.cpp", "int main() { return 0; }\n"}, {"src/command.cpp", "int main() { return 3; }\n"}}};
        auto r = p.mcpp({"pack"});
        expect(fatal(e2e::succeeded(r))) << "several programs, none named after the package, were not packed\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "packing COMMAND.COM, the first by name, as none is named after the package"))) << "the program packed is not said\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "(alpha)") && e2e::contains(s::text(r), "mcpp pack --all-programs"))) << "the others and the ways to choose are not said\n" << s::text(r);
        p.remove("target/dist");
        r = p.mcpp({"pack", "--all-programs"});
        expect(fatal(e2e::succeeded(r))) << "--all-programs did not pack\n" << s::text(r);
        const auto tarball = dist_file(p, "multi-0.1.0-", ".tar.gz");
        expect(fatal(tarball.has_value())) << "no archive\n" << s::text(r);
        const auto listing = s::tar_list(*tarball);
        auto listed = [&](std::string_view tail) { return std::ranges::any_of(listing, [&](const std::string& l) { return l.ends_with(tail); }); };
        expect(fatal(listed("/bin/alpha") && listed("/bin/COMMAND.COM"))) << "--all-programs did not ship both programs";
        p.remove("target/dist");
        r = p.mcpp({"pack", "COMMAND.COM", "--pkg-format", "single-file"});
        expect(fatal(e2e::succeeded(r))) << "the single file of COMMAND.COM was not made\n" << s::text(r);
        const auto file = dist_file(p, "multi-0.1.0-");
        expect(fatal(file.has_value())) << "no single file under target/dist";
        const auto rc = s::run(*file).status;
        expect(fatal(rc == 3)) << "the single file exited " << rc << ", not 3";
        auto m = p.read("mcpp.toml");
        m.replace(m.find("name    = \"multi\""), 17, "name    = \"alpha\"");
        p.write("mcpp.toml", m);
        r = p.mcpp({"pack"});
        expect(fatal(e2e::succeeded(r))) << "the program named after the package was not packed\n" << s::text(r);
        expect(e2e::contains(s::text(r), "packing alpha, the program named after the package; its other programs are not packed (COMMAND.COM)")) << "the programs left out are not said\n" << s::text(r);
    };
    "1061 an artifact for another system is packed without running it"_test[s::elf][s::llvm] = [] {
        // An artifact for another system is packed from what its file states, without running
        // the host's ldd: the library the build produced travels with the program; with the
        // library static, --mode static packs a static program.
        const auto tmp = s::case_dir();
        const auto LLVM_VERSION = s::llvm_version();
        s::project{tmp / "plat", {{"mcpp.toml", std::format(R"cpp([package]
name    = "away-platform"
version = "0.1.0"

[platform]
toolchain = "llvm@{0}"

[[platform.define]]
triple        = "x86_64-awayos"
clang-triple  = "x86_64-unknown-linux-gnu"
object-format = "elf"
family        = "unix"
execution     = "awayos"

[[platform.provider]]
serves = ["x86_64-awayos"]

[platform.build]
cflags  = ["-ffreestanding", "-fno-stack-protector"]
ldflags = ["-nostdlib"]

[platform.link]
driver = "compiler"

[platform.layers]
"kernel-abi"       = false
"c-abi"            = false
"c++-abi"          = false
"compiler-runtime" = false
)cpp", LLVM_VERSION)}}};
        s::project awaylib{tmp / "awaylib", {{"mcpp.toml", R"cpp([package]
name    = "awaylib"
version = "0.1.0"

[targets.awaylib]
kind = "shared"
)cpp"}, {"src/away.c", "int away_value(void) { return 7; }\n"}}};
        s::project app{tmp / "app", {{"mcpp.toml", R"cpp([package]
name    = "away"
version = "0.1.0"

[dependencies]
awaylib = { path = "../awaylib" }

[targets.away]
kind = "bin"
main = "src/main.c"

[target.x86_64-awayos]
platform = { path = "../plat" }
)cpp"}, {"src/main.c", R"cpp(int away_value(void);
void _start(void) { (void)away_value(); for (;;) {} }
)cpp"}}};
        auto r = app.mcpp({"build", "--target", "x86_64-awayos"});
        expect(fatal(e2e::succeeded(r))) << "the row did not build\n" << s::text(r);
        std::optional<fs::path> exe;
        for (const auto& f : s::find_files(app.at("target/x86_64-awayos"), "away"))
            if (!exe && f.generic_string().ends_with("/bin/away")) exe = f;
        expect(fatal(exe.has_value())) << "no program\n" << s::text(r);
        expect(fatal(!s::read_header(*exe).needed.empty())) << "the program names no shared library, so this test tests nothing";
        r = app.mcpp({"pack", "--target", "x86_64-awayos", "--all-programs"});
        expect(fatal(e2e::succeeded(r))) << "the vendored pack failed\n" << s::text(r);
        std::string lowered = s::text(r);
        std::ranges::transform(lowered, lowered.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        expect(fatal(s::lacks(lowered, "ldd failed"))) << "the host's ldd was run on the artifact\n" << s::text(r);
        const auto listing = s::tar_list(app.at("target/dist/away-0.1.0-x86_64-awayos.tar.gz"));
        expect(fatal(std::ranges::any_of(listing, [](const std::string& l) { return l.ends_with("/bin/away"); }))) << "the program is not packed";
        expect(fatal(std::ranges::any_of(listing, [](const std::string& l) { return std::regex_search(l, std::regex{R"(libawaylib[^/]*\.so)"}); })))
            << "the library the build produced does not travel";
        auto m = awaylib.read("mcpp.toml");
        m.replace(m.find("kind = \"shared\""), 15, "kind = \"lib\"");
        awaylib.write("mcpp.toml", m);
        r = app.mcpp({"pack", "--target", "x86_64-awayos", "--mode", "static"});
        expect(fatal(e2e::succeeded(r))) << "the static pack failed\n" << s::text(r);
        const auto static_tar = app.at("target/dist/away-0.1.0-x86_64-awayos-static.tar.gz");
        std::string static_exe;
        for (const auto& l : s::tar_list(static_tar))
            if (static_exe.empty() && l.ends_with("/bin/away")) static_exe = l;
        expect(fatal(!static_exe.empty())) << "the static archive holds no bin/away";
        s::tar_extract(static_tar, tmp, {static_exe});
        const auto h = s::read_header(tmp / static_exe);
        expect(h.needed.empty() && !h.elf_interpreter) << "--mode static packed a dynamic program";
    };
}};

}  // namespace

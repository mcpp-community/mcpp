// Mach-O and the Apple rows: a shared library relocates by its @rpath install name; pack reads a
// Mach-O program without running it, stages it with its closure and hands a dispatched format its
// tree; an Android row links on a macOS host; the deployment target is the target's, not the
// host's; an Apple SDK is located by SDKROOT on any host; a Mach-O image's C library is up before
// its initialisers (formerly tests/e2e/259, 266, 666, 668, 721, 746, 928, 1053).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

bool lines_contain(const std::string& text, std::string_view line) {
    for (auto part : std::views::split(std::string_view(text), '\n'))
        if (std::string_view(part.begin(), part.end()) == line) return true;
    return false;
}

std::string first_line(const std::string& text) { return text.substr(0, text.find('\n')); }

// The one directory under target/dist whose name begins `prefix`.
fs::path staged(const s::project& p, std::string_view prefix) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(p.at("target/dist"), ec))
        if (e.is_directory() && e.path().filename().string().starts_with(prefix)) return e.path();
    expect(fatal(false)) << "no staged tree under target/dist named " << prefix << "*";
    return {};
}

// The one archive under target/dist (`<prefix>*.tar.gz`).
fs::path archive(const s::project& p, std::string_view prefix) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(p.at("target/dist"), ec)) {
        const auto name = e.path().filename().string();
        if (e.is_regular_file() && name.starts_with(prefix) && name.ends_with(".tar.gz")) return e.path();
    }
    expect(fatal(false)) << "no archive under target/dist named " << prefix << "*.tar.gz";
    return {};
}

bool lists(const std::vector<std::string>& members, std::string_view suffix) {
    return std::ranges::any_of(members, [&](const std::string& m) { return m.ends_with(suffix); });
}

// Two modules' library and a program that imports it (259, 266).
constexpr std::string_view mathkit_interface = "export module mathkit;\nexport namespace mk { int answer(); }\n";
constexpr std::string_view mathkit_impl = "module mathkit;\nnamespace mk { int answer() { return 42; } }\n";

// A Mach-O section's 32-bit little-endian words, from `llvm-objdump --macho -s -j <section>`.
std::vector<std::uint32_t> section_words(const std::string& dump) {
    std::vector<std::uint32_t> words;
    const auto hex = [](std::string_view w) { return std::ranges::all_of(w, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }); };
    for (const auto& line : s::lines_with(dump, "")) {
        std::istringstream in(line);
        std::vector<std::string> parts;
        for (std::string w; in >> w;) parts.push_back(w);
        if (parts.size() < 2 || !hex(parts[0])) continue;
        for (std::size_t i = 1; i < parts.size() && i <= 4; ++i) {
            if (parts[i].size() != 8 || !hex(parts[i])) continue;
            std::uint32_t le = 0;
            for (int b = 3; b >= 0; --b) le = (le << 8) | static_cast<std::uint32_t>(std::stoul(parts[i].substr(2 * b, 2), nullptr, 16));
            words.push_back(le);
        }
    }
    return words;
}

E2E rows{"rows", [] {
    "259 shared library macho"_test[tag{"requires:macos"}] = [] {
        // A Mach-O shared library's install name is @rpath/<file>, not the build directory, so
        // it loads where it is extracted: packed, the build tree removed, a consumer builds and
        // runs against the package.
        s::needs({"macos"});
        const auto tmp = s::case_dir();
        s::project lib(tmp / "mathkit", {{"src/mathkit.cppm", mathkit_interface}, {"src/impl.cpp", mathkit_impl},
                                         {"mcpp.toml", "[package]\nname    = \"mathkit\"\nversion = \"0.1.0\"\n[build]\nsources = [\"src/*.cppm\", \"src/*.cpp\"]\n[targets.mathkit]\nkind = \"shared\"\n"}});
        auto r = lib.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "build failed: " << s::text(r);
        const auto dylib = s::find_file(lib.at("target"), "libmathkit.dylib");
        expect(fatal(dylib.has_value())) << "no .dylib";
        if (const auto otool = s::on_path("otool")) {
            const auto id = s::run(*otool, {"-D", dylib->string()});
            expect(fatal(s::last_line(id.out) == "@rpath/libmathkit.dylib")) << "the install name is '" << s::last_line(id.out)
                << "'; anything absolute is this machine's build directory, and the library stops loading once extracted elsewhere";
        }
        fs::remove_all(lib.at("target"));
        r = lib.mcpp({"pack", "mathkit", "--pkg-format", "dir"});
        expect(fatal(e2e::succeeded(r))) << "pack failed: " << s::text(r);
        const auto pkg = tmp / "pkg";
        fs::copy(staged(lib, "mathkit-0.1.0"), pkg, fs::copy_options::recursive | fs::copy_options::copy_symlinks);
        expect(fatal(s::find_file(pkg / "lib", "libmathkit.dylib").has_value())) << "no dylib in the package";
        fs::remove_all(lib.dir());
        s::project app(tmp / "app", {{"src/main.cpp", "#include <cstdio>\nimport mathkit;\nint main(){ std::printf(\"ok=%d\\n\", mk::answer()); return 0; }\n"},
                                     {"mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[dependencies]\nmathkit = {{ path = \"{}\" }}\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", pkg.generic_string())}});
        r = app.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "the consumer could not build or run against the packaged dylib ('image not found' naming a path under /private/var means the install name was the build directory): " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "ok=42"))) << "wrong answer: " << s::text(r);
    };

    "266 pack reads a macho program without running it"_test[tag{"requires:macos"}] = [] {
        // mcpp pack reads a Mach-O program's closure without running it (the program leaves a
        // marker when it runs), and a library target still packs.
        s::needs({"macos"});
        s::project p{{"src/mathkit.cppm", mathkit_interface}, {"src/impl.cpp", mathkit_impl},
                     {"src/main.cpp", "#include <cstdio>\nimport mathkit;\nint main() {\n    if (std::FILE* f = std::fopen(\"ran.marker\", \"w\")) std::fclose(f);\n    std::printf(\"ok=%d\\n\", mk::answer());\n    return 0;\n}\n"},
                     {"mcpp.toml", "[package]\nname    = \"proj\"\nversion = \"0.1.0\"\n[build]\nsources = [\"src/*.cppm\", \"src/*.cpp\"]\n[targets.proj]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n[targets.mathkit]\nkind = \"lib\"\n"}};
        auto r = p.mcpp({"pack", "proj"});
        expect(fatal(e2e::succeeded(r))) << "mcpp pack refused a Mach-O program whose closure is the OS's alone: " << s::text(r);
        expect(fatal(!p.exists("ran.marker") && !s::find_file(p.at("target"), "ran.marker"))) << "packing the program ran it (ran.marker exists)";
        expect(fatal(lists(s::tar_list(archive(p, "proj-0.1.0-")), "/bin/proj"))) << "the archive has no bin/proj";
        r = p.mcpp({"pack", "mathkit"});
        expect(fatal(e2e::succeeded(r))) << "a library target does not pack on macOS: " << s::text(r);
        expect(fatal(!s::find_files(p.at("target/dist"), "*.a").empty())) << "no archive in the library package";
    };

    "666 a macho program reaches a dispatched format with its tree"_test[tag{"requires:macos"}] = [] {
        // --pkg-format dir stages a Mach-O program with `closure = walked`; a dispatched format's
        // action sees the program, the deployed file and the same closure, with no reason line.
        s::needs({"macos"});
        auto p = s::project::scaffold("zapapp");
        p.write("share/notes.txt", "notes\n");
        p.write("mcpp.toml", p.read("mcpp.toml") + "\n[runtime]\ndeploy = [ { from = \"share/notes.txt\", to = \"data\" } ]\n");
        s::write_executable(p.at("dist.sh"), "set -e\nstage=\"$1\"; manifest=\"$2\"; out=\"$3\"\n{\n  echo \"staged:\"\n  ( cd \"$stage\" && find . -type f | sort )\n  echo \"manifest:\"\n  sed -n '1,2p' \"$manifest\"\n} > \"$out\"\n");
        p.write("build.mcpp", R"cpp(import std;
import mcpp;
int main() {
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
        auto r = p.mcpp({"pack", "--pkg-format", "dir"});
        expect(fatal(e2e::succeeded(r))) << "--pkg-format dir of a Mach-O program failed: " << s::text(r);
        const auto tree = staged(p, "zapapp-0.1.0-");
        expect(fatal(fs::is_regular_file(tree / "bin/zapapp"))) << "the staged tree has no bin/zapapp";
        expect(fatal(first_line(s::read_file(fs::path(tree.string() + ".stage-manifest"))) == "closure = walked")) << "the dir manifest does not say closure = walked";
        r = p.mcpp({"pack", "--pkg-format", "zap"});
        expect(fatal(e2e::succeeded(r))) << "mcpp pack --pkg-format zap failed: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), "staged without its dependency closure"))) << "pack still reports the tree as staged without its closure: " << s::text(r);
        const auto zap = s::find_file(p.at("target"), "app.zap");
        expect(fatal(zap.has_value())) << "--pkg-format zap produced nothing";
        const auto seen = s::read_file(*zap);
        expect(fatal(lines_contain(seen, "./bin/zapapp"))) << "the action's view of the tree lacks bin/zapapp: " << seen;
        expect(fatal(lines_contain(seen, "./bin/data/notes.txt"))) << "the action's view of the tree lacks the deployed file: " << seen;
        expect(fatal(lines_contain(seen, "closure = walked"))) << "the manifest does not say closure = walked: " << seen;
        expect(fatal(s::no_line(seen, "^reason = "))) << "a walked closure carries a reason line: " << seen;
    };

    "668 a macho program is staged with its closure"_test[tag{"requires:macos"}] = [] {
        // A program with a shared path dependency: bin/libfw.dylib is staged beside it, the
        // manifest names the dylib and libSystem as the platform's; the staged program runs with
        // the build tree deleted (exit 7) and stops with "Library not loaded" without the dylib;
        // the archive carries both.
        s::needs({"macos"});
        const auto tmp = s::case_dir();
        s::project fw(tmp / "fw", {{"mcpp.toml", "[package]\nnamespace = \"demo\"\nname      = \"fw\"\nversion   = \"0.1.0\"\n[targets.fw]\nkind = \"lib\"\n"},
                                   {"src/fw.cppm", "export module fw;\nexport int fw_anchor() { return 41; }\n"}});
        s::project app(tmp / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[dependencies]\ndemo.fw = { path = \"../fw\", linkage = \"shared\" }\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                                     {"src/main.cpp", "#include <cstdio>\nimport fw;\nint main() { std::puts(\"1-2-3\"); return fw_anchor() == 41 ? 7 : 1; }\n"}});
        auto r = app.mcpp({"pack", "--pkg-format", "dir"});
        expect(fatal(e2e::succeeded(r))) << "mcpp pack --pkg-format dir failed: " << s::text(r);
        const auto tree = staged(app, "app-0.1.0-");
        expect(fatal(fs::is_regular_file(tree / "bin/app"))) << "the staged tree has no bin/app";
        expect(fatal(fs::is_regular_file(tree / "bin/libfw.dylib"))) << "the staged tree has no bin/libfw.dylib beside the program";
        const auto m = s::read_file(fs::path(tree.string() + ".stage-manifest"));
        expect(fatal(first_line(m) == "closure = walked")) << "the manifest's first line is not 'closure = walked': " << m;
        expect(fatal(lines_contain(m, "needs\t@rpath/libfw.dylib\tbin/libfw.dylib"))) << "the manifest does not state the dylib beside the program: " << m;
        expect(fatal(lines_contain(m, "needs\t/usr/lib/libSystem.B.dylib\tplatform"))) << "the manifest does not state libSystem as the platform's: " << m;
        const auto installed = tmp / "installed" / tree.filename();
        fs::create_directories(installed.parent_path());
        fs::copy(tree, installed, fs::copy_options::recursive | fs::copy_options::copy_symlinks);
        fs::remove_all(app.at("target"));
        auto ran = s::run(installed / "bin/app");
        expect(fatal(ran.status == 7)) << "the staged program exited " << ran.status << ", not 7: " << s::text(ran);
        expect(fatal(s::has_line(ran.out, "^1-2-3$"))) << "the staged program did not print 1-2-3: " << s::text(ran);
        fs::remove(installed / "bin/libfw.dylib");
        ran = s::run(installed / "bin/app");
        expect(fatal(ran.status != 0 && ran.status != 7)) << "the program still ran without its staged dylib (exit " << ran.status << ")";
        expect(fatal(e2e::contains(s::text(ran), "Library not loaded"))) << "the failure is not a missing library: " << s::text(ran);
        r = app.mcpp({"pack"});
        expect(fatal(e2e::succeeded(r))) << "mcpp pack (tar) failed: " << s::text(r);
        const auto members = s::tar_list(archive(app, "app-0.1.0-"));
        expect(fatal(lists(members, "/bin/app"))) << "the archive has no bin/app";
        expect(fatal(lists(members, "/bin/libfw.dylib"))) << "the archive has no bin/libfw.dylib";
    };

    "721 an android row links on a macos host"_test[tag{"requires:macos"}] = [] {
        // A the Android row links on a macOS host (libapp.so and libdep.so are x86-64 ELF shared
        // objects); B no Apple linker or Apple SDK token reaches the Android link.
        s::needs({"macos"});
        const auto tmp = s::case_dir();
        s::project dep(tmp / "dep", {{"mcpp.toml", "[package]\nname     = \"dep\"\nversion  = \"0.1.0\"\nstandard = \"c++20\"\n\n[targets.dep]\nkind = \"shared\"\n\n[build]\nsources = [\"src/*.cpp\"]\n"},
                                     {"src/dep.cpp", "[[gnu::visibility(\"default\")]] int dep_answer() { return 42; }\n"}});
        s::project app(tmp / "app", {{"mcpp.toml", "[package]\nname     = \"app\"\nversion  = \"0.1.0\"\nstandard = \"c++20\"\n\n[dependencies]\ndep = { path = \"../dep\" }\n\n[targets.app]\nkind = \"app\"\nmain = \"src/main.cpp\"\n\n[target.x86_64-linux-android]\nmin_api_level = 24\n"},
                                     {"src/main.cpp", "int dep_answer();\n[[gnu::visibility(\"default\")]] int app_entry() { return dep_answer() + 1; }\nint main() { return app_entry() == 43 ? 0 : 1; }\n"}});
        auto r = app.mcpp({"build", "--target", "x86_64-linux-android", "--verbose"});
        const auto out = s::text(r);
        if (failed(r)) {
            const bool fetch = std::regex_search(out, std::regex("cannot be downloaded|download.*failed|not installed", std::regex::icase));
            expect(fatal(!(fetch && s::lacks(out, "ld64.lld") && s::lacks(out, "unknown argument '-soname'")))) << "A: the NDK could not be installed on this runner: " << out;
            expect(fatal(false)) << "A: the Android row did not link on a macOS host: " << out;
        }
        for (std::string_view name : {"libapp.so", "libdep.so"}) {
            const auto f = s::find_file(app.at("target"), name);
            expect(fatal(f.has_value())) << "A: no " << name;
            const auto h = s::read_header(*f);
            expect(fatal(h.kind == s::format::elf && h.elf_type == 3 && !h.elf_interpreter && h.machine == s::elf_machine::x86_64))
                << "A: " << name << " is not an x86-64 ELF shared object";
        }
        expect(fatal(s::lacks(out, "ld64.lld"))) << "B: ld64.lld appears in the build: " << out;
        const auto graph = s::glob(app.at("target/x86_64-linux-android"), "*/build.ninja");
        expect(fatal(!graph.empty())) << "B: no build graph for the Android row";
        for (const auto& l : s::lines_with(s::read_file(graph.front()), "ldflags"))
            if (std::regex_search(l, std::regex(R"(^\s*ldflags\s*=)")))
                expect(fatal(s::lacks(l, "mmacosx-version-min") && s::lacks(l, "MacOSX.sdk"))) << "B: the Android link line carries the macOS SDK or deployment floor: " << l;
    };

    "746 macos deployment target is target keyed not host keyed"_test[tag{"requires:unix-shell"}] = [] {
        // mcpp#685: `macos_deployment_target` and MACOSX_DEPLOYMENT_TARGET reach the effective
        // triple, the flags and the fingerprint by the target on any host. 1 the manifest's 11.0
        // reaches the triple and the compile commands; 2 12.0 moves the flags and the output
        // directory; 3 MACOSX_DEPLOYMENT_TARGET beats the manifest. `--configure-only` stops
        // before any header is opened, so no macOS SDK is needed; an LLVM payload is.
        s::needs({"unix-shell"});
        const auto v = s::newest_payload("xim-x-llvm", "bin/clang++");
        if (!v) skip("no xim-x-llvm payload on this host to cross-resolve aarch64-macos with");
        s::project p{{"src/main.cpp", "int main(){return 0;}\n"}};
        const auto manifest = [&](std::string_view floor) {
            p.write("mcpp.toml", std::format("[package]\nname    = \"deploytarget-probe\"\nversion = \"0.1.0\"\n\n[build]\nmacos_deployment_target = \"{}\"\n\n"
                                             "[target.aarch64-macos]\ntoolchain = \"llvm@{}\"\n# The Apple provider by name: on a host without it, a request of the system\n"
                                             "# would otherwise take the next available row, aarch64-macos-musl (SPEC-016 §4).\nplatform  = \"builtin:apple\"\n", floor, *v));
        };
        const auto configure = [&](std::optional<std::string> env_floor) {
            fs::remove_all(p.at("target"));
            p.remove("compile_commands.json");
            auto r = p.mcpp({"build", "--target", "aarch64-macos", "--configure-only"},
                            {.env = {{"MCPP_COMPILE_COMMANDS", "compile_commands.json"}, {"MCPP_NO_AUTO_INSTALL", "1"}, {"MACOSX_DEPLOYMENT_TARGET", env_floor}}});
            expect(fatal(e2e::succeeded(r))) << "configure failed: " << s::text(r);
            return s::text(r);
        };
        const auto out_dir = [&] {
            for (const auto& d : s::glob(p.at("target/aarch64-macos"), "*"))
                if (fs::is_directory(d)) return d;
            expect(fatal(false)) << "no target/aarch64-macos/<fp>/ directory written";
            return fs::path{};
        };
        manifest("11.0");
        auto out = configure(std::nullopt);
        expect(e2e::contains(out, "aarch64-macos → arm64-apple-macos11.0")) << "1: the resolved triple did not carry the manifest's 11.0: " << out;
        expect(fatal(p.exists("compile_commands.json"))) << "1: --configure-only produced no compile_commands.json";
        auto cc = p.read("compile_commands.json");
        expect(e2e::contains(cc, "--target=arm64-apple-macos11.0")) << "1: --target=arm64-apple-macos11.0 absent from compile_commands.json";
        expect(e2e::contains(cc, "-mmacosx-version-min=11.0")) << "1: -mmacosx-version-min=11.0 absent from compile_commands.json";
        const auto dir11 = out_dir();
        manifest("12.0");
        out = configure(std::nullopt);
        expect(e2e::contains(out, "aarch64-macos → arm64-apple-macos12.0")) << "2: the resolved triple did not move to 12.0: " << out;
        expect(e2e::contains(p.read("compile_commands.json"), "--target=arm64-apple-macos12.0")) << "2: --target=arm64-apple-macos12.0 absent after the manifest changed";
        expect(out_dir() != dir11) << "2: the output directory is unchanged after macos_deployment_target moved 11.0 -> 12.0; a std.pcm built for 11.0 would be replayed for 12.0";
        out = configure("13.0");
        expect(e2e::contains(out, "aarch64-macos → arm64-apple-macos13.0")) << "3: MACOSX_DEPLOYMENT_TARGET=13.0 did not beat the manifest's 12.0: " << out;
        expect(e2e::contains(p.read("compile_commands.json"), "--target=arm64-apple-macos13.0")) << "3: --target=arm64-apple-macos13.0 absent with the environment's value set";
    };

    "928 an apple sdk is located on any host"_test[tag{"requires:unix-shell"}] = [] {
        // SDKROOT is read on every host (design 2026-10-23 §4.4, D18, S12): without it the
        // refusal says where it looked and how to produce one off macOS; with it the SDK gate
        // opens; its slices decide the row; a version is matched; the family names the platform.
        // The non-Apple arm: on macOS xcrun answers first (the iOS workflow covers it).
        s::needs({"unix-shell"});
        if (!s::on_linux()) skip("the non-Apple arm");
        const auto sdk = s::case_dir() / "iPhoneSimulator18.5.sdk";
        fs::create_directories(sdk / "usr/include");
        s::project sdk_files(sdk, {{"SDKSettings.json", "{\"CanonicalName\":\"iphonesimulator18.5\",\"Version\":\"18.5\"}\n"},
                                   {"usr/lib/libSystem.tbd", "--- !tapi-tbd\ntbd-version: 4\ntargets: [ arm64-ios-simulator ]\ninstall-name: /usr/lib/libSystem.B.dylib\n"}});
        s::project app(s::case_dir() / "app", {{"src/main.cpp", "int main() { return 0; }\n"}, {"mcpp.toml", "[package]\nname = \"app\"\nversion = \"0.1.0\"\n"}});
        const auto build = [&](std::string_view target, std::optional<std::string> sdkroot) {
            return s::text(app.mcpp({"build", "--target", std::string(target)}, {.env = {{"NO_COLOR", "1"}, {"MCPP_NO_AUTO_INSTALL", "1"}, {"SDKROOT", sdkroot}}}));
        };
        auto out = build("aarch64-ios-sim", std::nullopt);
        expect(fatal(e2e::contains(out, "needs the iphonesimulator SDK, which this machine does not provide"))) << "no refusal without an SDK: " << out;
        expect(fatal(e2e::contains(out, "SDKROOT (unset)"))) << "the refusal does not say SDKROOT was looked at: " << out;
        expect(fatal(e2e::contains(out, "xtool"))) << "the refusal does not say how to produce one off macOS: " << out;
        out = build("aarch64-ios-sim", sdk.string());
        expect(fatal(s::lacks(out, "which this machine does not provide"))) << "SDKROOT was not honoured off macOS: " << out;
        out = build("x86_64-ios-sim", sdk.string());
        expect(fatal(e2e::contains(out, "carries no x86_64-ios-simulator slice"))) << "a missing slice was not named: " << out;
        app.write("mcpp.toml", "[package]\nname = \"app\"\nversion = \"0.1.0\"\n[target.aarch64-ios-sim]\nsysroot = \"iphonesimulator@17.0\"\n");
        out = build("aarch64-ios-sim", sdk.string());
        expect(fatal(e2e::contains(out, "is version 18.5, and iphonesimulator@17.0 was asked for"))) << "a version mismatch was not named: " << out;
        app.write("mcpp.toml", "[package]\nname = \"app\"\nversion = \"0.1.0\"\n[target.aarch64-ios-sim]\nsysroot = \"iphoneos@system\"\n");
        out = build("aarch64-ios-sim", std::nullopt);
        expect(fatal(e2e::contains(out, "describes another platform"))) << "a locator of another platform was accepted: " << out;
    };

    "1053 a mach o images c library is up before its initialisers"_test[tag{"requires:llvm"}][tag{"requires:unix-shell"}] = [] {
        // 2026.11.9.1: A in the image built for aarch64-macos-musl over the hosted arrangement,
        // the C library's initialiser (okm_early_init) precedes the program's own; B on an arm64
        // macOS host the program runs, its global constructor allocating before main.
        s::needs({"llvm", "unix-shell"});
        s::project p{{"mcpp.toml", "[package]\nname    = \"early\"\nversion = \"0.1.0\"\n"},
                     {"src/main.cpp", "import std;\n// A global constructor that allocates, as a test framework's registration does.\n"
                                      "std::vector<std::string>* registered = new std::vector<std::string>{std::string(64, 'x')};\n"
                                      "int main() {\n    std::println(\"registered {}\", registered->front().size());\n    return 0;\n}\n"}};
        auto r = p.mcpp({"build", "--target", "aarch64-macos-musl"});
        expect(fatal(e2e::succeeded(r))) << "the hosted macOS build: " << s::text(r);
        if (s::lacks(s::text(r), "openkal-platform")) skip("aarch64-macos-musl is not served by openkal-platform's hosted arrangement here");
        const auto image = s::glob(p.at("target"), "*/*/bin/early");
        expect(fatal(!image.empty())) << "no image was produced";
        const auto llvm = s::payload_root("xim-x-llvm", s::llvm_version()) / "bin";
        if (fs::exists(llvm / "llvm-objdump") && fs::exists(llvm / "llvm-nm")) {
            const auto dump = s::run(llvm / "llvm-objdump", {"--macho", "-s", "-j", "__init_offsets", image.front().string()});
            expect(fatal(e2e::succeeded(dump))) << "A: llvm-objdump: " << s::text(dump);
            const auto nm = s::run(llvm / "llvm-nm", {"-n", image.front().string()});
            expect(fatal(e2e::succeeded(nm))) << "A: llvm-nm: " << s::text(nm);
            std::vector<std::pair<std::uint64_t, std::string>> symbols;
            for (const auto& line : s::lines_with(nm.out, "")) {
                std::istringstream in(line);
                std::vector<std::string> parts;
                for (std::string w; in >> w;) parts.push_back(w);
                if (parts.size() == 3) symbols.emplace_back(std::stoull(parts[0], nullptr, 16), parts[2]);
            }
            const auto name_at = [&](std::uint32_t offset) {
                const std::uint64_t address = 0x100000000ull + offset;
                std::string best = "?";
                for (const auto& [value, name] : symbols) {
                    if (value > address) break;
                    best = name;
                }
                return best;
            };
            std::vector<std::string> names;
            for (auto w : section_words(dump.out)) names.push_back(name_at(w));
            const auto early = std::ranges::find_if(names, [](const std::string& n) { return n.contains("okm_early_init"); });
            expect(fatal(early != names.end())) << "A: the C library's initialiser is not in the image" << names;
            const auto program = std::ranges::find_if(names, [](const std::string& n) { return n.contains("main.cpp"); });
            expect(fatal(program != names.end())) << "A: the program's initialiser is not in the image" << names;
            expect(fatal(early < program)) << "A: the program's initialiser runs before the C library's" << names;
        }
#if defined(__APPLE__) && defined(__aarch64__)
        auto ran = s::run(image.front());
        expect(fatal(e2e::succeeded(ran))) << "B: the image ended with " << ran.status << ": " << s::text(ran);
        expect(fatal(e2e::contains(ran.out, "registered 64"))) << "B: the program's output: " << s::text(ran);
#endif
    };
}};

}  // namespace

// The Android rows: the vocabulary (the rows name their payload and are not planned, the simulator
// is a row, an effective triple parses back, `min_api_level`), an application is a shared library,
// a universal tree is two legs in one, a pack stages its closure, test programs carry their C++
// run-time, and a dependency's floor on the API level (formerly tests/e2e/641, 652b, 664, 667,
// 675, 680).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

constexpr std::uint16_t et_dyn = 3;

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

const s::options no_install{.env = {{"MCPP_NO_AUTO_INSTALL", "1"}}};

// A package `c` with `int main` and `extra` manifest lines (the script's `pkg`).
s::project pkg(std::string_view name, std::string_view extra = "") {
    return s::project(s::case_dir() / std::string(name),
                      {{"mcpp.toml", std::format("[package]\nname = \"c\"\nversion = \"0.1.0\"\n{}", extra)}, {"src/main.cpp", "int main(){return 0;}\n"}});
}

// The entry of `toolchain list --format json` for `target`, or nothing.
std::optional<nlohmann::json> listed(const nlohmann::json& list, std::string_view target) {
    for (const auto& x : list["data"]["targets"])
        if (x["target"].get<std::string>() == target) return x;
    return std::nullopt;
}

nlohmann::json toolchain_list() {
    auto r = s::mcpp({"toolchain", "list", "--format", "json"}, s::case_dir());
    expect(fatal(e2e::succeeded(r))) << "toolchain list: " << s::text(r);
    return s::json(r.out);
}

// A shared object: ET_DYN without an interpreter (`file`'s "shared object"); a PIE executable has one.
bool shared_object(const fs::path& f) {
    const auto h = s::read_header(f);
    return h.kind == s::format::elf && h.elf_type == et_dyn && !h.elf_interpreter;
}
bool pie_executable(const fs::path& f) {
    const auto h = s::read_header(f);
    return h.kind == s::format::elf && h.elf_type == et_dyn && h.elf_interpreter;
}

// The one directory under target/dist whose name begins `prefix`.
fs::path staged(const s::project& p, std::string_view prefix) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(p.at("target/dist"), ec))
        if (e.is_directory() && e.path().filename().string().starts_with(prefix)) return e.path();
    expect(fatal(false)) << "no staged tree under target/dist named " << prefix << "*";
    return {};
}

// The regular files under `dir`, relative and sorted (`cd dir && find . -type f | sort`).
std::vector<std::string> files_in(const fs::path& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator{}; ++it)
        if (it->is_regular_file()) out.push_back(it->path().lexically_relative(dir).generic_string());
    std::ranges::sort(out);
    return out;
}

// A tool of the NDK's LLVM (`clang`, `llvm-readelf`); the case fails where none is found, since
// the android-ndk capability was detected.
fs::path ndk_tool(std::string_view name) {
    const auto v = s::newest_payload("xim-x-android-ndk", "toolchains/llvm/prebuilt/*/bin/clang++");
    expect(fatal(v.has_value())) << "the android-ndk capability was detected but no NDK payload was found";
    const auto found = s::glob(s::payload_root("xim-x-android-ndk", *v), std::format("toolchains/llvm/prebuilt/*/bin/{}", name));
    expect(fatal(!found.empty())) << "the NDK has no " << name;
    return found.front();
}

bool lines_contain(const std::string& text, std::string_view line) {
    for (auto part : std::views::split(std::string_view(text), '\n'))
        if (std::string_view(part.begin(), part.end()) == line) return true;
    return false;
}

std::string first_line(const std::string& text) { return text.substr(0, text.find('\n')); }

E2E rows{"rows", [] {
    "641 the android rows are wired and the simulator is a row"_test[s::gcc] = [] {
        // The vocabulary half, which a runner without the NDK can assert; nothing installs a
        // payload. 1 both Android rows name android-ndk; 2 they are not refused as planned; 3 the
        // simulator rows resolve and, off Apple hosts, are refused for the SDK; 4 an effective
        // triple mcpp prints parses back; 5 a bare aarch64-linux is never completed to Android;
        // 6 min_api_level has a floor; 7 the listing answers for this host; 8 an iOS row's SDK is
        // located before a payload is resolved, and its absence is a refusal that names it; 9
        // min_api_level on another row does not trip --strict.
        const auto list = toolchain_list();
        for (std::string_view t : {"aarch64-linux-android", "x86_64-linux-android"}) {
            const auto row = listed(list, t);
            expect(row.has_value() && row->dump().contains("android-ndk")) << "1: " << t << " does not name android-ndk in toolchain list: " << (row ? row->dump() : "absent");
        }
        for (std::string_view t : {"aarch64-linux-android", "x86_64-linux-android"}) {
            auto p = pkg(std::format("tier-{}", t));
            auto r = p.mcpp({"build", "--target", std::string(t)}, no_install);
            expect(s::lacks(s::text(r), "not yet supported (planned)")) << "2: " << t << " is still refused as planned";
        }
        for (std::string_view t : {"aarch64-ios-sim", "x86_64-ios-sim"}) {
            auto p = pkg(std::format("sim-{}", t));
            const auto out = s::text(p.mcpp({"build", "--target", std::string(t)}, no_install));
            if (s::on_macos()) {
                expect(!s::has_line(out, R"(unknown target|not yet supported \(planned\))")) << "3: " << t << " refused on a machine that can build it: " << out;
            } else {
                expect(s::lacks(out, "unknown target")) << "3: " << t << " reported as unknown; it is a registered row";
                expect(s::lacks(out, "not yet supported (planned)")) << "3: " << t << " still says planned; the table says otherwise";
                expect(e2e::contains(out, "needs the iphonesimulator SDK")) << "3: " << t << " refused for none of the three known reasons: " << out;
            }
        }
        {
            auto p = pkg("effective");
            const auto out = s::text(p.mcpp({"build", "--target", "aarch64-unknown-linux-android21"}, no_install));
            expect(s::lacks(out, "unknown target")) << "4: the effective triple mcpp prints does not parse back: " << out;
        }
        {
            auto p = pkg("bare");
            const auto out = s::text(p.mcpp({"build", "--target", "aarch64-linux"}, no_install));
            // The refusal may list Android among other targets this host serves: only the
            // selected target's resolution and diagnostic establish completion.
            std::string selected;
            for (const auto& l : s::lines_with(out, ""))
                if (std::regex_search(l, std::regex(R"((^|\s)(Target|Resolved)\s|^error:.*target|target default for)"))) selected += l + "\n";
            expect(std::regex_search(selected, std::regex("aarch64-linux-(gnu|musl)")) && s::lacks(selected, "android"))
                << "5: a bare aarch64-linux request did not select a Linux C ABI: " << out;
        }
        {
            auto p = pkg("apilevel", "\n[target.aarch64-linux-android]\nmin_api_level = 0\n");
            const auto out = s::text(p.mcpp({"build", "--target", "aarch64-linux-android"}, no_install));
            expect(e2e::contains(out, "min_api_level must be a positive integer")) << "6: min_api_level = 0 was not refused with its own message: " << out;
            auto ok = pkg("apilevel-ok", "\n[target.aarch64-linux-android]\nmin_api_level = 24\n");
            const auto out2 = s::text(ok.mcpp({"build", "--target", "aarch64-linux-android"}, no_install));
            expect(s::lacks(out2, "min_api_level must be")) << "6: a legal min_api_level = 24 was refused by the manifest";
        }
        if (s::on_macos()) {
            for (std::string_view t : {"aarch64-ios", "aarch64-ios-sim", "x86_64-ios-sim"}) {
                const auto row = listed(list, t);
                expect(row.has_value() && row->dump().contains("llvm")) << "7: " << t << " is not listed with its pin on a host that serves it";
            }
        } else {
            for (std::string_view t : {"aarch64-ios", "aarch64-ios-sim", "x86_64-ios-sim", "aarch64-macos"})
                expect(!listed(list, t).has_value()) << "7: " << t << " is listed on a host that cannot serve it";
            expect(listed(list, "x86_64-macos").has_value()) << "7: x86_64-macos is absent; the absences above are then unexplained";
        }
        for (std::string_view t : {"aarch64-ios", "aarch64-ios-sim"}) {
            auto p = pkg(std::format("sdk-{}", t), std::format("\n[target.{}]\ntoolchain = \"llvm@{}\"\n", t, s::llvm_version()));
            const auto out = s::text(p.mcpp({"build", "--target", std::string(t)}, no_install));
            if (s::on_macos()) {
                expect(!s::has_line(out, "needs the .* SDK")) << "8: " << t << " refused for a missing SDK on a machine that has one: " << out;
                continue;
            }
            expect(fatal(s::has_line(out, "needs the .* SDK, which this machine does not provide"))) << "8: " << t << " was not refused for its SDK on a non-Apple host: " << out;
            // What a reader acts on: which SDK, the command that must answer, and that the
            // compiler is not what is missing; and the refusal arrives before a payload resolves.
            expect(e2e::contains(out, "xcrun --sdk")) << "8: " << t << "'s refusal omits the xcrun command: " << out;
            expect(e2e::contains(out, "Command Line Tools")) << "8: " << t << "'s refusal omits the CLT note: " << out;
            expect(e2e::contains(out, "xim:llvm")) << "8: " << t << "'s refusal omits the compiler note: " << out;
            expect(s::lacks(out, "Resolved llvm@")) << "8: " << t << " resolved a payload before refusing for the SDK: " << out;
        }
        {
            auto p = pkg("apilevel-strict-other-row", "\n[target.aarch64-linux-android]\nmin_api_level = 24\n");
            auto r = p.mcpp({"build", "--strict"});
            expect(e2e::succeeded(r)) << "9: --strict build for this host failed with min_api_level declared on another row: " << s::text(r);
            expect(!s::contains_nocase(s::text(r), "unsupported key")) << "9: --strict build reported min_api_level as an unsupported key: " << s::text(r);
        }
    };

    "652b an application on android is a shared library"_test[s::elf][s::gcc][s::android_ndk] = [] {
        // #622 A3: on *-linux-android an application links as a shared library. 1 `app` links
        // lib<name>.so and no executable; 2 `bin` stays an executable; 3 `mcpp run` of the app
        // is refused naming --pkg-format, and not for a bin; 4 a dispatched format stages
        // lib/libmyapp.so and the deployed file; 5 MCPP_TARGET_MIN_PLATFORM_VERSION is numeric on
        // Android and empty on the host. MCPP_NO_AUTO_INSTALL is left unset: the row's own
        // resolution reaches the payload on this machine, as a user's first build would.
        const std::string t = "x86_64-linux-android";
        s::project p{{"mcpp.toml", "[package]\nname    = \"myapp\"\nversion = \"0.1.0\"\n\n[targets.myapp]\nkind = \"app\"\nmain = \"src/main.cpp\"\n\n[targets.tool]\nkind = \"bin\"\nmain = \"src/tool.cpp\"\n"},
                     {"src/main.cpp", "int main() { return 0; }\n"},
                     {"src/tool.cpp", "int main() { return 0; }\n"}};
        auto r = p.mcpp({"build", "--target", t});
        expect(fatal(e2e::succeeded(r))) << "the Android build failed: " << s::text(r);
        const auto ninja = s::newest_file(p.at("target") / t, "build.ninja");
        expect(fatal(ninja.has_value())) << "no build.ninja under target/" << t;
        const auto out = ninja->parent_path();
        expect(fatal(fs::is_regular_file(out / "bin/libmyapp.so"))) << "1: libmyapp.so was not linked: " << s::text(r);
        expect(fatal(!fs::exists(out / "bin/myapp"))) << "1: bin/myapp exists; an Android app has no executable form";
        expect(fatal(shared_object(out / "bin/libmyapp.so"))) << "1: bin/libmyapp.so is not a shared object";
        expect(fatal(fs::is_regular_file(out / "bin/tool"))) << "2: bin/tool was not linked";
        expect(fatal(pie_executable(out / "bin/tool"))) << "2: bin/tool is not an executable (bin must be unaffected by A3)";
        r = p.mcpp({"run", "myapp", "--target", t});
        expect(fatal(failed(r))) << "3: mcpp run of an app whose form is a library must be refused: " << s::text(r);
        const auto expected = std::format("error: 'myapp' is an application, and on {} an application is a shared library that a package installs. Run it through a distributable: mcpp run --pkg-format <name>, where <name> is one of: none declared", t);
        expect(fatal(e2e::contains(s::text(r), expected))) << "3: the refusal sentence does not match the design record's wording: " << s::text(r);
        r = p.mcpp({"run", "tool", "--target", t, "--no-runner"});
        expect(fatal(s::lacks(s::text(r), "is an application"))) << "3: the app refusal fired for a plain bin target: " << s::text(r);
        // 4 comes after 3: the build program declares `provides_pack_format("blob")`
        // unconditionally, which would change the list 3's refusal states.
        p.write("res.txt", "resource-1\n");
        s::write_executable(p.at("copy.sh"), "#!/usr/bin/env bash\nset -e\ncp \"$1\" \"$2\"\n");
        p.write("build.mcpp", R"cpp(import mcpp;
#include <cstdio>
#include <string>
#include <string_view>
int main() {
    if (FILE* f = std::fopen((std::string(mcpp::out_dir()) + "/minplat.txt").c_str(), "w")) {
        std::fputs(mcpp::min_platform_version(), f);
        std::fclose(f);
    }
    mcpp::deploy((std::string(mcpp::manifest_dir()) + "/res.txt").c_str(), "myres");
    mcpp::provides_pack_format("blob");
    if (std::string_view(mcpp::pack_format()) != "blob") return 0;
    const std::string root = mcpp::manifest_dir();
    const std::string out  = std::string(mcpp::out_dir()) + "/myapp.blob";
    mcpp::action a;
    a.id          = "blob";
    a.role        = "artifact";
    a.description = "blob";
    a.arg((root + "/copy.sh").c_str())
     .arg("${mcpp.target_file:myapp}")
     .arg(out.c_str())
     .input("${mcpp.target_file:myapp}")
     .output(out.c_str())
     .submit();
    return 0;
}
)cpp");
        r = p.mcpp({"pack", "--pkg-format", "blob", "--target", t});
        expect(fatal(e2e::succeeded(r))) << "4: mcpp pack --pkg-format blob on Android failed: " << s::text(r);
        const auto tree = staged(p, "myapp-0.1.0-");
        expect(fatal(fs::is_regular_file(tree / "lib/libmyapp.so"))) << "4: the staged tree has no lib/libmyapp.so: " << s::text(r);
        expect(fatal(fs::is_regular_file(tree / "bin/myres/res.txt"))) << "4: the deployed file was not staged under bin/myres/ on the Android row";
        expect(fatal(s::find_file(p.at("target"), "myapp.blob").has_value())) << "4: the reported artifact myapp.blob does not exist";
        // The build program's out_dir is one directory per configuration and package, so the
        // newest minplat.txt is the build that just ran.
        const auto android_file = s::newest_file(p.at("target/.build-mcpp/out"), "minplat.txt");
        expect(fatal(android_file.has_value())) << "5: build.mcpp did not write minplat.txt";
        const auto android_level = s::read_file(*android_file);
        expect(fatal(!android_level.empty() && std::ranges::all_of(android_level, [](char c) { return c >= '0' && c <= '9'; })))
            << "5: MCPP_TARGET_MIN_PLATFORM_VERSION '" << android_level << "' is not numeric on " << t;
        r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "5: the host build failed: " << s::text(r);
        const auto host_file = s::newest_file(p.at("target/.build-mcpp/out"), "minplat.txt");
        expect(fatal(host_file.has_value() && *host_file != *android_file)) << "5: the host build wrote into the Android configuration's out_dir";
        expect(fatal(s::read_file(*host_file).empty())) << "5: MCPP_TARGET_MIN_PLATFORM_VERSION was '" << s::read_file(*host_file) << "', not empty, on the host";
    };

    "664 a universal apk is two legs in one tree"_test[s::elf][s::gcc][s::android_ndk] = [] {
        // #630 A9: the route is chosen by the artifact's form. 1 one triple stages a flat
        // lib/libmyapp.so; 2 two Android triples stage lib/arm64-v8a/ and lib/x86_64/ in one
        // tree, the deployed file once; 3 other rows are a package each, named by the triple, a
        // target named twice is one leg, and `-o` naming one file is refused.
        s::project p{{"mcpp.toml", "[package]\nname    = \"myapp\"\nversion = \"0.1.0\"\n\n[targets.myapp]\nkind = \"app\"\nmain = \"src/main.cpp\"\n\n[[runtime.deploy]]\nfrom = \"res.txt\"\nto   = \"myres\"\n"},
                     {"src/main.cpp", "int main() { return 0; }\n"},
                     {"res.txt", "resource-1\n"}};
        auto r = p.mcpp({"pack", "myapp", "--target", "aarch64-linux-android", "--pkg-format", "dir"});
        expect(fatal(e2e::succeeded(r))) << "1: single-target Android pack failed: " << s::text(r);
        auto tree = staged(p, "myapp-0.1.0-");
        expect(fatal(fs::is_regular_file(tree / "lib/libmyapp.so"))) << "1: lib/libmyapp.so (flat) is missing";
        expect(fatal(!fs::is_directory(tree / "lib/arm64-v8a"))) << "1: a single-target pack introduced lib/arm64-v8a/";
        expect(fatal(fs::is_regular_file(tree / "bin/myres/res.txt"))) << "1: the deployed file is missing";
        fs::remove_all(p.at("target/dist"));
        r = p.mcpp({"pack", "myapp", "--target", "aarch64-linux-android", "--target", "x86_64-linux-android", "--pkg-format", "dir"});
        expect(fatal(e2e::succeeded(r))) << "2: two-target Android pack failed: " << s::text(r);
        tree = staged(p, "myapp-0.1.0-");
        expect(fatal(fs::is_regular_file(tree / "lib/arm64-v8a/libmyapp.so"))) << "2: lib/arm64-v8a/libmyapp.so is missing";
        expect(fatal(s::read_header(tree / "lib/arm64-v8a/libmyapp.so").machine == s::elf_machine::aarch64)) << "2: lib/arm64-v8a/libmyapp.so is not an aarch64 object";
        expect(fatal(fs::is_regular_file(tree / "lib/x86_64/libmyapp.so"))) << "2: lib/x86_64/libmyapp.so is missing";
        expect(fatal(s::read_header(tree / "lib/x86_64/libmyapp.so").machine == s::elf_machine::x86_64)) << "2: lib/x86_64/libmyapp.so is not an x86_64 object";
        expect(fatal(!fs::exists(tree / "lib/libmyapp.so"))) << "2: the flat lib/libmyapp.so also exists";
        expect(fatal(fs::is_regular_file(tree / "bin/myres/res.txt"))) << "2: the deployed file is missing from the multi-leg tree";
        expect(fatal(s::find_files(tree / "bin", "res.txt").size() == 1)) << "2: the deployed file was not staged once";
        r = p.mcpp({"pack", "myapp", "--target", "x86_64-linux-gnu", "--target", "x86_64-linux-gnu", "--pkg-format", "dir"});
        expect(fatal(e2e::succeeded(r))) << "3: a target named twice is one leg: " << s::text(r);
        expect(fatal(fs::is_directory(p.at("target/dist/myapp-0.1.0-x86_64-linux-gnu")))) << "3: the one leg's package is not named by its triple";
        r = p.mcpp({"pack", "myapp", "--target", "aarch64-linux-android", "--target", "x86_64-linux-gnu"});
        expect(fatal(e2e::succeeded(r))) << "3: an Android row and an executable row are a package each: " << s::text(r);
        expect(fatal(!s::glob(p.at("target/dist"), "myapp-0.1.0-x86_64-linux-gnu*.tar.gz").empty())) << "3: the executable row's package is not named by its triple";
        r = p.mcpp({"pack", "myapp", "--target", "aarch64-linux-android", "--target", "x86_64-linux-gnu", "-o", "one.tar.gz"});
        expect(fatal(failed(r))) << "3: -o naming one file was accepted for two packages";
        expect(fatal(e2e::contains(s::text(r), "use: -o <directory>/"))) << "3: the refusal names no directory: " << s::text(r);
    };

    "667 an android pack stages its closure"_test[s::elf][s::gcc][s::android_ndk] = [] {
        // An app with a shared path dependency: lib/ holds the object, libfw.so and
        // libc++_shared.so, the stubs are the platform's (the stage manifest says so); the
        // libraries carry their file names as SONAME; a prebuilt library from [runtime] is
        // staged, and when it is gone dir and tar refuse naming it; a dispatched format receives
        // `closure = not-walked`, the unresolved name and the members; two triples stage a
        // closure each.
        const auto tmp = s::case_dir();
        s::project fw(tmp / "fw", {{"mcpp.toml", "[package]\nnamespace = \"demo\"\nname      = \"fw\"\nversion   = \"0.1.0\"\n\n[targets.fw]\nkind = \"lib\"\n"},
                                   {"src/fw.cppm", "export module fw;\nexport int fw_anchor() { return 41; }\n"}});
        const std::string manifest = "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[targets.app]\nkind = \"app\"\nmain = \"src/main.cpp\"\n\n[dependencies]\ndemo.fw = { path = \"../fw\", linkage = \"shared\" }\n";
        s::project app(tmp / "app", {{"mcpp.toml", manifest}, {"src/main.cpp", "import fw;\nint main() { return fw_anchor() == 41 ? 0 : 1; }\n"}});
        auto r = app.mcpp({"pack", "--target", "x86_64-linux-android", "--pkg-format", "dir"});
        expect(fatal(e2e::succeeded(r))) << "mcpp pack --pkg-format dir failed: " << s::text(r);
        const auto tree = app.at("target/dist/app-0.1.0-x86_64-linux-android");
        expect(fatal(fs::is_directory(tree))) << "no staged tree at " << tree.string();
        const std::vector<std::string> three{"libapp.so", "libc++_shared.so", "libfw.so"};
        expect(fatal(files_in(tree / "lib") == three)) << "lib/ does not hold the object and its two libraries" << files_in(tree / "lib");
        const auto stage_manifest = fs::path(tree.string() + ".stage-manifest");
        auto m = s::read_file(stage_manifest);
        expect(fatal(first_line(m) == "closure = walked")) << "the manifest's first line is not 'closure = walked': " << m;
        for (std::string_view line : {"needs\tlibfw.so\tlib/libfw.so", "needs\tlibc++_shared.so\tlib/libc++_shared.so", "needs\tlibc.so\tplatform", "needs\tlibm.so\tplatform", "needs\tlibdl.so\tplatform"})
            expect(fatal(lines_contain(m, line))) << "the manifest lacks the line '" << line << "': " << m;
        for (std::string_view so : {"libapp.so", "libfw.so"})
            expect(fatal(s::read_header(tree / "lib" / std::string(so)).soname == so)) << "lib/" << so << " carries SONAME '" << s::read_header(tree / "lib" / std::string(so)).soname << "'";
        expect(fatal(std::ranges::contains(s::read_header(tree / "lib/libapp.so").needed, std::string("libfw.so")))) << "libapp.so no longer needs libfw.so by its file name";
        // A prebuilt library from [runtime] link_library_dirs.
        const auto cc = ndk_tool("clang");
        app.write("ext/ext.c", "int ext_value(void) { return 1; }\n");
        fs::create_directories(app.at("ext/lib"));
        const auto built = s::run(cc, {"--target=x86_64-linux-android24", "-shared", "-fPIC", "-Wl,-soname,libext.so", "-o", app.at("ext/lib/libext.so").string(), app.at("ext/ext.c").string()}, app.dir());
        expect(fatal(e2e::succeeded(built))) << "the NDK's clang did not build libext.so: " << s::text(built);
        app.write("mcpp.toml", manifest + "\n[target.'cfg(env = \"android\")'.runtime]\nlink_library_dirs = [\"ext/lib\"]\nlibraries = [\"ext\"]\n");
        app.write("src/main.cpp", "import fw;\nextern \"C\" int ext_value();\nint main() { return fw_anchor() == 41 && ext_value() == 1 ? 0 : 1; }\n");
        r = app.mcpp({"pack", "--target", "x86_64-linux-android", "--pkg-format", "dir"});
        expect(fatal(e2e::succeeded(r))) << "the pack with a prebuilt library failed: " << s::text(r);
        expect(fatal(fs::is_regular_file(tree / "lib/libext.so"))) << "the prebuilt libext.so is not staged";
        expect(fatal(lines_contain(s::read_file(stage_manifest), "needs\tlibext.so\tlib/libext.so"))) << "the manifest does not state libext.so as staged: " << s::read_file(stage_manifest);
        fs::remove(app.at("ext/lib/libext.so"));
        for (std::string_view fmt : {"dir", "tar"}) {
            r = app.mcpp({"pack", "--target", "x86_64-linux-android", "--pkg-format", std::string(fmt)});
            expect(fatal(failed(r))) << "--pkg-format " << fmt << " succeeded with libext.so gone";
            expect(fatal(e2e::contains(s::text(r), "libext.so: found in none of"))) << "--pkg-format " << fmt << " did not name libext.so: " << s::text(r);
            expect(fatal(e2e::contains(s::text(r), "incomplete"))) << "--pkg-format " << fmt << " did not say the closure is incomplete: " << s::text(r);
        }
        // A dispatched format receives the stage manifest as it stands.
        app.write("dist.sh", "set -e\nmanifest=\"$1\"; out=\"$2\"\ncp \"$manifest\" \"$out\"\n");
        app.write("build.mcpp", R"cpp(import std;
import mcpp;
int main() {
    mcpp::provides_pack_format("copy");
    if (std::string_view(mcpp::pack_format()) != "copy") return 0;
    const std::string root  = mcpp::manifest_dir();
    const std::string stage = std::string("${mcpp.stage_dir}");
    const std::string out   = std::string(mcpp::out_dir()) + "/stage.txt";
    mcpp::action a;
    a.id          = "copy";
    a.role        = "artifact";
    a.description = "copy";
    a.arg("/bin/sh")
     .arg((root + "/dist.sh").c_str())
     .arg((stage + ".stage-manifest").c_str())
     .arg(out.c_str())
     .input("${mcpp.target_file:app}")
     .output(out.c_str())
     .submit();
    return 0;
}
)cpp");
        r = app.mcpp({"pack", "--target", "x86_64-linux-android", "--pkg-format", "copy"});
        expect(fatal(e2e::succeeded(r))) << "the dispatched format failed: " << s::text(r);
        const auto seen = s::find_file(app.at("target"), "stage.txt");
        expect(fatal(seen.has_value())) << "the provider wrote nothing";
        m = s::read_file(*seen);
        expect(fatal(first_line(m) == "closure = not-walked")) << "the provider did not receive 'closure = not-walked': " << m;
        expect(fatal(lines_contain(m, "needs\tlibext.so\tunresolved"))) << "the provider did not receive libext.so as unresolved: " << m;
        expect(fatal(lines_contain(m, "needs\tlibfw.so\tlib/libfw.so"))) << "the provider lost the members that did resolve: " << m;
        // Two triples: a closure each.
        app.remove("build.mcpp");
        app.remove("dist.sh");
        app.write("mcpp.toml", manifest);
        app.write("src/main.cpp", "import fw;\nint main() { return fw_anchor() == 41 ? 0 : 1; }\n");
        fs::remove_all(app.at("target/dist"));
        r = app.mcpp({"pack", "--target", "x86_64-linux-android", "--target", "aarch64-linux-android", "--pkg-format", "dir"});
        expect(fatal(e2e::succeeded(r))) << "the two-triple pack failed: " << s::text(r);
        const auto tree2 = staged(app, "");
        for (std::string_view abi : {"x86_64", "arm64-v8a"}) {
            expect(fatal(files_in(tree2 / "lib" / std::string(abi)) == three)) << "lib/" << abi << " does not hold the closure" << files_in(tree2 / "lib" / std::string(abi));
            expect(fatal(lines_contain(s::read_file(fs::path(tree2.string() + ".stage-manifest")), std::format("needs\tlibfw.so\tlib/{}/libfw.so", abi)))) << "the manifest lacks lib/" << abi << "/libfw.so";
        }
        expect(fatal(s::read_header(tree2 / "lib/arm64-v8a/libc++_shared.so").machine == s::elf_machine::aarch64)) << "lib/arm64-v8a/libc++_shared.so is not the aarch64 run-time";
    };

    "675 android test programs carry their cxx runtime"_test[s::elf][s::gcc][s::android_ndk] = [] {
        // A test program on the Android row links the sysroot's per-API libc++.a and needs no
        // libc++_shared.so; a self-contained shared library hides the run-time it embeds
        // (`--exclude-libs`) and exports none of it.
        const auto runner = s::case_dir() / "ok-runner.sh";
        s::write_executable(runner, "#!/bin/sh\nexit 0\n");
        auto p = s::project::scaffold("droid");
        p.write("mcpp.toml", std::regex_replace(p.read("mcpp.toml"), std::regex("\n\\[dev-dependencies\\]\nmcpplibs\\.mtest = \"[^\"]*\"\n"), "")
                                 + std::format("\n[target.x86_64-linux-android]\nrunner = [\"{}\"]\n", runner.generic_string()));
        p.remove("tests/test_smoke.cpp");
        p.write("tests/strings.cpp", "#include <string>\n#include <cstdio>\nint main() { std::string s = std::to_string(42); std::puts(s.c_str()); return s == \"42\" ? 0 : 1; }\n");
        auto r = p.mcpp({"test", "--target", "x86_64-linux-android"});
        expect(fatal(e2e::succeeded(r))) << "mcpp test on the Android row failed: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), "ships no libc++.a"))) << "the contract still degraded to toolchain-coupled: " << s::text(r);
        const auto progs = s::glob(p.at("target/x86_64-linux-android"), "*/bin/strings");
        expect(fatal(!progs.empty())) << "no test program was built for x86_64-linux-android";
        const auto h = s::read_header(progs.front());
        expect(fatal(!h.needed.empty())) << "the test program's dynamic section could not be read";
        expect(fatal(!std::ranges::contains(h.needed, std::string("libc++_shared.so")))) << "the test program still needs libc++_shared.so";
        bool per_api = false;
        for (const auto& n : s::glob(p.at("target/x86_64-linux-android"), "*/build.ninja"))
            per_api = per_api || std::regex_search(s::read_file(n), std::regex("sysroot/usr/lib/x86_64-linux-android/[0-9]*/libc\\+\\+\\.a"));
        expect(fatal(per_api)) << "the link line does not name the sysroot's per-API libc++.a";

        s::project fw(s::case_dir() / "fw", {{"mcpp.toml", "[package]\nname    = \"fw\"\nversion = \"0.1.0\"\n\n[build]\ncxx_runtime = { shared = \"self-contained\" }\n\n[targets.fw]\nkind = \"shared\"\n"},
                                             {"src/fw.cppm", "module;\n#include <string>\nexport module fw;\nexport std::string fw_name() { return \"fw-\" + std::to_string(7); }\n"}});
        r = fw.mcpp({"build", "--target", "x86_64-linux-android"});
        expect(fatal(e2e::succeeded(r))) << "the shared library did not build: " << s::text(r);
        const auto so = s::find_file(fw.at("target/x86_64-linux-android"), "libfw.so");
        expect(fatal(so.has_value())) << "no libfw.so was built";
        bool hidden = false;
        for (const auto& n : s::glob(fw.at("target/x86_64-linux-android"), "*/build.ninja"))
            hidden = hidden || s::read_file(n).contains("exclude-libs,libc++_static.a");
        expect(fatal(hidden)) << "the link does not hide the archive the linker script opens";
        expect(fatal(!std::ranges::contains(s::read_header(*so).needed, std::string("libc++_shared.so")))) << "the self-contained shared library needs libc++_shared.so";
        const auto exported = s::elf_symbols(*so, ".dynsym", true);
        expect(fatal(!std::ranges::contains(exported, std::string("_ZNSt6__ndk19to_stringEi")))) << "the shared library exports the C++ run-time it embeds (std::__ndk1::to_string(int))";
    };

    "680 a dependency floor on the android api level"_test[s::elf][s::gcc][s::android_ndk] = [] {
        // A dependency states `android.api-level >= 23`: A an unset level is refused, saying so;
        // B level 21 is refused naming the key that set it; C level 24 builds; D the host row is
        // not refused and does not mention the floor.
        const std::string t = "x86_64-linux-android";
        const auto tmp = s::case_dir();
        s::project fw(tmp / "fw", {{"mcpp.toml", "[package]\nnamespace = \"demo\"\nname      = \"fw\"\nversion   = \"0.1.0\"\n\n[targets.fw]\nkind = \"lib\"\n\n[[runtime.requirements]]\nkind  = \"version-floor\"\nvalue = \"android.api-level >= 23\"\n"},
                                   {"src/fw.cppm", "export module fw;\nexport int fw_anchor() { return 41; }\n"}});
        const std::string base = "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\ndemo.fw = { path = \"../fw\" }\n";
        s::project app(tmp / "app", {{"mcpp.toml", base}, {"src/main.cpp", "import fw;\nint main() { return fw_anchor() == 41 ? 0 : 1; }\n"}});
        auto r = app.mcpp({"build", "--target", t});
        expect(fatal(failed(r))) << "A: an unset level below the floor built";
        expect(fatal(e2e::contains(s::text(r), "requires android.api-level >= 23, and this build targets"))) << "A: refusal wording: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "min_api_level is not set"))) << "A: the refusal does not say where the level came from: " << s::text(r);
        app.write("mcpp.toml", std::format("{}\n[target.{}]\nmin_api_level = 21\n", base, t));
        r = app.mcpp({"build", "--target", t});
        expect(fatal(failed(r))) << "B: level 21 built";
        expect(fatal(e2e::contains(s::text(r), "requires android.api-level >= 23, and this build targets 21"))) << "B: refusal wording: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), std::format("set by: [target.{}] min_api_level", t)))) << "B: the refusal does not name the key: " << s::text(r);
        app.write("mcpp.toml", std::format("{}\n[target.{}]\nmin_api_level = 24\n", base, t));
        r = app.mcpp({"build", "--target", t});
        expect(fatal(e2e::succeeded(r))) << "C: level 24 was refused: " << s::text(r);
        app.write("mcpp.toml", base);
        r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "D: the host row was refused: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), "android.api-level"))) << "D: the host row mentions the Android floor: " << s::text(r);
    };
}};

}  // namespace

// Library packages: the interface and headers a package publishes and what it withholds, the
// gate a prebuilt package keeps, a fat package's legs, shared libraries, an older client,
// undetermined partitions, platform coverage, module extensions, relocation, stripping, and
// the interface a library states (formerly tests/e2e/242, 243, 244, 245, 251, 252, 253, 254,
// 261, 264, 265, 828).
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
std::optional<fs::path> dist_file(const s::project& p, std::string_view prefix, std::string_view suffix) {
    for (const auto& e : entries(p.at("target/dist"))) {
        const auto n = e.filename().string();
        if (fs::is_regular_file(e) && n.starts_with(prefix) && n.ends_with(suffix)) return e;
    }
    return std::nullopt;
}

// A manifest path as the manifest spells it (`host_path`: forward slashes on every host).
std::string host_path(const fs::path& p) { return p.generic_string(); }

// Every line of `text` with the first match of `re` replaced (`sed -E 's/re/to/'`).
std::string sed(std::string_view text, const std::regex& re, const std::string& to) {
    std::string out;
    std::istringstream in{std::string(text)};
    for (std::string l; std::getline(in, l);)
        out += std::regex_replace(l, re, to, std::regex_constants::format_first_only) + "\n";
    return out;
}

// Whether some file under `root` holds `needle` (`grep -RIl needle root`).
bool any_file_holds(const fs::path& root, std::string_view needle) {
    for (const auto& f : s::find_files(root, ".*"))
        if (s::read_file(f).find(needle) != std::string::npos) return true;
    return false;
}

// SHT_SYMTAB or a .debug_* section: what stripping removes.
bool has_symtab(const fs::path& f) {
    for (const auto& sec : s::elf_sections(f))
        if (sec.type == 2 || sec.name.starts_with(".debug_")) return true;
    return false;
}

E2E library{"library", [] {
    "242 pack library interface and headers"_test = [] {
        // A library package carries its module interface and its headers; a consumer by
        // header, by module and by both at once builds and runs against it. --compress zstd
        // writes a zstd frame, --pkg-format zip a zip, and single-file is refused for a library.
        const auto tmp = s::case_dir();
        s::project lib{tmp / "mathkit", {{"src/mathkit.cppm", R"cpp(export module mathkit;
export import :api;
)cpp"}, {"src/api.cppm", R"cpp(export module mathkit:api;
export namespace mk { int add(int a, int b); }
)cpp"}, {"src/impl.cpp", R"cpp(module mathkit;
namespace mk { int add(int a, int b) { return a + b; } }
)cpp"},
                                         {"src/capi.c", R"cpp(int mathkit_add(int a, int b) { return a + b; }
)cpp"}, {"include/mathkit_c.h", R"cpp(#ifdef __cplusplus
extern "C" {
#endif
int mathkit_add(int a, int b);
#ifdef __cplusplus
}
#endif
)cpp"}, {"mcpp.toml", R"cpp([package]
name    = "mathkit"
version = "0.1.0"
[build]
sources      = ["src/*.cppm", "src/*.cpp", "src/*.c"]
include_dirs = ["include"]
[targets.mathkit]
kind = "lib"
)cpp"}}};
        auto r = lib.mcpp({"pack", "mathkit"});
        expect(fatal(e2e::succeeded(r))) << "pack failed\n" << s::text(r);
        r = lib.mcpp({"pack", "mathkit", "--pkg-format", "tar", "--compress", "zstd"});
        expect(fatal(e2e::succeeded(r))) << "pack --compress zstd failed\n" << s::text(r);
        const auto zst = dist_file(lib, "mathkit-0.1.0-", ".tar.zst");
        expect(fatal(zst.has_value())) << "no .tar.zst for --compress zstd\n" << s::text(r);
        expect(fatal(s::read_file(*zst).starts_with(std::string("\x28\xb5\x2f\xfd", 4)))) << zst->string() << " is not a zstd frame";
        r = lib.mcpp({"pack", "mathkit", "--pkg-format", "zip"});
        expect(fatal(e2e::succeeded(r))) << "pack --pkg-format zip failed\n" << s::text(r);
        expect(fatal(dist_file(lib, "mathkit-0.1.0-", ".zip").has_value())) << "no zip for --pkg-format zip\n" << s::text(r);
        r = lib.mcpp({"pack", "mathkit", "--pkg-format", "single-file"});
        expect(fatal(failed(r))) << "a library was packed as a single file\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "is a library"))) << "the single-file refusal does not say why\n" << s::text(r);
        r = lib.mcpp({"pack", "mathkit"});
        expect(fatal(e2e::succeeded(r))) << "pack failed\n" << s::text(r);
        const auto pkg = dist_dir(lib, "mathkit-0.1.0-");
        expect(fatal(pkg.has_value())) << "no package directory\n" << s::text(r);
        for (std::string_view f : {"mcpp.toml", "interface/mathkit.cppm", "interface/api.cppm", "include/mathkit_c.h"})
            expect(fatal(fs::is_regular_file(*pkg / f))) << "package is missing " << f;
        expect(fatal(!s::find_files(*pkg / "lib", R"((lib)?mathkit\..*)").empty())) << "package has no artifact under lib/<triple>/";
        expect(fatal(s::has_line(s::text(r), "Interface.*mathkit.cppm"))) << "no interface list";
        expect(fatal(s::has_line(s::text(r), "Withheld.*impl.cpp"))) << "no withheld list";
        auto consume = [&](std::string_view name, std::string_view main_cpp) {
            s::project c{tmp / name, {{"src/main.cpp", std::string(main_cpp)}, {"mcpp.toml", std::format(R"cpp([package]
name    = "{1}"
version = "0.1.0"
[dependencies]
mathkit = {{ path = "{0}" }}
[targets.{1}]
kind = "bin"
main = "src/main.cpp"
)cpp", host_path(*pkg), name)}}};
            auto run = c.mcpp({"run"});
            expect(fatal(e2e::succeeded(run))) << name << " failed\n" << s::text(run);
            expect(fatal(e2e::contains(s::text(run), "ok=5"))) << name << " printed the wrong answer\n" << s::text(run);
        };
        consume("c_hdr", "#include <cstdio>\n#include <mathkit_c.h>\nint main(){ std::printf(\"ok=%d\\n\", mathkit_add(2,3)); return 0; }\n");
        consume("c_mod", "#include <cstdio>\nimport mathkit;\nint main(){ std::printf(\"ok=%d\\n\", mk::add(2,3)); return 0; }\n");
        consume("c_both", "#include <cstdio>\n#include <mathkit_c.h>\nimport mathkit;\nint main(){ std::printf(\"ok=%d\\n\", mathkit_add(2,3) + mk::add(2,3) - 5); return 0; }\n");
    };
    "243 pack library interface closure"_test = [] {
        // The interface closure decides what is published and what is dropped: an
        // implementation partition's source is withheld and its object kept in the archive;
        // the published units' objects leave it; a root that imports the partition publishes
        // it with a warning; restoring the root withholds it again; the consumer links.
        const auto tmp = s::case_dir();
        s::project lib{tmp / "mathkit", {{"src/mathkit.cppm", R"cpp(export module mathkit;
export import :api;
)cpp"}, {"src/api.cppm", R"cpp(export module mathkit:api;
export namespace mk { int answer(); }
)cpp"}, {"src/secret.cppm", R"cpp(module mathkit:secret;
namespace mk { int secret_bias() { return 40; } }
)cpp"},
                                         {"src/impl.cpp", R"cpp(module mathkit;
namespace mk {
int secret_bias();
int answer() { return secret_bias() + 2; }
}
)cpp"}, {"mcpp.toml", R"cpp([package]
name    = "mathkit"
version = "0.1.0"
[build]
sources = ["src/*.cppm", "src/*.cpp"]
[targets.mathkit]
kind = "lib"
)cpp"}}};
        auto r = lib.mcpp({"pack", "mathkit"});
        expect(fatal(e2e::succeeded(r))) << "pack failed\n" << s::text(r);
        auto pkg = dist_dir(lib, "mathkit-0.1.0-");
        expect(fatal(pkg.has_value())) << "no package directory\n" << s::text(r);
        expect(fatal(fs::is_regular_file(*pkg / "interface/mathkit.cppm"))) << "root interface not published";
        expect(fatal(fs::is_regular_file(*pkg / "interface/api.cppm"))) << "interface partition not published";
        expect(fatal(!fs::exists(*pkg / "interface/secret.cppm"))) << "LEAK: the implementation partition's source was published";
        expect(fatal(!any_file_holds(*pkg / "interface", "secret_bias") && !any_file_holds(*pkg / "include", "secret_bias")))
            << "LEAK: implementation source text found in the published interface";
        const auto archives = s::find_files(*pkg / "lib", R"((lib)?mathkit\..*)");
        if (!archives.empty() && s::read_header(archives.front()).kind == s::format::archive) {
            std::string members;
            for (const auto& m : s::archive_members(archives.front())) members += m + "\n";
            expect(fatal(e2e::contains(members, "secret.m.o"))) << "the implementation partition's OBJECT was dropped; nothing would link\n" << members;
            expect(fatal(s::lacks(members, "mathkit.m.o"))) << "a published interface unit's object is still in the archive";
            expect(fatal(s::lacks(members, "api.m.o"))) << "a published interface unit's object is still in the archive";
        }
        const auto root = lib.read("src/mathkit.cppm");
        lib.write("src/mathkit.cppm", root + "import :secret;\n");
        lib.remove("target");
        r = lib.mcpp({"pack", "mathkit"});
        expect(fatal(e2e::succeeded(r))) << "hazard pack failed\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "secret.cppm is an implementation partition"))) << "publishing an implementation partition's source was not reported\n" << s::text(r);
        const auto hazpkg = dist_dir(lib, "mathkit-0.1.0-");
        expect(fatal(hazpkg && fs::exists(*hazpkg / "interface/secret.cppm"))) << "the warning fired but the source was not actually published";
        lib.write("src/mathkit.cppm", root);
        lib.remove("target");
        r = lib.mcpp({"pack", "mathkit"});
        expect(fatal(e2e::succeeded(r))) << "repack failed\n" << s::text(r);
        pkg = dist_dir(lib, "mathkit-0.1.0-");
        expect(fatal(pkg && !fs::exists(*pkg / "interface/secret.cppm"))) << "restore did not withhold it again";
        s::project app{tmp / "app", {{"src/main.cpp", R"cpp(#include <cstdio>
import mathkit;
int main(){ std::printf("ok=%d\n", mk::answer()); return 0; }
)cpp"}, {"mcpp.toml", std::format(R"cpp([package]
name    = "app"
version = "0.1.0"
[dependencies]
mathkit = {{ path = "{0}" }}
[targets.app]
kind = "bin"
main = "src/main.cpp"
)cpp", host_path(*pkg))}}};
        auto run = app.mcpp({"run"});
        expect(fatal(e2e::succeeded(run))) << "consumer failed\n" << s::text(run);
        expect(e2e::contains(s::text(run), "ok=42")) << "wrong answer\n" << s::text(run);
    };
    "244 pack library gate"_test = [] {
        // The three refusals a prebuilt package makes: an edited interface (its digest), a
        // package for another compiler (its abi tag, listing the published tags and naming
        // the one found), and a build inside the package; restored, the consumer runs again.
        const auto tmp = s::case_dir();
        s::project lib{tmp / "mathkit", {{"src/mathkit.cppm", R"cpp(export module mathkit;
export namespace mk { int answer(); }
)cpp"}, {"src/impl.cpp", R"cpp(module mathkit;
namespace mk { int answer() { return 42; } }
)cpp"}, {"mcpp.toml", R"cpp([package]
name    = "mathkit"
version = "0.1.0"
[build]
sources = ["src/*.cppm", "src/*.cpp"]
[targets.mathkit]
kind = "lib"
)cpp"}}};
        auto r = lib.mcpp({"pack", "mathkit"});
        expect(fatal(e2e::succeeded(r))) << "pack failed\n" << s::text(r);
        const auto found = dist_dir(lib, "mathkit-0.1.0-");
        expect(fatal(found.has_value())) << "no package directory\n" << s::text(r);
        const fs::path pkg = *found;
        s::project app{tmp / "app", {{"src/main.cpp", R"cpp(#include <cstdio>
import mathkit;
int main(){ std::printf("ok=%d\n", mk::answer()); return 0; }
)cpp"}, {"mcpp.toml", std::format(R"cpp([package]
name    = "app"
version = "0.1.0"
[dependencies]
mathkit = {{ path = "{0}" }}
[targets.app]
kind = "bin"
main = "src/main.cpp"
)cpp", host_path(pkg))}}};
        r = app.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "baseline failed\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "ok=42"))) << "baseline wrong answer\n" << s::text(r);

        const auto interface = s::read_file(pkg / "interface/mathkit.cppm");
        s::project{pkg, {{"interface/mathkit.cppm", interface + "\n// tampered\n"}}};
        app.remove("target");
        r = app.mcpp({"build"});
        expect(fatal(failed(r))) << "an edited interface built anyway — this is the silent-wrong-data case\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "does not match what was packaged"))) << "refused, but not for the interface digest\n" << s::text(r);
        s::project{pkg, {{"interface/mathkit.cppm", interface}}};

        const auto manifest = s::read_file(pkg / "mcpp.toml");
        const std::regex abi{R"re((abi[[:space:]]*=[[:space:]]*"[^"]*-)[a-z]+[0-9]+(-))re"};
        s::project{pkg, {{"mcpp.toml", sed(manifest, abi, "$1forgedstl99$2")}}};
        app.remove("target");
        r = app.mcpp({"build"});
        expect(fatal(failed(r))) << "a package built for another compiler was accepted\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "no prebuilt artifact matches this toolchain"))) << "refused, but not for the abi tag\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "published tags"))) << "the refusal did not list the published tags\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "forgedstl99"))) << "the refusal did not name the tag it found\n" << s::text(r);
        s::project{pkg, {{"mcpp.toml", manifest}}};

        r = s::mcpp({"build"}, pkg);
        expect(fatal(failed(r))) << "building inside a distribution package 'succeeded' — it compiles declarations, links nothing, and reports Finished\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "distribution package produced by"))) << "refused, but not as a distribution package\n" << s::text(r);
        app.remove("target");
        r = app.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "restore failed\n" << s::text(r);
        expect(e2e::contains(s::text(r), "ok=42")) << "restore wrong answer\n" << s::text(r);
    };
    "245 pack library fat target selection"_test[s::gcc][s::musl] = [] {
        // One package, several targets: legs selected by cfg() predicates, never a bare
        // triple, and each build (native included) picks exactly its own leg.
        const auto tmp = s::case_dir();
        s::project lib{tmp / "mathkit", {{"src/mathkit.cppm", R"cpp(export module mathkit;
export namespace mk { int answer(); }
)cpp"}, {"src/impl.cpp", R"cpp(module mathkit;
namespace mk { int answer() { return 42; } }
)cpp"}, {"mcpp.toml", R"cpp([package]
name    = "mathkit"
version = "0.1.0"
[build]
sources = ["src/*.cppm", "src/*.cpp"]
[targets.mathkit]
kind = "lib"
)cpp"}}};
        auto r = lib.mcpp({"pack", "mathkit", "--target", "x86_64-linux-gnu", "--target", "x86_64-linux-musl"});
        expect(fatal(e2e::succeeded(r))) << "fat pack failed\n" << s::text(r);
        const fs::path pkg = lib.at("target/dist/mathkit-0.1.0");
        for (std::string_view t : {"x86_64-linux-gnu", "x86_64-linux-musl"})
            expect(fatal(!s::find_files(pkg / "lib" / t, R"(libmathkit\.a)").empty())) << "no artifact for " << t;
        const auto manifest = s::read_file(pkg / "mcpp.toml");
        expect(fatal(e2e::contains(manifest, "target.'cfg("))) << "legs are not selected by cfg() predicates\n" << manifest;
        expect(fatal(s::no_line(manifest, R"(^\[target\.'x86_64-)"))) << "a leg is selected by a BARE TRIPLE, which is inert on a native build";
        s::project app{tmp / "app", {{"src/main.cpp", R"cpp(#include <cstdio>
import mathkit;
int main(){ std::printf("ok=%d\n", mk::answer()); return 0; }
)cpp"}, {"mcpp.toml", std::format(R"cpp([package]
name    = "app"
version = "0.1.0"
[dependencies]
mathkit = {{ path = "{0}" }}
[targets.app]
kind = "bin"
main = "src/main.cpp"
)cpp", host_path(pkg))}}};
        auto check = [&](std::string_view label, std::string_view want, std::vector<std::string> args) {
            app.remove("target");
            args.insert(args.begin(), "build");
            auto b = app.mcpp(args);
            expect(fatal(e2e::succeeded(b))) << label << " build failed\n" << s::text(b);
            std::optional<fs::path> nj;
            for (const auto& f : s::find_files(app.at("target"), R"(build\.ninja)"))
                if (!nj && f.generic_string().find("/.build-mcpp/") == std::string::npos) nj = f;
            expect(fatal(nj.has_value())) << label << ": no build.ninja";
            const auto text = s::read_file(*nj);
            const std::regex leg{R"(mathkit-0\.1\.0[\\/]lib[\\/]([A-Za-z0-9_-]+))"};
            std::set<std::string> legs;
            for (auto it = std::sregex_iterator(text.begin(), text.end(), leg); it != std::sregex_iterator(); ++it)
                legs.insert((*it)[1].str());
            std::string seen;
            for (const auto& l : legs) seen += l + " ";
            expect(fatal(legs.size() == 1u)) << label << " saw " << legs.size() << " leg(s), expected exactly 1: " << seen;
            expect(fatal(*legs.begin() == want)) << label << " picked the wrong leg: " << seen;
        };
        check("native", "x86_64-linux-gnu", {});
        check("gnu", "x86_64-linux-gnu", {"--target", "x86_64-linux-gnu"});
        check("musl", "x86_64-linux-musl", {"--target", "x86_64-linux-musl"});
    };
    "251 pack library shared"_test = [] {
        // A `kind = "shared"` package is produced correctly or refused clearly. Off ELF it
        // carries a .dylib or .dll. On ELF it carries the SONAME the loader asks for, records
        // the artifact as a shared library with runtime_search_dirs, and the consumer runs
        // and needs the soname.
        const auto tmp = s::case_dir();
        s::project lib{tmp / "mathkit", {{"src/mathkit.cppm", R"cpp(export module mathkit;
export namespace mk { int answer(); }
)cpp"}, {"src/impl.cpp", R"cpp(module mathkit;
namespace mk { int answer() { return 42; } }
)cpp"}, {"mcpp.toml", R"cpp([package]
name    = "mathkit"
version = "0.1.0"
[build]
sources = ["src/*.cppm", "src/*.cpp"]
[targets.mathkit-shared]
kind   = "shared"
soname = "libmathkit.so.1"
)cpp"}}};
        if (!s::on_linux()) {
            auto r = lib.mcpp({"pack", "mathkit-shared"});
            expect(fatal(e2e::succeeded(r))) << "shared pack failed off ELF\n" << s::text(r);
            const auto nonelf = dist_dir(lib, "mathkit-0.1.0-");
            expect(fatal(nonelf.has_value())) << "no package directory\n" << s::text(r);
            bool found = false;
            for (const auto& e : fs::recursive_directory_iterator(*nonelf))
                found = found || e.path().extension() == ".dylib" || e.path().extension() == ".dll";
            expect(found) << "no shared library in the package";
            return;
        }
        auto r = lib.mcpp({"pack", "mathkit-shared"});
        expect(fatal(e2e::succeeded(r))) << "shared pack failed\n" << s::text(r);
        const auto found = dist_dir(lib, "mathkit-0.1.0-");
        expect(fatal(found.has_value())) << "no package directory\n" << s::text(r);
        const fs::path pkg = *found;
        const auto so = s::find_files(pkg / "lib", R"(libmathkit-shared\.so)");
        expect(fatal(!so.empty())) << "no .so in the package";
        const auto libdir = so.front().parent_path();
        expect(fatal(fs::exists(libdir / "libmathkit.so.1"))) << "the package does not carry the SONAME the loader will ask for";
        const auto manifest = s::read_file(pkg / "mcpp.toml");
        expect(fatal(s::has_line(manifest, R"(role *= *"shared-library")"))) << "artifact is not recorded as a shared library\n" << manifest;
        expect(fatal(e2e::contains(manifest, "runtime_search_dirs"))) << "no runtime_search_dirs — the consumer could not find it\n" << manifest;
        s::project app{tmp / "app", {{"src/main.cpp", R"cpp(#include <cstdio>
import mathkit;
int main(){ std::printf("ok=%d\n", mk::answer()); return 0; }
)cpp"}, {"mcpp.toml", std::format(R"cpp([package]
name    = "app"
version = "0.1.0"
[dependencies]
mathkit = {{ path = "{0}" }}
[targets.app]
kind = "bin"
main = "src/main.cpp"
)cpp", host_path(pkg))}}};
        r = app.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "consumer failed to run\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "ok=42"))) << "wrong answer\n" << s::text(r);
        const auto exe = s::find_file(app.at("target"), "app");
        expect(fatal(exe.has_value())) << "no consumer binary";
        const auto needed = s::read_header(*exe).needed;
        expect(std::ranges::contains(needed, std::string("libmathkit.so.1"))) << "the consumer does not NEED the soname";
    };
    "252 pack library old client"_test = [] {
        // An mcpp that predates library packaging still builds against a package this one
        // produces: the generated manifest uses only sections an older mcpp reads, and when
        // $MCPP_BOOT names a released mcpp of another version, that client runs the consumer.
        const auto tmp = s::case_dir();
        s::project lib{tmp / "mathkit", {{"src/mathkit.cppm", R"cpp(export module mathkit;
export namespace mk { int answer(); }
)cpp"}, {"src/impl.cpp", R"cpp(module mathkit;
namespace mk { int answer() { return 42; } }
)cpp"}, {"mcpp.toml", R"cpp([package]
name    = "mathkit"
version = "0.1.0"
[build]
sources = ["src/*.cppm", "src/*.cpp"]
[targets.mathkit]
kind = "lib"
)cpp"}}};
        auto r = lib.mcpp({"pack", "mathkit"});
        expect(fatal(e2e::succeeded(r))) << "pack failed\n" << s::text(r);
        const auto found = dist_dir(lib, "mathkit-0.1.0-");
        expect(fatal(found.has_value())) << "no package directory\n" << s::text(r);
        const fs::path pkg = *found;
        const std::regex known{R"(^\[\[?(package|build|modules|targets\.|target\.|dependencies|dev-dependencies|runtime|profile\.|features|lib|pack|workspace|indices|resources|xlings|capabilities|tools))"};
        std::string bad;
        for (const auto& l : s::lines_with(s::read_file(pkg / "mcpp.toml"), "["))
            if (l.starts_with("[") && !std::regex_search(l, known)) bad += l + "\n";
        expect(fatal(bad.empty())) << "the generated manifest uses sections an older mcpp cannot read:\n" << bad;
        s::project app{tmp / "app", {{"src/main.cpp", R"cpp(#include <cstdio>
import mathkit;
int main(){ std::printf("ok=%d\n", mk::answer()); return 0; }
)cpp"}, {"mcpp.toml", std::format(R"cpp([package]
name    = "app"
version = "0.1.0"
[dependencies]
mathkit = {{ path = "{0}" }}
[targets.app]
kind = "bin"
main = "src/main.cpp"
)cpp", host_path(pkg))}}};
        r = app.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "the PR binary could not consume its own package\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "ok=42"))) << "wrong answer\n" << s::text(r);
        const auto boot = s::env("MCPP_BOOT");
        if (!boot || boot->empty() || !fs::is_regular_file(*boot)) return;  // the real old-client check did not run
        // Asked where the old client is to run: the bootstrap is an xvm shim, which answers only
        // inside the project that declares it, and outside it prints no version.
        auto version = [&](const std::string& program) {
            auto v = e2e::run({.argv = {program, "--version"}, .cwd = app.dir()});
            auto out = v.out;
            while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
            return out;
        };
        const auto boot_ver = version(*boot);
        const auto new_ver = version(s::mcpp_path());
        if (!std::regex_search(boot_ver, std::regex{R"(^mcpp [0-9])"}) || boot_ver == new_ver) return;
        app.remove("target");
        auto old = e2e::run({.argv = {*boot, "run"}, .cwd = app.dir()});
        expect(fatal(e2e::succeeded(old))) << "the released mcpp (" << boot_ver << ") cannot build against a package this one produced\n" << s::text(old);
        expect(e2e::contains(s::text(old), "ok=42")) << "the old client built it but ran it wrong\n" << s::text(old);
    };
    "253 pack library undetermined partition"_test = [] {
        // A scanned implementation partition is named as one and not as undetermined; a
        // partition declared in [scan_overrides] is published with "cannot tell which kind"
        // and not called an implementation partition; the consumer builds against it.
        const auto tmp = s::case_dir();
        s::project lib{tmp / "mathkit", {{"src/mathkit.cppm", R"cpp(export module mathkit;
import :secret;
export namespace mk { int answer(); }
)cpp"}, {"src/secret.cppm", R"cpp(module mathkit:secret;
namespace mk { int secret_bias() { return 40; } }
)cpp"}, {"src/impl.cpp", R"cpp(module mathkit;
namespace mk {
int secret_bias();
int answer() { return secret_bias() + 2; }
}
)cpp"}}};
        auto manifest = [&](std::string_view extra) { lib.write("mcpp.toml", std::format(R"cpp([package]
name    = "mathkit"
version = "0.1.0"
[build]
sources = ["src/*.cppm", "src/*.cpp"]
[targets.mathkit]
kind = "lib"
{0}
)cpp", extra)); };
        manifest("");
        auto r = lib.mcpp({"pack", "mathkit"});
        expect(fatal(e2e::succeeded(r))) << "pack failed\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "secret.cppm is an implementation partition"))) << "the scanned case lost its known-implementation-partition warning\n" << s::text(r);
        expect(fatal(s::lacks(s::text(r), "cannot tell which kind"))) << "mcpp read `module mathkit:secret;` and still claims it cannot tell";
        manifest("\n[scan_overrides.\"src/secret.cppm\"]\nprovides = [\"mathkit:secret\"]");
        lib.remove("target");
        r = lib.mcpp({"pack", "mathkit"});
        expect(fatal(e2e::succeeded(r))) << "override pack failed\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "cannot tell which kind"))) << "a partition declared in [scan_overrides] was published without a word\n" << s::text(r);
        expect(fatal(s::lacks(s::text(r), "secret.cppm is an implementation partition"))) << "mcpp asserted it IS an implementation partition. Nothing told it so";
        const auto found = dist_dir(lib, "mathkit-0.1.0-");
        expect(fatal(found && fs::exists(*found / "interface/secret.cppm"))) << "the warning fired but the source was not published";
        const fs::path pkg = *found;
        s::project app{tmp / "app", {{"src/main.cpp", R"cpp(#include <cstdio>
import mathkit;
int main(){ std::printf("ok=%d\n", mk::answer()); return 0; }
)cpp"}, {"mcpp.toml", std::format(R"cpp([package]
name    = "app"
version = "0.1.0"
[dependencies]
mathkit = {{ path = "{0}" }}
[targets.app]
kind = "bin"
main = "src/main.cpp"
)cpp", host_path(pkg))}}};
        r = app.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "consumer failed\n" << s::text(r);
        expect(e2e::contains(s::text(r), "ok=42")) << "wrong answer\n" << s::text(r);
    };
    "254 pack library platform coverage"_test = [] {
        // `[package] platforms` is checked against the legs packed: a platform the package
        // serves but does not claim is reported; an unservable claimed platform stays quiet;
        // a servable claimed platform with no leg is reported.
        const auto tmp = s::case_dir();
        s::project lib{tmp / "mathkit", {{"src/mathkit.cppm", R"cpp(export module mathkit;
export namespace mk { int answer(); }
)cpp"}, {"src/impl.cpp", R"cpp(module mathkit;
namespace mk { int answer() { return 42; } }
)cpp"}}};
        auto manifest = [&](std::string_view platforms) { lib.write("mcpp.toml", std::format(R"cpp([package]
name      = "mathkit"
version   = "0.1.0"
platforms = [{0}]
[build]
sources = ["src/*.cppm", "src/*.cpp"]
[targets.mathkit]
kind = "lib"
)cpp", platforms)); };
        auto pack = [&] {
            lib.remove("target");
            auto r = lib.mcpp({"pack", "mathkit"});
            expect(fatal(e2e::succeeded(r))) << "pack failed\n" << s::text(r);
            return s::text(r);
        };
        manifest(R"("linux", "macos", "windows")");
        pack();
        std::string host_triple;
        for (const auto& e : entries(lib.at("target")))
            if (host_triple.empty() && fs::is_directory(e) && e.filename() != "dist") host_triple = e.filename().string();
        std::string host_os, unservable, servable_unpacked;
        if (host_triple.find("-linux-") != std::string::npos || host_triple.ends_with("-linux")) host_os = "linux";
        else if (host_triple.find("-macos") != std::string::npos) host_os = "macos";
        else if (host_triple.find("-windows-") != std::string::npos) host_os = "windows";
        expect(fatal(!host_os.empty())) << "could not read the host platform out of target/" << host_triple;
        if (host_os == "linux") { unservable = "macos"; servable_unpacked = "windows"; }
        if (host_os == "windows") { unservable = "macos"; servable_unpacked = "linux"; }
        if (host_os == "macos") { unservable = "linux"; servable_unpacked = "windows"; }
        manifest(std::format(R"("{}")", unservable));
        auto out = pack();
        expect(fatal(e2e::contains(out, std::format("ships a {} binary", host_os)))) << "the package ships a " << host_os << " artifact while [package] platforms lists only " << unservable << ", and mcpp said nothing\n" << out;
        manifest(std::format(R"("{}", "{}")", host_os, unservable));
        out = pack();
        expect(fatal(s::lacks(out, std::format("claims {}", unservable)))) << "mcpp asked for a " << unservable << " leg on a " << host_os << " host, which cannot build one";
        expect(fatal(s::lacks(out, std::format("ships a {} binary", host_os)))) << host_os << " is declared AND packed, and mcpp still complained about it";
        manifest(std::format(R"("{}", "{}")", host_os, servable_unpacked));
        out = pack();
        expect(e2e::contains(out, std::format("claims {}", servable_unpacked))) << "[package] platforms claims " << servable_unpacked << ", this host can build for it, no such leg was packed, and mcpp said nothing\n" << out;
    };
    "261 pack follows the module extension"_test = [] {
        // `mcpp pack` follows the project's module extension (.ixx), publishes the interface
        // and not the implementation, keeps a C++ abi tag, states module_extensions for its
        // consumers (who then build without being told), and a .cppm-only package gains no
        // such key.
        const auto tmp = s::case_dir();
        s::project lib{tmp / "mathkit", {{"src/mathkit.ixx", R"cpp(export module mathkit;
export import :api;
)cpp"}, {"src/api.ixx", R"cpp(export module mathkit:api;
export namespace mk { int answer(); }
)cpp"}, {"src/impl.cpp", R"cpp(module mathkit;
namespace mk { int answer() { return 42; } }
)cpp"}, {"mcpp.toml", R"cpp([package]
name    = "mathkit"
version = "0.1.0"
[build]
sources = ["src/*.ixx", "src/*.cpp"]
module_extensions = [".ixx"]
[targets.mathkit]
kind = "lib"
)cpp"}}};
        auto r = lib.mcpp({"pack", "mathkit", "--pkg-format", "dir"});
        expect(fatal(e2e::succeeded(r))) << "pack failed\n" << s::text(r);
        const auto found = dist_dir(lib, "mathkit-0.1.0-");
        expect(fatal(found.has_value())) << "no package directory\n" << s::text(r);
        const fs::path pkg = *found;
        expect(fatal(fs::is_regular_file(pkg / "interface/mathkit.ixx") && fs::is_regular_file(pkg / "interface/api.ixx")))
            << "the module interface was not published (the convention looked for .cppm)\n" << s::text(r);
        expect(fatal(!fs::exists(pkg / "interface/impl.cpp"))) << "the implementation unit was published";
        const auto manifest = s::read_file(pkg / "mcpp.toml");
        expect(fatal(s::has_line(manifest, R"(^abi +=.*-c\+\+[0-9]+")"))) << "the leg carries a C-surface tag\n" << manifest;
        expect(fatal(e2e::contains(manifest, "module_extensions"))) << "the generated manifest lists .ixx sources without declaring the extension\n" << manifest;
        s::project app{tmp / "app", {{"src/main.cpp", R"cpp(#include <cstdio>
import mathkit;
int main(){ std::printf("pack-ixx=%d\n", mk::answer()); return 0; }
)cpp"}, {"mcpp.toml", std::format(R"cpp([package]
name    = "app"
version = "0.1.0"
[dependencies]
mathkit = {{ path = "{0}" }}
[targets.app]
kind = "bin"
main = "src/main.cpp"
)cpp", host_path(pkg))}}};
        r = app.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "the consumer had to be told about .ixx. The package should carry it.\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "pack-ixx=42"))) << "wrong answer\n" << s::text(r);
        s::project plain{tmp / "plain", {{"src/plain.cppm", R"cpp(export module plain;
export namespace pl { int v() { return 1; } }
)cpp"}, {"mcpp.toml", R"cpp([package]
name    = "plain"
version = "0.1.0"
[build]
sources = ["src/*.cppm"]
[targets.plain]
kind = "lib"
)cpp"}}};
        r = plain.mcpp({"pack", "plain", "--pkg-format", "dir"});
        expect(fatal(e2e::succeeded(r))) << "plain pack failed\n" << s::text(r);
        const auto plainpkg = dist_dir(plain, "plain-0.1.0-");
        expect(fatal(plainpkg.has_value())) << "no plain package directory";
        expect(s::lacks(s::read_file(*plainpkg / "mcpp.toml"), "module_extensions")) << "a .cppm-only package gained a module_extensions key it does not need";
    };
    "264 pack library is relocatable"_test[s::pack][s::python3] = [] {
        // A packed shared library carries no loader search path (#460); the consumer starts
        // against it with no injected environment; restoring a stale RUNPATH on the library
        // makes the consumer fail (the guard can see the defect), and the packed file restores it.
        const auto tmp = s::case_dir();
        s::project lib{tmp / "mathkit", {{"src/mathkit.cppm", R"cpp(export module mathkit;
export namespace mk { int answer(); }
)cpp"}, {"src/impl.cpp", R"cpp(module mathkit;
namespace mk { int answer() { return 42; } }
)cpp"}, {"mcpp.toml", R"cpp([package]
name    = "mathkit"
version = "0.1.0"
[build]
sources = ["src/*.cppm", "src/*.cpp"]
[targets.mathkit-shared]
kind   = "shared"
soname = "libmathkit.so.1"
)cpp"}}};
        auto r = lib.mcpp({"pack", "mathkit-shared"});
        expect(fatal(e2e::succeeded(r))) << "pack\n" << s::text(r);
        const auto found = dist_dir(lib, "mathkit-0.1.0-");
        expect(fatal(found.has_value())) << "no package directory\n" << s::text(r);
        const fs::path pkg = *found;
        fs::path libdir = pkg / "lib/x86_64-linux-gnu";
        if (!fs::is_directory(libdir)) {
            const auto so = s::find_files(pkg / "lib", R"(libmathkit-shared\.so)");
            if (!so.empty()) libdir = so.front().parent_path();
        }
        const auto so = libdir / "libmathkit-shared.so";
        expect(fatal(fs::is_regular_file(so))) << "no .so in the package";
        int swept = 0;
        for (const auto& e : s::find_files(libdir, ".*")) {  // a file, or a link to one
            const auto h = s::read_header(e);
            if (h.kind != s::format::elf || !h.wide) continue;
            ++swept;
            expect(!h.elf_rpath && !h.elf_runpath) << "a packaged library carries a loader search path (" << h.runpath << "): " << e.filename().string();
        }
        expect(fatal(swept >= 1)) << "swept no ELF — the test proved nothing";
        s::project app{tmp / "app", {{"src/main.cpp", R"cpp(#include <cstdio>
import mathkit;
int main(){ std::printf("ok=%d\n", mk::answer()); return 0; }
)cpp"}, {"mcpp.toml", std::format(R"cpp([package]
name    = "app"
version = "0.1.0"
[dependencies]
mathkit = {{ path = "{0}" }}
[targets.app]
kind = "bin"
main = "src/main.cpp"
)cpp", host_path(pkg))}}};
        r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "consumer build\n" << s::text(r);
        std::optional<fs::path> exe;
        for (const auto& f : s::find_files(app.at("target"), "app"))
            if (!exe && f.generic_string().ends_with("/bin/app")) exe = f;
        expect(fatal(exe.has_value())) << "no consumer binary";
        auto run_bare = [&] { return s::run(*exe); };  // NOT `mcpp run`: no injected environment
        auto b = run_bare();
        expect(fatal(b.status == 0 && e2e::contains(s::text(b), "ok=42"))) << "the consumer does not start against the packed library: rc=" << b.status << " out=" << s::text(b);
        fs::copy_file(so, tmp / "packed.so");
        auto pe = e2e::run({.argv = {"patchelf", "--set-rpath", "/nonexistent-machine/.mcpp/registry/data/xpkgs/xim-x-gcc/16.1.0/lib64", so.string()}});
        expect(fatal(e2e::succeeded(pe))) << "patchelf failed\n" << s::text(pe);
        b = run_bare();
        expect(fatal(b.status != 0)) << "the consumer still ran with a stale RUNPATH restored on the library: the test cannot observe the defect it exists for";
        expect(fatal(e2e::contains(s::text(b), "cannot open shared object file"))) << "the restored defect produced an unexpected failure: " << s::text(b);
        fs::copy_file(tmp / "packed.so", so, fs::copy_options::overwrite_existing);
        b = run_bare();
        expect(b.status == 0 && e2e::contains(s::text(b), "ok=42")) << "restoring the packed library did not restore the behaviour: rc=" << b.status << " out=" << s::text(b);
    };
    "265 pack strips but stays usable"_test[s::pack][s::python3] = [] {
        // `mcpp pack` ships stripped artifacts that stay usable: a static archive stays
        // linkable, a shared library loses its symbol and debug tables and still loads;
        // --no-strip and `[pack] strip = false` keep them; --debug-symbols separates them into
        // a per-triple directory and leaves a .gnu_debuglink.
        const auto tmp = s::case_dir();
        s::project lib{tmp / "mathkit", {{"src/mathkit.cppm", R"cpp(export module mathkit;
export namespace mk { int answer(); }
)cpp"}, {"src/impl.cpp", R"cpp(module mathkit;
namespace mk { int answer() { return 42; } }
)cpp"}, {"mcpp.toml", R"cpp([package]
name    = "mathkit"
version = "0.1.0"
[build]
sources = ["src/*.cppm", "src/*.cpp"]
[targets.mathkit]
kind = "lib"
[targets.mathkit-shared]
kind   = "shared"
soname = "libmathkit.so.1"
)cpp"}}};
        auto consume = [&](const fs::path& pkg, std::string_view label) {
            fs::remove_all(tmp / "consumer");
            s::project c{tmp / "consumer", {{"src/main.cpp", R"cpp(#include <cstdio>
import mathkit;
int main(){ std::printf("ok=%d\n", mk::answer()); return 0; }
)cpp"}, {"mcpp.toml", std::format(R"cpp([package]
name    = "consumer"
version = "0.1.0"
[dependencies]
mathkit = {{ path = "{0}" }}
[targets.consumer]
kind = "bin"
main = "src/main.cpp"
)cpp", host_path(pkg))}}};
            auto r = c.mcpp({"run"});
            expect(fatal(e2e::succeeded(r))) << "[" << label << "]: a consumer cannot use the stripped package\n" << s::text(r);
            expect(fatal(e2e::contains(s::text(r), "ok=42"))) << "[" << label << "]: wrong answer\n" << s::text(r);
        };
        auto pack = [&](std::vector<std::string> args, std::string_view what) {
            lib.remove("target/dist");
            args.insert(args.begin(), "pack");
            auto r = lib.mcpp(args);
            expect(fatal(e2e::succeeded(r))) << what << "\n" << s::text(r);
            const auto pkg = dist_dir(lib, "mathkit-0.1.0-");
            expect(fatal(pkg.has_value())) << what << ": no package directory";
            return std::pair{s::text(r), *pkg};
        };
        auto shared_of = [](const fs::path& pkg) {
            const auto so = s::find_files(pkg / "lib", R"(libmathkit-shared\.so)");
            expect(fatal(!so.empty())) << "no .so";
            return so.front();
        };
        auto [a_log, pkg_a] = pack({"mathkit"}, "static pack");
        expect(fatal(e2e::contains(a_log, "Stripped"))) << "the static leg was not stripped\n" << a_log;
        consume(pkg_a, "static");
        auto [b_log, pkg_b] = pack({"mathkit-shared"}, "shared pack");
        expect(fatal(e2e::contains(b_log, "Stripped"))) << "the shared leg was not stripped\n" << b_log;
        expect(fatal(!has_symtab(shared_of(pkg_b)))) << "the shared library still carries a symbol/debug table after stripping";
        consume(pkg_b, "shared");
        auto [c_log, pkg_c] = pack({"mathkit-shared", "--no-strip"}, "--no-strip pack");
        expect(fatal(s::lacks(c_log, "Stripped"))) << "--no-strip stripped anyway";
        expect(fatal(has_symtab(shared_of(pkg_c)))) << "--no-strip produced an artifact with no symbols — the flag did nothing";
        const auto saved = lib.read("mcpp.toml");
        lib.write("mcpp.toml", saved + "\n[pack]\nstrip = false\n");
        auto [d_log, pkg_d] = pack({"mathkit-shared"}, "[pack] strip pack");
        expect(fatal(s::lacks(d_log, "Stripped"))) << "[pack] strip = false was ignored";
        lib.write("mcpp.toml", saved);
        auto [e_log, pkg_e] = pack({"mathkit-shared", "--debug-symbols", "dbg"}, "--debug-symbols pack");
        const auto dbg = s::find_files(lib.at("dbg"), R"(libmathkit-shared\.so\.debug)");
        expect(fatal(!dbg.empty())) << "no separated debug file";
        const auto rel = fs::relative(dbg.front(), lib.dir()).generic_string();
        expect(fatal(std::regex_match(rel, std::regex{R"(dbg/[^/]+/libmathkit-shared\.so\.debug)"})))
            << "the debug file is not under a per-triple directory: " << rel;
        expect(fatal(has_symtab(dbg.front()))) << "the separated debug file carries no debug information";
        const auto so_e = shared_of(pkg_e);
        expect(fatal(!has_symtab(so_e))) << "--debug-symbols left the debug information in the shipped artifact too";
        bool debuglink = false;
        for (const auto& sec : s::elf_sections(so_e)) debuglink = debuglink || sec.name == ".gnu_debuglink";
        expect(debuglink) << "no .gnu_debuglink in the stripped artifact";
    };
    "828 a library states its interface"_test[s::unix_shell] = [] {
        // A library's interface is its root module and what it re-exports (#734 E6): without a
        // root the build warns (W1) and the pack names the modules it does not ship (W2) and
        // withholds both units; with a facade both ship; a consumer importing a non-public
        // module is warned (W3) naming the public ones.
        const auto tmp = s::case_dir();
        s::project lib{tmp / "lib", {{"mcpp.toml", R"cpp([package]
namespace = "probe"
name      = "flatlib"
version   = "0.1.0"

[build]
sources = ["src/alpha.cppm", "src/beta.cppm"]

[targets.flatlib]
kind = "lib"
)cpp"},
                                     {"src/alpha.cppm", "export module Alpha;\nexport int alpha() { return 1; }\n"},
                                     {"src/beta.cppm", "export module Beta;\nimport Alpha;\nexport int beta() { return alpha() + 1; }\n"}}};
        auto r = lib.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "I1: the build failed\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "without conventional lib root"))) << "I1: W1 is missing\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "impact: `mcpp pack` publishes this library without a module interface"))) << "I1: W1 has no impact line\n" << s::text(r);
        r = lib.mcpp({"pack", "flatlib"});
        expect(fatal(e2e::succeeded(r))) << "I1: the pack failed\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "exports modules Alpha, Beta that the packed form does not ship")
                     || e2e::contains(s::text(r), "exports modules Beta, Alpha that the packed form does not ship"))) << "I1: W2 does not name both modules\n" << s::text(r);
        expect(fatal(s::has_line(s::text(r), "Withheld.*alpha.cppm") && s::has_line(s::text(r), "Withheld.*beta.cppm"))) << "I1: the Withheld row does not list both units\n" << s::text(r);
        expect(fatal(s::lacks(s::text(r), "Withheld (nothing)"))) << "I1: the Withheld row still reads (nothing)";
        auto m = lib.read("mcpp.toml") + R"cpp(
[lib]
path = "src/flatlib.cppm"
)cpp";
        const std::string from = R"(sources = ["src/alpha.cppm", "src/beta.cppm"])";
        expect(fatal(m.find(from) != std::string::npos)) << "the fixture line is missing";
        m.replace(m.find(from), from.size(), R"(sources = ["src/flatlib.cppm", "src/alpha.cppm", "src/beta.cppm"])");
        lib.write("mcpp.toml", m);
        lib.write("src/flatlib.cppm", "export module probe.flatlib;\nexport import Alpha;\nexport import Beta;\n");
        r = lib.mcpp({"pack", "flatlib"});
        expect(fatal(e2e::succeeded(r))) << "I2: the pack failed\n" << s::text(r);
        expect(fatal(s::lacks(s::text(r), "does not ship"))) << "I2: W2 appeared with a facade";
        expect(fatal(s::has_line(s::text(r), "Interface.*alpha.cppm") && s::has_line(s::text(r), "Interface.*beta.cppm"))) << "I2: the facade's modules are not both shipped\n" << s::text(r);
        lib.write("src/flatlib.cppm", "export module probe.flatlib;\nexport import Alpha;\nimport Beta;\n");
        s::project app{tmp / "app", {{"mcpp.toml", R"cpp([package]
name    = "app828"
version = "0.1.0"

[dependencies]
"probe.flatlib" = { path = "../lib" }
)cpp"}, {"src/main.cpp", "import Alpha;\nint main() { return alpha() == 1 ? 0 : 1; }\n"}}};
        r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "I3: the build importing a public module failed\n" << s::text(r);
        expect(fatal(s::lacks(s::text(r), "not one of that package's public modules"))) << "I3: a public module was warned about";
        app.write("src/main.cpp", "import Beta;\nint main() { return beta() == 2 ? 0 : 1; }\n");
        r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "I3: the build importing a non-public module failed\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "imports 'Beta' of 'probe.flatlib', which is not one of that package's public modules"))) << "I3: W3 is missing\n" << s::text(r);
        expect(e2e::contains(s::text(r), "import a public module of 'probe.flatlib': Alpha")) << "I3: W3 does not name the public modules\n" << s::text(r);
    };
}};

}  // namespace

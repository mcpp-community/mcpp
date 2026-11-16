// `[resources]` of a PE image: the generated version script names its resource by ordinal 1, the
// icon and the metadata reach the image and follow an edit, an author-written script takes the
// generated one over byte for byte, a VS_VERSION_INFO without windows.h is diagnosed, a missing
// file is refused naming its key, and a target that is not PE says and emits nothing of it; each
// member's and each dependency image's resources reach its own image alone (formerly tests/e2e/197,
// 198, 199, 837, 922, 923, _windows_resources_body.sh and _dependency_resources_body.sh).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

std::string bytes_of(const fs::path& f) {
    std::ifstream in(f, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

// `text` as UTF-16LE, as a version resource stores its strings.
std::string utf16(std::string_view text) {
    std::string out;
    for (char c : text) { out += c; out += '\0'; }
    return out;
}

// A 4x1 32-bit icon whose pixels are `pixels` (16 bytes).
std::string icon(std::string_view pixels) {
    using namespace std::string_literals;
    return "\x00\x00\x01\x00\x01\x00\x04\x01\x00\x00\x01\x00\x20\x00\x3c\x00\x00\x00\x16\x00\x00\x00"
           "\x28\x00\x00\x00\x04\x00\x00\x00\x02\x00\x00\x00\x01\x00\x20\x00\x00\x00\x00\x00\x14\x00"
           "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"s
           + std::string(pixels) + "\x00\x00\x00\x00"s;
}
const std::string icon_a = "\xd3\x1c\x7a\x45\x92\xe6\x0b\xa8\x41\xf7\x2d\x63\xbe\x50\x84\x19";
const std::string icon_b = "\x6c\xa2\x38\xd7\xe1\x4b\x95\x0f\x77\xc4\x1a\x8e\x2b\xf3\x60\xd5";

std::string manifest(std::string_view resources, std::string_view package_extra = "description = \"Resource fixture\"\nlicense     = \"MIT\"\nauthors     = [\"Acme Corp\"]\n") {
    return std::format("[package]\nname        = \"resapp\"\nversion     = \"1.2.3\"\n{}\n[resources]\n{}\n\n[targets.resapp]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n",
                       package_extra, resources);
}

// The words of `rc = ...` in build.ninja, as a shell splits them.
std::vector<std::string> rc_tool(const fs::path& ninja) {
    for (const auto& l : s::lines_with(s::read_file(ninja), "rc ")) {
        std::smatch m;
        if (!std::regex_match(l, m, std::regex(R"(^rc *= *(.*)$)"))) continue;
        std::vector<std::string> words;
        std::istringstream in(m[1].str());
        for (std::string w; in >> w;) words.push_back(w);
        return words;
    }
    return {};
}

// The version resource of `file` (a compiled resource or an image) is named by ordinal 1 or by the
// string VS_VERSION_INFO: read back through the build's own resource compiler where it writes a
// script (`-J coff -O rc`), else through llvm-readobj --coff-resources.
void version_named(const fs::path& file, const fs::path& ninja, bool ordinal) {
    if (auto words = rc_tool(ninja); !words.empty()) {
        const auto out = s::case_dir() / std::format("rt.{}.rc", file.filename().string());
        auto argv = words;
        for (std::string a : {"-J", "coff", "-O", "rc", "-i"}) argv.push_back(a);
        argv.push_back(file.string());
        argv.push_back("-o");
        argv.push_back(out.string());
        auto r = e2e::run({.argv = argv});
        if (e2e::succeeded(r) && fs::exists(out) && s::read_file(out).contains("VERSIONINFO")) {
            const auto text = s::read_file(out);
            expect(fatal(ordinal ? s::has_line(text, R"(^\s*1\s+VERSIONINFO)") : s::has_line(text, R"("VS_VERSION_INFO"\s+VERSIONINFO)")))
                << "the version resource is not named by " << (ordinal ? "ordinal" : "string") << " in " << file.string() << ": " << text;
            return;
        }
    }
    std::optional<fs::path> readobj;
    for (const auto& v : s::glob(s::shared_home() / "registry/data/xpkgs/xim-x-llvm", "*"))
        if (fs::exists(v / "bin" / s::exe("llvm-readobj"))) readobj = v / "bin" / s::exe("llvm-readobj");
    expect(fatal(readobj.has_value())) << "no way to read back " << file.string() << ": the build's resource compiler cannot round-trip and no llvm-readobj is installed";
    auto r = s::run(*readobj, {"--coff-resources", file.string()});
    expect(fatal(e2e::succeeded(r))) << "llvm-readobj could not read " << file.string() << ": " << s::text(r);
    const auto at = r.out.find("Type: VERSIONINFO");
    expect(fatal(at != std::string::npos)) << file.string() << " carries no version resource at all: " << r.out;
    const auto after = s::lines_with(r.out.substr(at), "");
    bool named = false;
    for (std::size_t i = 0; i < after.size() && i <= 5; ++i)
        named = named || after[i].contains(ordinal ? "Name: (ID 1)" : "Name: VS_VERSION_INFO");
    expect(fatal(named)) << "the version resource is not named by " << (ordinal ? "ordinal" : "string") << " in " << file.string() << ": " << r.out;
}

// The body the PE builds share (197 native, 198 cross).
void resources_reach_the_image(std::vector<std::string> build_args) {
    s::project p{{"src/main.cpp", "int main() { return 0; }\n"}, {"assets/app.ico", icon(icon_a)}, {"mcpp.toml", manifest("icon = \"assets/app.ico\"")}};
    const auto build = [&](std::string_view step) {
        auto args = std::vector<std::string>{"build"};
        args.insert(args.end(), build_args.begin(), build_args.end());
        auto r = p.mcpp(args);
        expect(fatal(e2e::succeeded(r))) << step << ": " << s::text(r);
        return r;
    };
    build("build with [resources]");
    std::optional<fs::path> ninja;
    for (const auto& n : s::find_files(p.at("target"), "build.ninja"))
        if (!n.generic_string().contains("/.build-mcpp/")) { ninja = n; break; }
    expect(fatal(ninja.has_value())) << "no build dir";
    const auto dir = ninja->parent_path();
    const auto gen = dir / "res/resapp.mcpp.rc";
    expect(fatal(fs::is_regular_file(gen))) << "no generated resource script at " << gen.string();
    const auto script = s::read_file(gen);
    expect(fatal(s::has_line(script, "^1 VERSIONINFO"))) << "the generated script must use ordinal 1: " << script;
    expect(fatal(s::lacks(script, "VS_VERSION_INFO"))) << "the generated script must never name the macro: " << script;
    expect(fatal(e2e::contains(script, "FILEVERSION    1,2,3,0"))) << "FILEVERSION must come from [package].version: " << script;
    expect(fatal(e2e::contains(script, "\"CompanyName\", \"Acme Corp\""))) << "metadata must default from [package]: " << script;
    std::optional<fs::path> art;
    for (std::string_view n : {"res/resapp.mcpp.res", "res/resapp.mcpp.o"})
        if (fs::is_regular_file(dir / n)) { art = dir / n; break; }
    expect(fatal(art.has_value())) << "no compiled resource artifact under " << (dir / "res").string();
    expect(fatal(e2e::contains(s::read_file(*ninja), "rc_object"))) << "no rc_object edge";
    if (art->extension() == ".res") {
        const auto b = bytes_of(*art);
        expect(fatal(b.size() >= 48 && b.substr(40, 8) == std::string("\xff\xff\x10\x00\xff\xff\x01\x00", 8))) << "the version resource is not at ordinal 1 (the #365 defect)";
    }
    version_named(*art, *ninja, true);
    const auto exe = dir / "bin/resapp.exe";
    expect(fatal(fs::is_regular_file(exe))) << "no executable at " << exe.string();
    version_named(exe, *ninja, true);
    auto image = bytes_of(exe);
    expect(fatal(image.contains(utf16("Acme Corp")))) << "the version metadata did not reach the executable";
    expect(fatal(image.contains(icon_a))) << "the icon payload did not reach the executable";
    auto r = build("no-op rebuild");
    expect(fatal(s::has_line(s::text(r), "no work to do|Finished"))) << "unexpected rebuild output: " << s::text(r);
    std::this_thread::sleep_for(std::chrono::seconds{1});
    p.write("assets/app.ico", icon(icon_b));
    build("rebuild after an icon change");
    image = bytes_of(exe);
    expect(fatal(image.contains(icon_b))) << "editing the icon did not reach the executable (the #365 symptom)";
    expect(fatal(!image.contains(icon_a))) << "the old icon is still embedded: the resource was not rebuilt";
    std::this_thread::sleep_for(std::chrono::seconds{1});
    p.write("mcpp.toml", manifest("icon = \"assets/app.ico\"", "description = \"Changed description\"\nlicense     = \"MIT\"\nauthors     = [\"Acme Corp\"]\n"));
    build("rebuild after a metadata change");
    expect(fatal(e2e::contains(s::read_file(gen), "\"FileDescription\", \"Changed description\""))) << "the generated script did not follow [package].description";
    expect(fatal(bytes_of(exe).contains(utf16("Changed description")))) << "the changed description did not reach the executable";
    // An author-written script that is the generated one compiles to the same bytes, and mcpp adds
    // no second VERSIONINFO behind it.
    const auto generated = bytes_of(*art);
    p.write("res/app.rc", s::read_file(gen));
    p.write("mcpp.toml", manifest("files = [\"res/app.rc\"]", "description = \"Changed description\"\nlicense     = \"MIT\"\nauthors     = [\"Acme Corp\"]\n"));
    build("build with an author-written .rc");
    std::optional<fs::path> authored;
    for (std::string_view n : {"res/app.res", "res/app.o"})
        if (fs::is_regular_file(dir / n)) { authored = dir / n; break; }
    expect(fatal(authored.has_value())) << "the author-written script was not compiled";
    expect(fatal(bytes_of(*authored) == generated)) << "taking over the generated script changed the resource bytes";
    expect(fatal(s::lacks(s::read_file(*ninja), "resapp.mcpp."))) << "mcpp must not add a second VERSIONINFO behind an author-written script";
    // VS_VERSION_INFO without windows.h is the string "VS_VERSION_INFO": diagnosed with its impact.
    std::this_thread::sleep_for(std::chrono::seconds{1});
    p.write("res/app.rc", "VS_VERSION_INFO VERSIONINFO\n FILEVERSION 1,2,3,0\n PRODUCTVERSION 1,2,3,0\nBEGIN\n  BLOCK \"StringFileInfo\"\n  BEGIN\n    BLOCK \"040904b0\"\n    BEGIN\n"
                          "      VALUE \"ProductName\", \"resapp\"\n    END\n  END\n  BLOCK \"VarFileInfo\"\n  BEGIN\n    VALUE \"Translation\", 0x409, 1200\n  END\nEND\n");
    auto args = std::vector<std::string>{"build"};
    args.insert(args.end(), build_args.begin(), build_args.end());
    r = p.mcpp(args);
    expect(fatal(e2e::contains(s::text(r), "instead of ordinal 1"))) << "the VS_VERSION_INFO-without-windows.h shape must be diagnosed: " << s::text(r);
    expect(fatal(e2e::contains(s::text(r), "GetFileVersionInfo"))) << "the diagnostic must state the impact: " << s::text(r);
    std::optional<fs::path> broken;
    for (std::string_view n : {"res/app.res", "res/app.o"})
        if (fs::is_regular_file(dir / n)) { broken = dir / n; break; }
    expect(fatal(broken.has_value())) << "the broken script produced no artifact to compare against";
    version_named(*broken, *ninja, false);
    fs::remove_all(p.at("res"));
    p.write("mcpp.toml", manifest("icon = \"assets/missing.ico\"", ""));
    r = p.mcpp(args);
    expect(fatal(failed(r))) << "a missing declared resource must fail the build";
    expect(fatal(e2e::contains(s::text(r), "does not exist"))) << "expected a clear missing-file error: " << s::text(r);
}

// The bytes of a PE image's .rsrc section (empty when it has none or is not PE).
std::string rsrc_of(const fs::path& f) {
    const auto b = bytes_of(f);
    const auto u16 = [&](std::size_t at) { return at + 2 <= b.size() ? std::uint32_t(std::uint8_t(b[at])) | std::uint32_t(std::uint8_t(b[at + 1])) << 8 : 0u; };
    const auto u32 = [&](std::size_t at) { return at + 4 <= b.size() ? u16(at) | u16(at + 2) << 16 : 0u; };
    if (b.size() < 64 || b[0] != 'M' || b[1] != 'Z') return {};
    const auto pe = u32(60);
    const auto count = u16(pe + 6);
    const auto section = pe + 24 + u16(pe + 20);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto h = section + 40 * i;
        if (h + 40 > b.size()) break;
        auto name = b.substr(h, 8);
        name.erase(name.find_last_not_of('\0') + 1);
        if (name != ".rsrc") continue;
        const auto size = u32(h + 16), off = u32(h + 20);
        return off < b.size() ? b.substr(off, size) : std::string{};
    }
    return {};
}

// Each of the workspace's images (app's image.exe, updater.exe, the library's DLL) carries its own
// package's manifest in its resources and no other's, and all three were seen.
void own_resources(const fs::path& root) {
    const std::map<std::string, std::string> expected{{"app", "Fixture.Owner.app"}, {"updater", "Fixture.Owner.updater"}, {"library", "Fixture.Owner.library"}};
    std::set<std::string> seen;
    for (auto it = fs::recursive_directory_iterator(root / "target"); it != fs::recursive_directory_iterator{}; ++it) {
        const auto name = it->path().filename().string();
        if (!it->is_regular_file() || (name != "image.exe" && name != "image.dll" && name != "libimage.dll" && name != "updater.exe")) continue;
        if (bytes_of(it->path()).substr(0, 2) != "MZ") continue;
        const auto data = rsrc_of(it->path());
        const auto path = it->path().generic_string();
        std::string owner;
        if (it->path().extension() == ".dll") owner = "library";
        else if (name == "updater.exe") owner = "updater";
        else if (path.contains("/bin/app/") || path.contains("/bin/fixture.app/")) owner = "app";
        else continue;
        expect(fatal(data.contains(expected.at(owner)))) << owner << " " << path << ": missing its own manifest";
        for (const auto& [other, marker] : expected)
            if (other != owner) expect(fatal(!data.contains(marker))) << owner << " " << path << " carries " << other << "'s manifest";
        seen.insert(owner);
    }
    expect(fatal(seen.size() == 3)) << "not every image was seen" << std::vector<std::string>(seen.begin(), seen.end());
}

// The dependency-resources body (922 native, 923 cross).
void dependency_images(std::vector<std::string> target) {
    const auto root = s::case_dir() / "ws";
    const s::options inherited{.where = s::home::inherited};
    std::vector<std::pair<std::string, std::string>> files{
        {"mcpp.toml", "[workspace]\nmembers = [\"app\", \"updater\", \"library\"]\n"},
        {"app/mcpp.toml", "[package]\nnamespace = \"fixture\"\nname = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\n\"fixture.updater\" = {path = \"../updater\", artifacts = [\"updater\"]}\n\"fixture.library\" = {path = \"../library\"}\n\n[resources]\nfiles = [\"owner.rc\"]\nextra-inputs = [\"owner.manifest\"]\n\n[targets.image]\nkind = \"bin\"\nmain = \"main.cpp\"\n"},
        {"app/main.cpp", "int main() { return 0; }\n"},
        {"updater/main.cpp", "int main() { return 0; }\n"},
        {"updater/mcpp.toml", "[package]\nnamespace = \"fixture\"\nname = \"updater\"\nversion = \"0.1.0\"\n\n[resources]\nfiles = [\"owner.rc\"]\nextra-inputs = [\"owner.manifest\"]\n\n[targets.updater]\nkind = \"bin\"\nmain = \"main.cpp\"\nwindows_code_page = \"legacy\"\n"},
        {"library/mcpp.toml", "[package]\nnamespace = \"fixture\"\nname = \"library\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"lib.cpp\"]\n\n[resources]\nfiles = [\"owner.rc\"]\nextra-inputs = [\"owner.manifest\"]\n\n[targets.image]\nkind = \"shared\"\n"},
        {"library/lib.cpp", "extern \"C\" __declspec(dllexport) int library_value() { return 42; }\n"}};
    const auto manifest_of = [](std::string_view owner) {
        return std::format("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\"><assemblyIdentity version=\"1.0.0.0\" name=\"Fixture.Owner.{}\" type=\"win32\"/></assembly>\n", owner);
    };
    s::project ws(root, {});
    for (const auto& [f, c] : files) ws.write(f, c);
    for (std::string_view owner : {"app", "updater", "library"}) {
        ws.write(std::format("{}/owner.rc", owner), "1 24 \"owner.manifest\"\n");
        ws.write(std::format("{}/owner.manifest", owner), manifest_of(owner));
    }
    const auto build = [&](std::vector<std::string> args, std::string_view what) {
        args.insert(args.end(), target.begin(), target.end());
        auto r = ws.mcpp(args, inherited);
        expect(fatal(e2e::succeeded(r))) << what << ": " << s::text(r);
    };
    build({"build", "-p", "app"}, "dependency images");
    own_resources(root);
    build({"build", "-p", "updater"}, "direct updater");
    own_resources(root);
    build({"build", "--workspace"}, "workspace resources");
    own_resources(root);
    ws.write("updater/owner.manifest", std::regex_replace(ws.read("updater/owner.manifest"), std::regex("Fixture\\.Owner\\.updater"), "Fixture.Owner.updater.updated"));
    build({"build", "-p", "app"}, "manifest rebuild");
    std::vector<fs::path> artifacts;
    for (auto it = fs::recursive_directory_iterator(root / "target"); it != fs::recursive_directory_iterator{}; ++it)
        if (it->path().filename() == "updater.exe" && it->path().parent_path().filename() == "fixture_updater" && it->path().parent_path().parent_path().filename() == "obj")
            artifacts.push_back(it->path());
    expect(fatal(!artifacts.empty())) << "no updater artifact under obj/fixture_updater";
    for (const auto& f : artifacts) expect(fatal(bytes_of(f).contains("Fixture.Owner.updater.updated"))) << "the artifact's manifest stayed stale: " << f.string();
    // A standalone project over the library: its image and the library's carry their own.
    fs::rename(root / "mcpp.toml", root / "workspace.toml");
    s::project single(root / "single", {{"main.cpp", "int main() { return 0; }\n"}, {"owner.rc", "1 24 \"owner.manifest\"\n"}, {"owner.manifest", manifest_of("app")},
                                        {"mcpp.toml", "[package]\nname = \"single\"\nversion = \"0.1.0\"\n\n[dependencies]\n\"fixture.library\" = {path = \"../library\"}\n\n[resources]\nfiles = [\"owner.rc\"]\n\n[targets.image]\nkind = \"bin\"\nmain = \"main.cpp\"\n"}});
    auto args = std::vector<std::string>{"build"};
    args.insert(args.end(), target.begin(), target.end());
    auto r = single.mcpp(args, inherited);
    expect(fatal(e2e::succeeded(r))) << "standalone resources: " << s::text(r);
    std::vector<fs::path> exes, dlls;
    for (auto it = fs::recursive_directory_iterator(single.at("target")); it != fs::recursive_directory_iterator{}; ++it) {
        if (!it->is_regular_file() || it->path().parent_path().filename() != "bin") continue;
        if (it->path().filename() == "image.exe") exes.push_back(it->path());
        if (it->path().filename().string().ends_with("image.dll")) dlls.push_back(it->path());
    }
    expect(fatal(!exes.empty() && !dlls.empty())) << "standalone images";
    for (const auto& f : exes) expect(fatal(bytes_of(f).contains("Fixture.Owner.app") && !bytes_of(f).contains("Fixture.Owner.library"))) << f.string();
    for (const auto& f : dlls) expect(fatal(bytes_of(f).contains("Fixture.Owner.library") && !bytes_of(f).contains("Fixture.Owner.app"))) << f.string();
    // File existence is a target-independent fact: a native configure refuses the dependency
    // image's missing declared resource, naming it.
    fs::remove(root / "library/owner.manifest");
    fs::rename(root / "workspace.toml", root / "mcpp.toml");
    r = ws.mcpp({"build", "-p", "app", "--configure-only"}, inherited);
    expect(fatal(failed(r))) << "a missing dependency resource was accepted";
    expect(fatal(e2e::contains(s::text(r), "owner.manifest"))) << "the missing resource was not attributed: " << s::text(r);
}

E2E resources{"resources", [] {
    "197 windows resources"_test[s::windows] = [] {
        // mcpp#365 on a Windows host: see resources_reach_the_image.
        resources_reach_the_image({});
    };

    "198 windows resources cross"_test[s::mingw_cross] = [] {
        // The same through the GNU resource compiler of x86_64-windows-gnu; then the host build
        // of the same project says and emits nothing of resources, and refuses a missing file
        // as a PE build does.
        resources_reach_the_image({"--target", "x86_64-windows-gnu"});
        s::project p(s::case_dir() / "host", {{"src/main.cpp", "int main() { return 0; }\n"}, {"assets/app.ico", icon(icon_a)},
                                              {"mcpp.toml", "[package]\nname    = \"resapp\"\nversion = \"1.2.3\"\n\n[resources]\nicon = \"assets/app.ico\"\n\n[targets.resapp]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"}});
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "host build with [resources] failed: " << s::text(r);
        expect(fatal(!s::contains_nocase(s::text(r), "resource"))) << "a non-PE build must say nothing about resources: " << s::text(r);
        for (const auto& n : s::find_files(p.at("target"), "build.ninja"))
            if (!n.generic_string().contains("/.build-mcpp/") && s::lacks(s::read_file(n), "rc_object"))
                expect(fatal(!fs::is_directory(n.parent_path() / "res"))) << "a non-PE build must not emit resource units";
        p.write("mcpp.toml", std::regex_replace(p.read("mcpp.toml"), std::regex("icon = .*"), "icon = \"assets/typo.ico\""));
        r = p.mcpp({"build"});
        expect(fatal(failed(r))) << "a missing declared resource must fail on non-PE targets too";
        expect(fatal(e2e::contains(s::text(r), "does not exist"))) << "expected the same missing-file error as on Windows: " << s::text(r);
    };

    "199 resources validation"_test = [] {
        // mcpp#365, the half identical on every target: a missing file under each key is refused
        // naming the key; off PE a valid [resources] says and emits nothing.
        s::project p{{"src/main.cpp", "int main() { return 0; }\n"}, {"assets/app.ico", ""}};
        const auto write = [&](std::string_view body) {
            p.write("mcpp.toml", std::format("[package]\nname    = \"resval\"\nversion = \"1.2.3\"\n\n[resources]\n{}\n\n[targets.resval]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", body));
            fs::remove_all(p.at("target"));
        };
        for (std::string_view c : {"icon = \"assets/typo.ico\"", "extra-inputs = [\"assets/typo.h\"]", "files = [\"res/typo.rc\"]"}) {
            write(c);
            auto r = p.mcpp({"build"});
            expect(fatal(failed(r))) << "a missing declared resource must fail the build (" << c << ")";
            expect(fatal(e2e::contains(s::text(r), "does not exist"))) << "expected a clear missing-file error (" << c << "): " << s::text(r);
            const auto key = std::string(c.substr(0, c.find(' ')));
            expect(fatal(e2e::contains(s::text(r), key))) << "the error must name the [resources] key it came from (" << key << "): " << s::text(r);
        }
        if (s::on_windows()) return;  // a PE host: 197 holds the rest
        write("icon = \"assets/app.ico\"");
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "build with a valid [resources] failed: " << s::text(r);
        expect(fatal(!s::contains_nocase(s::text(r), "resource"))) << "a non-PE build must say nothing about resources: " << s::text(r);
        for (auto it = fs::recursive_directory_iterator(p.at("target")); it != fs::recursive_directory_iterator{}; ++it)
            expect(fatal(!(it->is_directory() && it->path().filename() == "res"))) << "a non-PE build must not emit resource units: " << it->path().string();
    };
    "837 a members resources reach its image"_test[s::mingw_cross] = [] {
        // R1 each member's program carries its own icon and not the other's; R2 the synthesised
        // script of a member is under res/<member>/ with that member's version and metadata; R3 a
        // member's script compiles with that member's include_dirs.
        s::project ws{{"mcpp.toml", "[workspace]\nmembers = [\"cli\", \"gui\"]\n"},
                      {"cli/assets/app.ico", icon(icon_a)}, {"cli/src/main.cpp", "int main() { return 0; }\n"},
                      {"cli/mcpp.toml", "[package]\nname    = \"cli\"\nversion = \"1.2.3\"\nauthors = [\"Cli Corp\"]\n\n[resources]\nicon = \"assets/app.ico\"\n\n[targets.cli]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                      {"gui/assets/app.ico", icon(icon_b)}, {"gui/src/main.cpp", "int main() { return 0; }\n"},
                      {"gui/include/gui_ids.h", "#define GUI_ICON 101\n"},
                      {"gui/gui.rc", "#include \"gui_ids.h\"\nGUI_ICON ICON \"assets/app.ico\"\n"},
                      {"gui/mcpp.toml", "[package]\nname    = \"gui\"\nversion = \"4.5.6\"\n\n[build]\ninclude_dirs = [\"include\"]\n\n[resources]\nfiles = [\"gui.rc\"]\nextra-inputs = [\"assets/app.ico\"]\n\n[targets.gui]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"}};
        auto r = ws.mcpp({"build", "--workspace", "--target", "x86_64-windows-gnu"});
        expect(fatal(e2e::succeeded(r))) << "workspace build: " << s::text(r);
        const auto cli = s::glob(ws.at("target"), "*/*/bin/cli/cli.exe");
        const auto gui = s::glob(ws.at("target"), "*/*/bin/gui/gui.exe");
        expect(fatal(!cli.empty() && !gui.empty())) << "the members' programs: " << s::text(r);
        expect(fatal(bytes_of(cli.front()).contains(icon_a) && !bytes_of(cli.front()).contains(icon_b))) << "R1 cli.exe does not carry cli's icon alone";
        expect(fatal(bytes_of(gui.front()).contains(icon_b) && !bytes_of(gui.front()).contains(icon_a))) << "R1 gui.exe does not carry gui's icon alone";
        const auto gen = s::glob(ws.at("target"), "*/*/res/cli/cli.mcpp.rc");
        expect(fatal(!gen.empty())) << "R2 no synthesised script for cli under res/cli/";
        const auto script = s::read_file(gen.front());
        expect(fatal(e2e::contains(script, "FILEVERSION    1,2,3,0"))) << "R2 cli's FILEVERSION is not cli's [package].version: " << script;
        expect(fatal(e2e::contains(script, "\"CompanyName\", \"Cli Corp\""))) << "R2 cli's CompanyName: " << script;
        const auto ninja = cli.front().parent_path().parent_path().parent_path() / "build.ninja";
        const auto lines = s::lines_with(s::read_file(ninja), "");
        bool with_includes = false;
        for (std::size_t i = 0; i < lines.size(); ++i)
            if (std::regex_search(lines[i], std::regex(R"(rc_object .*gui\.rc)")))
                for (std::size_t j = i; j < lines.size() && j <= i + 2; ++j) with_includes = with_includes || lines[j].contains("gui/include");
        expect(fatal(with_includes)) << "R3 gui.rc is not compiled with gui's include_dirs";
    };

    "922 dependency images keep their resources"_test[s::windows][s::python3] = [] {
        // A workspace's application, an artifact dependency's program and a library's DLL each
        // embed their own resources alone, whichever member is built, and a manifest edit
        // rebuilds the artifact; a missing dependency resource is refused naming it.
        dependency_images({});
    };

    "923 cross dependency images keep their resources"_test[s::mingw_cross][s::python3] = [] {
        // 922 through x86_64-windows-gnu.
        dependency_images({"--target", "x86_64-windows-gnu"});
    };
}};

}  // namespace

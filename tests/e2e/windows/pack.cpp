// Packing for Windows: a Windows package is a zip whoever built it, its closure is beside the
// program and excludes Windows' own DLLs, `--mode system` bundles none and refuses a
// toolchain-coupled C++ run-time; a fat library package crosses an OS boundary and each build
// takes its own leg (formerly tests/e2e/240, 248).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

// python3 running `script` with `args`: the zip is read by a reader that is not mcpp's own, since
// mcpp writes the archive itself and its own reader agreeing with it would show nothing.
e2e::outcome python(std::string_view name, std::string_view script, std::vector<std::string> args) {
    const auto file = s::case_dir() / std::format("{}.py", name);
    s::project(s::case_dir(), {{std::format("{}.py", name), script}});
    args.insert(args.begin(), file.string());
    return s::run(s::on_path("python3").value_or("python3"), std::move(args));
}

std::optional<fs::path> first_with_suffix(const fs::path& root, std::string_view suffix) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator{}; ++it)
        if (it->is_regular_file() && it->path().filename().string().ends_with(suffix)) return it->path();
    return std::nullopt;
}

constexpr std::string_view zip_closure = R"py(import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
bad = z.testzip()
assert bad is None, f"corrupt entry: {bad}"
names = z.namelist()
tops = {n.split('/')[0] for n in names}
assert len(tops) == 1, f"archive has no single wrapper directory: {tops}"
wrapper = tops.pop()
exe = f"{wrapper}/winpack.exe"
assert exe in names, f"the executable is missing: {names}"
dll = f"{wrapper}/msvcrt.dll"
assert dll in names, ("msvcrt.dll was not bundled; it is in the image's import table and in "
                      f"[pack] force_bundle, so the closure reader failed: {names}")
assert z.read(dll) == b'MZ-not-a-real-dll', "bundled the wrong file"
lower = [n.lower() for n in names]
for sysdll in ("kernel32.dll", "ntdll.dll", "ucrtbase.dll"):
    assert not any(n.endswith(sysdll) for n in lower), f"{sysdll} was bundled"
info = z.getinfo(exe)
assert (info.external_attr >> 16) & 0o111, "the executable bit was lost"
)py";

constexpr std::string_view zip_no_dll = R"py(import sys, zipfile
names = zipfile.ZipFile(sys.argv[1]).namelist()
assert not any(n.lower().endswith('.dll') for n in names), f"--mode system bundled a DLL: {names}"
)py";

E2E pack{"pack", [] {
    "240 pack pe zip cross"_test[s::mingw_cross][s::python3] = [] {
        // A x86_64-windows-gnu program packs as a zip: one wrapper directory, the program with its
        // executable bit, the DLL its import table and `force_bundle` name flat beside it, and no
        // DLL of Windows'; `--mode system` bundles no DLL; a toolchain-coupled C++ run-time with
        // `--mode system` is refused, naming the contract and the way out. `[build]
        // default-profile = "dev"` makes pack and build use one tree, into which the case drops
        // a stand-in msvcrt.dll.
        const std::string t = "x86_64-windows-gnu";
        auto p = s::project::scaffold("winpack");
        p.write("src/main.cpp", "#include <cstdio>\nint main() { std::printf(\"winpack ok\\n\"); return 0; }\n");
        p.remove("src/winpack.cppm");
        const std::string manifest = "[package]\nname = \"winpack\"\nversion = \"0.1.0\"\n\n[build]\ndefault-profile = \"dev\"\n\n[pack.bundle-project]\nforce_bundle = [\"msvcrt.dll\"]\n";
        p.write("mcpp.toml", manifest);
        auto r = p.mcpp({"build", "--target", t});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        const auto exe = s::find_file(p.at("target") / t, "winpack.exe");
        expect(fatal(exe.has_value())) << "no PE was produced";
        const auto h = s::read_header(*exe);
        expect(fatal(h.kind == s::format::pe && h.machine == 0x8664)) << "not an x86-64 PE image (machine " << h.machine << ")";
        s::project(exe->parent_path(), {{"msvcrt.dll", "MZ-not-a-real-dll"}});
        r = p.mcpp({"pack", "--target", t});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        auto zip = first_with_suffix(p.at("target/dist"), ".zip");
        expect(fatal(zip.has_value())) << "no .zip produced: a Windows package is a zip whoever built it: " << s::text(r);
        expect(fatal(!first_with_suffix(p.at("target/dist"), ".tar.gz"))) << "a .tar.gz was produced for a Windows target";
        auto v = python("closure", zip_closure, {zip->string()});
        expect(fatal(e2e::succeeded(v))) << s::text(v);
        fs::remove_all(p.at("target/dist"));
        r = p.mcpp({"pack", "--target", t, "--mode", "system"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        zip = first_with_suffix(p.at("target/dist"), ".zip");
        expect(fatal(zip.has_value())) << "no .zip from --mode system";
        v = python("nodll", zip_no_dll, {zip->string()});
        expect(fatal(e2e::succeeded(v))) << s::text(v);
        p.write("mcpp.toml", manifest + "\n[build]\ncxx_runtime = \"toolchain-coupled\"\n");
        fs::remove_all(p.at("target/dist"));
        r = p.mcpp({"pack", "--target", t, "--mode", "system"});
        expect(fatal(failed(r))) << "cxx_runtime = toolchain-coupled with --mode system was accepted: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "toolchain-coupled"))) << "the refusal does not name the contract: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "vendored"))) << "the refusal offers no way out: " << s::text(r);
    };

    "248 pack library fat pe leg"_test[s::gcc][s::mingw_cross] = [] {
        // A library packed for x86_64-linux-gnu and x86_64-windows-gnu is one package with a leg
        // each, tagged by its ABI; a PE consumer builds over it and its graph names the PE leg
        // alone.
        const auto tmp = s::case_dir();
        s::project lib(tmp / "mathkit", {{"src/mathkit.cppm", "export module mathkit;\nexport namespace mk { int answer(); }\n"},
                                         {"src/impl.cpp", "module mathkit;\nnamespace mk { int answer() { return 42; } }\n"},
                                         {"mcpp.toml", "[package]\nname    = \"mathkit\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/*.cppm\", \"src/*.cpp\"]\n\n[targets.mathkit]\nkind = \"lib\"\n"}});
        auto r = lib.mcpp({"pack", "mathkit", "--target", "x86_64-linux-gnu", "--target", "x86_64-windows-gnu"});
        expect(fatal(e2e::succeeded(r))) << "the cross-OS fat pack failed: " << s::text(r);
        const auto pkg = lib.at("target/dist/mathkit-0.1.0");
        for (std::string_view t : {"x86_64-linux-gnu", "x86_64-windows-gnu"})
            expect(fatal(s::find_file(pkg / "lib" / std::string(t), "libmathkit.a").has_value())) << "no artifact for " << t;
        const auto m = s::read_file(pkg / "mcpp.toml");
        expect(fatal(s::has_line(m, R"(abi *= *"x86_64-linux-gnu-)"))) << "no linux-gnu tag: " << m;
        expect(fatal(s::has_line(m, R"(abi *= *"x86_64-windows-gnu-)"))) << "no windows-gnu tag (the compiler's spelling leaked?): " << m;
        s::project app(tmp / "app", {{"src/main.cpp", "#include <cstdio>\nimport mathkit;\nint main(){ std::printf(\"ok=%d\\n\", mk::answer()); return 0; }\n"},
                                     {"mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\nmathkit = {{ path = \"{}\" }}\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", pkg.generic_string())}});
        r = app.mcpp({"build", "--target", "x86_64-windows-gnu"});
        expect(fatal(e2e::succeeded(r))) << "the PE consumer failed: " << s::text(r);
        const auto exe = s::find_file(app.at("target/x86_64-windows-gnu"), "app.exe");
        expect(fatal(exe.has_value())) << "no .exe produced";
        const auto h = s::read_header(*exe);
        expect(fatal(h.kind == s::format::pe && h.wide)) << "not a PE32+ image";
        const auto nj = s::find_file(app.at("target/x86_64-windows-gnu"), "build.ninja");
        expect(fatal(nj.has_value())) << "no build.ninja";
        std::set<std::string> legs;
        const auto text = s::read_file(*nj);
        const std::regex leg("dist/mathkit-0\\.1\\.0/lib/[a-z0-9_-]*");
        for (auto it = std::sregex_iterator(text.begin(), text.end(), leg); it != std::sregex_iterator{}; ++it) legs.insert(it->str());
        expect(fatal(legs.size() == 1)) << "more than one leg" << std::vector<std::string>(legs.begin(), legs.end());
        expect(fatal(legs.begin()->ends_with("lib/x86_64-windows-gnu"))) << "the PE build did not take the PE leg: " << *legs.begin();
    };
}};

}  // namespace

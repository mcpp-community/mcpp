// The runtime placement resolver (SPEC-006 §3.7.1, SPEC-007 R4.1, R4.3, R4.5; the 2026-09-28
// design WS1, WS3): the MSVC C++ runtime is placed by its rule, not by the order of the search
// directories, on every host that links a PE program (819) and on Windows, where the program runs
// (820); a DLL a runtime search directory holds reaches `mcpp run` through PATH and `mcpp pack` beside
// the packed program (794). The PE images of 819 and 820 are synthesised here: an import directory,
// the linker version and an RT_VERSION resource, what mcpp reads (formerly tests/e2e/794, 819, 820
// and _synth_pe.py).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

void put16(std::string& b, std::size_t at, std::uint32_t v) {
    b[at] = char(v & 0xff);
    b[at + 1] = char((v >> 8) & 0xff);
}
void put32(std::string& b, std::size_t at, std::uint32_t v) {
    put16(b, at, v & 0xffff);
    put16(b, at + 2, v >> 16);
}

// A minimal PE32+ image: a program importing `imports`, or a DLL whose VERSIONINFO file version
// is `version`. Never loaded.
std::string pe_image(std::vector<std::string> imports, std::optional<std::array<std::uint32_t, 4>> version) {
    std::string b(0x400, '\0');
    b[0] = 'M';
    b[1] = 'Z';
    put32(b, 0x3C, 0x80);
    const std::size_t nt = 0x80, opt = nt + 24, dirs = opt + 112, sec = opt + 240;
    b.replace(nt, 4, std::string("PE\0\0", 4));
    put16(b, nt + 4, 0x8664);
    put16(b, nt + 6, 1);
    put16(b, nt + 20, 240);
    put16(b, opt, 0x20B);
    b[opt + 2] = char(14);
    b[opt + 3] = char(44);
    put32(b, opt + 108, 16);
    b.replace(sec, 8, std::string(".data\0\0\0", 8));
    const std::uint32_t va = 0x1000, raw = 0x400, size = 0x400;
    put32(b, sec + 8, size);
    put32(b, sec + 12, va);
    put32(b, sec + 16, size);
    put32(b, sec + 20, raw);
    std::string data(size, '\0');
    if (!imports.empty()) {
        std::uint32_t names = 0x100;
        for (std::size_t i = 0; i < imports.size(); ++i) {
            data.replace(names, imports[i].size() + 1, imports[i] + std::string(1, '\0'));
            put32(data, i * 20, va + 0x80);
            put32(data, i * 20 + 12, va + names);
            put32(data, i * 20 + 16, va + 0x80);
            names += std::uint32_t(imports[i].size() + 1);
        }
        put32(b, dirs + 8, va);
        put32(b, dirs + 12, std::uint32_t((imports.size() + 1) * 20));
    }
    if (version) {
        const std::uint32_t r = 0x200, v = r + 0x60;
        put32(b, dirs + 16, va + r);
        put32(b, dirs + 20, 0x100);
        put16(data, r + 14, 1);
        put32(data, r + 16, 16);
        put32(data, r + 20, 0x80000000u | 0x18);
        put16(data, r + 0x18 + 14, 1);
        put32(data, r + 0x18 + 16, 1);
        put32(data, r + 0x18 + 20, 0x80000000u | 0x30);
        put16(data, r + 0x30 + 14, 1);
        put32(data, r + 0x30 + 16, 1033);
        put32(data, r + 0x30 + 20, 0x48);
        put32(data, r + 0x48, va + r + 0x60);
        put32(data, r + 0x4C, 92);
        put16(data, v, 92);
        put16(data, v + 2, 52);
        const std::string_view key = "VS_VERSION_INFO";
        for (std::size_t i = 0; i < key.size(); ++i) put16(data, v + 6 + 2 * i, std::uint8_t(key[i]));
        const auto [ma, mi, bu, rv] = *version;
        put32(data, v + 40, 0xFEEF04BDu);
        put32(data, v + 44, 0x00010000u);
        put32(data, v + 48, (ma << 16) | mi);
        put32(data, v + 52, (bu << 16) | rv);
    }
    return b + data;
}

std::array<std::uint32_t, 4> version_of(std::string_view text) {
    std::array<std::uint32_t, 4> v{};
    std::size_t i = 0;
    for (auto part : std::views::split(text, '.'))
        if (i < 4) v[i++] = std::uint32_t(std::stoul(std::string(part.begin(), part.end())));
    return v;
}

void write_dll(const fs::path& file, std::string_view version) {
    s::project(file.parent_path(), {{file.filename().string(), pe_image({}, version_of(version))}});
}
void write_program(const fs::path& file) {
    // Lower case, as the files are named: the closure matches a name exactly on a case-sensitive
    // file system, as a Windows one does not.
    s::project(file.parent_path(), {{file.filename().string(), pe_image({"vcruntime140.dll", "KERNEL32.dll"}, std::nullopt)}});
}
void write_set(const fs::path& dir, std::string_view version, const std::vector<std::string>& names) {
    for (const auto& n : names) write_dll(dir / n, version);
}

bool same_bytes(const fs::path& a, const fs::path& b) {
    return fs::exists(a) && fs::exists(b) && s::read_file(a) == s::read_file(b);
}

std::string lower(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

// ---- 820's builds --------------------------------------------------------------------------------

std::string app_manifest(std::string_view build, std::string_view extra = {}) {
    return std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[build]\ndefault-profile = \"dev\"\n{}\n\n{}\n", build, extra);
}

s::project make_app(std::string_view name, std::string_view build, const fs::path& rt, std::string_view extra = {}) {
    s::project p(s::case_dir() / std::string(name), {{"src/main.cpp", "import std;\nint main() { std::println(\"crt-ok\"); return 0; }\n"},
                                                     {"mcpp.toml", app_manifest(build, extra)}});
    if (!rt.empty())
        p.write("build.mcpp", std::format("import mcpp;\nint main() {{\n    mcpp::runtime_search_dir(\"{}\");\n    return 0;\n}}\n", rt.generic_string()));
    return p;
}

fs::path bindir(const s::project& p) {
    for (const auto& f : s::find_files(p.at("target"), "app\\.exe"))
        if (f.parent_path().generic_string().contains("/bin")) return f.parent_path();
    expect(fatal(false)) << "no app.exe under " << p.dir().string();
    return {};
}

nlohmann::json resolution(const s::project& p) {
    for (const auto& f : s::find_files(p.at("target"), "resolution\\.json"))
        if (!f.generic_string().contains("/.build-mcpp/")) return s::json_file(f);
    expect(fatal(false)) << "no resolution.json under " << p.dir().string();
    return {};
}

// The program run from its own directory with only the system's directory on PATH.
e2e::outcome run_clean(const fs::path& dir) {
    const auto root = s::env("SystemRoot").value_or("C:\\Windows");
    return s::run(dir / "app.exe", {}, dir, {{"PATH", root + "\\System32"}});
}

E2E placement{"placement", [] {
    "819 the crt is placed by its rule not by search order"_test = [] {
        // L1 host-coupled (--crt system) places no copy of the runtime; L2 toolchain-coupled with the
        // set already placed by the plan: the dependency's older copy is neither placed nor stated per
        // link; L3 a runtime name the plan did not see (--crt static): the toolset's whole set over an
        // older complete set, the dependency's copy stated once as a packaging fault; L4 a strictly
        // newer complete set is placed, saying so (D1); L5 a newer incomplete set does not replace
        // the toolset's; L6 a graph written before the rule (no --crt) keeps search order. The edge
        // reads the PE files, so it runs on every host.
        const std::vector<std::string> set{"vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll"};
        const auto tmp = s::case_dir();
        write_set(tmp / "toolset", "14.44.35112.1", set);
        write_set(tmp / "dep-old", "14.29.30139.0", set);
        write_set(tmp / "dep-new", "14.51.36231.0", set);
        write_set(tmp / "dep-part", "14.51.36231.0", {"vcruntime140.dll"});
        const auto toolset = (tmp / "toolset").string();
        const auto leg = [&](std::string_view name) {
            const auto d = tmp / std::string(name);
            fs::remove_all(d);
            write_program(d / "app.exe");
            return d;
        };
        const auto place = [&](const fs::path& d, std::vector<std::string> args) {
            args.insert(args.begin(), {"place-dlls", "--output", "app.exe.dlls", "--depfile", "app.exe.dlls.d"});
            auto r = s::mcpp(args, d);
            expect(fatal(e2e::succeeded(r))) << "place-dlls failed in " << d.string() << ": " << s::text(r);
        };
        const auto advice = [](const fs::path& d) {
            const auto f = d / ".mcpp-advice/app.exe.dlls.advice";
            return fs::exists(f) ? s::read_file(f) : std::string{};
        };
        auto d = leg("l1");
        place(d, {"--crt", "system", "app.exe", (tmp / "dep-old").string()});
        expect(fatal(!fs::exists(d / "vcruntime140.dll"))) << "L1: host-coupled placed a copy of the runtime";

        d = leg("l2");
        for (const auto& n : set) fs::copy_file(tmp / "toolset" / n, d / n);
        place(d, {"--crt", "carry", "--toolset-crt", toolset, "app.exe", (tmp / "dep-old").string()});
        expect(fatal(same_bytes(d / "vcruntime140.dll", tmp / "toolset/vcruntime140.dll"))) << "L2: the toolset's copy was replaced";
        expect(fatal(!advice(d).contains("vcruntime140"))) << "L2: a per-link statement about the dependency's runtime copy: " << advice(d);

        d = leg("l3");
        place(d, {"--crt", "static", "--toolset-crt", toolset, "app.exe", (tmp / "dep-old").string()});
        for (const auto& n : set) expect(fatal(same_bytes(d / n, tmp / "toolset" / n))) << "L3: " << n << " is not the toolset's copy: " << advice(d);
        expect(fatal(advice(d).contains("does not carry the compiler's runtime"))) << "L3: the dependency's copy was not stated as a packaging fault: " << advice(d);
        expect(fatal(s::count_lines(advice(d), "ships the MSVC C++ runtime") == 1)) << "L3: the packaging fault was not stated exactly once: " << advice(d);

        d = leg("l4");
        place(d, {"--crt", "static", "--toolset-crt", toolset, "app.exe", (tmp / "dep-new").string()});
        for (const auto& n : set) expect(fatal(same_bytes(d / n, tmp / "dep-new" / n))) << "L4: " << n << " is not the newer set's copy: " << advice(d);
        expect(fatal(advice(d).contains("newer than the toolset's"))) << "L4: the newer set was placed without saying so: " << advice(d);

        d = leg("l5");
        place(d, {"--crt", "static", "--toolset-crt", toolset, "app.exe", (tmp / "dep-part").string()});
        expect(fatal(same_bytes(d / "vcruntime140.dll", tmp / "toolset/vcruntime140.dll"))) << "L5: an incomplete set replaced the toolset's: " << advice(d);

        d = leg("l6");
        place(d, {"app.exe", (tmp / "dep-old").string()});
        expect(fatal(same_bytes(d / "vcruntime140.dll", tmp / "dep-old/vcruntime140.dll"))) << "L6: a graph without --crt no longer placed by search order";
    };

    "820 a windows program takes its runtime from one resolver"_test[s::windows] = [] {
        // W1 toolchain-coupled over an OLDER complete set: the toolset's set beside the program, one
        // note of the packaging fault, no warning about the runtime, and it runs with Visual Studio
        // off PATH; W2 a NEWER complete set is placed, with one note; W3 host-coupled places no copy
        // and runs; W4 a runtime file declared under host-coupled is refused; W5 `mcpp pack` carries
        // the files the build placed; W6 an action's PATH begins with the toolset's runtime
        // directory; W7 a five-member workspace states an inherited redundant CRT word once, naming
        // [workspace.build]. Read where the program's contract carries the toolset's runtime.
        const auto tmp = s::case_dir();
        auto base = make_app("base", "", {});
        auto r = base.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "the baseline build failed: " << s::text(r);
        const auto doc = resolution(base);
        const auto rule = doc["runtime"]["crt_set"].value("rule", std::string{});
        const auto spec = doc["toolchain"].value("spec", std::string{});
        if (rule != "carry") skip(std::format("the default program here does not carry the toolset's runtime (rule={}, toolchain={})", rule, spec));
        std::vector<std::pair<std::string, std::string>> toolset;  // name beside the program -> its source
        for (const auto& p : doc["runtime"]["placement"])
            if (p.value("kind", std::string{}) == "toolchain")
                toolset.emplace_back(fs::path(p["dest"].get<std::string>()).filename().string(), p["sources"][0].get<std::string>());
        expect(fatal(!toolset.empty())) << "the toolset's runtime set is not recorded in resolution.json: " << doc.dump();
        std::vector<std::string> names;
        for (const auto& [n, _] : toolset) names.push_back(n);
        const auto source_of = [&](std::string_view n) {
            for (const auto& [name, src] : toolset)
                if (lower(name) == lower(std::string(n))) return fs::path(src);
            return fs::path{};
        };
        const auto toolset_dir = fs::path(toolset.front().second).parent_path();
        std::println("820: toolchain={}, toolset runtime={}", spec, toolset_dir.string());
        write_set(tmp / "dep-old", "14.29.30139.0", names);
        write_set(tmp / "dep-new", "14.99.65000.0", names);

        // W1
        auto w1 = make_app("w1", "", tmp / "dep-old");
        r = w1.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "W1: the build failed: " << s::text(r);
        const auto b1 = bindir(w1);
        for (const auto& n : names) expect(fatal(same_bytes(b1 / n, source_of(n)))) << "W1: " << n << " beside the program is not the toolset's copy";
        expect(fatal(s::count_lines(s::text(r), "ships the MSVC C++ runtime") == 1)) << "W1: the dependency's runtime copy was not stated exactly once: " << s::text(r);
        for (const auto& line : s::lines_with(lower(s::text(r)), "warning"))
            expect(fatal(!line.contains("runtime") && !line.contains(".dll"))) << "W1: a warning concerns the runtime: " << line;
        auto run = run_clean(b1);
        expect(fatal(e2e::succeeded(run) && run.out.contains("crt-ok"))) << "W1: the program did not run with Visual Studio off PATH: " << s::text(run);

        // W2
        auto w2 = make_app("w2", "", tmp / "dep-new");
        r = w2.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "W2: the build failed: " << s::text(r);
        for (const auto& n : names) expect(fatal(same_bytes(bindir(w2) / n, tmp / "dep-new" / n))) << "W2: " << n << " is not the newer set's copy";
        expect(fatal(s::count_lines(s::text(r), "newer than the toolset's") == 1)) << "W2: the newer set was not stated exactly once: " << s::text(r);

        // W3
        auto w3 = make_app("w3", "cxx_runtime = \"host-coupled\"", tmp / "dep-old");
        r = w3.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "W3: the build failed: " << s::text(r);
        for (const auto& n : names) expect(fatal(!fs::exists(bindir(w3) / n))) << "W3: host-coupled placed " << n << " beside the program";
        expect(fatal(e2e::contains(s::text(r), "under host-coupled"))) << "W3: the dependency's runtime copy was not stated: " << s::text(r);
        run = run_clean(bindir(w3));
        expect(fatal(e2e::succeeded(run) && run.out.contains("crt-ok"))) << "W3: the host-coupled program did not run: " << s::text(run);

        // W4
        write_dll(tmp / "w4/crt/vcruntime140.dll", "14.44.35211.0");
        auto w4 = make_app("w4", "cxx_runtime = \"host-coupled\"", {}, "[runtime]\ndeploy = [ { from = \"crt/vcruntime140.dll\", to = \".\" } ]");
        r = w4.mcpp({"build"});
        expect(fatal(failed(r))) << "W4: a runtime file declared under host-coupled was accepted";
        expect(fatal(e2e::contains(s::text(r), "contract is host-coupled"))) << "W4: the refusal does not name the contract: " << s::text(r);

        // W5
        r = w1.mcpp({"pack", "--pkg-format", "dir"});
        expect(fatal(e2e::succeeded(r))) << "W5: mcpp pack failed: " << s::text(r);
        std::size_t packed = 0;
        for (auto it = fs::recursive_directory_iterator(w1.at("target/dist")); it != fs::recursive_directory_iterator{}; ++it) {
            const auto n = lower(it->path().filename().string());
            if (!it->is_regular_file() || !(n.starts_with("vcruntime140") || n.starts_with("msvcp140")) || !n.ends_with(".dll")) continue;
            expect(fatal(same_bytes(it->path(), b1 / it->path().filename()))) << "W5: the package's " << n << " is not the file the build placed";
            ++packed;
        }
        expect(fatal(packed >= 1)) << "W5: the package carries no runtime file: " << s::text(r);

        // W6
        auto python = s::on_path("python3");
        if (!python) python = s::on_path("python");
        expect(fatal(python.has_value())) << "W6: no python interpreter to report an action's PATH with";
        s::project w6(tmp / "w6", {{"src/main.cpp", "int main() { return 0; }\n"}, {"mcpp.toml", "[package]\nname    = \"act\"\nversion = \"0.1.0\"\n"},
                                   {"build.mcpp", std::format(R"cpp(import std;
import mcpp;
int main() {{
    const std::string out = std::string(mcpp::out_dir()) + "/path.txt";
    mcpp::action a;
    a.id   = "report-path";
    a.role = mcpp::roles::check;
    a.arg("{}").arg("-c")
     .arg("import os,sys; open(sys.argv[1],'w').write(os.environ.get('PATH',''))")
     .arg(out.c_str())
     .output(out.c_str())
     .submit();
    return 0;
}}
)cpp", python->generic_string())}});
        r = w6.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "W6: the build failed: " << s::text(r);
        const auto path_file = s::find_file(w6.at("target"), "path.txt");
        expect(fatal(path_file.has_value())) << "W6: the action wrote no PATH";
        auto first = s::read_file(*path_file);
        first = first.substr(0, first.find(';'));
        std::erase(first, '"');
        const auto norm = [](const fs::path& p) { return lower(p.lexically_normal().generic_string()); };
        expect(fatal(norm(first) == norm(toolset_dir) || norm(first + "/") == norm(toolset_dir) || norm(first) == norm(toolset_dir.string() + "/")))
            << "W6: the action's PATH begins with '" << first << "', not the toolset's runtime directory " << toolset_dir.string();

        // W7
        const auto word = spec.starts_with("msvc") ? std::string("/MD") : std::string("-fms-runtime-lib=dll");
        s::project ws(tmp / "ws", {{"mcpp.toml", std::format("[workspace]\nmembers = [\"m1\", \"m2\", \"m3\", \"m4\", \"m5\"]\n\n[workspace.build]\ncxxflags = [\"{}\"]\n", word)}});
        for (int i = 1; i <= 5; ++i) {
            ws.write(std::format("m{}/mcpp.toml", i), std::format("[package]\nname    = \"m{}\"\nversion = \"0.1.0\"\n", i));
            ws.write(std::format("m{}/src/main.cpp", i), "int main() { return 0; }\n");
        }
        r = ws.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "W7: the workspace build failed: " << s::text(r);
        const auto said = s::lines_with(s::text(r), "agrees with the CRT model");
        expect(fatal(said.size() == 1)) << "W7: the inherited word was stated " << said.size() << " time(s), not once: " << s::text(r);
        expect(fatal(said.front().contains("[workspace.build] cxxflags"))) << "W7: the statement does not name [workspace.build] cxxflags: " << said.front();
    };

    "794 a windows program finds a dll through runtime search dir"_test[s::windows] = [] {
        // PE has no run path, so mcpp::runtime_search_dir(dir) reaches the program two ways: `mcpp
        // run` through PATH, and `mcpp pack` by placing the DLL beside the packed program, which runs
        // from its own directory. The DLL is discovered, not a dependency: mathkit is built alone and
        // handed to app's build.mcpp as files (the vcpkg/Qt shape), under the MinGW payload for a
        // deterministic libmathkit.dll / libmathkit.dll.a pair.
        const auto tmp = s::case_dir();
        s::project mathkit(tmp / "mathkit", {{"src/mathkit.cppm", "export module mathkit;\nexport namespace mk { int answer(); }\n"},
                                             {"src/impl.cpp", "module mathkit;\nnamespace mk { int answer() { return 42; } }\nextern \"C\" __declspec(dllexport) int mk_answer_extern() { return mk::answer(); }\n"},
                                             {"mcpp.toml", "[package]\nname    = \"mathkit\"\nversion = \"0.1.0\"\n[toolchain]\nwindows = \"gcc@16.1.0\"\n[build]\nsources = [\"src/*.cppm\", \"src/*.cpp\"]\n[targets.mathkit]\nkind = \"shared\"\n"}});
        auto r = mathkit.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "mathkit build failed: " << s::text(r);
        const auto dll = s::find_file(mathkit.at("target"), "libmathkit.dll");
        const auto imp = s::find_file(mathkit.at("target"), "libmathkit.dll.a");
        expect(fatal(dll && imp)) << "mathkit did not produce a DLL and an import library";
        s::project app(tmp / "app", {{"src/main.cpp", "extern \"C\" int mk_answer_extern();\nint main() { return mk_answer_extern() == 42 ? 0 : 1; }\n"},
                                     {"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[toolchain]\nwindows = \"gcc@16.1.0\"\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                                     {"build.mcpp", std::format("import mcpp;\nint main() {{\n    mcpp::link_flag(\"-L{}\");\n    mcpp::link_flag(\"-Wl,-Bdynamic\");\n    mcpp::link_lib(\"mathkit\");\n    mcpp::runtime_search_dir(\"{}\");\n    return 0;\n}}\n",
                                                                fs::canonical(imp->parent_path()).generic_string(), fs::canonical(dll->parent_path()).generic_string())}});
        r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "app build failed: " << s::text(r);
        r = app.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "1: mcpp run did not find the library through PATH: " << s::text(r);
        r = app.mcpp({"pack", "--pkg-format", "dir"});
        expect(fatal(e2e::succeeded(r))) << "pack failed: " << s::text(r);
        const auto packed = s::find_file(app.at("target/dist"), "app.exe");
        expect(fatal(packed.has_value())) << "no packed app.exe";
        expect(fatal(fs::exists(packed->parent_path() / "libmathkit.dll"))) << "2: mcpp pack did not place the DLL from the runtime search directory beside the exe";
        const auto alone = s::run(*packed, {}, packed->parent_path());
        expect(fatal(e2e::succeeded(alone))) << "3: the packed program did not run from its own directory: " << s::text(alone);
    };
}};

}  // namespace

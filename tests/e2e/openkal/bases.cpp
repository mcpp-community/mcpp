// The target base a build stands on: a stated sysroot is the C library the artifact is built
// against and runs on, the host's own root is a base, and a toolchain phase states the base
// (formerly tests/e2e/925, 926, 930).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

// `sort -V` of numeric components.
bool version_less(std::string_view a, std::string_view b) {
    const auto parts = [](std::string_view v) {
        std::vector<long long> out;
        for (auto p : std::views::split(v, '.')) {
            long long n = 0;
            for (char c : p) {
                if (c < '0' || c > '9') break;
                n = n * 10 + (c - '0');
            }
            out.push_back(n);
        }
        return out;
    };
    return parts(a) < parts(b);
}

// The newest version directory of `package` in the shared home's registry (`ls -d .../*/ | sort -V | tail -1`).
std::optional<fs::path> newest_version_dir(std::string_view package) {
    std::optional<fs::path> best;
    for (const auto& v : s::glob(s::shared_home() / "registry/data/xpkgs" / package, "*"))
        if (fs::is_directory(v) && (!best || version_less(best->filename().string(), v.filename().string()))) best = v;
    return best;
}

// tests/e2e/_sysroot_fixture.sh's make_glibc_sysroot: a glibc sysroot in crosstool-ng's layout
// (usr/include, usr/lib64, lib64 -> usr/lib64) assembled from the home's glibc and linux-headers
// payloads, the linker scripts' absolute paths made sysroot-relative. The glibc version
// ("2.44"), or nothing when no payload is installed.
std::optional<std::string> make_glibc_sysroot(const fs::path& dest) {
    const auto g = newest_version_dir("xim-x-glibc");
    const auto h = newest_version_dir("xim-x-linux-headers");
    if (!g || !h || !fs::is_directory(*g / "include")) return std::nullopt;
    fs::remove_all(dest);
    for (std::string_view d : {"usr/include", "usr/lib64", "lib", "usr/lib"}) fs::create_directories(dest / d);
    fs::create_directory_symlink("usr/lib64", dest / "lib64");
    constexpr auto copy = fs::copy_options::recursive | fs::copy_options::copy_symlinks | fs::copy_options::overwrite_existing;
    fs::copy(*g / "include", dest / "usr/include", copy);
    fs::copy(*h / "include", dest / "usr/include", copy);
    // `find <g>/lib/ -maxdepth 1 \( -type f -o -type l \) -exec cp -a {} usr/lib64/ \;`
    for (const auto& e : fs::directory_iterator(*g / "lib")) {
        if (e.is_symlink()) fs::copy_symlink(e.path(), dest / "usr/lib64" / e.path().filename());
        else if (e.is_regular_file()) fs::copy_file(e.path(), dest / "usr/lib64" / e.path().filename(), fs::copy_options::overwrite_existing);
    }
    for (const auto& e : fs::recursive_directory_iterator(dest))
        if (!e.is_symlink()) fs::permissions(e.path(), fs::perms::owner_write, fs::perm_options::add);
    // The text .so files that are linker scripts (`grep -lI "GROUP\|INPUT"`).
    const auto gs = g->string();
    for (const auto& so : s::glob(dest / "usr/lib64", "*.so")) {
        if (fs::is_symlink(so) || !fs::is_regular_file(so)) continue;
        auto text = s::read_file(so);
        if (text.contains('\0') || !(text.contains("GROUP") || text.contains("INPUT"))) continue;
        for (const auto& from : {gs + "/lib64/", gs + "/lib/"})
            for (std::size_t at; (at = text.find(from)) != std::string::npos;) text.replace(at, from.size(), "/lib64/");
        std::ofstream(so, std::ios::binary | std::ios::trunc) << text;
    }
    std::smatch m;
    const auto features = s::read_file(dest / "usr/include/features.h");
    if (!std::regex_search(features, m, std::regex{R"((^|\n)#define[ \t]*__GLIBC_MINOR__[ \t]*([0-9]*))"})) return std::nullopt;
    return "2." + m[2].str();
}

// `str` as a literal of an ECMAScript expression.
std::string literal(std::string_view str) {
    static const std::regex special{R"([.^$|()\[\]{}*+?\\])"};
    return std::regex_replace(std::string(str), special, R"(\$&)");
}

// The newest of `target/x86_64-linux-gnu/*/bin/app` (`ls ... | xargs ls -t | head -1`).
std::optional<fs::path> artifact(const s::project& p) {
    std::optional<fs::path> best;
    for (const auto& f : s::glob(p.at("target/x86_64-linux-gnu"), "*/bin/app"))
        if (!best || fs::last_write_time(f) > fs::last_write_time(*best)) best = f;
    return best;
}

const s::options plain{.env = {{"NO_COLOR", "1"}}};

E2E bases{"bases", [] {
    "925 a stated sysroot is the c library"_test[s::gcc][s::elf][s::unix_shell][s::python3] = [] {
        // S2: a sysroot named by a relative path is the base the artifact is built against and
        // runs on (PT_INTERP and RUNPATH the sysroot's, not the SubOS farm), in a directory of
        // its own, described in resolution.json and by `why sysroot`. S4: a base rewritten in
        // place is built again in its directory. S7: overrides outrank the project, the machine's default fills
        // a gap. S6: what is not a sysroot, and what is not there. S3: a base older than the
        // compiler's runtime is refused naming both. S1: a toolchain named by path with the
        // sysroot it carries, and pack states the C library floor.
#if !defined(__x86_64__) && !defined(_M_X64)
        skip("the fixture sysroot is x86_64");
#endif
        const auto work = s::case_dir();
        s::project app(work / "app", {{"src/main.cpp", "#include <gnu/libc-version.h>\nimport std;\nint main() { std::println(\"built {}.{} ran {}\", __GLIBC__, __GLIBC_MINOR__, gnu_get_libc_version()); }\n"}});
        const auto manifest = [&](std::string_view target_body, std::string_view more, bool with_default = true) {
            app.write("mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[toolchain]\n{}{}\n[target.x86_64-linux-gnu]\n{}\n",
                                               with_default ? std::format("default = \"gcc@{}\"\n", s::gcc_version()) : "", more, target_body));
        };
        // The default build first: it installs the payloads the fixture is made of, and its
        // output directory is the one a stated base must not reuse.
        manifest("", "");
        auto r = app.mcpp({"build"}, plain);
        expect(fatal(e2e::succeeded(r))) << "the default build failed: " << s::text(r);
        const auto defaults = s::glob(app.at("target/x86_64-linux-gnu"), "*");
        expect(fatal(!defaults.empty())) << "no default output directory";
        const auto default_dir = defaults.front();
        const auto glibc = make_glibc_sysroot(app.at("third_party/sysroot"));
        if (!glibc) skip("no glibc payload to build a sysroot from");
        const auto sr = app.at("third_party/sysroot");

        // S2
        manifest(R"(sysroot = { path = "third_party/sysroot" })", "");
        r = app.mcpp({"run"}, plain);
        auto out = s::text(r);
        expect(fatal(e2e::succeeded(r))) << "S2: the build or the run failed: " << out;
        expect(fatal(e2e::contains(out, std::format("Using sysroot glibc {} ← third_party/sysroot  [custom · mcpp.toml:", *glibc)))) << "S2: no Using line naming the sysroot, its C library and where it was stated: " << out;
        expect(fatal(e2e::contains(out, std::format("built {0} ran {0}", *glibc)))) << "S2: the program did not run on the sysroot's glibc: " << out;
        auto a = artifact(app);
        expect(fatal(a.has_value())) << "S2: no artifact";
        auto h = s::read_header(*a);
        expect(fatal(h.interpreter == (sr / "lib64/ld-linux-x86-64.so.2").string())) << "S2: PT_INTERP is " << h.interpreter;
        expect(fatal(h.runpath.starts_with((sr / "lib64").string() + ":"))) << "S2: RUNPATH does not start with the sysroot: " << h.runpath;
        expect(fatal(s::lacks(h.runpath, "subos"))) << "S2: the SubOS farm reached a stated base's RUNPATH: " << h.runpath;
        const auto stated_dir = a->parent_path().parent_path();
        expect(fatal(stated_dir != default_dir)) << "S2: the stated base reused the default output directory (" << default_dir.filename().string() << ")";
        const auto base = s::json_file(stated_dir / "resolution.json")["base"];
        expect(fatal(base.value("form", "") == "path" && base.value("runtime", "") == "binding" && base["c_abi"].value("impl", "") == "glibc")) << "S2: resolution.json does not describe the base: " << base.dump();
        expect(fatal(base.contains("identity") && !base["identity"].is_null() && base["identity"] != "" && base["identity"] != false)) << "S2: resolution.json's base has no identity: " << base.dump();
        r = app.mcpp({"why", "sysroot"}, plain);
        expect(fatal(e2e::succeeded(r))) << "S2: mcpp why sysroot failed: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "address=gnu-sysroot  runtime=binding"))) << "S2: why sysroot omits the address and runtime: " << s::text(r);

        // S4: `touch -d '2001-01-01'` of the base's features.h
        const auto first = artifact(app)->parent_path().parent_path();
        {
            std::tm when{};
            when.tm_year = 101, when.tm_mon = 0, when.tm_mday = 1, when.tm_isdst = -1;
            const auto t = std::chrono::system_clock::from_time_t(std::mktime(&when));
            fs::last_write_time(sr / "usr/include/features.h", s::file_time(t));
        }
        r = app.mcpp({"build"}, plain);
        expect(fatal(e2e::succeeded(r))) << "S4: the rebuild failed: " << s::text(r);
        // The base as stated names the configuration's directory; what it holds evolves (design
        // 2026-11-12 §17): the same directory, emptied and built again.
        expect(fatal(artifact(app)->parent_path().parent_path() == first)) << "S4: a sysroot rewritten in place moved to another directory";
        expect(fatal(e2e::contains(s::text(r), "the base changed, and nothing compiled there is reused"))) << "S4: a sysroot rewritten in place reused what was compiled against it: " << s::text(r);

        // S7
        const auto other = work / "other";
        expect(fatal(make_glibc_sysroot(other).has_value()));
        r = app.mcpp({"build"}, {.env = {{"NO_COLOR", "1"}, {"MCPP_SYSROOT", "path:" + other.string()}}});
        expect(fatal(e2e::succeeded(r))) << "S7: MCPP_SYSROOT failed: " << s::text(r);
        expect(fatal(s::has_line(s::text(r), std::format("Using sysroot glibc .* ← {}  \\[custom · env MCPP_SYSROOT\\]", literal(other.string()))))) << "S7: MCPP_SYSROOT did not outrank the manifest: " << s::text(r);
        r = app.mcpp({"build"}, {.env = {{"NO_COLOR", "1"}, {"MCPP_TARGET_X86_64_LINUX_GNU_SYSROOT", "path:" + other.string()}}});
        expect(fatal(e2e::succeeded(r))) << "S7: the per-target variable failed: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "env MCPP_TARGET_X86_64_LINUX_GNU_SYSROOT"))) << "S7: the per-target variable was not used: " << s::text(r);
        r = app.mcpp({"--managed-only", "build"}, plain);
        expect(fatal(e2e::contains(s::text(r), "sysroot ← third_party/sysroot"))) << "S7: --managed-only did not name the sysroot: " << s::text(r);
        // The machine's default: [target.<triple>].sysroot in config.toml, appended to the
        // case's inherited home's copy (the script appended to the shared home's and restored it).
        manifest("", "");
        const auto home = s::case_home(s::home::inherited);
        {
            std::ofstream cfg(home / "config.toml", std::ios::app);
            cfg << std::format("\n[target.x86_64-linux-gnu]\nsysroot = {{ path = \"{}\" }}\n", other.string());
        }
        r = app.mcpp({"build"}, {.where = s::home::inherited, .env = {{"NO_COLOR", "1"}}});
        expect(fatal(e2e::succeeded(r))) << "S7: the machine default failed: " << s::text(r);
        expect(fatal(s::has_line(s::text(r), std::format("Using sysroot glibc .* ← {}  \\[custom · .*config\\.toml", literal(other.string()))))) << "S7: config.toml's [target.<triple>].sysroot was not the default: " << s::text(r);

        // S6
        manifest(R"(sysroot = { path = "does/not/exist" })", "");
        out = s::text(app.mcpp({"build"}, plain));
        expect(fatal(e2e::contains(out, "which does not exist"))) << "S6: a missing path was not named: " << out;
        expect(fatal(e2e::contains(out, "publish it as a package in your index"))) << "S6: the message omits how to make it exist: " << out;
        expect(fatal(e2e::contains(out, "MCPP_TARGET_X86_64_LINUX_GNU_SYSROOT=path:<dir>"))) << "S6: the message omits the local override: " << out;
        fs::create_directories(app.at("libs-only/usr/include"));
        manifest(R"(sysroot = { path = "libs-only" })", "");
        out = s::text(app.mcpp({"build"}, plain));
        expect(fatal(e2e::contains(out, "library prefix rather than a sysroot"))) << "S6: a directory with no C library was accepted: " << out;

        // S3: `sed -i 's/^\(#define[[:space:]]*__GLIBC_MINOR__[[:space:]]*\)[0-9]*/\120/'`
        const auto old = work / "old";
        expect(fatal(make_glibc_sysroot(old).has_value()));
        {
            const auto file = old / "usr/include/features.h";
            std::string text;
            std::istringstream in{s::read_file(file)};
            const std::regex minor{R"(^(#define[ \t]*__GLIBC_MINOR__[ \t]*)[0-9]*(.*)$)"};
            for (std::string l; std::getline(in, l);) {
                std::smatch m;
                text += (std::regex_match(l, m, minor) ? m[1].str() + "20" + m[2].str() : l) + "\n";
            }
            fs::remove(file);
            std::ofstream(file, std::ios::binary) << text;
        }
        manifest(std::format("sysroot = {{ path = \"{}\" }}", old.string()), "");
        out = s::text(app.mcpp({"build"}, plain));
        expect(fatal(e2e::contains(out, "runtime requires glibc >= ") && e2e::contains(out, "this sysroot provides glibc 2.20"))) << "S3: a base older than the compiler's runtime was not refused naming both versions: " << out;
        expect(fatal(s::no_line(out, "error:.*std\\.cc"))) << "S3: the refusal arrived as a std module error: " << out;

        // S1: [toolchain.linux] { path, sysroot }, the `default = ` line removed
        manifest("", std::format("[toolchain.linux]\npath    = \"{}\"\nsysroot = \"{}\"", s::payload_root("xim-x-gcc", s::gcc_version()).string(), sr.string()), false);
        r = app.mcpp({"run"}, plain);
        out = s::text(r);
        expect(fatal(e2e::succeeded(r))) << "S1: [toolchain] { path, sysroot } failed: " << out;
        expect(fatal(e2e::contains(out, std::format("built {0} ran {0}", *glibc)))) << "S1: the program did not run on the carried sysroot: " << out;
        h = s::read_header(*artifact(app));
        expect(fatal(h.interpreter == (sr / "lib64/ld-linux-x86-64.so.2").string())) << "S1: PT_INTERP is not the carried sysroot's: " << h.interpreter;
        // `--no-strip`: a tree named by path need not carry binutils.
        r = app.mcpp({"pack", "--no-strip"}, plain);
        expect(fatal(e2e::succeeded(r))) << "S1: pack failed: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "Requires glibc >= "))) << "S1: pack did not state the C library floor: " << s::text(r);
    };

    "926 the hosts own root is a base"_test[s::gcc][s::llvm][s::elf][s::unix_shell] = [] {
        // `glibc@system` with gcc and with llvm: the Using line names the host's base, the
        // program runs on the host's glibc with the host's loader and no SubOS farm; the
        // multiarch directories are supplied to gcc and not to clang, which knows them.
#if defined(__x86_64__) || defined(_M_X64)
        const std::string tuple = "x86_64-linux-gnu";
#elif defined(__aarch64__)
        const std::string tuple = "aarch64-linux-gnu";
#else
        const std::string tuple = "unknown-linux-gnu";
#endif
        if (!fs::exists("/usr/lib/" + tuple + "/crt1.o")) skip(std::format("this host is not a Debian multiarch layout (/usr/lib/{}/crt1.o)", tuple));
        // `getconf GNU_LIBC_VERSION`
        const auto getconf = s::on_path("getconf");
        expect(fatal(getconf.has_value())) << "getconf is not on PATH";
        const auto version_line = s::last_line(s::run(*getconf, {"GNU_LIBC_VERSION"}).out);
        const auto host_glibc = version_line.substr(version_line.find(' ') + 1);
        s::project app({{"src/main.cpp", "#include <gnu/libc-version.h>\nimport std;\nint main() { std::println(\"built {}.{} ran {}\", __GLIBC__, __GLIBC_MINOR__, gnu_get_libc_version()); }\n"}});
        for (const auto& tc : {"gcc@" + s::gcc_version(), "llvm@" + s::llvm_version()}) {
            app.write("mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[toolchain]\ndefault = \"{}\"\n[target.{}]\nsysroot = \"glibc@system\"\n", tc, tuple));
            fs::remove_all(app.at("target"));
            auto r = app.mcpp({"run"}, plain);
            const auto out = s::text(r);
            expect(fatal(e2e::succeeded(r))) << tc << ": glibc@system failed: " << out;
            expect(fatal(s::has_line(out, R"(Using sysroot glibc .* ← glibc@system  \[host · mcpp\.toml:)"))) << tc << ": no Using line for the host's base: " << out;
            expect(fatal(e2e::contains(out, "ran " + host_glibc))) << tc << ": the program did not run on the host's glibc " << host_glibc << ": " << out;
            const auto bins = s::glob(app.at("target"), "*/*/bin/app");
            expect(fatal(!bins.empty())) << tc << ": no artifact";
            const auto h = s::read_header(bins.front());
            // `readelf -d | grep subos`: the dynamic section's strings.
            std::string dynamic = h.runpath + " " + h.soname;
            for (const auto& n : h.needed) dynamic += " " + n;
            expect(fatal(s::lacks(dynamic, "subos"))) << tc << ": the SubOS farm reached a host-based artifact: " << dynamic;
            expect(fatal(h.interpreter.starts_with("/lib"))) << tc << ": PT_INTERP is not the host's loader: " << h.interpreter;
            const auto why = s::text(app.mcpp({"why", "sysroot"}, plain));
            if (tc.starts_with("gcc@"))
                expect(fatal(e2e::contains(why, "multiarch=" + tuple))) << "gcc: the multiarch directories were not measured: " << why;
            else
                expect(fatal(s::lacks(why, "multiarch="))) << "llvm: multiarch was supplied to a driver that knows it: " << why;
        }
    };

    "930 a toolchain phase states the base"_test[s::llvm][s::elf][s::unix_shell] = [] {
        // A toolchain phase stating `spec` and `sysroot` beside a managed compiler: the base is
        // reported as the program's, and the artifact loads it; `spec` with `prefix` (a key of
        // a toolchain named by path) is refused by name.
        const auto v = s::llvm_version();
        if (!fs::exists(s::payload_root("xim-x-llvm", v) / "bin/clang++")) skip(std::format("no llvm payload installed ({})", s::payload_root("xim-x-llvm", v).string()));
#if !defined(__linux__) || (!defined(__x86_64__) && !defined(_M_X64))
        skip("the fixture sysroot is x86_64 Linux");
#endif
        const auto work = s::case_dir();
        if (!make_glibc_sysroot(work / "sr")) skip("no glibc payload to build a sysroot from");
        s::project app(work / "app", {{"mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[toolchain]\ndefault   = {{ configure = \"build.mcpp\" }}\nbootstrap = \"llvm@{}\"\n", v)},
                                      {"src/main.cpp", "#include <gnu/libc-version.h>\nimport std;\nint main() { std::println(\"ran {}\", gnu_get_libc_version()); }\n"}});
        const auto phase = [&](std::string_view body) {
            app.write("build.mcpp", std::format("import std;\nimport mcpp;\nint main() {{\n    if (std::string_view(mcpp::phase()) == \"toolchain\") {{ {} return 0; }}\n    return 0;\n}}\n", body));
        };
        phase(std::format(R"(mcpp::toolchain("spec", "llvm@{}"); mcpp::toolchain("sysroot", "{}"); mcpp::toolchain("origin", "build.mcpp:4");)", v, (work / "sr").string()));
        auto r = app.mcpp({"run"}, plain);
        expect(fatal(e2e::succeeded(r))) << "spec + sysroot from the phase failed: " << s::text(r);
        expect(fatal(s::has_line(s::text(r), std::format(R"(Using sysroot glibc .* ← {}  \[program · build\.mcpp:4\])", literal((work / "sr").string())))))
            << "the base stated by the phase is not reported as the program's: " << s::text(r);
        const auto bins = s::glob(app.at("target"), "*/*/bin/app");
        expect(fatal(!bins.empty())) << "no artifact";
        expect(fatal(s::read_header(bins.front()).interpreter.starts_with((work / "sr").string() + "/"))) << "the artifact does not load the phase's base: " << s::read_header(bins.front()).interpreter;
        phase(std::format(R"(mcpp::toolchain("spec", "llvm@{}"); mcpp::toolchain("prefix", "x-");)", v));
        fs::remove_all(app.at("target"));
        r = app.mcpp({"build"}, plain);
        expect(fatal(e2e::contains(s::text(r), "stated `spec` with prefix"))) << "spec + prefix was not refused by name: " << s::text(r);
    };
}};

}  // namespace

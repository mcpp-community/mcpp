// The fast path: a build replayed from its record only for the profile, the toolchain request
// and the engine that recorded it, and never past an edit it cannot see -- a new file or a
// manifest of a path dependency, a provider's module extension, an edit after a confirmed one
// (formerly tests/e2e/173, 324, 645, 832, 878, 879).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

// The build was served by the fast path (under -v: no "declined", no "Resolving toolchain").
bool replayed(const e2e::outcome& r) {
    const auto t = s::text(r);
    return !t.contains("declined") && !t.contains("Resolving toolchain");
}

std::size_t resolutions(const e2e::outcome& r) { return s::count_lines(s::text(r), "Resolving toolchain"); }

void build_ok(const e2e::outcome& r, std::string_view what) {
    expect(fatal(e2e::succeeded(r))) << what << "\n" << s::text(r);
}

// The graphs under `target`, outside the build programs' tree.
std::vector<fs::path> graphs(const fs::path& target) {
    std::vector<fs::path> out;
    for (const auto& f : s::find_files(target, R"(build\.ninja)"))
        if (!f.generic_string().contains("/.build-mcpp/")) out.push_back(f);
    return out;
}

void sleep_s(double seconds) { std::this_thread::sleep_for(std::chrono::duration<double>(seconds)); }

// mcpp at another path: the same command as the program under test, started from `exe`.
e2e::outcome engine(const fs::path& exe, std::vector<std::string> args, const fs::path& cwd) {
    auto c = s::mcpp_command(std::move(args), cwd, {.env = {{"NO_COLOR", "1"}}});
    c.argv[0] = exe.string();
    return e2e::run(std::move(c));
}

// `sort -V` of two version strings: numeric runs compared as numbers.
bool version_less(std::string_view a, std::string_view b) {
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::isdigit(static_cast<unsigned char>(a[i])) && std::isdigit(static_cast<unsigned char>(b[j]))) {
            std::size_t ei = i, ej = j;
            while (ei < a.size() && std::isdigit(static_cast<unsigned char>(a[ei]))) ++ei;
            while (ej < b.size() && std::isdigit(static_cast<unsigned char>(b[ej]))) ++ej;
            const auto x = std::stoull(std::string(a.substr(i, ei - i))), y = std::stoull(std::string(b.substr(j, ej - j)));
            if (x != y) return x < y;
            i = ei, j = ej;
        } else {
            if (a[i] != b[j]) return a[i] < b[j];
            ++i, ++j;
        }
    }
    return a.size() - i < b.size() - j;
}

}  // namespace

E2E fastpath{"fastpath", [] {
    "173 build profile isolation"_test[tag{"requires:gcc"}][tag{"requires:fresh-sandbox"}] = [] {
        // The profile is an invalidation axis: --dev, --release and --profile dist have three build
        // directories (-O0, -O2, -O3, read from build.ninja), each announced as built; a bare
        // build after --release is dev, not the release graph's fast path; and switching back is
        // incremental.
        s::needs({"gcc", "fresh-sandbox"});
        const s::options home{.where = s::home::inherited};
        s::project app({{"src/main.cpp", "#include <cstdio>\nint main() { std::printf(\"hi\\n\"); return 0; }\n"},
                        {"mcpp.toml", "[package]\nname = \"app\"\nversion = \"0.1.0\"\nstandard = \"c++23\"\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"}});
        // The -O levels a build directory's build.ninja records, sorted and unique, joined.
        const auto opt_of = [](const fs::path& d) {
            std::set<std::string> found;
            const auto text = s::read_file(d / "build.ninja");
            const std::regex re{R"(-O[0-9s])"};
            for (auto it = std::sregex_iterator(text.begin(), text.end(), re); it != std::sregex_iterator(); ++it) found.insert(it->str());
            std::string out;
            for (const auto& f : found) out += f;
            return out;
        };
        const auto dir_for_opt = [&](std::string_view want) -> std::optional<fs::path> {
            for (const auto& d : s::glob(app.at("target"), "*/*"))
                if (fs::is_regular_file(d / "build.ninja") && opt_of(d) == want) return d;
            return std::nullopt;
        };
        const auto dirs = [&] {
            std::string out;
            for (const auto& d : s::glob(app.at("target"), "*/*"))
                if (fs::is_regular_file(d / "build.ninja")) out += "  " + d.string() + " -> " + opt_of(d) + "\n";
            return out;
        };
        const auto dev = app.mcpp({"build", "--dev"}, home);
        build_ok(dev, "--dev");
        const auto release = app.mcpp({"build", "--release"}, home);
        build_ok(release, "--release");
        const auto dist = app.mcpp({"build", "--profile", "dist"}, home);
        build_ok(dist, "--profile dist");
        expect(fatal(graphs(app.at("target")).size() == 3u)) << "expected one build dir per profile\n" << dirs();
        expect(fatal(dir_for_opt("-O0").has_value())) << "no -O0 (dev) build dir";
        expect(fatal(dir_for_opt("-O2").has_value())) << "no -O2 (release) build dir";
        expect(fatal(dir_for_opt("-O3").has_value())) << "no -O3 (dist) build dir";
        expect(fatal(e2e::contains(s::text(dev), "Finished dev"))) << "dev not announced\n" << s::text(dev);
        expect(fatal(e2e::contains(s::text(release), "Finished release"))) << "release not announced\n" << s::text(release);
        expect(fatal(e2e::contains(s::text(dist), "Finished dist"))) << "dist not announced\n" << s::text(dist);
        expect(fatal(e2e::contains(s::text(dev), "Finished dev [unoptimized"))) << "dev build described as optimized\n" << s::text(dev);

        app.remove("target");
        build_ok(app.mcpp({"build", "--release"}, home), "--release");
        const auto bare = app.mcpp({"build"}, home);
        build_ok(bare, "bare build");
        expect(fatal(e2e::contains(s::text(bare), "Finished dev"))) << "bare build after --release did not resolve to the dev profile\n" << s::text(bare);
        const auto devdir = dir_for_opt("-O0");
        expect(fatal(devdir.has_value())) << "bare build produced no -O0 build dir (fast path served release)\n" << dirs() << s::text(bare);
        expect(fatal(fs::is_regular_file(*devdir / "obj/main.o"))) << "dev build dir has no object";
        if (s::on_path("readelf")) {
            const auto r = e2e::run({.argv = {"readelf", "--debug-dump=info", (*devdir / "obj/main.o").string()}});
            expect(fatal(e2e::contains(r.out, "DW_AT_producer"))) << "dev object has no debug info (it is a release object)";
        }
        build_ok(app.mcpp({"build", "--release"}, home), "--release");
        const auto bare2 = app.mcpp({"build"}, home);
        build_ok(bare2, "bare build");
        expect(fatal(s::no_line(s::text(bare2), R"(src/main\.cpp)"))) << "returning to the dev profile recompiled from scratch\n" << s::text(bare2);
        expect(fatal(e2e::contains(s::text(bare2), "Finished dev"))) << "wrong profile\n" << s::text(bare2);
    };

    "324 fast path sees path deps"_test[tag{"requires:gcc"}] = [] {
        // A new file in a `path` dependency is compiled (the staleness sweep reaches the
        // dependency's tree), and an edit of the dependency's manifest plans again.
        s::needs({"gcc"});
        const s::options cdb{.env = {{"MCPP_COMPILE_COMMANDS", "compile_commands.json"}}};
        const auto tmp = s::case_dir();
        s::project dep(tmp / "dep", {{"mcpp.toml", "[package]\nname = \"dep\"\nversion = \"0.1.0\"\nstandard = \"c++23\"\n[targets.dep]\nkind = \"lib\"\n"},
                                     {"src/dep.cppm", "export module dep;\nexport int one(){ return 1; }\n"}});
        s::project app(tmp / "app", {{"mcpp.toml", "[package]\nname = \"app\"\nversion = \"0.1.0\"\nstandard = \"c++23\"\n[dependencies]\ndep = { path = \"../dep\" }\n"},
                                     {"src/main.cpp", "import dep;\nint main(){ return one()==1 ? 0 : 1; }\n"}});
        build_ok(app.mcpp({"build"}, cdb), "the first build");
        build_ok(app.mcpp({"build"}, cdb), "the second build");  // records the dependency's roots
        sleep_s(1);
        build_ok(app.mcpp({"build"}, cdb), "the warm build");
        const auto gcms = [&] { return s::find_files(app.dir(), R"(.*\.gcm)").size() + s::find_files(dep.dir(), R"(.*\.gcm)").size(); };
        const auto before = gcms();
        sleep_s(1);
        dep.write("src/extra.cppm", "export module dep.extra;\nexport int two(){ return 2; }\n");
        auto r = app.mcpp({"build"}, cdb);
        build_ok(r, "the build after the new file");
        expect(fatal(s::find_file(app.dir(), "dep.extra.gcm") || s::find_file(dep.dir(), "dep.extra.gcm"))) << "a new source file in a path dependency was never compiled, and the build reported success\n" << s::text(r);
        expect(fatal(gcms() > before)) << "the module-interface count did not grow (" << before << " -> " << gcms() << ")";
        app.mcpp({"build"}, cdb);
        sleep_s(1);
        dep.write("mcpp.toml", dep.read("mcpp.toml") + "\n[build]\ncxxflags = [\"-DDEP_TOUCHED=1\"]\n");
        r = app.mcpp({"build"}, cdb);
        build_ok(r, "the build after the manifest edit");
        expect(fatal(e2e::contains(app.read("compile_commands.json"), "DEP_TOUCHED"))) << "editing the path dependency's mcpp.toml did not re-plan\n" << s::text(r);
    };

    "645 the fast path compares the toolchain request"_test = [] {
        // T1 of the 2026-09-12 engine-gaps record: the fast path replays a build only for the
        // toolchain request that recorded it, A-B-A through `build` and `run`, and a changed
        // machine default resolves again.
        s::project p({{"src/main.cpp", "int main() { return 0; }\n"}, {"mcpp.toml", "[package]\nname    = \"fastpath\"\nversion = \"0.1.0\"\n"}});
        // The first `toolchain=` line of the record (`grep '^toolchain=' | head -1`).
        const auto request = [&] {
            if (!p.exists("target/.build_cache")) return std::string();
            for (const auto& l : s::lines_with(p.read("target/.build_cache"), "toolchain="))
                if (l.starts_with("toolchain=")) return l;
            return std::string();
        };
        auto a1 = p.mcpp({"build"});
        build_ok(a1, "the first build");
        std::smatch m;
        const auto t1 = s::text(a1);
        expect(fatal(std::regex_search(t1, m, std::regex{R"(Resolved ([^ ]*) )"}))) << "could not learn this platform's toolchain\n" << t1;
        const std::string own = m[1];
        const auto first_request = request();
        auto a2 = p.mcpp({"build"});
        build_ok(a2, "the second build");
        if (resolutions(a2) != 0) {
            // Two plain builds make the same request: equal recorded requests mean the fast path
            // declined for a reason this case does not measure (macOS, Windows: no ELF verdict).
            const auto second_request = request();
            expect(fatal(!first_request.empty() && first_request == second_request)) << "control: an unchanged second build resolved the toolchain, and the recorded requests differ ('" << first_request << "', then '" << second_request << "')\n" << s::text(a2);
            skip(std::format("an unchanged second build did not take the fast path on this host ({})", first_request));
        }
        auto r = p.mcpp({"build", "--toolchain", own});
        build_ok(r, "--toolchain " + own);
        expect(fatal(resolutions(r) != 0)) << "--toolchain " << own << " was answered by the fast path of a build that did not request it\n" << s::text(r);
        r = p.mcpp({"build"}, {.env = {{"MCPP_TOOLCHAIN", own}}});
        build_ok(r, "MCPP_TOOLCHAIN=" + own);
        expect(fatal(resolutions(r) == 0)) << "MCPP_TOOLCHAIN=" << own << " is the request --toolchain makes, and it was not answered by that build's fast path\n" << s::text(r);
        r = p.mcpp({"build"});
        build_ok(r, "the build after --toolchain");
        expect(fatal(resolutions(r) != 0)) << "a build without --toolchain was answered by the fast path of the --toolchain build\n" << s::text(r);
        r = p.mcpp({"build"});
        build_ok(r, "the settling build");
        expect(fatal(resolutions(r) == 0)) << "the default request did not return to the fast path\n" << s::text(r);

        build_ok(p.mcpp({"run"}), "mcpp run");
        r = p.mcpp({"run"});
        build_ok(r, "the second mcpp run");
        expect(fatal(resolutions(r) == 0)) << "control: an unchanged second run resolved the toolchain\n" << s::text(r);
        r = p.mcpp({"run"}, {.env = {{"MCPP_TOOLCHAIN", own}}});
        build_ok(r, "MCPP_TOOLCHAIN=" + own + " mcpp run");
        expect(fatal(resolutions(r) != 0)) << "MCPP_TOOLCHAIN=" << own << " mcpp run was answered by the fast path of a run that did not request it\n" << s::text(r);

        // The machine default, in a home of the case: a second installed version of the family.
        const auto family = own.substr(0, own.find('@'));
        const auto own_version = own.substr(own.find('@') + 1);
        const s::options home{.where = s::home::inherited};
        const auto family_dir = s::case_home(s::home::inherited) / "registry/data/xpkgs" / ("xim-x-" + family);
        std::vector<std::string> versions;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(family_dir, ec)) versions.push_back(e.path().filename().string());
        std::ranges::sort(versions, [](const auto& a, const auto& b) { return version_less(b, a); });
        std::string other;
        for (const auto& v : versions) {
            const auto d = family_dir / v;
            if (v != own_version && (fs::exists(d / "bin/g++") || fs::exists(d / "bin/clang++") || fs::exists(d / "bin/clang++.exe"))) {
                other = family + "@" + v;
                break;
            }
        }
        if (other.empty()) return;  // the machine-default leg is not measured: no second version installed
        p.remove("target");
        p.remove(".mcpp");
        build_ok(p.mcpp({"toolchain", "default", own}, home), "mcpp toolchain default " + own);
        build_ok(p.mcpp({"build"}, home), "the build with default " + own);
        r = p.mcpp({"build"}, home);
        build_ok(r, "the second build with default " + own);
        expect(fatal(resolutions(r) == 0)) << "control: an unchanged build in the isolated home resolved the toolchain\n" << s::text(r);
        build_ok(p.mcpp({"toolchain", "default", other}, home), "mcpp toolchain default " + other);
        r = p.mcpp({"build"}, home);
        build_ok(r, "the build with default " + other);
        expect(fatal(resolutions(r) != 0)) << "changing the machine default to " << other << " was answered by the fast path of the " << own << " build\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "Resolved " + other))) << "the build did not resolve the new default " << other << "\n" << s::text(r);
        std::optional<fs::path> artefact;
        for (const auto& f : s::find_files(p.at("target"), "fastpath.*"))
            if (f.generic_string().contains("/bin/") && (!artefact || fs::last_write_time(f) > fs::last_write_time(*artefact))) artefact = f;
        if (family == "gcc" && artefact && s::read_header(*artefact).kind == s::format::elf)
            expect(fatal(e2e::contains(s::read_file(*artefact), other.substr(other.find('@') + 1)))) << "the artefact the build left does not carry " << other << "'s version string\n" << s::text(r);
    };

    "832 the fast path resumes after an edit"_test[tag{"requires:unix-shell"}] = [] {
        // #734: after an edit and one build, the next build is replayed by the fast path, and the
        // replay still sees the next edit.
        s::needs({"unix-shell"});
        s::project p({{"mcpp.toml", "[package]\nname = \"resume832\"\nversion = \"0.1.0\"\n"}, {"src/main.cpp", "#include <cstdio>\nint main() { std::puts(\"one\"); return 0; }\n"}});
        build_ok(p.mcpp({"build"}), "the first build failed");
        sleep_s(1.1);
        p.write("src/main.cpp", "#include <cstdio>\nint main() { std::puts(\"two\"); return 0; }\n");
        build_ok(p.mcpp({"build"}), "the build after the edit failed");
        auto r = p.mcpp({"build", "-v"});
        build_ok(r, "R1: the build failed");
        expect(fatal(replayed(r))) << "R1: the build after a confirmed edit was not replayed by the fast path\n" << s::text(r);
        sleep_s(1.1);
        p.write("src/main.cpp", "#include <cstdio>\nint main() { std::puts(\"three\"); return 0; }\n");
        r = p.mcpp({"run"});
        build_ok(r, "R2: the run failed");
        expect(fatal(s::has_line(s::text(r), "^three$"))) << "R2: the next edit did not reach the program\n" << s::text(r);
    };

    "878 an upgraded engine does not replay the old graph"_test[tag{"requires:unix-shell"}] = [] {
        // #757: the record carries the engine that wrote it, and every fast path compares it; the
        // graph runs the engine through the build directory's link (design 2026-11-12 §17). The
        // same binary run from another path is what a reinstall looks like to the record.
        s::needs({"unix-shell"});
        const auto tmp = s::case_dir();
        const auto at = [&](std::string_view d) { return tmp / d / s::exe("mcpp"); };
        fs::create_directories(tmp / "engine-one");
        fs::create_directories(tmp / "engine-two");
        fs::create_directories(tmp / "engine-three");
        fs::copy_file(s::mcpp_path(), at("engine-one"));
        s::project app(tmp / "app", {{"mcpp.toml", "[package]\nname    = \"upgrade878\"\nversion = \"0.1.0\"\n[runtime]\ndeploy = [ { from = \"assets/data.txt\", to = \".\" } ]\n"},
                                     {"src/main.cpp", "#include <cstdio>\nint main() { std::puts(\"hello\"); return 0; }\n"},
                                     {"assets/data.txt", "one\n"}});
        const auto target = app.at("target");
        // Does a graph under target/ name the directory.
        const auto names = [&](std::string_view dir) {
            for (const auto& g : s::find_files(target, R"(build\.ninja)"))
                if (s::read_file(g).contains(dir)) return true;
            return false;
        };
        // The program every graph runs as `$mcpp` is `exe`: the same file or a copy of it.
        const auto runs = [&](const fs::path& exe) {
            const auto all = graphs(target);
            for (const auto& g : all) {
                std::smatch m;
                const auto text = s::read_file(g);
                if (!std::regex_search(text, m, std::regex{R"((^|\n)mcpp *= *([^\n]*))"})) return false;
                const fs::path named = std::string(m[2]);
                std::error_code ec;
                if (!fs::equivalent(named, exe, ec) && !(fs::exists(named) && s::read_file(named) == s::read_file(exe))) return false;
            }
            return !all.empty();
        };
        const auto deployed = [&] {
            for (const auto& f : s::find_files(target, R"(data\.txt)"))
                if (f.generic_string().contains("/bin/")) return f;
            return fs::path{};
        };
        const auto one = at("engine-one"), two = at("engine-two"), three = at("engine-three");
        auto r = engine(one, {"build"}, app.dir());
        build_ok(r, "the first build failed");
        auto b1 = engine(one, {"build", "-v"}, app.dir());
        build_ok(b1, "the warm build failed");
        expect(fatal(replayed(b1))) << "the warm build was not replayed, so nothing below is measured against the fast path\n" << s::text(b1);
        expect(fatal(runs(one))) << "the graph does not run the engine that wrote it, so the criterion has nothing to detect\n" << s::text(r);
        expect(fatal(s::has_line(app.read("target/.build_cache"), "^engine="))) << "the record does not name the engine that wrote it\n" << app.read("target/.build_cache");

        // A. The engine moves; nothing else changes.
        fs::rename(one, two);
        r = engine(two, {"build", "-v"}, app.dir());
        build_ok(r, "A: the build with the engine at its new path failed");
        expect(fatal(!names("engine-one"))) << "A: a graph still names the path the engine was moved from\n" << s::text(r);
        expect(fatal(runs(two))) << "A: the regenerated graph does not run the engine that built it\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "fast-path: build declined"))) << "A: the build did not say that the fast path declined\n" << s::text(r);

        // B. Replayed again, and an action that starts the engine runs.
        r = engine(two, {"build", "-v"}, app.dir());
        build_ok(r, "B: the build after the regeneration failed");
        expect(fatal(replayed(r))) << "B: the build after the regeneration was not replayed by the fast path\n" << s::text(r);
        fs::rename(two, three);
        sleep_s(1.1);
        app.write("assets/data.txt", "two\n");
        r = engine(three, {"build"}, app.dir());
        build_ok(r, "B: the build after the second move and an edit of a deployed file failed");
        expect(fatal(!deployed().empty() && s::read_file(deployed()) == "two\n")) << "B: the edit of the deployed file did not reach the output directory\n" << s::text(r);
        expect(fatal(!names("engine-two"))) << "B: a graph still names the path the engine was moved from";

        // C. A record written before the engine was recorded declines once.
        std::string aged;  // `grep -v '^engine='`
        const auto record = app.read("target/.build_cache");
        for (auto line : std::views::split(std::string_view(record).substr(0, record.ends_with('\n') ? record.size() - 1 : record.size()), '\n')) {
            std::string_view l(line.begin(), line.end());
            if (!l.starts_with("engine=")) aged += std::string(l) + "\n";
        }
        app.write("target/.build_cache", aged);
        r = engine(three, {"build", "-v"}, app.dir());
        build_ok(r, "C: the build with an aged record failed");
        expect(fatal(e2e::contains(s::text(r), "predates the engine identity"))) << "C: the decline did not name the missing engine identity\n" << s::text(r);
        expect(fatal(s::has_line(app.read("target/.build_cache"), "^engine="))) << "C: the build did not record the engine";
        r = engine(three, {"build", "-v"}, app.dir());
        build_ok(r, "C: the build after the aged record failed");
        expect(fatal(replayed(r))) << "C: the record written after the decline was not replayed\n" << s::text(r);

        // D. `mcpp run`'s fast path asks the same question.
        fs::rename(three, one);
        r = engine(one, {"run", "-v"}, app.dir());
        build_ok(r, "D: the run with the engine at another path failed");
        expect(fatal(s::has_line(s::text(r), "^hello$"))) << "D: the program did not run\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "fast-path: run declined"))) << "D: the run did not say that the fast path declined\n" << s::text(r);
        expect(fatal(!names("engine-three"))) << "D: a graph still names the path the engine was moved from";

        // E. A graph another engine rewrote, while the record still names this one.
        build_ok(engine(one, {"build"}, app.dir()), "E: the build before the rewrite failed");
        const auto gone = (tmp / "engine-gone" / s::exe("mcpp")).string();
        for (const auto& g : graphs(target)) {
            std::string out;
            for (auto line : std::views::split(s::read_file(g), '\n')) {
                std::string_view l(line.begin(), line.end());
                out += std::regex_match(std::string(l), std::regex{R"(mcpp *=.*)"}) ? "mcpp      = " + gone : std::string(l);
                out += "\n";
            }
            out.pop_back();
            std::ofstream(g, std::ios::binary) << out;
        }
        expect(fatal(names("engine-gone"))) << "E: the rewrite did not take";
        r = engine(one, {"build", "-v"}, app.dir());
        build_ok(r, "E: the build of a graph another engine wrote failed");
        expect(fatal(e2e::contains(s::text(r), "runs another engine"))) << "E: the decline did not name the graph's engine\n" << s::text(r);
        expect(fatal(!names("engine-gone"))) << "E: a graph still names the engine that rewrote it";
    };

    "879 a providers module extension reaches the fast path"_test[tag{"requires:unix-shell"}] = [] {
        // #756: the sweep classifies a path dependency's files with that package's own extension
        // table, so an edit to a provider's `.ixx` re-runs the consumer's build program, as a
        // project (build and run) and as a workspace member.
        s::needs({"unix-shell"});
        const auto tmp = s::case_dir();
        const auto value = [](const fs::path& rules, int v) {
            std::ofstream(rules / "rules.ixx", std::ios::binary) << std::format("export module repro.rules;\n\nexport const char* repro_definition() {{\n    return \"REPRO_VALUE={}\";\n}}\n", v);
        };
        const auto provider = [&](const fs::path& d) {
            s::project(d, {{"mcpp.toml", "[package]\nnamespace = \"repro\"\nname      = \"rules\"\nversion   = \"0.1.0\"\nstandard  = \"c++23\"\n[lib]\npath = \"rules.ixx\"\n[build]\nsources           = [\"rules.ixx\"]\nmodule_extensions = [\".ixx\"]\n"}});
            value(d, 1);
        };
        const auto consumer = [](const fs::path& d) {
            return s::project(d, {{"mcpp.toml", "[package]\nnamespace = \"repro\"\nname      = \"app\"\nversion   = \"0.1.0\"\nstandard  = \"c++23\"\n[build]\nsources = [\"main.cpp\"]\n[build-dependencies]\n\"repro.rules\" = { path = \"../rules\", host-module = true }\n[targets.app]\nkind = \"bin\"\nmain = \"main.cpp\"\n"},
                                  {"build.mcpp", "import mcpp;\nimport repro.rules;\nint main() {\n    mcpp::define(repro_definition());\n    return 0;\n}\n"},
                                  {"main.cpp", "#include <cstdio>\nint main() {\n    std::printf(\"REPRO_VALUE=%d\\n\", REPRO_VALUE);\n}\n"}});
        };
        const auto bin_of = [](const fs::path& root) {
            for (const auto& f : s::find_files(root, s::exe("app")))
                if (f.generic_string().contains("/bin/")) return f;
            return fs::path{};
        };
        const auto printed = [](const fs::path& program) {
            auto out = s::run(program).out;
            while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
            return out;
        };
        const s::options quiet{.env = {{"NO_COLOR", "1"}}};
        std::vector<std::string> logs;
        const auto mcpp = [&](const s::project& p, std::vector<std::string> args) {
            auto r = p.mcpp(std::move(args), quiet);
            logs.push_back(s::text(r));
            return r;
        };

        provider(tmp / "proj/rules");
        auto app = consumer(tmp / "proj/app");
        build_ok(mcpp(app, {"build"}), "project: the first build failed");
        auto r = app.mcpp({"run"}, quiet);
        expect(fatal(s::last_starting(r.out, "REPRO_VALUE=") == "REPRO_VALUE=1")) << "project: the program did not print the provider's value\n" << s::text(r);
        r = mcpp(app, {"build", "-v"});
        build_ok(r, "project: the warm build failed");
        expect(fatal(replayed(r))) << "project: the warm build was not replayed, so the next assertion measures nothing\n" << s::text(r);
        sleep_s(1.1);
        value(tmp / "proj/rules", 2);
        r = mcpp(app, {"build"});
        build_ok(r, "project: the build after the edit failed");
        auto bin = bin_of(app.at("target"));
        expect(fatal(!bin.empty())) << "project: the program was not found\n" << s::text(r);
        expect(fatal(printed(bin) == "REPRO_VALUE=2")) << "A: the edit of the provider's rules.ixx did not reach the program (build)\n" << s::text(r);
        r = mcpp(app, {"build", "-v"});
        build_ok(r, "project: the build after the confirmed edit failed");
        expect(fatal(replayed(r))) << "B: the build after a confirmed edit was not replayed by the fast path\n" << s::text(r);
        sleep_s(1.1);
        value(tmp / "proj/rules", 3);
        r = app.mcpp({"run"}, quiet);
        build_ok(r, "project: the run after the second edit failed");
        expect(fatal(s::has_line(s::text(r), "^REPRO_VALUE=3$"))) << "A: the edit of the provider's rules.ixx did not reach the program (run)\n" << s::text(r);

        // D. A record that lists the roots without their tables declines once.
        build_ok(mcpp(app, {"build"}), "project: the build before ageing the record failed");
        const auto record = app.read("target/.build_cache");
        expect(fatal(s::has_line(record, "^depSources=1$"))) << "D: the record does not carry the root with its tables\n" << record;
        std::string aged;
        std::vector<std::string> lines;
        for (auto line : std::views::split(record, '\n')) lines.emplace_back(line.begin(), line.end());
        if (!lines.empty() && lines.back().empty()) lines.pop_back();
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (!lines[i].starts_with("depSources=")) { aged += lines[i] + "\n"; continue; }
            auto n = std::stoi(lines[i].substr(11));
            aged += std::format("depSourceRoots={}\n", n);
            while (n-- > 0 && i + 1 < lines.size()) {
                ++i;
                aged += lines[i].substr(0, lines[i].find('\t')) + "\n";
            }
        }
        app.write("target/.build_cache", aged);
        expect(fatal(s::has_line(app.read("target/.build_cache"), "^depSourceRoots=1$"))) << "D: the record was not aged";
        r = mcpp(app, {"build", "-v"});
        build_ok(r, "D: the build with an aged record failed");
        expect(fatal(e2e::contains(s::text(r), "predates the list of path-dependency roots"))) << "D: the decline did not name the missing tables\n" << s::text(r);
        r = mcpp(app, {"build", "-v"});
        build_ok(r, "D: the build after the aged record failed");
        expect(fatal(replayed(r))) << "D: the record written after the decline was not replayed\n" << s::text(r);

        // A workspace member: the workspace fast path.
        s::project ws(tmp / "ws", {{"mcpp.toml", "[workspace]\nmembers = [\"app\"]\n"}});
        provider(tmp / "ws/rules");
        consumer(tmp / "ws/app");
        build_ok(mcpp(ws, {"build", "-p", "app"}), "workspace: the first build failed");
        r = mcpp(ws, {"build", "-p", "app", "-v"});
        build_ok(r, "workspace: the warm build failed");
        expect(fatal(replayed(r))) << "workspace: the warm build was not replayed, so the next assertion measures nothing\n" << s::text(r);
        bin = bin_of(ws.at("target"));
        expect(fatal(!bin.empty() && printed(bin) == "REPRO_VALUE=1")) << "workspace: the program did not print the provider's value";
        sleep_s(1.1);
        value(tmp / "ws/rules", 2);
        r = mcpp(ws, {"build", "-p", "app"});
        build_ok(r, "workspace: the build after the edit failed");
        expect(fatal(printed(bin) == "REPRO_VALUE=2")) << "A: the edit of the provider's rules.ixx did not reach the program (workspace)\n" << s::text(r);
        r = mcpp(ws, {"build", "-p", "app", "-v"});
        build_ok(r, "workspace: the build after the confirmed edit failed");
        expect(fatal(replayed(r))) << "B: the workspace build after a confirmed edit was not replayed by the fast path\n" << s::text(r);

        // C. The consumer declares nothing about `.ixx`, and no build warned of a dead entry.
        for (const auto& l : logs) expect(fatal(s::lacks(l, "dead entry"))) << "C: a build printed the dead-entry warning";
        expect(fatal(s::lacks(app.read("mcpp.toml"), "module_extensions"))) << "C: the consumer declares module_extensions, which the criterion forbids";
    };
}};

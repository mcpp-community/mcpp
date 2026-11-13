// BMIs and their cache: path dependencies stay out of the build cache, a custom index's nested
// objects are cached and staged, purview and header edits rebuild, staging neither cascades nor
// rewrites an equal destination, the split schedule end to end, a staged BMI that waits for a
// module compiled here, and the std module compiled once by two builds that find it missing
// (formerly tests/e2e/19, 49, 118, 170, 171, 234, 849, 1039). 1039 removes the shared home's std
// entry that 170 and 171 stage from; in one program its `machine` resource keeps them apart.
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

std::optional<fs::path> graph_of(const fs::path& root) {
    auto all = s::find_files(root / "target", R"(build\.ninja)");
    std::ranges::sort(all);
    for (const auto& f : all)
        if (!f.generic_string().contains("/.build-mcpp/")) return f;
    return std::nullopt;
}

// Any path below `root` whose text contains `part` (`find <root> -path "*<part>*"`).
bool any_path(const fs::path& root, std::string_view part) {
    std::error_code ec;
    if (!fs::exists(root, ec)) return false;
    for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->path().string().contains(part)) return true;
    return false;
}

// `sed 's/<from>/<to>/'` over a file: the first occurrence on each line.
void subst(const fs::path& file, std::string_view from, std::string_view to) {
    std::istringstream in(s::read_file(file));
    std::string out;
    for (std::string l; std::getline(in, l);) {
        if (auto at = l.find(from); at != std::string::npos) l.replace(at, from.size(), to);
        out += l + "\n";
    }
    std::ofstream(file, std::ios::binary) << out;
}

// A ninja path as the file system names it (`$:` and `$$` unescaped).
std::string unescape_ninja(std::string_view p) {
    std::string out;
    for (std::size_t i = 0; i < p.size(); ++i) {
        if (p[i] == '$' && i + 1 < p.size() && (p[i + 1] == ':' || p[i + 1] == '$')) ++i;
        out.push_back(p[i]);
    }
    return out;
}

std::vector<std::string> words(std::string_view line) {
    std::vector<std::string> out;
    std::istringstream in{std::string(line)};
    for (std::string w; in >> w;) out.push_back(w);
    return out;
}

// The first `build <dst> : stage_file ... <src>` edge of a graph: (dst, src), unescaped.
std::pair<std::string, std::string> first_stage(const std::string& graph) {
    const auto lines = s::lines_with(graph, " : stage_file ");
    for (const auto& l : lines)
        if (std::regex_search(l, std::regex{R"(^build [^ ]+ : stage_file )"})) {
            const auto w = words(l);
            return {unescape_ninja(w[1]), unescape_ninja(w.back())};
        }
    return {};
}

std::string slashes(std::string p) {
    std::ranges::replace(p, '\\', '/');
    return p;
}

void touch_now(const fs::path& f) { fs::last_write_time(f, fs::file_time_type::clock::now()); }

constexpr std::string_view lua_header = R"(    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux = {
            ["1.0.0"] = {
                url = "https://example.invalid/{0}-1.0.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
    },
)";

std::string lua_xpm(std::string_view name) {
    std::string out(lua_header);
    out.replace(out.find("{0}"), 3, name);
    return out;
}

E2E bmi{"bmi", [] {
    "19 bmi cache reuse"_test = [] {
        // Path dependencies, direct and transitive, never enter the build cache nor `cache
        // list`, and an edit to a transitive one reaches the binary.
        const s::options home{.where = s::home::inherited};
        const auto mcpp_home = s::case_home(s::home::inherited);
        const auto tmp = s::case_dir();
        const auto run = [&](const fs::path& dir, std::vector<std::string> args) { return s::mcpp(std::move(args), dir, home); };
        expect(fatal(e2e::succeeded(run(tmp, {"new", "mylibA"}))));
        s::project a(tmp / "mylibA", {{"src/greet.cppm", "export module mylibA.greet;\nimport std;\nexport auto greet() -> void { std::println(\"hi\"); }\n"},
                                      {"mcpp.toml", "[package]\nname        = \"mylibA\"\nversion     = \"0.1.0\"\n[language]\nstandard   = \"c++23\"\nmodules    = true\nimport_std = true\n[modules]\nsources = [\"src/**/*.cppm\"]\n[targets.mylibA]\nkind = \"lib\"\n"}});
        a.remove("src/main.cpp");
        expect(fatal(e2e::succeeded(run(tmp, {"new", "myapp"}))));
        s::project app(tmp / "myapp", {{"src/main.cpp", "import std;\nimport mylibA.greet;\nint main() { greet(); return 0; }\n"},
                                       {"mcpp.toml", "[package]\nname        = \"myapp\"\nversion     = \"0.1.0\"\n[language]\nstandard   = \"c++23\"\nmodules    = true\nimport_std = true\n[modules]\nsources = [\"src/**/*.cppm\", \"src/**/*.cpp\"]\n[targets.myapp]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n[dependencies.mylibA]\npath = \"../mylibA\"\n"}});
        auto r = app.mcpp({"build", "-v"}, home);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(fs::is_directory(mcpp_home / "build-cache/v1"))) << "missing " << (mcpp_home / "build-cache/v1").string();
        expect(fatal(!any_path(mcpp_home / "build-cache/v1/pkg", "mylibA"))) << "path dep mylibA was populated into the build cache (must be skipped)";
        expect(fatal(s::no_line(s::text(r), R"(Cached mylibA|Compiling mylibA .*cached [0-9]+ unit)"))) << "path dep wrongly labeled Cached\n" << s::text(r);

        expect(fatal(e2e::succeeded(run(tmp, {"new", "mylibB"}))));
        s::project b(tmp / "mylibB", {{"src/inner.cppm", "export module mylibB.inner;\nexport auto inner() -> int { return 7; }\n"},
                                      {"mcpp.toml", "[package]\nname        = \"mylibB\"\nversion     = \"0.1.0\"\n[language]\nstandard   = \"c++23\"\nmodules    = true\nimport_std = true\n[modules]\nsources = [\"src/**/*.cppm\"]\n[targets.mylibB]\nkind = \"lib\"\n"}});
        b.remove("src/main.cpp");
        a.write("src/greet.cppm", "export module mylibA.greet;\nimport std;\nimport mylibB.inner;\nexport auto greet() -> void { std::println(\"hi {}\", inner()); }\n");
        a.write("mcpp.toml", a.read("mcpp.toml") + "[dependencies.mylibB]\npath = \"../mylibB\"\n");
        app.remove("target");
        r = app.mcpp({"build"}, home);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        for (std::string_view name : {"mylibA", "mylibB"})
            expect(fatal(!any_path(mcpp_home / "build-cache/v1/pkg", name))) << "local package " << name << " entered the build cache";
        const auto list = s::text(app.mcpp({"cache", "list"}, home));
        for (std::string_view name : {"mylibA", "mylibB"})
            expect(fatal(s::lacks(list, name))) << "cache list shows local package " << name << "\n" << list;
        b.write("src/inner.cppm", "export module mylibB.inner;\nexport auto inner() -> int { return 99; }\n");
        r = app.mcpp({"run"}, home);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "hi 99"))) << "edit to a transitive path dep did not reach the binary\n" << s::text(r);
    };

    "49 bmi cache nested custom index"_test[s::gcc][s::fresh_sandbox] = [] {
        // A local-path index package with two same-named units in nested directories is cached
        // with its nested object paths, and a cold rebuild stages it instead of compiling it.
        const s::options home{.where = s::home::inherited};
        const auto mcpp_home = s::case_home(s::home::inherited);
        const auto tmp = s::case_dir();
        const auto index = tmp / "local-index";
        s::project idx(index, {});
        idx.write("pkgs/c/collision-lib.lua", "package = {\n    spec = \"1\",\n    name = \"collision-lib\",\n    description = \"Collision object path package\",\n" + lua_xpm("collision-lib") +
                                                  "    mcpp = {\n        language = \"c++23\",\n        import_std = true,\n        sources = { \"src/**/*.cppm\" },\n        targets = { [\"collision-lib\"] = { kind = \"lib\" } },\n        deps = {},\n    },\n}\n");
        const std::string payload = ".mcpp/.xlings/data/xpkgs/local-dev.collision-lib/1.0.0";
        s::project p(tmp / "project/app", {{"src/main.cpp", "import std;\nimport collision.a;\nimport collision.b;\nint main() {\n    std::println(\"{} {}\", collision_a(), collision_b());\n    return 0;\n}\n"}});
        p.write(payload + "/src/a/foo.cppm", "export module collision.a;\nexport int collision_a() { return 1; }\n");
        p.write(payload + "/src/b/foo.cppm", "export module collision.b;\nexport int collision_b() { return 2; }\n");
        p.write("mcpp.toml", std::format("[package]\nname = \"app\"\nversion = \"0.1.0\"\n[indices]\nlocal-dev = {{ path = \"{}\" }}\n[dependencies]\n\"local-dev.collision-lib\" = \"1.0.0\"\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", index.generic_string()));
        auto r = p.mcpp({"build"}, home);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(s::lacks(s::text(r), "Fetching custom index repos"))) << "local path index should not trigger xlings update noise\n" << s::text(r);
        expect(fatal(s::lacks(s::text(r), "bmi cache populate failed"))) << "BMI cache populate warning should not appear\n" << s::text(r);
        auto entries = s::find_files(mcpp_home / "build-cache/v1/pkg/local-dev", R"(entry\.json)");
        std::ranges::sort(entries);
        expect(fatal(!entries.empty())) << "missing custom-index cache entry\n" << s::text(r);
        const auto d = s::json_file(entries.front());
        std::vector<std::string> objs;
        if (d.contains("obj"))
            for (const auto& o : d["obj"]) objs.push_back(o.get<std::string>());
        expect(fatal(std::ranges::any_of(objs, [](const std::string& o) { return o.contains('/') && o.ends_with("foo.m.o"); })))
            << "entry did not preserve a nested object path:" << objs;
        for (const auto& o : objs)
            expect(fatal(fs::exists(entries.front().parent_path() / "obj" / o))) << "listed object missing on disk: " << o;
        p.remove("target");
        r = p.mcpp({"build", "-v"}, home);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(s::has_line(s::text(r), R"(Cached local-dev\.collision-lib v1\.0\.0 \((index [^,)]+, )?[0-9]+ units?\))"))) << "second cold build did not reuse the build cache\n" << s::text(r);
        const auto g = graph_of(p.dir());
        expect(fatal(g.has_value())) << "no build.ninja";
        const auto text = s::read_file(*g);
        expect(fatal(s::no_line(text, R"(: (cxx_module|cxx_object) .*collision-lib)"))) << "cached dependency still has compile edges";
        expect(fatal(s::has_line(text, R"(^build .* : stage_file .*collision-lib)"))) << "cached dependency has no stage_file edge";
    };

    "118 purview include rebuild"_test = [] {
        // Editing a file included in a module's purview, or a header a .cpp includes, rebuilds;
        // the build states no missing depfile; under GCC and, with its payload, Clang.
        const auto leg = [](std::string_view name, std::string_view toolchain_section) {
            auto p = s::project::scaffold(name);
            p.write("src/vals.inc", "export inline int answer() { return 41; }\n");
            p.write("src/m.cppm", "export module m;\n#include \"vals.inc\"\n");
            p.write("src/main.cpp", "import std;\nimport m;\nint main() {\n    std::println(\"{}\", answer());\n    return 0;\n}\n");
            p.write("mcpp.toml", std::format("[package]\nname    = \"{}\"\nversion = \"0.1.0\"\n{}", name, toolchain_section));
            const auto last = [&] {
                auto r = p.mcpp({"run"});
                auto out = r.out;
                std::erase(out, '\r');
                return std::pair{s::last_line(out), s::text(r)};
            };
            auto [out, log] = last();
            expect(fatal(out == "41")) << name << ": unexpected initial output: " << out << "\n" << log;
            expect(fatal(s::lacks(log, "emits no GNU depfile"))) << name << ": the build still reports that it emits no GNU depfile\n" << log;
            subst(p.at("src/vals.inc"), "41", "42");
            std::tie(out, log) = last();
            expect(fatal(out == "42")) << name << ": editing a purview #include'd file did not trigger a rebuild; got: " << out;
            p.write("src/helper.h", "inline int helper_val() { return 100; }\n");
            p.write("src/helper_user.cpp", "#include \"helper.h\"\nint use_helper() { return helper_val(); }\n");
            p.write("src/main.cpp", "import std;\nimport m;\nextern int use_helper();\nint main() {\n    std::println(\"{} {}\", answer(), use_helper());\n    return 0;\n}\n");
            std::tie(out, log) = last();
            expect(fatal(out == "42 100")) << name << ": unexpected output before header edit: " << out;
            subst(p.at("src/helper.h"), "100", "200");
            std::tie(out, log) = last();
            expect(fatal(out == "42 200")) << name << ": editing a plain .h included by a .cpp did not trigger a rebuild; got: " << out;
        };
        leg("purviewinc", "");
        const auto llvm = s::llvm_version();
        if (fs::is_directory(s::payload_root("xim-x-llvm", llvm)))
            leg("purviewinc_clang", std::format("[toolchain]\ndefault = \"llvm@{}\"\n", llvm));
        else
            std::println("  clang leg skipped (no llvm payload installed)");
    };

    "170 bmi staging no cascade"_test[resource{"machine"}] = [] {
        // The std BMI is staged from the home's cache; a re-staged, unchanged BMI does not
        // recompile its importer, and a settled staging edge stays clean.
        s::project p({{"src/main.cpp", "import std;\nint main() { std::println(\"staged\"); return 0; }\n"},
                      {"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n"}});
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "initial build\n" << s::text(r);
        const auto g = graph_of(p.dir());
        expect(fatal(g.has_value())) << "no build.ninja produced";
        const auto [dst, src] = first_stage(s::read_file(*g));
        expect(fatal(!dst.empty())) << "no stage_file edge for the std BMI (did import std resolve?)";
        const auto cache_dir = s::mcpp({"cache", "dir"}, p.dir()).out;
        const auto cache_root = slashes(cache_dir.substr(0, cache_dir.find('\n')));
        expect(fatal(!cache_root.empty())) << "mcpp cache dir printed nothing";
        expect(fatal(slashes(src).starts_with(cache_root + "/std/"))) << "std BMI cache is '" << src << "', expected under '" << cache_root << "/std'";
        for (const auto& leftover : {s::case_dir() / ".mcpp-bmi", p.at(".mcpp-bmi")})
            expect(fatal(!fs::is_directory(leftover))) << "legacy cache dir created at " << leftover.string();
        touch_now(src);
        r = p.mcpp({"build", "-v"});
        expect(fatal(e2e::succeeded(r))) << "rebuild after re-stage\n" << s::text(r);
        expect(fatal(s::has_line(s::text(r), "stage .*--output"))) << "the staging edge did not re-run at all\n" << s::text(r);
        expect(fatal(s::no_line(s::text(r), R"((-c|/c) .*main\.cpp)"))) << "re-staging an unchanged std BMI recompiled main.cpp (cascade)\n" << s::text(r);
        expect(fatal(s::read_file(src) == s::read_file(g->parent_path() / dst))) << "staged BMI differs from the cache after staging";
        r = p.mcpp({"build", "-v"});
        expect(fatal(e2e::succeeded(r))) << "third build\n" << s::text(r);
        expect(fatal(s::no_line(s::text(r), "stage .*--output"))) << "staging edge stayed dirty after a settled re-stage\n" << s::text(r);
        expect(fatal(s::no_line(s::text(r), R"((-c|/c) .*main\.cpp)"))) << "settled staging edge recompiled main.cpp on the third build\n" << s::text(r);
    };

    "171 bmi staging locked dest"_test = [] {
        // Staging an equivalent BMI onto a destination that cannot be written (read-only; on
        // Windows, mapped by another process) succeeds without rewriting it; staging onto an
        // unusable destination fails with a hint naming clangd and clobbers nothing.
        s::project p({{"src/main.cpp", "import std;\nint main() { std::println(\"locked\"); return 0; }\n"},
                      {"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n"}});
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "initial build\n" << s::text(r);
        const auto g = graph_of(p.dir());
        expect(fatal(g.has_value())) << "no build.ninja";
        const auto [dst_rel, src] = first_stage(s::read_file(*g));
        expect(fatal(!dst_rel.empty())) << "no stage_file edge for the std BMI";
        const auto dst = g->parent_path() / dst_rel;
        expect(fatal(fs::is_regular_file(dst))) << "staged BMI missing at " << dst.string();
        const auto before = s::file_identity(dst);
        touch_now(src);
        std::optional<e2e::session> holder;
        if (s::on_windows()) {
            const auto ready = s::case_dir() / "mapped.flag";
            holder = e2e::session::start({.argv = {"powershell", "-NoProfile", "-Command",
                                                   std::format("$mm = [System.IO.MemoryMappedFiles.MemoryMappedFile]::CreateFromFile('{}', [System.IO.FileMode]::Open); Set-Content -Path '{}' -Value 'mapped'; Start-Sleep -Seconds 120",
                                                               dst.string(), ready.string())},
                                          .timeout = std::chrono::seconds{180}});
            for (int i = 0; i < 40 && !fs::exists(ready); ++i) std::this_thread::sleep_for(std::chrono::seconds{1});
            expect(fatal(fs::exists(ready))) << "could not map the staged BMI — the test would not have proven anything";
        } else {
            fs::permissions(dst, fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read);
        }
        r = p.mcpp({"build", "-v"});
        holder.reset();
        std::error_code ec;
        fs::permissions(dst, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read, ec);
        expect(fatal(e2e::succeeded(r))) << "staging an already-equivalent BMI failed while the destination was held\n" << s::text(r);
        expect(fatal(s::has_line(s::text(r), "stage .*--output"))) << "the staging edge did not run\n" << s::text(r);
        const auto after = s::file_identity(dst);
        expect(fatal(before == after)) << "an equivalent destination was rewritten (was '" << before << "', now '" << after << "')";
        expect(fatal(s::read_file(src) == s::read_file(dst))) << "held destination no longer matches the cache";

        // Outside the repository, as the script's $TMP was: no manifest above it.
        const auto tmp = s::case_home() / "stage";
        fs::create_directories(tmp);
        std::ofstream(tmp / "src.bin") << "payload\n";
        fs::create_directories(tmp / "occupied/child");
        std::ofstream(tmp / "occupied/child/keep.txt") << "keep\n";
        r = s::mcpp({"stage", "--output", "occupied", "src.bin"}, tmp);
        expect(fatal(failed(r))) << "staging onto an unusable destination reported success\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "help:"))) << "staging failure carried no hint\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "clangd"))) << "hint does not name the usual holder (clangd)\n" << s::text(r);
        expect(fatal(fs::is_regular_file(tmp / "occupied/child/keep.txt"))) << "failed staging clobbered the destination";
    };

    "234 bmi schedule on"_test[resource{"machine"}] = [] {
        // bmi_schedule = "on": the schedule is reported and the program runs; the graph declares
        // its shape; under detach-codegen an incremental build works, a full set of stale tokens
        // neither hangs a build nor survives it, a BMI's removal is not its publication, the
        // compilers share the cap, cached replays keep graph, objects and BMIs, missing cached
        // objects recover, and the final status is reread after the controller exits.
        const auto llvm = s::llvm_version();
        s::project p({{"mcpp.toml", std::format(R"([package]
name    = "schedon"
version = "0.1.0"
[toolchain]
default = "gcc@16.1.0"
macos   = "llvm@{0}"
windows = "llvm@{0}"
[build]
default-profile = "release"
bmi_schedule = "on"
)", llvm)},
                      {"src/core.cppm", "export module schedon.core;\nimport std;\nexport int core_value() { return 7; }\n"},
                      {"src/mid.cppm", "export module schedon.mid;\nimport std;\nimport schedon.core;\nexport int mid_value() { return core_value() + 1; }\n"},
                      {"src/main.cpp", "import std;\nimport schedon.mid;\nint main() { std::println(\"{}\", mid_value()); return mid_value() == 8 ? 0 : 1; }\n"}});
        auto r = p.mcpp({"build", "--release", "--verbose"});
        expect(fatal(e2e::succeeded(r))) << "build with bmi_schedule=on failed\n" << s::text(r);
        const auto sched_lines = s::lines_with(s::text(r), "schedule:");
        expect(fatal(!sched_lines.empty())) << "--verbose printed no schedule line — the decision is not being reported";
        const auto sched_line = sched_lines.front();
        std::string strategy;
        if (sched_line.contains("detach-codegen")) strategy = "detach";
        else if (sched_line.contains("two-phase")) strategy = "twophase";
        else mcpplibs::mtest::skip(std::format("the schedule resolved to '{}' on this host; this test covers the split strategies", sched_line.substr(sched_line.find("schedule: ") + 10)));
        const auto newest_bin = [&] {
            std::optional<fs::path> best;
            for (const auto& b : s::glob(p.at("target"), "*/*/bin/" + s::exe("schedon")))
                if (!best || fs::last_write_time(b) > fs::last_write_time(*best)) best = b;
            return best;
        };
        auto bin = newest_bin();
        expect(fatal(bin.has_value())) << "no binary produced";
        const auto value = [](const fs::path& b) {
            auto out = s::run(b).out;
            std::erase(out, '\r');
            while (!out.empty() && out.back() == '\n') out.pop_back();
            return out;
        };
        expect(fatal(value(*bin) == "8")) << "binary printed '" << value(*bin) << "', expected 8";
        const auto build_dir = bin->parent_path().parent_path();
        expect(fatal(s::has_line(s::read_file(build_dir / "build.ninja"), "^# mcpp:graph="))) << "build.ninja carries no '# mcpp:graph=' line";
        if (strategy != "detach") return;  // no semaphore under the two-phase strategy

        touch_now(p.at("src/core.cppm"));
        r = p.mcpp({"build", "--release"});
        expect(fatal(e2e::succeeded(r))) << "incremental build after touching an interface failed\n" << s::text(r);

        const auto sem = build_dir / ".mcpp-sched";
        for (int i = 0; i < 64; ++i) fs::create_directories(sem / std::to_string(i));
        const auto count = [&] {
            std::error_code ec;
            return fs::is_directory(sem, ec) ? std::distance(fs::directory_iterator(sem), fs::directory_iterator()) : 0;
        };
        const auto planted = count();
        expect(fatal(planted > 0)) << "could not plant tokens";
        std::ofstream(p.at("src/core.cppm"), std::ios::app) << "\nexport int core_extra() { return 1; }\n";
        r = p.mcpp({"build", "--release"}, {.timeout = std::chrono::seconds{600}});
        expect(fatal(!r.timed_out)) << "the build HUNG with a full set of stale tokens in " << sem.string() << "\n" << s::text(r);
        expect(fatal(e2e::succeeded(r))) << "build after planting stale tokens failed\n" << s::text(r);
        const auto left = count();
        expect(fatal(left < planted)) << left << " of " << planted << " stale tokens survived — they were not reclaimed";
        bin = newest_bin();
        expect(fatal(value(*bin) == "8")) << "binary printed '" << value(*bin) << "' after the stale-token build";

        // 5. A BMI's removal is not its publication.
        const auto pub = p.at("publication");
        fs::create_directories(pub);
        std::ofstream(pub / "published.gcm", std::ios::binary) << "old BMI";
        std::ofstream(pub / "publish.sh", std::ios::binary) << "rm published.gcm\nsleep 0.2\nprintf 'new BMI' > next.gcm\nmv next.gcm published.gcm\nsleep 0.05\nprintf object > unit.o\n";
        const auto bash = s::env("BASH").value_or(s::on_path("bash").value_or("bash").string());
        std::ofstream(pub / "command.txt", std::ios::binary) << std::format("\"{}\" publish.sh\n", bash);
        (void)s::mcpp({"bmi-compile", "--bmi", "published.gcm", "--slot", "unit", "--self", s::mcpp_path(), "--command-file", "command.txt"}, pub);
        const bool published = fs::is_regular_file(pub / "published.gcm");
        (void)s::mcpp({"bmi-await", "--slot", "unit", "--object", "unit.o"}, pub);
        expect(fatal(published)) << "phase 1 released an importer while the BMI was absent";
        expect(fatal(s::read_file(pub / "published.gcm") == "new BMI")) << "phase 1 did not wait for the new BMI";
        std::ofstream(pub / "command.txt", std::ios::binary) << std::format("\"{}\" -c \"rm published.gcm\"\n", bash);
        r = s::mcpp({"bmi-compile", "--bmi", "published.gcm", "--slot", "unit", "--self", s::mcpp_path(), "--command-file", "command.txt"}, pub);
        expect(fatal(failed(r))) << "a successful compiler without a BMI was accepted";
        expect(fatal(s::has_line(s::text(r), "compiler reported success but.*is missing"))) << "the missing BMI was not diagnosed\n" << s::text(r);
        expect(fatal(s::read_file(pub / "published.gcm") == "new BMI")) << "the previous BMI was not restored after missing publication";

        // 6, 8, 9: the checks of tests/e2e/helpers, as the script ran them.
        const auto helper = [&](std::string_view name, std::vector<std::string> args) {
            std::vector<std::string> argv{"python3", (s::repository() / "tests/e2e/helpers" / name).string(), s::mcpp_path()};
            argv.insert(argv.end(), args.begin(), args.end());
            auto h = e2e::run({.argv = argv, .cwd = p.dir(), .timeout = std::chrono::minutes{10}});
            expect(fatal(e2e::succeeded(h))) << name << "\n" << s::text(h);
        };
        helper("check_shared_compiler_cap.py", {(s::case_dir() / "cap-load").string()});

        // 7. Cached replays, plain and quiet, keep the graph, the objects and the BMIs.
        const auto graph = build_dir / "build.ninja";
        const auto graph_before = std::pair{fs::last_write_time(graph), s::read_file(graph)};
        const auto identity = [&] {
            std::map<std::string, std::pair<fs::file_time_type, std::string>> out;
            for (const auto& [dir, pattern] : {std::pair{"obj", R"(.*\.o)"}, std::pair{"gcm.cache", R"(.*\.gcm)"}})
                for (const auto& f : s::find_files(build_dir / dir, pattern))
                    out[fs::relative(f, build_dir).generic_string()] = {fs::last_write_time(f), s::sha256(s::read_file(f))};
            return out;
        };
        const auto before = identity();
        expect(fatal(!before.empty())) << "no compiler outputs to verify";
        for (const std::vector<std::string> flags : {std::vector<std::string>{}, std::vector<std::string>{"--quiet"}}) {
            std::vector<std::string> args{"build"};
            args.insert(args.end(), flags.begin(), flags.end());
            r = p.mcpp(args, {.timeout = std::chrono::seconds{60}});
            expect(fatal(e2e::succeeded(r))) << s::text(r);
            expect(fatal(identity() == before)) << "cached replay recompiled unchanged inputs";
            expect(fatal(std::pair{fs::last_write_time(graph), s::read_file(graph)} == graph_before)) << "cached replay rewrote its graph";
        }
        expect(fatal(s::lacks(graph_before.second, "MCPP_COMPILER_RUN"))) << "the graph persists a compiler-run generation";
        expect(fatal(s::lacks(graph_before.second, ".mcpp/compiler-runs"))) << "the graph persists a compiler-run directory";

        helper("check_cached_object_recovery.py", {p.dir().string(), build_dir.string()});
        helper("check_completion_observation.py", {(s::case_dir() / "completion-observation").string()});
    };

    "849 a staged bmi waits for a module compiled here"_test = [] {
        // A cached package whose module imports a module its build program generates is staged;
        // the stage edge waits for the generated module's BMI, the aggregate holds no such
        // stage, and the consumer compiles after the generated module.
        const s::options home{.where = s::home::inherited};
        const auto mcpp_home = s::case_home(s::home::inherited);
        const auto tmp = s::case_dir();
        s::project idx(tmp / "local-index", {});
        idx.write("pkgs/g/gen-dep.lua", "package = {\n    spec = \"1\",\n    name = \"gen-dep\",\n    description = \"A package whose module imports a module its build program generates\",\n" + lua_xpm("gen-dep") + "}\n");
        const std::string payload = ".mcpp/.xlings/data/xpkgs/local-dev.gen-dep/1.0.0";
        s::project app(tmp / "app", {{"src/main.cpp", "#include <cstdio>\nimport gen.dep;\nint main() { std::printf(\"%d\\n\", dep_value()); }\n"}});
        app.write(payload + "/mcpp.toml", "[package]\nname    = \"gen-dep\"\nversion = \"1.0.0\"\n[targets.gen-dep]\nkind = \"lib\"\n");
        app.write(payload + "/build.mcpp", R"cpp(#include <cstdio>
#include <string>
import mcpp;
int main() {
    std::string p = std::string(mcpp::out_dir()) + "/gen-table.cppm";
    std::FILE* f = std::fopen(p.c_str(), "w");
    std::fputs("export module gen.dep.table;\nexport int table_value() { return 42; }\n", f);
    std::fclose(f);
    mcpp::generated(p.c_str());
    return 0;
}
)cpp");
        app.write(payload + "/src/gen.dep.cppm", "export module gen.dep;\nimport gen.dep.table;\nexport int dep_value() { return table_value(); }\n");
        app.write("mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[indices]\nlocal-dev = {{ path = \"{}\" }}\n[dependencies]\n\"local-dev.gen-dep\" = \"1.0.0\"\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", idx.dir().generic_string()));
        auto r = app.mcpp({"build"}, home);
        expect(fatal(e2e::succeeded(r))) << "the first build failed (a fixture problem)\n" << s::text(r);
        (void)app.mcpp({"clean"}, home);
        r = app.mcpp({"build"}, home);
        expect(fatal(e2e::succeeded(r))) << "A: the build that stages the package failed\n" << s::text(r);
        expect(fatal(s::has_line(s::text(r), R"(Cached local-dev\.gen-dep v1\.0\.0)"))) << "A: the package was not staged from the cache, so nothing below is exercised\n" << s::text(r);
        const auto bins = s::glob(app.at("target"), "*/*/bin/" + s::exe("app"));
        expect(fatal(!bins.empty())) << "A: no program";
        auto out = s::run(bins.front()).out;
        std::erase(out, '\r');
        expect(fatal(s::last_line(out) == "42")) << "A: the program does not print 42";

        const auto n = graph_of(app.dir());
        expect(fatal(n.has_value())) << "B: no build.ninja";
        const auto d = n->parent_path();
        const auto graph = s::read_file(*n);
        std::string stage;
        for (const auto& l : s::lines_with(graph, " : stage_file "))
            if (std::regex_search(l, std::regex{R"(^build [a-z]+\.cache/[^ ]*gen\.dep\.[a-z]+ : stage_file )"})) stage = l;
        expect(fatal(!stage.empty())) << "B: the cached BMI has no stage edge";
        const auto bmi = words(stage)[1];
        const auto ext = bmi.substr(bmi.rfind('.') + 1);
        const auto table = (fs::path(bmi).parent_path() / std::format("gen.dep.table.{}", ext)).generic_string();
        expect(fatal(stage.contains("|| " + table))) << "B: the stage edge does not wait for the generated module's BMI: " << stage;
        const auto phony = s::lines_with(graph, "build _mcpp_staged_cache : phony");
        for (const auto& l : phony)
            if (l.starts_with("build _mcpp_staged_cache : phony"))
                expect(fatal(!(" " + l + " ").contains(" " + bmi + " "))) << "B: the aggregate holds a stage that waits for a compile: " << l;

        std::optional<fs::path> ninja;
        for (const auto& pattern : {"xim-x-ninja/*/" + s::exe("ninja"), "xim-x-ninja/*/bin/" + s::exe("ninja")})
            for (const auto& c : s::glob(mcpp_home / "registry/data/xpkgs", pattern))
                if (!ninja && fs::is_regular_file(c)) ninja = c;
        expect(fatal(ninja.has_value())) << "C: no ninja binary";
        std::string main_obj;
        for (const auto& l : s::lines_with(graph, " : cxx_object "))
            if (main_obj.empty() && std::regex_search(l, std::regex{R"(^build [^ ]*main\.[a-z.]+ : cxx_object )"})) main_obj = words(l)[1];
        expect(fatal(!main_obj.empty())) << "C: no compile edge for main.cpp";
        std::error_code ec;
        fs::remove(d / ".ninja_deps", ec);
        fs::remove(d / table, ec);
        fs::remove(d / main_obj, ec);
        auto c = e2e::run({.argv = {ninja->string(), "-C", d.string(), main_obj}, .timeout = std::chrono::minutes{10}});
        expect(fatal(e2e::succeeded(c))) << "C: the consumer compiled before the module it reaches\n" << s::text(c);
        expect(fatal(fs::is_regular_file(d / table))) << "C: the generated module was not compiled";
    };

    "1039 two builds that find the std module missing compile it once"_test[resource{"machine"}] = [] {
        // With the std module entry two projects use removed, the two build at once and both
        // succeed; the entry is whole afterwards (its record and its object).
        auto one = s::project::scaffold("one");
        auto two = s::project::scaffold("two");
        auto r = one.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "the first build\n" << s::text(r);
        const auto root = s::shared_home() / "build-cache/v1/std";
        std::optional<fs::path> entry;
        fs::file_time_type newest{};
        std::error_code ec;
        if (fs::is_directory(root, ec))
            for (const auto& d : fs::directory_iterator(root)) {
                const auto rec = d.path() / "std-module.json";
                if (fs::is_regular_file(rec, ec) && (!entry || fs::last_write_time(rec) > newest)) {
                    entry = d.path();
                    newest = fs::last_write_time(rec);
                }
            }
        expect(fatal(entry.has_value())) << "no std module entry under " << root.string();
        fs::remove_all(*entry);
        auto a = s::mcpp_command({"build"}, one.dir());
        auto b = s::mcpp_command({"build"}, two.dir());
        std::optional<e2e::outcome> ra, rb;
        {
            std::jthread ta([&] { ra = e2e::run(a); });
            std::jthread tb([&] { rb = e2e::run(b); });
        }
        expect(fatal(e2e::succeeded(*ra))) << "the first of two builds at once\n" << s::text(*ra) << s::text(*rb);
        expect(fatal(e2e::succeeded(*rb))) << "the second of two builds at once\n" << s::text(*ra) << s::text(*rb);
        expect(fatal(fs::is_regular_file(*entry / "std-module.json"))) << "the entry has no record afterwards";
        bool object = false;
        for (const auto& f : fs::directory_iterator(*entry)) object = object || f.path().extension() == ".o";
        expect(fatal(object)) << "the entry has no object afterwards";
    };
}};

}  // namespace

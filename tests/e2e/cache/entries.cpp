// The global build cache: an entry shared across projects and keyed by what it was compiled
// against, the cache modes and the maintenance commands, an entry's layout, a hit's std BMI, and
// the age of a std module (formerly tests/e2e/172, 174, 184, 212, 959, 1029).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

// A descriptor of a local index whose payload the project seeds under `.mcpp/.xlings/data/xpkgs`.
std::string descriptor(std::string_view name, std::string_view description, std::string_view sources, bool import_std) {
    return std::format(R"(package = {{
    spec = "1",
    name = "{0}",
    description = "{1}",
    licenses = {{"MIT"}},
    type = "package",
    xpm = {{
        linux = {{
            ["1.0.0"] = {{
                url = "https://example.invalid/{0}-1.0.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            }},
        }},
    }},
    mcpp = {{
        language = "c++23",
{3}        sources = {{ "{2}" }},
        targets = {{ ["{0}"] = {{ kind = "lib" }} }},
        deps = {{}},
    }},
}}
)",
                       name, description, sources, import_std ? "        import_std = true,\n" : "");
}

// A consumer of the local index at `index` (a path as a manifest spells it: forward slashes).
std::string consumer(std::string_view name, std::string_view version, const fs::path& index, std::string_view deps) {
    return std::format("[package]\nname = \"{0}\"\nversion = \"{1}\"\n[indices]\nlocal-dev = {{ path = \"{2}\" }}\n[dependencies]\n{3}\n[targets.{0}]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n",
                       name, version, index.generic_string(), deps);
}

// The first build.ninja under the project's target, outside the build programs' tree.
std::string graph(const fs::path& project) {
    for (const auto& f : s::find_files(project / "target", R"(build\.ninja)"))
        if (!f.generic_string().contains("/.build-mcpp/")) return s::read_file(f);
    return {};
}
fs::path graph_dir(const fs::path& project) {
    for (const auto& f : s::find_files(project / "target", R"(build\.ninja)"))
        if (!f.generic_string().contains("/.build-mcpp/")) return f.parent_path();
    return {};
}

// What the program `target/*/*/bin/<name>` prints.
std::string output_of(const s::project& p, std::string_view name) {
    const auto bins = s::glob(p.at("target"), std::format("*/*/bin/{}", name));
    expect(fatal(!bins.empty())) << "no program " << name << " under target";
    const auto r = s::run(bins.front());
    expect(fatal(e2e::succeeded(r))) << s::text(r);
    return r.out;
}

std::size_t files_named(const fs::path& root, std::string_view name) {
    return s::find_files(root, name).size();
}

// The files below `root` and `root` itself, written `ago` before now.
void age(const fs::path& root, std::chrono::hours ago) {
    const auto t = fs::file_time_type::clock::now() - ago;
    for (const auto& e : fs::recursive_directory_iterator(root)) fs::last_write_time(e.path(), t);
    fs::last_write_time(root, t);
}

}  // namespace

E2E entries{"entries", [] {
    "172 build cache cross project"_test[tag{"requires:gcc"}][tag{"requires:fresh-sandbox"}] = [] {
        // The global build cache saves work across projects: a dependency's key carries no
        // consumer identity, and the second project has zero compile edges for it (read from
        // build.ninja, not from a status line), while a new dependency version is compiled again.
        s::needs({"gcc", "fresh-sandbox"});
        const s::options home{.where = s::home::inherited};
        const auto tmp = s::case_dir();
        const auto index = tmp / "local-index";
        s::project(index, {{"pkgs/s/shared-lib.lua", descriptor("shared-lib", "Shared across projects", "src/**/*.cppm", true)}});
        const auto make = [&](std::string_view name, std::string_view version) {
            return s::project(tmp / name, {{".mcpp/.xlings/data/xpkgs/local-dev.shared-lib/1.0.0/src/lib.cppm", "export module shared.lib;\nexport int shared_value() { return 41; }\n"},
                                           {"src/main.cpp", "import std;\nimport shared.lib;\nint main() { std::println(\"{}\", shared_value() + 1); return 0; }\n"},
                                           {"mcpp.toml", consumer(name, version, index, "\"local-dev.shared-lib\" = \"1.0.0\"")}});
        };
        constexpr std::string_view compile_edges = R"(: (cxx_module|cxx_object|cxx_scan) .*shared-lib)";
        auto one = make("projone", "0.1.0");
        auto r = one.mcpp({"build"}, home);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        auto n = graph(one.dir());
        expect(fatal(!n.empty())) << "projone has no build.ninja";
        expect(fatal(s::count_matching(n, compile_edges) > 0u)) << "cold build had no compile edges for the dependency";
        const auto list = s::text(one.mcpp({"cache", "list"}, home));
        expect(fatal(e2e::contains(list, "shared-lib"))) << "cold build did not populate a cache entry\n" << list << s::text(r);

        auto two = make("projtwo", "9.9.9");
        r = two.mcpp({"build", "-v"}, home);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        n = graph(two.dir());
        expect(fatal(!n.empty())) << "projtwo has no build.ninja";
        expect(fatal(s::count_matching(n, compile_edges) == 0u)) << "second project recompiled the dependency: the cache key still carries the consumer's identity";
        expect(fatal(s::count_matching(n, R"(^build .* : stage_file .*shared-lib)") > 0u)) << "dependency has neither compile nor stage edges";
        expect(fatal(s::has_line(s::text(r), R"(Cached local-dev\.shared-lib v1\.0\.0 \((index [^,)]+, )?[0-9]+ units?\))"))) << "no 'cached N units' line for the reused dependency\n" << s::text(r);
        expect(fatal(s::has_line(output_of(two, "projtwo"), "^42$"))) << "staged artifacts produced wrong output";

        // The consumer's own version does not invalidate the dependency.
        auto toml = one.read("mcpp.toml");
        toml.replace(toml.find("version = \"0.1.0\""), 17, "version = \"0.2.0\"");
        one.write("mcpp.toml", toml);
        one.remove("target");
        r = one.mcpp({"build"}, home);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(s::count_matching(graph(one.dir()), compile_edges) == 0u)) << "bumping the consumer's own version invalidated the dependency";

        // A new dependency version is a different entry, compiled again.
        const auto lua = index / "pkgs/s/shared-lib.lua";
        auto text = s::read_file(lua);
        text.replace(text.find("[\"1.0.0\"]"), 9, "[\"1.0.1\"]");
        std::ofstream(lua, std::ios::binary) << text;
        fs::rename(one.at(".mcpp/.xlings/data/xpkgs/local-dev.shared-lib/1.0.0"), one.at(".mcpp/.xlings/data/xpkgs/local-dev.shared-lib/1.0.1"));
        toml = one.read("mcpp.toml");
        toml.replace(toml.find("\"local-dev.shared-lib\" = \"1.0.0\""), 32, "\"local-dev.shared-lib\" = \"1.0.1\"");
        one.write("mcpp.toml", toml);
        one.remove("target");
        r = one.mcpp({"build"}, home);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(s::count_matching(graph(one.dir()), compile_edges) > 0u)) << "a new dependency version was served from the old entry\n" << s::text(r);
    };

    "174 cache modes and commands"_test[tag{"requires:gcc"}][tag{"requires:fresh-sandbox"}] = [] {
        // --cache=global|local|off, --no-cache, MCPP_BUILD_CACHE and [build] cache; the fast path
        // honours the mode; cache dir, list, info, verify, gc (by last use), clean.
        s::needs({"gcc", "fresh-sandbox"});
        const s::options home{.where = s::home::inherited};
        const auto mcpp_home = s::case_home(s::home::inherited);
        const auto tmp = s::case_dir();
        const auto index = tmp / "local-index";
        s::project(index, {{"pkgs/m/mode-lib.lua", descriptor("mode-lib", "Cache mode fixture", "src/**/*.cppm", true)}});
        const auto base = consumer("app", "0.1.0", index, "\"local-dev.mode-lib\" = \"1.0.0\"");
        s::project app(tmp / "app", {{".mcpp/.xlings/data/xpkgs/local-dev.mode-lib/1.0.0/src/lib.cppm", "export module mode.lib;\nexport int mode_value() { return 5; }\n"},
                                     {"src/main.cpp", "import std;\nimport mode.lib;\nint main() { std::println(\"{}\", mode_value()); return 0; }\n"},
                                     {"mcpp.toml", base}});
        const auto pkg = mcpp_home / "build-cache/v1/pkg";
        const auto stdc = mcpp_home / "build-cache/v1/std";
        const auto entries = [&] { return files_named(pkg, R"(entry\.json)"); };
        constexpr std::string_view compiled = R"(: (cxx_module|cxx_object) .*mode-lib)";
        const auto build = [&](std::vector<std::string> args, const s::options& o) {
            auto r = app.mcpp(std::move(args), o);
            expect(fatal(e2e::succeeded(r))) << s::text(r);
            return r;
        };

        auto r = app.mcpp({"cache", "dir"}, home);
        expect(fatal(s::text(r).starts_with((mcpp_home / "build-cache/v1").string()))) << "cache dir reported '" << s::text(r) << "'";

        // local: nothing read, nothing written; the dependency compiles in the project.
        build({"build", "--cache=local"}, home);
        expect(fatal(entries() == 0u)) << "--cache=local wrote " << entries() << " cache entries";
        expect(fatal(s::count_matching(graph(app.dir()), compiled) > 0u)) << "--cache=local did not compile the dependency locally";
        expect(fatal(s::has_line(output_of(app, "app"), "^5$"))) << "local build produced wrong output";

        // global (the default): writes an entry.
        app.remove("target");
        r = build({"build"}, home);
        expect(fatal(entries() == 1u)) << "default build wrote " << entries() << " entries, expected 1\n" << s::text(r);

        r = app.mcpp({"cache", "list"}, home);
        expect(fatal(e2e::contains(s::text(r), "mode-lib"))) << "list omits the entry\n" << s::text(r);
        r = app.mcpp({"cache", "list", "--json"}, home);
        const auto d = s::json(r.out);
        expect(fatal(d.contains("root") && d.contains("entries"))) << "list --json missing keys: " << r.out;
        nlohmann::json e;
        for (const auto& x : d["entries"])
            if (x["kind"] == "pkg") { e = x; break; }
        expect(fatal(!e.is_null())) << "list --json has no package entries";
        for (std::string_view k : {"kind", "label", "key", "dir", "bytes", "files", "accessed", "complete"})
            expect(fatal(e.contains(k))) << "entry missing '" << k << "': " << e.dump();
        expect(fatal(e["complete"].get<bool>())) << "fresh entry reported incomplete: " << e.dump();
        expect(fatal(e["bytes"].get<long long>() > 0)) << "entry reports " << e["bytes"].dump() << " bytes";
        r = app.mcpp({"cache", "info", "mode-lib"}, home);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "inputs"))) << "info omits key inputs\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "opt_level"))) << "info omits the profile axis\n" << s::text(r);
        r = app.mcpp({"cache", "verify"}, home);
        expect(fatal(e2e::succeeded(r))) << "verify failed on a healthy cache\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "all complete"))) << "verify message\n" << s::text(r);
        fs::path victim;
        for (const auto& f : s::find_files(pkg, ".*"))
            if (f.generic_string().contains("/obj/")) { victim = f; break; }
        expect(fatal(!victim.empty())) << "no cached object to remove";
        fs::rename(victim, victim.string() + ".bak");
        r = app.mcpp({"cache", "verify"}, home);
        expect(fatal(!e2e::succeeded(r))) << "verify passed with a missing artifact\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "incomplete"))) << "verify wording\n" << s::text(r);
        fs::rename(victim.string() + ".bak", victim);

        // off: clears this build's directory (not all of target/) and touches no cache.
        const auto marker = graph_dir(app.dir()) / "_off_marker";
        std::ofstream(marker).put('\n');
        const auto before = entries();
        build({"build", "--cache=off"}, home);
        expect(fatal(!fs::exists(marker))) << "--cache=off did not clear the build dir";
        expect(fatal(entries() == before)) << "--cache=off changed the cache (" << before << " -> " << entries() << ")";
        expect(fatal(s::count_matching(graph(app.dir()), compiled) > 0u)) << "--cache=off served the dependency from the cache";
        std::ofstream(marker).put('\n');
        build({"build", "--no-cache"}, home);
        expect(fatal(!fs::exists(marker))) << "--no-cache did not clear the build dir";
        r = app.mcpp({"build", "--cache=bogus", "--strict"}, home);
        expect(fatal(!e2e::succeeded(r))) << "--cache=bogus accepted under --strict\n" << s::text(r);
        expect(fatal(s::contains_nocase(s::text(r), "cache mode"))) << "unhelpful error\n" << s::text(r);

        // MCPP_BUILD_CACHE, then [build] cache, which --cache beats.
        app.remove("target");
        build({"build"}, {.where = s::home::inherited, .env = {{"MCPP_BUILD_CACHE", "local"}}});
        expect(fatal(s::count_matching(graph(app.dir()), compiled) > 0u)) << "MCPP_BUILD_CACHE=local was ignored";
        const auto local = base + "[build]\ncache = \"local\"\n";
        app.write("mcpp.toml", local);
        app.remove("target");
        build({"build"}, home);
        expect(fatal(s::count_matching(graph(app.dir()), compiled) > 0u)) << "[build] cache = local was ignored";
        app.remove("target");
        build({"build", "--cache=global"}, home);
        expect(fatal(s::count_matching(graph(app.dir()), compiled) == 0u)) << "--cache=global did not override [build] cache = local";

        // The fast path honours the declared mode: a global graph is not replayed for a local
        // request (mcpp.toml unchanged between the two builds, so only the recorded mode decides).
        app.remove("target");
        r = build({"build", "--cache=global"}, home);
        expect(fatal(s::count_matching(graph(app.dir()), R"(: stage_file .*mode-lib)") > 0u)) << "--cache=global did not produce stage edges\n" << s::text(r);
        r = build({"build"}, home);
        expect(fatal(s::count_matching(graph(app.dir()), R"(: stage_file .*mode-lib)") == 0u)) << "bare build replayed the global-mode graph: [build] cache = local was bypassed by the fast path\n" << s::text(r);
        expect(fatal(s::count_matching(graph(app.dir()), compiled) > 0u)) << "local mode did not compile the dependency";
        app.write("mcpp.toml", base);
        app.remove("target");
        build({"build"}, home);

        // gc: by last use, not by when the entry was written.
        const auto entry = s::find_files(pkg, R"(entry\.json)").front();
        auto doc = s::json_file(entry);
        doc["accessed"] = "1000";
        std::ofstream(entry, std::ios::binary) << doc.dump(2);
        fs::last_write_time(entry, s::file_time(std::chrono::sys_seconds{std::chrono::seconds{1000}}));
        r = app.mcpp({"cache", "gc", "--older-than", "1s"}, home);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(entries() == 0u)) << "gc did not collect a long-unused entry\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "Collected"))) << "gc wording\n" << s::text(r);
        app.remove("target");
        build({"build"}, home);
        build({"build"}, home);  // a hit
        r = app.mcpp({"cache", "gc", "--older-than", "1h"}, home);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(entries() == 1u)) << "gc collected an entry that was just used\n" << s::text(r);
        r = app.mcpp({"cache", "gc"}, home);
        expect(fatal(!e2e::succeeded(r))) << "gc with no budget succeeded\n" << s::text(r);
        expect(fatal(fs::is_directory(stdc))) << "no std entries to protect";
        r = app.mcpp({"cache", "gc", "--max-size", "1B"}, home);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(entries() == 0u)) << "gc --max-size kept package entries\n" << s::text(r);
        expect(fatal(fs::is_directory(stdc))) << "gc --max-size evicted std entries\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "package and tool entries now"))) << "gc summary must scope its figure to package and tool entries\n" << s::text(r);

        // clean --std / --all / --legacy.
        app.remove("target");
        build({"build"}, home);
        expect(fatal(fs::is_directory(stdc))) << "no std cache dir";
        app.mcpp({"cache", "clean", "--std"}, home);
        expect(fatal(!fs::is_directory(stdc))) << "clean --std left the std entries";
        expect(fatal(entries() == 1u)) << "clean --std dropped package entries too";
        app.mcpp({"cache", "clean", "--all"}, home);
        expect(fatal(entries() == 0u)) << "clean --all left package entries";
        s::project(mcpp_home / "bmi/deadbeef/deps/idx/pkg@1.0.0", {{"old.o", "x\n"}});
        r = app.mcpp({"cache", "clean", "--legacy"}, home);
        expect(fatal(!fs::exists(mcpp_home / "bmi"))) << "clean --legacy left " << (mcpp_home / "bmi").string() << "\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "pre-v1"))) << "legacy wording\n" << s::text(r);
    };

    "184 build cache object layout"_test[tag{"requires:gcc"}][tag{"requires:fresh-sandbox"}] = [] {
        // An entry's object layout excludes the consumer (mcpp#344): two libraries that collide
        // on compress.c, in both orders, and the second project reuses lib-a with zero compile
        // edges and prints 42.
        s::needs({"gcc", "fresh-sandbox"});
        const s::options home{.where = s::home::inherited};
        const auto mcpp_home = s::case_home(s::home::inherited);
        const auto tmp = s::case_dir();
        const auto index = tmp / "local-index";
        s::project(index, {{"pkgs/l/lib-a.lua", descriptor("lib-a", "collides on compress.c", "src/**/*.c", false)},
                           {"pkgs/l/lib-b.lua", descriptor("lib-b", "collides on compress.c", "src/**/*.c", false)}});
        const auto make = [&](std::string_view name, bool with_b) {
            const auto deps = with_b ? "\"local-dev.lib-a\" = \"1.0.0\"\n\"local-dev.lib-b\" = \"1.0.0\"" : "\"local-dev.lib-a\" = \"1.0.0\"";
            const auto main = with_b ? "import std;\nextern \"C\" int a_value(void);\nextern \"C\" int b_value(void);\nint main() { std::println(\"{}\", a_value() + b_value() + 1); return 0; }\n"
                                     : "import std;\nextern \"C\" int a_value(void);\nint main() { std::println(\"{}\", a_value() + 2); return 0; }\n";
            s::project p(tmp / name, {{".mcpp/.xlings/data/xpkgs/local-dev.lib-a/1.0.0/src/compress.c", "int a_value(void) { return 40; }\n"},
                                      {"src/main.cpp", main},
                                      {"mcpp.toml", consumer(name, "0.1.0", index, deps)}});
            if (with_b) p.write(".mcpp/.xlings/data/xpkgs/local-dev.lib-b/1.0.0/src/compress.c", "int b_value(void) { return 1; }\n");
            return p;
        };
        const auto both = make("both", true);
        const auto onlya = make("onlya", false);
        const auto check = [&](const s::project& p, std::string_view name, bool reuses) {
            p.remove("target");
            auto r = p.mcpp({"build"}, home);
            expect(fatal(e2e::succeeded(r))) << name << " failed to build (" << (reuses ? "reuses" : "cold") << ")\n" << s::text(r);
            const auto n = graph(p.dir());
            expect(fatal(!n.empty())) << name << " has no build.ninja";
            if (reuses) {
                expect(fatal(s::count_matching(n, R"(: (cxx_module|cxx_object|c_object|cxx_scan) .*lib-a)") == 0u)) << name << " recompiled lib-a instead of reusing the entry the other project populated";
                expect(fatal(s::count_matching(n, R"(^build .* : stage_file .*lib-a)") > 0u)) << name << " has neither compile nor stage edges for lib-a";
            }
            expect(fatal(s::has_line(output_of(p, name), "^42$"))) << name << " produced the wrong answer: staged the wrong object?";
        };
        fs::remove_all(mcpp_home / "build-cache");
        check(both, "both", false);
        check(onlya, "onlya", true);
        fs::remove_all(mcpp_home / "build-cache");
        check(onlya, "onlya", false);
        check(both, "both", true);
        const auto r = both.mcpp({"cache", "verify"}, home);
        expect(fatal(e2e::succeeded(r))) << "cache verify reported incomplete entries\n" << s::text(r);
    };

    "212 cached dep std is ordered"_test[tag{"requires:gcc"}][tag{"requires:fresh-sandbox"}] = [] {
        // mcpp#405: a dependency that imports std, consumed by a project that does not, builds on
        // a cache hit: the staged-cache aggregate names the std BMI. Nothing is deleted between
        // the builds.
        s::needs({"gcc", "fresh-sandbox"});
        const s::options home{.where = s::home::inherited};
        const auto tmp = s::case_dir();
        const auto index = tmp / "local-index";
        s::project(index, {{"pkgs/s/stdlib-dep.lua", descriptor("stdlib-dep", "A dependency whose module imports std", "src/**/*.cppm", true)}});
        const auto make = [&](std::string_view name) {
            return s::project(tmp / name, {{".mcpp/.xlings/data/xpkgs/local-dev.stdlib-dep/1.0.0/src/lib.cppm", "export module stdlib.dep;\nimport std;\nexport std::string dep_greeting() { return std::string(\"42\"); }\n"},
                                           {"src/main.cpp", "import stdlib.dep;\nextern \"C\" int puts(const char*);\nint main() { puts(dep_greeting().c_str()); return 0; }\n"},
                                           {"mcpp.toml", consumer(name, "0.1.0", index, "\"local-dev.stdlib-dep\" = \"1.0.0\"")}});
        };
        const auto miss = make("projmiss");
        auto r = miss.mcpp({"build"}, home);
        expect(fatal(e2e::succeeded(r))) << "cold build (cache miss) failed: a fixture problem, not #405\n" << s::text(r);
        const auto hit = make("projhit");
        r = hit.mcpp({"build", "-v"}, home);
        expect(fatal(e2e::succeeded(r))) << "second project (cache hit) did not build" << (e2e::contains(s::text(r), "std.gcm") ? " (mcpp#405: the restored BMI's std edge is not in the graph)" : "") << "\n" << s::text(r);
        expect(fatal(s::has_line(s::text(r), R"(Cached local-dev\.stdlib-dep v1\.0\.0 \((index [^,)]+, )?[0-9]+ units?\))"))) << "the second project did not hit the cache, so #405 was not exercised\n" << s::text(r);
        const auto n = graph(hit.dir());
        expect(fatal(!n.empty())) << "projhit has no build.ninja";
        std::string phony;
        for (const auto& l : s::lines_with(n, "build _mcpp_staged_cache : phony"))
            if (l.starts_with("build _mcpp_staged_cache : phony")) { phony = l; break; }
        expect(fatal(!phony.empty())) << "no _mcpp_staged_cache aggregate, so the dependency was not staged";
        expect(fatal(e2e::contains(phony, "std.gcm"))) << "the staged-cache aggregate does not include the std BMI\n" << phony;
        expect(fatal(s::has_line(output_of(hit, "projhit"), "^42$"))) << "staged artifacts produced wrong output";
    };

    "959 a package is keyed by every module it imports"_test[tag{"requires:gcc"}][tag{"requires:fresh-sandbox"}] = [] {
        // A cached package's key covers every package whose module it imports, declared or not:
        // ex.lib imports ex.api without stating it, and a new ex.api interface compiles ex.lib
        // again rather than serving the entry compiled against the old one.
        s::needs({"gcc", "fresh-sandbox"});
        const s::options home{.where = s::home::inherited};
        const auto mcpp_home = s::case_home(s::home::inherited);
        const auto tmp = s::case_dir();
        const auto descriptor = [](std::string_view name, std::vector<std::string_view> versions) {
            std::string rows;
            for (auto v : versions)
                rows += std::format("            [\"{0}\"] = {{ url = \"https://example.invalid/{1}-{0}.tar.gz\", sha256 = \"{2}\" }},\n", v, name, std::string(64, '0'));
            return std::format("package = {{\n    spec = \"1\",\n    namespace = \"ex\",\n    name = \"ex.{0}\",\n    description = \"959\",\n    licenses = {{\"MIT\"}},\n    type = \"package\",\n    xpm = {{\n        linux   = {{\n{1}        }},\n        macosx  = {{\n{1}        }},\n        windows = {{\n{1}        }},\n    }},\n    mcpp = {{\n        language = \"c++23\",\n        import_std = false,\n        sources = {{ \"src/{0}.cppm\" }},\n        targets = {{ [\"{0}\"] = {{ kind = \"lib\" }} }},\n        deps = {{}},\n    }},\n}}\n",
                               name, rows);
        };
        s::project(tmp / "index", {{"pkgs/e/ex.api.lua", descriptor("api", {"0.1.0", "0.2.0"})}, {"pkgs/e/ex.lib.lua", descriptor("lib", {"0.1.0"})}});
        s::project app(tmp / "app", {{".mcpp/.xlings/data/xpkgs/ex.api/0.1.0/src/api.cppm", "export module ex.api;\nexport namespace api { inline int value() { return 1; } }\n"},
                                     {".mcpp/.xlings/data/xpkgs/ex.api/0.2.0/src/api.cppm", "export module ex.api;\nexport namespace api { struct Box { int v; }; inline int value() { return Box{2}.v; } }\n"},
                                     {".mcpp/.xlings/data/xpkgs/ex.lib/0.1.0/src/lib.cppm", "export module ex.lib;\nimport ex.api;\nexport namespace lib { inline int twice() { return 2 * api::value(); } }\n"},
                                     {"src/main.cpp", "import ex.lib;\nint main() { return lib::twice() == EXPECT ? 0 : 1; }\n"}});
        const auto manifest = [&](std::string_view api, int expected) {
            app.write("mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[build]\ncxxflags = [\"-DEXPECT={}\"]\n[dependencies]\n\"ex.api\" = \"={}\"\n\"ex.lib\" = \"=0.1.0\"\n[indices]\nex = {{ path = \"{}\" }}\n",
                                               expected, api, (tmp / "index").generic_string()));
        };
        const auto pkg = mcpp_home / "build-cache/v1/pkg";
        manifest("0.1.0", 2);
        auto r = app.mcpp({"run"}, home);
        expect(fatal(e2e::succeeded(r))) << "A: the project over ex.api 0.1.0\n" << s::text(r);
        expect(fatal(!s::glob(pkg, "*/ex.lib@0.1.0").empty())) << "A: ex.lib was not cached\n" << s::text(r);
        manifest("0.2.0", 4);
        r = app.mcpp({"run"}, home);
        expect(fatal(e2e::succeeded(r))) << "B: the project over ex.api 0.2.0 with the same ex.lib\n" << s::text(r);
        expect(fatal(s::lacks(s::text(r), "CRC mismatch"))) << "B: ex.lib compiled against ex.api 0.1.0 was served";
        const auto keyed = s::glob(pkg, "*/ex.lib@0.1.0/*");
        expect(fatal(std::ranges::count_if(keyed, [](const fs::path& p) { return fs::is_directory(p); }) == 2)) << "B: ex.lib has not one entry per interface it imported\n" << s::text(r);
    };

    "1029 an unused std module is collected by age"_test = [] {
        // mcpp#874: `cache prune --older-than` and `cache gc --older-than` collect a std module
        // unused for 3 days and keep one used now; `cache gc --max-size 0` keeps std modules.
        const s::options home{.where = s::home::fresh};
        const auto std_dir = s::case_home(s::home::fresh) / "build-cache/v1/std";
        const auto nowhere = s::case_dir();
        const auto seed = [&](std::string_view name, int days) {
            const auto e = std_dir / name;
            fs::create_directories(e / "pcm.cache");
            std::ofstream(e / "std-module.json") << "{\"compiler\":\"gcc\",\"compiler_version\":\"16.1.0\",\"cpp_standard\":\"c++23\",\"stdlib\":\"libstdc++\"}\n";
            std::ofstream(e / "std.o") << "x";
            age(e, std::chrono::hours{24 * days});
        };
        const std::vector<std::vector<std::string>> commands{{"cache", "prune", "--older-than", "1d"}, {"cache", "gc", "--older-than", "1d"}};
        for (const auto& cmd : commands) {
            fs::remove_all(std_dir);
            seed("old", 3);
            seed("fresh", 0);
            const auto r = s::mcpp(cmd, nowhere, home);
            expect(fatal(e2e::succeeded(r))) << r.line << "\n" << s::text(r);
            expect(fatal(!fs::exists(std_dir / "old"))) << r.line << " kept a std module unused for 3 days\n" << s::text(r);
            expect(fatal(fs::exists(std_dir / "fresh"))) << r.line << " removed a std module used now\n" << s::text(r);
        }
        fs::remove_all(std_dir);
        seed("old", 3);
        const auto r = s::mcpp({"cache", "gc", "--max-size", "0"}, nowhere, home);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(fs::exists(std_dir / "old"))) << "a size budget evicted a std module\n" << s::text(r);
    };
}};

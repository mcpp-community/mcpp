// What the index commands state: `index list` and `index status`, the [indices] forms of a project
// and of a workspace, `index pin` and `index unpin`, `emit xpkg`, `xpkg parse`, and the doctor,
// cache and publish commands over a home (formerly tests/e2e/06, 11, 22, 42, 43, 44, 75, 93).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

std::string mirror() { return s::env("MCPP_E2E_TOOLCHAIN_MIRROR").value_or("GLOBAL"); }

// A home of the case, configured for the mirror (`self config --mirror`), and mcpp run in it.
s::options configured_home(const fs::path& cwd, std::vector<std::pair<std::string, std::optional<std::string>>> more = {}) {
    s::options o{.where = s::home::fresh, .env = std::move(more)};
    auto r = s::mcpp({"self", "config", "--mirror", mirror()}, cwd, o);
    expect(fatal(e2e::succeeded(r))) << "self config: " << s::text(r);
    return o;
}

constexpr std::string_view test_pkg_lua = R"(package = {
    homepage = "https://example.com",
    description = "A test package for E2E testing",
    license = "MIT",
}
xpm = {
    linux = {
        ["1.0.0"] = {
            url = "https://example.com/test-pkg-1.0.0.tar.gz",
            sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
        },
    },
}
)";

std::string myapp_manifest(std::string_view indices) {
    return std::format("[package]\nname    = \"myapp\"\nversion = \"0.1.0\"\n\n[indices]\n{}\n[targets.myapp]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", indices);
}

bool any_of_words(std::string_view text, std::initializer_list<std::string_view> words) {
    return std::ranges::any_of(words, [&](std::string_view w) { return text.contains(w); });
}

void pin_and_unpin(const s::project& p, const s::options& o) {
    auto out = s::text(p.mcpp({"index", "pin", "acme", "abc123def0123456789abcdef0123456789abcdef"}, o));
    expect(fatal(any_of_words(out, {"Pinned", "pinned", "Pin"}))) << "pin output unexpected: " << out;
    expect(fatal(e2e::contains(p.read("mcpp.toml"), "rev"))) << "mcpp.toml missing rev after pin";
    out = s::text(p.mcpp({"index", "unpin", "acme"}, o));
    expect(fatal(any_of_words(out, {"Unpinned", "unpinned", "Unpin", "no rev"}))) << "unpin output unexpected: " << out;
}

E2E listing{"listing", [] {
    "06 emit xpkg"_test = [] {
        // `emit xpkg` writes a Lua entry from the manifest: the header, the name, the module, the
        // manifest pointer and import_std; `--output` writes the file, which lua loads.
        auto p = s::project::scaffold("emit-test");
        p.write("src/foo.cppm", "export module emit-test;\nimport std;\nexport int answer() { return 42; }\n");
        auto r = p.mcpp({"emit", "xpkg"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        for (std::string_view w : {"AUTO-GENERATED", "name = \"emit-test\"", "\"emit-test\"", "manifest = \"mcpp.toml\"", "import_std = true"})
            expect(fatal(e2e::contains(r.out, w))) << "missing " << w << ": " << r.out;
        r = p.mcpp({"emit", "xpkg", "--output", "myout.lua"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(p.exists("myout.lua"))) << "--output didn't write file";
        if (auto lua = s::on_path("lua")) {
            auto l = e2e::run({.argv = {lua->string(), "-e", "loadfile('myout.lua')()"}, .cwd = p.dir()});
            expect(fatal(l.started && l.status == 0)) << "Lua failed to load output: " << l.err;
        }
    };

    "11 index list"_test = [] {
        // After `self config`, `index list` shows mcpplibs.
        const auto o = [] {
            s::options h{.where = s::home::fresh};
            auto r = s::mcpp({"self", "config", "--mirror", "auto"}, s::case_dir(), h);
            expect(fatal(e2e::succeeded(r))) << s::text(r);
            return h;
        }();
        auto r = s::mcpp({"index", "list"}, s::case_dir(), o);
        expect(fatal(e2e::succeeded(r) && e2e::contains(s::text(r), "mcpplibs"))) << "index list missing mcpplibs: " << s::text(r);
    };

    "42 custom local index"_test = [] {
        // The [indices] forms (a local path, a short url, a long url with a tag) are listed.
        const auto tmp = s::case_dir();
        const auto o = configured_home(tmp);
        s::project index(tmp / "my-local-index", {{"pkgs/t/test-pkg.lua", test_pkg_lua}});
        auto app = s::project::scaffold("myapp");
        app.write("mcpp.toml", myapp_manifest(std::format(
            "local-dev = {{ path = \"{}\" }}\nacme = \"git@gitlab.example.com:platform/mcpp-index.git\"\nacme-stable = {{ url = \"git@gitlab.example.com:stable.git\", tag = \"v2.0\" }}\n",
            index.dir().generic_string())));
        const auto out = s::text(app.mcpp({"index", "list"}, o));
        for (std::string_view w : {"local-dev", "local path", "acme", "acme-stable", "tag: v2.0"})
            expect(fatal(e2e::contains(out, w))) << "missing " << w << " in output: " << out;
    };

    "43 indices lockfile"_test = [] {
        // A v1 and a v2 lockfile beside [indices]; `index pin` writes a rev, `index unpin` answers;
        // `index list` still lists the indices.
        const auto tmp = s::case_dir();
        const auto o = configured_home(tmp);
        s::project index(tmp / "my-local-index", {{"pkgs/t/test-pkg.lua", test_pkg_lua}});
        auto app = s::project::scaffold("myapp");
        app.write("mcpp.toml", myapp_manifest(std::format(
            "local-dev = {{ path = \"{}\" }}\nacme = {{ url = \"git@gitlab.example.com:platform/mcpp-index.git\" }}\n", index.dir().generic_string())));
        app.write("mcpp.lock", "version = 1\n\n[package.\"gtest\"]\nversion = \"1.15.2\"\nsource  = \"mcpplibs+https://github.com/mcpp-community/mcpp-index.git\"\nhash    = \"sha256:abc123\"\n");
        expect(fatal(app.exists("mcpp.lock"))) << "mcpp.lock not found";
        app.write("mcpp.lock", "# Auto-generated by mcpp. Do not edit by hand.\nversion = 2\n\n[indices.\"acme\"]\nurl = \"git@gitlab.example.com:platform/mcpp-index.git\"\nrev = \"abc123def0123456789abcdef0123456789abcdef\"\n\n[package.\"gtest\"]\nnamespace = \"mcpplibs\"\nversion = \"1.15.2\"\nsource  = \"index+mcpplibs@sha:<from-xlings>\"\nhash    = \"sha256:abc123\"\n");
        const auto lock = app.read("mcpp.lock");
        for (std::string_view w : {"version = 2", "[indices.\"acme\"]", "namespace = \"mcpplibs\"", "rev = \"abc123def"})
            expect(fatal(e2e::contains(lock, w))) << "missing " << w;
        pin_and_unpin(app, o);
        const auto out = s::text(app.mcpp({"index", "list"}, o));
        for (std::string_view w : {"local-dev", "local path", "acme"})
            expect(fatal(e2e::contains(out, w))) << "missing " << w << " in list: " << out;
    };

    "44 indices e2e integration"_test = [] {
        // A local path index and a git index are listed; a workspace's [workspace.indices] is
        // listed at its root; pin and unpin answer; a lockfile of the present format; the indices
        // are listed again after the manifest is rewritten.
        const auto tmp = s::case_dir();
        const auto o = configured_home(tmp, {{"MCPP_NO_AUTO_INSTALL", "1"}});
        s::project index(tmp / "my-local-index", {{"pkgs/h/hello-lib.lua", R"(package = {
    homepage = "https://example.com/hello-lib",
    description = "A hello library for testing",
    license = "MIT",
}
mcpp = {
    sources = { "src/**/*.cppm" },
}
xpm = {
    linux = {
        ["1.0.0"] = {
            url = "https://example.com/hello-lib-1.0.0.tar.gz",
            sha256 = "deadbeef00000000000000000000000000000000000000000000000000000000",
        },
    },
}
)"}});
        const auto idx = index.dir().generic_string();
        auto app = s::project::scaffold("myapp");
        const auto manifest = myapp_manifest(std::format("local-dev = {{ path = \"{}\" }}\nacme = {{ url = \"https://github.com/example/fake-index.git\" }}\n", idx));
        app.write("mcpp.toml", manifest);
        auto out = s::text(app.mcpp({"index", "list"}, o));
        for (std::string_view w : {"local-dev", "local path", "acme"})
            expect(fatal(e2e::contains(out, w))) << "test 1: missing " << w << " in output: " << out;

        s::project ws(tmp / "workspace", {
            {"mcpp.toml", std::format("[workspace]\nmembers = [\"member-a\"]\n\n[workspace.indices]\ncorp-index = {{ path = \"{}\" }}\n", idx)},
            {"member-a/mcpp.toml", "[package]\nname    = \"member-a\"\nversion = \"0.1.0\"\n\n[targets.member-a]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
            {"member-a/src/main.cpp", "import std;\nint main() { std::println(\"hello from member-a\"); return 0; }\n"}});
        out = s::text(ws.mcpp({"index", "list"}, o));
        for (std::string_view w : {"corp-index", "local path"})
            expect(fatal(e2e::contains(out, w))) << "test 2: workspace root missing " << w << ": " << out;

        pin_and_unpin(app, o);

        app.write("mcpp.lock", "# Auto-generated by mcpp. Do not edit by hand.\nversion = 2\n\n[package.\"gtest\"]\nnamespace = \"mcpplibs\"\nversion = \"1.15.2\"\nsource  = \"index+mcpplibs@1.15.2\"\nhash    = \"fnv1a:a1b2c3d4e5f60708\"\n");
        const auto lock = app.read("mcpp.lock");
        for (std::string_view w : {"version = 2", "fnv1a:", "index+mcpplibs@1.15.2"})
            expect(fatal(e2e::contains(lock, w))) << "test 4: missing " << w;
        for (std::string_view w : {"sha:<from-xlings>", "sha256:<from-xlings>"})
            expect(fatal(s::lacks(lock, w))) << "test 4: lockfile has the old placeholder " << w;

        app.write("mcpp.toml", manifest);
        out = s::text(app.mcpp({"index", "list"}, o));
        for (std::string_view w : {"local-dev", "acme"})
            expect(fatal(e2e::contains(out, w))) << "test 5: missing " << w << " after re-read: " << out;
    };

    "75 index status offline"_test = [] {
        // After `self init`, `index status` lists xim (present) and mcpplibs under its header, and
        // a second run lists them again.
        const s::options o{.where = s::home::fresh};
        auto r = s::mcpp({"self", "init"}, s::case_dir(), o);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        r = s::mcpp({"index", "status"}, s::case_dir(), o);
        const auto out = s::text(r);
        expect(fatal(e2e::succeeded(r))) << out;
        for (std::string_view w : {"xim", "mcpplibs", "refreshed"})
            expect(fatal(e2e::contains(out, w))) << "index status missing " << w << ": " << out;
        for (const auto& line : s::lines_with(out, "xim"))
            if (std::regex_search(line, std::regex{R"(^\s*xim\s)"}))
                expect(fatal(!line.contains("missing"))) << "xim index reported missing right after init: " << out;
        r = s::mcpp({"index", "status"}, s::case_dir(), o);
        expect(fatal(e2e::succeeded(r) && e2e::contains(s::text(r), "mcpplibs"))) << "second index status failed: " << s::text(r);
    };

    "93 xpkg parse"_test = [] {
        // `xpkg parse`: the descriptor grammar (long brackets, a block comment), `--json` with the
        // generated content verbatim, an unknown key an error unless allowed, a malformed segment
        // refused, and a Form A descriptor's versions in both spellings.
        s::project d({{"good.lua", R"LUA(--[==[ decoy block comment: mcpp = { sources = { "WRONG" } } }}}} ]==]
package = {
    spec      = "1",
    namespace = "demo",
    name      = "demo.gen",
    xpm = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        schema  = "0.1",
        sources = { [[mcpp_generated/gen.cc]] },
        -- mcpp#296: `defines` is a real descriptor key (package-level bare
        -- macros), not a did-you-mean redirect to `flags`. The two manifest
        -- surfaces are two spellings of one schema, so a key accepted in an
        -- mcpp.toml must not be an unknown-key error here. The strict parse
        -- below plus the `unknown_keys == []` assertion are the guard.
        defines = { [[XPKG_DEFINE]], [[N=1]] },
        generated_files = {
            ["mcpp_generated/gen.cc"] = [==[
export module gen;
int f() { return "x]]y"[0]; }
]==],
        },
        targets = { ["gen"] = { kind = "lib" } },
    },
}
)LUA"},
                      {"broken.lua", "package = {\n    spec = \"1\",\n    name = \"broken\",\n    xpm  = { linux = { [\"1.0.0\"] = { url = \"u\", sha256 = \"h\" } } },\n    mcpp = { sources = { },\n}\n"},
                      {"forma.lua", R"(package = {
    spec      = "1",
    namespace = "demo",
    name      = "demo.forma",
    xpm = {
        linux   = { ["0.9.0"] = { url = "u", sha256 = "h" } },
        macosx  = { ["0.9.0"] = { url = "u", sha256 = "h" },
                    ["1.0.0"] = { url = "u2", sha256 = "h2" } },
        windows = { ["1.0.0"] = { url = "u", sha256 = "h" } },
    },
}
)"}});
        auto r = d.mcpp({"xpkg", "parse", "good.lua"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(fatal(e2e::contains(r.out, "parse OK") && e2e::contains(r.out, "demo.gen"))) << r.out;
        expect(fatal(s::has_line(r.out, "34 bytes|generated"))) << r.out;
        r = d.mcpp({"xpkg", "parse", "--json", "good.lua"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        auto j = s::json(r.out);
        expect(fatal(j["namespace"] == "demo" && j["name"] == "gen")) << j.dump();
        expect(fatal(j["generated_contents"]["mcpp_generated/gen.cc"] == "export module gen;\nint f() { return \"x]]y\"[0]; }\n")) << j.dump();
        expect(fatal(j["unknown_keys"] == nlohmann::json::array())) << j.dump();

        auto bad = d.read("good.lua");
        const std::string from = "schema  = \"0.1\",";
        bad.replace(bad.find(from), from.size(), "schema = \"0.1\", bogus_key = \"x\",");
        d.write("bad.lua", bad);
        r = d.mcpp({"xpkg", "parse", "bad.lua"});
        expect(fatal(r.started && r.status != 0)) << "an unknown key should be an error by default";
        expect(fatal(e2e::contains(s::text(r), "unknown mcpp-segment key 'bogus_key'"))) << s::text(r);
        r = d.mcpp({"xpkg", "parse", "--allow-unknown", "bad.lua"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        r = d.mcpp({"xpkg", "parse", "broken.lua"});
        expect(fatal(r.started && r.status != 0)) << "a broken descriptor should fail";

        r = d.mcpp({"xpkg", "parse", "forma.lua"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        for (std::string_view w : {"form       A", "versions   linux", "1.0.0", "parse OK"})
            expect(fatal(e2e::contains(r.out, w))) << "missing " << w << ": " << r.out;
        r = d.mcpp({"xpkg", "parse", "--json", "forma.lua"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        j = s::json(r.out);
        const auto sorted = [](const nlohmann::json& v) {
            auto x = v.get<std::vector<std::string>>();
            std::ranges::sort(x);
            return x;
        };
        using names = std::vector<std::string>;
        expect(fatal(j["namespace"] == "demo" && j["name"] == "forma" && j["form"] == "A")) << j.dump();
        expect(fatal(sorted(j["versions"]["linux"]) == names{"0.9.0"})) << j.dump();
        expect(fatal(sorted(j["versions"]["macosx"]) == names{"0.9.0", "1.0.0"})) << j.dump();
        expect(fatal(sorted(j["versions"]["windows"]) == names{"1.0.0"})) << j.dump();
        r = d.mcpp({"xpkg", "parse", "--format", "json", "forma.lua"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        const auto e = s::json(r.out);
        expect(fatal(e["kind"] == "mcpp.xpkg")) << e.dump();
        expect(fatal(sorted(e["data"]["versions"]["macosx"]) == names{"0.9.0", "1.0.0"})) << e.dump();
    };

    "22 doctor cache publish"_test[s::elf][s::gcc] = [] {
        // On a cold home: `cache list` is empty, `self doctor` reports its checks and changes
        // nothing. Over the machine's payloads: a build's std entry is listed with a plausible
        // age, `cache dir` and `cache verify` agree, `publish --dry-run` packages, `cache prune`
        // needs --older-than, `cache clean` cleans, and `--explain` explains a known code only.
        const auto tmp = s::case_dir();
        const auto cold = s::another_home(s::home::fresh);
        const s::options in_cold{.env = {{"MCPP_HOME", cold.string()}}};
        auto r = s::mcpp({"cache", "list"}, tmp, in_cold);
        expect(fatal(e2e::contains(s::text(r), "empty"))) << "cache list on an empty cache: " << s::text(r);
        r = s::mcpp({"self", "doctor"}, tmp, in_cold);
        expect(fatal(r.started && r.status <= 1)) << "doctor exit code " << r.status << " unexpected: " << s::text(r);
        for (std::string_view w : {"Checking toolchain", "Checking std module", "Checking registry", "Checking cache health", "Doctor result"})
            expect(fatal(e2e::contains(s::text(r), w))) << "no '" << w << "': " << s::text(r);
        r = s::mcpp({"cache", "list"}, tmp, in_cold);
        expect(fatal(e2e::contains(s::text(r), "empty"))) << "doctor wrote to the build cache: " << s::text(r);
        expect(fatal(!fs::exists(cold / "registry"))) << "doctor bootstrapped the cold home";

        const auto home = s::another_home(s::home::inherited);
        const s::options o{.env = {{"MCPP_HOME", home.string()}}};
        auto probe = s::project(tmp / "stdprobe-dir", {});
        r = s::mcpp({"new", "stdprobe"}, probe.dir(), o);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        r = s::mcpp({"build"}, probe.dir() / "stdprobe", o);
        expect(fatal(e2e::succeeded(r))) << "the build that precompiles std failed: " << s::text(r);
        r = s::mcpp({"cache", "list"}, tmp, o);
        auto out = s::text(r);
        expect(fatal(e2e::contains(out, "std"))) << "cache list omits the std entry: " << out;
        std::smatch m;
        if (std::regex_search(out, m, std::regex{R"(([0-9]+)d ago)"}))
            expect(fatal(std::stol(m[1]) <= 3650)) << "cache list reports an implausible age (clock epoch bug): " << out;
        r = s::mcpp({"cache", "dir"}, tmp, o);
        expect(fatal(s::text(r).starts_with((home / "build-cache/v1").string()))) << "cache dir '" << s::text(r) << "' != " << (home / "build-cache/v1").string();
        r = s::mcpp({"cache", "verify"}, tmp, o);
        expect(fatal(e2e::succeeded(r))) << "verify failed: " << s::text(r);

        r = s::mcpp({"new", "myapp"}, tmp, o);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        const s::project app(tmp / "myapp", {});
        auto manifest = std::regex_replace(app.read("mcpp.toml"), std::regex{R"((^|\n)repo[ \t]*=[^\n]*)"}, "$1repo = \"https://github.com/example/myapp\"");
        if (!s::has_line(manifest, "^repo")) manifest += "\n[package]\nrepo = \"https://github.com/example/myapp\"\n";
        app.write("mcpp.toml", manifest);
        for (std::vector<std::string> git : {std::vector<std::string>{"init", "-q"}, {"config", "user.email", "test@local"}, {"config", "user.name", "test"}, {"add", "."}, {"commit", "-q", "-m", "init"}}) {
            git.insert(git.begin(), "git");
            auto g = e2e::run({.argv = git, .cwd = app.dir()});
            expect(fatal(g.started && g.status == 0)) << "git " << git[1] << ": " << g.err;
        }
        r = app.mcpp({"publish", "--dry-run"}, o);
        out = s::text(r);
        for (std::string_view w : {"Packaging myapp", "Tarball", "SHA-256", "--- xpkg.lua content ---", "name = \"myapp\"", "--- end ---", "Next steps"})
            expect(fatal(e2e::contains(out, w))) << "missing " << w << ": " << out;
        expect(fatal(app.exists("target/dist/myapp-0.1.0.tar.gz"))) << "tarball not created";
        expect(fatal(app.exists("target/dist/myapp.lua"))) << "xpkg.lua not created";

        r = s::mcpp({"cache", "prune"}, tmp, o);
        expect(fatal(r.started && r.status != 0)) << "prune without --older-than should fail";
        expect(fatal(e2e::contains(s::text(r), "older-than"))) << "no helpful error: " << s::text(r);
        r = s::mcpp({"cache", "clean"}, tmp, o);
        expect(fatal(e2e::succeeded(r) && e2e::contains(s::text(r), "Cleaned"))) << "no clean message: " << s::text(r);
        r = s::mcpp({"--explain", "E0001"}, tmp, o);
        expect(fatal(e2e::contains(s::text(r), "E0001") && e2e::contains(s::text(r), "name mismatch"))) << "--explain output: " << s::text(r);
        r = s::mcpp({"--explain", "E9999"}, tmp, o);
        expect(fatal(r.started && r.status != 0)) << "an unknown code should fail";
    };
}};

}  // namespace

// How a dependency is named: a package's name and namespace, bare names, identity-first
// resolution and the wire address, quoted dotted keys, a package named like a specification's
// field (formerly tests/e2e/161, 162, 163, 165, 1032, 1074).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

// A descriptor of `namespace`, `name` and one version for the three systems.
std::string descriptor(std::string_view ns, std::string_view name, std::string_view version, bool all_systems = true) {
    const auto v = std::format("{{ [\"{}\"] = {{ url = \"u\", sha256 = \"h\" }} }}", version);
    return std::format("package = {{\n    spec = \"1\", namespace = \"{}\", name = \"{}\",\n    xpm = {{ linux = {}{} }},\n    mcpp = {{ schema = \"0.1\", sources = {{ \"*.cpp\" }} }},\n}}\n",
                       ns, name, v, all_systems ? std::format(",\n            macosx = {0},\n            windows = {0}", v) : "");
}

// sed -e 's/^\( *<key> *= *\)"[^"]*"/\1"<value>"/' over every line of `text`.
std::string rewrite(const std::string& text, std::string_view key, std::string_view value) {
    std::string out;
    const std::regex re(std::format(R"re(^( *{} *= *)"[^"]*")re", key));
    for (auto line : std::views::split(std::string_view(text), '\n')) {
        std::string l(line.begin(), line.end());
        out += std::regex_replace(l, re, std::format("$1\"{}\"", value), std::regex_constants::format_first_only) + "\n";
    }
    if (!text.ends_with('\n') && !out.empty()) out.pop_back();
    return out;
}

// The registry's asio descriptor, renamed; the case is skipped where the registry has none.
std::string renamed_asio(std::string_view ns, std::string_view name) {
    const auto src = s::shared_home() / "indices/mcpplibs/pkgs/c/chriskohlhoff.asio.lua";
    if (!fs::is_regular_file(src))
        skip("no asio descriptor in registry");
    const auto out = rewrite(rewrite(s::read_file(src), "namespace", ns), "name", name);
    expect(fatal(s::has_line(out, std::format(R"(^ *namespace *= *"{}")", ns)))) << "fixture namespace rewrite missed (upstream descriptor shape changed?)";
    expect(fatal(s::has_line(out, std::format(R"(^ *name *= *"{}")", std::regex_replace(std::string(name), std::regex(R"(\.)"), R"(\.)")))))
        << "fixture name rewrite missed (upstream descriptor shape changed?)";
    return out;
}

E2E names{"names", [] {
    "161 xpkg name form"_test = [] {
        // SPEC-001 §3.2: `package.name` is a single atomic segment; the hierarchy belongs in
        // `package.namespace` (the legacy fully qualified `compat.zlib` keeps working). A dotted
        // short name is refused, in --json too, and a build fails before any download. (The script
        // read the JSON with python3.)
        s::project p{{"short.lua", descriptor("chriskohlhoff", "asio", "1.0.0", false)},
                     {"nested.lua", descriptor("mcpplibs.capi", "lua", "1.0.0", false)},
                     {"legacy.lua", descriptor("compat", "compat.zlib", "1.0.0", false)},
                     {"bad.lua", descriptor("mcpplibs", "capi.lua", "1.0.0", false)},
                     {"nons.lua", descriptor("", "mcpplibs.capi.lua", "1.0.0", false)}};
        for (auto ok : {"short.lua", "nested.lua", "legacy.lua"})
            expect(e2e::succeeded(p.mcpp({"xpkg", "parse", ok}))) << ok;
        auto bad = p.mcpp({"xpkg", "parse", "bad.lua"});
        expect(bad.started && bad.status != 0) << "a short name containing '.' must be rejected";
        expect(e2e::contains(s::text(bad), "single atomic segment"));
        expect(e2e::contains(s::text(bad), "mcpplibs.capi"));
        expect(e2e::contains(s::text(bad), "\"lua\""));
        auto nons = p.mcpp({"xpkg", "parse", "nons.lua"});
        expect(nons.started && nons.status != 0) << "dotted name without a namespace must be rejected";
        auto json = p.mcpp({"xpkg", "parse", "--json", "bad.lua"});
        expect(json.started && json.status != 0) << "--json must still exit non-zero on violation";
        const auto j = s::json(json.out);
        expect(fatal(j.contains("error"))) << json.out;
        expect(e2e::contains(j["error"].get<std::string>(), "atomic"));
        // A build fails before any download.
        p.write("idx/pkgs/d/thing.lua", descriptor("demo", "sub.thing", "1.0.0"));
        p.write("app/mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[indices]\ndemo = { path = \"../idx\" }\n\n[dependencies.demo]\n\"sub.thing\" = \"1.0.0\"\n");
        p.write("app/src/main.cpp", "int main() { return 0; }\n");
        auto b = p.mcpp_in("app", {"build"});
        expect(b.started && b.status != 0) << "non-atomic short name must not build";
        expect(e2e::contains(s::text(b), "single atomic segment"));
        expect(s::lacks(s::text(b), "Downloading")) << "must fail before any download";
    };

    "162 bare name namespace scope"_test = [] {
        // INV-RESOLVE (#278): a bare name resolves in exactly one place (mcpplibs; compat during
        // the migration window, with a warning naming the exact replacement); every other
        // namespace is written out; a refused add writes nothing and names what exists.
        s::project p{{"idx/pkgs/a/acme.widget.lua", descriptor("acme", "acme.widget", "1.0.0")},
                     {"idx/pkgs/c/compat.gadget.lua", descriptor("compat", "gadget", "2.0.0")},
                     {"app/src/main.cpp", "int main() { return 0; }\n"}};
        auto write_manifest = [&](std::string_view deps) {
            p.write("app/mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[indices]\nacme = {{ path = \"../idx\" }}\ncompat = {{ path = \"../idx\" }}\n{}\n", deps));
        };
        auto build = [&] { return p.mcpp_in("app", {"build"}); };
        write_manifest("[dependencies]\nwidget = \"1.0.0\"");
        auto bare = build();
        expect(bare.started && bare.status != 0) << "a bare name must not reach the acme namespace";
        for (auto want : {"no package found for exact selector", "tried:", "acme.widget", "acme.widget (1.0.0)", "[dependencies.acme]"})
            expect(e2e::contains(s::text(bare), want)) << want;
        write_manifest("[dependencies]\n\"acme.widget\" = \"1.0.0\"");
        expect(s::lacks(s::text(build()), "no package found for exact selector")) << "dotted selector must resolve to (acme, widget)";
        write_manifest("[dependencies.acme]\nwidget = \"1.0.0\"");
        expect(s::lacks(s::text(build()), "no package found for exact selector")) << "sub-table form must resolve to (acme, widget)";
        write_manifest("[dependencies]\ngadget = \"2.0.0\"");
        const auto compat = s::text(build());
        expect(s::lacks(compat, "no package found for exact selector")) << "bare gadget must still reach compat during the migration window";
        expect(e2e::contains(compat, "deprecated bare-name search")) << "the bare-name fallback must announce itself";
        expect(e2e::contains(compat, "compat.gadget")) << "the warning must name the exact replacement";
        expect(e2e::contains(compat, "[dependencies.compat]")) << "the warning must show the manifest edit";
        write_manifest("[dependencies.compat]\ngadget = \"2.0.0\"");
        const auto explicit_compat = s::text(build());
        expect(s::lacks(explicit_compat, "no package found for exact selector")) << "explicit compat.gadget must resolve";
        expect(s::lacks(explicit_compat, "deprecated bare-name search")) << "an already-exact selector must not warn";
        write_manifest("[dependencies.mcpplibs]\ngadget = \"2.0.0\"");
        auto def = build();
        expect(def.started && def.status != 0) << "mcpplibs.gadget states an identity and must not reach compat";
        expect(e2e::contains(s::text(def), "no package found for exact selector")) << "expected the explicit not-found error";
        write_manifest("");
        auto add = p.mcpp_in("app", {"add", "acme.gadget@9.9.9"}, {.env = {{"MCPP_OFFLINE", "1"}}});
        expect(add.started && add.status != 0) << "adding a package the acme index does not serve must fail";
        expect(e2e::contains(s::text(add), "compat.gadget (2.0.0)")) << "add hint must name compat.gadget with its published version";
        expect(s::lacks(p.read("app/mcpp.toml"), "gadget")) << "a rejected add must not write the dependency";
    };

    "163 identity first resolution"_test = [] {
        // SPEC-001 §3.4, §6: a package is found by its declared identity (namespace, name), not
        // its file name, and addressed on the wire by its literal `package.name`; two namespaces
        // may hold one short name (with xlings >= 0.4.69, #381). The fixture is the registry's
        // asio descriptor, renamed.
        const auto asio = s::shared_home() / "indices/mcpplibs/pkgs/c/chriskohlhoff.asio.lua";
        if (!fs::is_regular_file(asio))
            skip("no asio descriptor in registry");
        s::project root{temp_dir(), {}};
        auto mkidx = [&](std::string_view dir, std::string_view rel, std::string_view ns, std::string_view name) {
            root.write(std::format("{}/pkgs/{}", dir, rel), renamed_asio(ns, name));
        };
        auto mkapp = [&](std::string_view dir, std::string_view index, std::string_view path, std::string_view ns, std::string_view name) {
            root.write(std::format("{}/mcpp.toml", dir), std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[indices]\n{} = {{ path = \"{}\" }}\n\n[dependencies.{}]\n{} = \"1.38.1\"\n", index, path, ns, name));
            root.write(std::format("{}/src/main.cpp", dir), "int main() { return 0; }\n");
        };
        mkidx("idx1", "a/acme.widget.lua", "acme", "widget");
        mkapp("app1", "acme", "../idx1", "acme", "widget");
        auto b1 = root.mcpp_in("app1", {"build", "-v"});
        expect(fatal(e2e::succeeded(b1)));
        expect(s::has_line(s::text(b1), R"((Compiling|Cached) acme\.widget)")) << "acme.widget was neither compiled nor served from cache";
        expect(root.exists("app1/.mcpp/.xlings/data/xpkgs/acme-x-widget")) << "expected store dir acme-x-widget";
        mkidx("idx2", "a/acme.widget.lua", "acme", "acme.widget");
        mkapp("app2", "acme", "../idx2", "acme", "widget");
        expect(fatal(e2e::succeeded(root.mcpp_in("app2", {"build"}))));
        expect(root.exists("app2/.mcpp/.xlings/data/xpkgs/acme-x-acme.widget")) << "expected store dir acme-x-acme.widget";
        mkidx("idx3", "z/totally-unrelated-name.lua", "acme", "widget");
        mkapp("app3", "acme", "../idx3", "acme", "widget");
        expect(e2e::succeeded(root.mcpp_in("app3", {"build"}))) << "descriptor must be discoverable by identity, not filename";
        // Step 4 needs the sandbox's xlings at 0.4.69 or later.
        std::string version;
        if (const auto xl = s::shared_home() / "registry/bin/xlings"; fs::is_regular_file(xl)) {
            std::smatch m;
            const auto out = s::run(xl, {"--version"}).out;
            const auto first = out.substr(0, out.find('\n'));
            if (std::regex_search(first, m, std::regex(R"([0-9]+\.[0-9]+\.[0-9]+)"))) version = m.str();
        }
        auto at_least = [](const std::string& v) {
            int a = 0, b = 0, c = 0;
            if (std::sscanf(v.c_str(), "%d.%d.%d", &a, &b, &c) != 3) return false;
            return std::tuple{a, b, c} >= std::tuple{0, 4, 69};
        };
        if (!at_least(version)) {
            std::println("step 4 skipped: sandbox xlings {} < 0.4.69", version.empty() ? "unknown" : version);
            return;
        }
        mkidx("idx4", "a/alpha.widget.lua", "alpha", "widget");
        mkidx("idx4", "b/beta.widget.lua", "beta", "widget");
        mkapp("app4a", "alpha", "../idx4", "alpha", "widget");
        expect(fatal(e2e::succeeded(root.mcpp_in("app4a", {"build"}))));
        expect(root.exists("app4a/.mcpp/.xlings/data/xpkgs/alpha-x-widget")) << "alpha:widget should land in alpha-x-widget";
        mkapp("app4b", "beta", "../idx4", "beta", "widget");
        expect(fatal(e2e::succeeded(root.mcpp_in("app4b", {"build"})))) << "beta:widget unreachable — needs xlings >= 0.4.69 (#381)";
        expect(root.exists("app4b/.mcpp/.xlings/data/xpkgs/beta-x-widget")) << "beta:widget should land in beta-x-widget";
    };

    "165 bare name cross namespace wire address"_test = [] {
        // SPEC-001 §6: the descriptor the identity gate accepted supplies both halves of the wire
        // address: an explicitly selected compat dependency is installed as compat:widget, never
        // mcpplibs:widget (2026-07-25, 6 of 7 workflows red on main).
        s::project p{{"idx/pkgs/c/compat.widget.lua", renamed_asio("compat", "widget")},
                     {"app/mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[indices]\ncompat = { path = \"../idx\" }\n\n[dependencies.compat]\nwidget = \"1.38.1\"\n"},
                     {"app/src/main.cpp", "int main() { return 0; }\n"}};
        auto b = p.mcpp_in("app", {"build"}, {.env = {{"MCPP_VERBOSE", "1"}}});
        const auto out = std::regex_replace(s::text(b), std::regex(R"(\\")"), "\"");
        std::string targets;
        const std::regex target_list(R"re("targets":\["[^"]*"\])re");
        for (std::sregex_iterator it(out.begin(), out.end(), target_list), end; it != end; ++it)
            targets += it->str() + "\n";
        expect(fatal(!targets.empty())) << "no install target was emitted at all";
        expect(s::lacks(targets, "\"mcpplibs:widget@1.38.1\"")) << "wire address took its namespace from the REQUEST (mcpplibs:widget)";
        expect(fatal(e2e::contains(targets, "\"compat:widget@1.38.1\""))) << "expected the wire address compat:widget@1.38.1";
        expect(fatal(e2e::succeeded(b))) << "build failed even though the wire address was correct";
        bool store = false;
        for (auto it = fs::recursive_directory_iterator(p.at("app/.mcpp")); it != fs::recursive_directory_iterator(); ++it)
            store = store || (it->is_directory() && it->path().filename() == "compat-x-widget");
        expect(store) << "expected a store dir named compat-x-widget";
    };

    "1032 a quoted dotted dependency key is deprecated"_test = [] {
        // `"acme.lib" = ...` is read as the selector `acme.lib` and is the deprecated reading
        // quoted-dotted-dependency-key: warned with the unquoted key, listed by `mcpp why compat`;
        // the unquoted key, and a package beside its parent, are not warned about.
        const auto tmp = temp_dir();
        s::project{tmp / "lib", {{"mcpp.toml", "[package]\nnamespace = \"acme\"\nname      = \"lib\"\nversion   = \"0.1.0\"\n"},
                                 {"src/lib.cppm", "export module acme.lib;\nexport int answer() { return 42; }\n"}}};
        s::project app{tmp / "app", {{"src/main.cpp", "import acme.lib;\nint main() { return answer() == 42 ? 0 : 1; }\n"},
                                     {"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\n\"acme.lib\" = { path = \"../lib\" }\n"}}};
        auto quoted = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(quoted))) << "the quoted key no longer builds";
        expect(e2e::contains(s::text(quoted), "\"acme.lib\" is deprecated since mcpp 2026.11.8.1 and removed from mcpp 2027.2")) << "no deprecation warning for the quoted key";
        expect(e2e::contains(s::text(quoted), "Write the key unquoted, `acme.lib = ...` instead")) << "the warning does not show the unquoted spelling of its own key";
        expect(e2e::contains(s::text(app.mcpp({"why", "compat"})), "quoted-dotted-dependency-key: [dependencies] \"acme.lib\"")) << "mcpp why compat does not list the use";
        app.write("mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\nacme.lib = { path = \"../lib\" }\n");
        auto unquoted = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(unquoted))) << "the unquoted key does not build";
        expect(s::no_line(s::text(unquoted), "quoted-dotted-dependency-key|is deprecated since")) << "the unquoted key is warned about";
        s::project{tmp / "extra", {{"mcpp.toml", "[package]\nnamespace = \"acme.lib\"\nname      = \"extra\"\nversion   = \"0.1.0\"\n"},
                                   {"src/extra.cppm", "export module acme.lib.extra;\nexport int more() { return 1; }\n"}}};
        app.write("mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\nacme.lib = { path = \"../lib\" }\n\"acme.lib.extra\" = { path = \"../extra\" }\n");
        auto pair = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(pair))) << "a package beside its parent does not build";
        expect(s::no_line(s::text(pair), "quoted-dotted-dependency-key|is deprecated since")) << "the only spelling of the pair is warned about";
    };

    "1074 a namespace package named like a spec field"_test = [] {
        // `gpp.version = { path = ... }` is the package `version` in the namespace `gpp`: only a
        // string `path`/`version`/`git` or a boolean `workspace` makes a table one specification.
        const auto tmp = temp_dir();
        s::project{tmp / "version", {{"mcpp.toml", "[package]\nnamespace = \"gpp\"\nname      = \"version\"\nversion   = \"0.1.0\"\n"},
                                     {"src/version.cppm", "export module gpp.version;\nexport int gpp_version() { return 7; }\n"}}};
        s::project app{tmp / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[dependencies]\ngpp.version = { path = \"../version\" }\n"},
                                     {"src/main.cpp", "import gpp.version;\nint main() { return gpp_version() == 7 ? 0 : 1; }\n"}}};
        auto r = app.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "the package gpp.version was not read as a package";
        expect(s::lacks(s::text(r), "must specify")) << "the namespace was read as one specification";
        auto db = app.mcpp({"emit", "build-database", "--format", "json"});
        expect(fatal(e2e::succeeded(db))) << "the build database was not described";
        expect(e2e::contains(db.out, "version.cppm")) << "the build database does not hold gpp.version";
    };
}};

}  // namespace

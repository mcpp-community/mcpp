// What a member receives from its workspace: [workspace.package] and [workspace.build] at every
// position of the graph and however the member was fetched, a published member's archive that
// carries them, and the commands outside the build that read the effective manifest (formerly
// tests/e2e/321, 770, 772, 773, 774).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

const s::options cdb_at_root{.env = {{"MCPP_COMPILE_COMMANDS", "compile_commands.json"}}};
const s::options inherited{.where = s::home::inherited};

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

// git in `dir` with a committer: its standard output.
std::string git(const fs::path& dir, std::vector<std::string> args) {
    const auto program = s::on_path("git");
    if (!program) skip("git is not installed");
    args.insert(args.begin(), {"-C", dir.string(), "-c", "user.email=e2e@example.invalid", "-c", "user.name=e2e"});
    auto r = s::run(*program, args);
    expect(fatal(e2e::succeeded(r))) << "git failed: " << s::text(r);
    return r.out;
}

void commit_all(const fs::path& dir) {
    git(dir, {"init", "-q", "-b", "main", "."});
    git(dir, {"add", "-A"});
    git(dir, {"commit", "-qm", "init"});
}

// The words of `file`'s entry in `cdb` (the case fails when there is none).
std::vector<std::string> entry(const fs::path& cdb, std::string_view file) {
    auto words = s::cdb_arguments(cdb, file);
    expect(fatal(words.has_value())) << "no entry for " << file << " in " << cdb;
    return *words;
}

std::size_t occurrences(const std::vector<std::string>& words, std::string_view word) {
    return static_cast<std::size_t>(std::ranges::count(words, word));
}

// The first `a` precedes the first `b`, both present.
bool precedes(const std::vector<std::string>& words, std::string_view a, std::string_view b) {
    const auto ia = std::ranges::find(words, a), ib = std::ranges::find(words, b);
    return ia != words.end() && ib != words.end() && ia < ib;
}

// `grep -oE '-std=c++[0-9a-z]+|-DFROM_[A-Z]+=1' | sort -u`: the words 321 asserts on.
std::set<std::string> flags_of(const std::vector<std::string>& words) {
    static const std::regex re{R"(-std=c\+\+[0-9a-z]+|-DFROM_[A-Z]+=1)"};
    std::set<std::string> out;
    for (const auto& w : words)
        for (std::sregex_iterator it(w.begin(), w.end(), re), end; it != end; ++it) out.insert(it->str());
    return out;
}

std::string joined(const std::set<std::string>& words) {
    std::string out;
    for (const auto& w : words) out += w + " ";
    return out;
}

// One member of an archive, extracted beside it (`tar -xzf <archive> -O <member>`).
std::string archived(const fs::path& archive, std::string_view member) {
    const auto into = s::case_dir() / "unpacked-one";
    fs::remove_all(into);
    s::tar_extract(archive, into, {std::string(member)});
    return s::read_file(into / member);
}

E2E inheritance{"inheritance", [] {
    "321 workspace inheritance"_test[s::gcc] = [] {
        // [workspace.package] and [workspace.build] (#527), asserted on the compile command: a
        // member that declares nothing inherits the standard and the flags; a member that pins
        // the default standard keeps it; a member's own cxxflags follow the workspace's; the
        // build from inside a member inherits too; a member with no version anywhere is refused;
        // a sibling member reached as a path dependency inherits, a non-member path dependency
        // does not.
        const auto main_cpp = "#include <shared.h>\n#if !defined(FROM_WORKSPACE) || !defined(SHARED_HEADER_FOUND)\n"
                              "#error \"workspace [build] did not reach the member\"\n#endif\nint main(){return 0;}\n";
        s::project ws({
            {"shared/inc/shared.h", "#define SHARED_HEADER_FOUND 1\n"},
            {"mcpp.toml", R"([workspace]
members = ["silent", "pinned", "adds"]

[workspace.package]
standard = 26
version  = "0.4.2"
license  = "Apache-2.0"

[workspace.build]
cxxflags     = ["-DFROM_WORKSPACE=1"]
include_dirs = ["shared/inc"]
)"},
            {"silent/mcpp.toml", "[package]\nname = \"silent\"\n"},
            {"pinned/mcpp.toml", "[package]\nname = \"pinned\"\nstandard = \"c++23\"\n"},
            {"adds/mcpp.toml", "[package]\nname = \"adds\"\n\n[build]\ncxxflags = [\"-DFROM_MEMBER=1\"]\n"},
            {"silent/src/main.cpp", main_cpp},
            {"pinned/src/main.cpp", main_cpp},
            {"adds/src/main.cpp", main_cpp},
        });
        const auto cdb = ws.at("compile_commands.json");
        std::map<std::string, std::vector<std::string>> entries;
        std::map<std::string, e2e::outcome> builds;
        for (std::string m : {"silent", "pinned", "adds"}) {
            builds[m] = ws.mcpp({"build", "-p", m}, cdb_at_root);
            expect(fatal(e2e::succeeded(builds[m]))) << s::text(builds[m]);
            entries[m] = entry(cdb, m + "/src/main.cpp");
        }

        auto got = flags_of(entries["silent"]);
        expect(fatal(got.contains("-DFROM_WORKSPACE=1"))) << "member 'silent' did not inherit [workspace.build] cxxflags; got: " << joined(got);
        expect(fatal(got.contains("-std=c++26"))) << "member 'silent' did not inherit [workspace.package] standard; got: " << joined(got);
        bool version_seen = s::text(builds["silent"]).contains("0.4.2");
        for (const auto& f : s::glob(ws.at("target"), "*/*/resolution.json"))
            version_seen = version_seen || std::regex_search(s::read_file(f), std::regex{R"("version": *"0\.4\.2")"});
        expect(fatal(version_seen)) << "member 'silent' did not inherit [workspace.package] version: " << s::text(builds["silent"]);

        // c++23 is also the default: a declaredness faked by comparing with the default would
        // overwrite this pin.
        got = flags_of(entries["pinned"]);
        expect(fatal(got.contains("-std=c++23"))) << "member 'pinned' declared standard = c++23 and did not get it; got: " << joined(got);
        expect(fatal(got.contains("-DFROM_WORKSPACE=1"))) << "member 'pinned' should still inherit [workspace.build]; got: " << joined(got);

        got = flags_of(entries["adds"]);
        for (std::string_view want : {"-DFROM_WORKSPACE=1", "-DFROM_MEMBER=1"})
            expect(fatal(got.contains(std::string(want)))) << "member 'adds' is missing " << want << "; got: " << joined(got);
        expect(fatal(precedes(entries["adds"], "-DFROM_WORKSPACE=1", "-DFROM_MEMBER=1"))) << "workspace cxxflags must precede the member's own";

        // The second inheritance site: the command issued inside the member.
        auto r = ws.mcpp_in("silent", {"build"}, cdb_at_root);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        got = flags_of(entry(cdb, "silent/src/main.cpp"));
        expect(fatal(got.contains("-DFROM_WORKSPACE=1") && got.contains("-std=c++26")))
            << "inheritance did not happen when the command was issued inside the member directory; got: " << joined(got);

        // The denominator: without the workspace's version, the same member is refused.
        {
            auto text = ws.read("mcpp.toml");
            text.replace(text.find("version  = \"0.4.2\""), std::string_view("version  = \"0.4.2\"").size(), "");
            ws.write("mcpp.toml", text);
        }
        fs::remove_all(ws.at("silent/target"));
        r = ws.mcpp_in("silent", {"build"}, cdb_at_root);
        expect(fatal(failed(r))) << "a member with no version and no [workspace.package] version built: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "package.version"))) << "the refusal does not name the missing field: " << s::text(r);

        // A sibling member reached as a path dependency inherits; a non-member does not.
        ws.write("mcpp.toml", R"([workspace]
members = ["silent", "pinned", "adds", "consumer"]

[workspace.package]
standard = 26
version  = "0.4.2"

[workspace.build]
cxxflags = ["-DFROM_WORKSPACE=1"]
)");
        ws.write("consumer/mcpp.toml", "[package]\nname = \"consumer\"\n\n[dependencies]\nadds = { path = \"../adds\" }\nvend = { path = \"../vendored\" }\n");
        ws.write("consumer/src/main.cpp", "import addslib;\nimport vend;\nint main(){ return addslib()+vend_v()==3 ? 0 : 1; }\n");
        ws.write("adds/mcpp.toml", "[package]\nname = \"adds\"\n\n[targets.adds]\nkind = \"lib\"\n\n[build]\ncxxflags = [\"-DFROM_MEMBER=1\"]\n");
        ws.remove("adds/src/main.cpp");
        ws.write("adds/src/addslib.cppm", "#ifndef FROM_WORKSPACE\n#error \"a SIBLING member built as a path dependency did not inherit\"\n#endif\n"
                                          "export module addslib;\nexport int addslib(){ return 1; }\n");
        ws.write("vendored/mcpp.toml", "[package]\nname = \"vend\"\nversion = \"0.1.0\"\n\n[targets.vend]\nkind = \"lib\"\n");
        ws.write("vendored/src/vend.cppm", "#ifdef FROM_WORKSPACE\n#error \"a NON-member path dependency must not acquire workspace flags\"\n#endif\n"
                                           "export module vend;\nexport int vend_v(){ return 2; }\n");
        r = ws.mcpp({"build", "-p", "consumer"}, cdb_at_root);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
    };

    "770 workspace member as dependency"_test[s::python3] = [] {
        // A member is compiled the same way whichever position it holds and however it was
        // fetched (#690): [workspace.build] reaches it once as the selected root and as a
        // sibling's path dependency, workspace words before the member's; a member reached as a
        // dependency resolves its `x.workspace = true`; `defines` is a set keyed by name (a
        // restated name keeps the member's value, `!NAME` removes); a member of a git-hosted
        // workspace receives its repository's [workspace.build].
        s::project ws(s::case_dir() / "ws", {
            {"mcpp.toml", R"([workspace]
members = ["lib", "app", "util"]

[workspace.package]
version = "0.1.0"

[workspace.dependencies]
util = { path = "util" }

[workspace.build]
defines  = ["WS_DEF=1", "LEVEL=1", "WS_ONLY=1"]
cflags   = ["-DWS_CFLAG=1"]
cxxflags = ["-DWS_CXXFLAG=1"]
)"},
            {"util/mcpp.toml", "[package]\nname = \"util\"\n\n[targets.util]\nkind = \"lib\"\n\n[build]\nsources = [\"u.cpp\"]\n"},
            {"util/u.cpp", "int util_v() { return 5; }\n"},
            {"lib/mcpp.toml", R"([package]
name = "lib"

[dependencies]
util.workspace = true

[targets.lib]
kind = "lib"

[build]
sources  = ["lib.cpp", "c.c"]
defines  = ["MEMBER_DEF=1", "LEVEL=2", "!WS_ONLY"]
cflags   = ["-DMEMBER_CFLAG=1"]
cxxflags = ["-DMEMBER_CXXFLAG=1"]
)"},
            {"lib/lib.cpp", R"(#if !defined(WS_DEF) || !defined(WS_CXXFLAG) || !defined(MEMBER_DEF)
#error "lib.cpp: a workspace or member definition is missing"
#endif
#if LEVEL != 2
#error "lib.cpp: the member's LEVEL must replace the workspace's"
#endif
#ifdef WS_ONLY
#error "lib.cpp: !WS_ONLY must remove the inherited name"
#endif
int util_v();
extern "C" int lib_c();
int lib_v() { return util_v() + lib_c(); }
)"},
            {"lib/c.c", R"(#if !defined(WS_DEF) || !defined(WS_CFLAG) || !defined(MEMBER_DEF)
#error "c.c: a workspace or member definition is missing"
#endif
int lib_c(void) { return LEVEL; }
)"},
            {"app/mcpp.toml", "[package]\nname = \"app\"\n\n[dependencies]\nlib = { path = \"../lib\" }\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
            {"app/src/main.cpp", "int lib_v();\nint main() { return lib_v() == 7 ? 0 : 1; }\n"},
        });
        const auto cdb = ws.at("compile_commands.json");
        for (std::string position : {"lib", "app"}) {
            // The database is removed before each build, so it holds that build's plan alone.
            fs::remove(cdb);
            for (const auto& f : s::glob(ws.at("target"), "*/*/compile_commands.json")) fs::remove(f);
            auto r = ws.mcpp({"build", "-p", position}, cdb_at_root);
            expect(fatal(e2e::succeeded(r))) << "A/B: -p " << position << ": " << s::text(r);
            expect(fatal(fs::exists(cdb))) << "A: no compile_commands.json (-p " << position << ")";
            const auto cxx = entry(cdb, "lib/lib.cpp"), c = entry(cdb, "lib/c.c");
            for (std::string_view w : {"-DWS_DEF=1", "-DWS_CXXFLAG=1", "-DMEMBER_CXXFLAG=1", "-DMEMBER_DEF=1", "-DLEVEL=2"})
                expect(fatal(occurrences(cxx, w) == 1u)) << "A: " << w << " occurs " << occurrences(cxx, w) << " times in lib.cpp (-p " << position << ")";
            for (std::string_view w : {"-DWS_DEF=1", "-DWS_CFLAG=1", "-DMEMBER_CFLAG=1", "-DMEMBER_DEF=1", "-DLEVEL=2"})
                expect(fatal(occurrences(c, w) == 1u)) << "A: " << w << " occurs " << occurrences(c, w) << " times in c.c (-p " << position << ")";
            for (std::string_view w : {"-DLEVEL=1", "-DWS_ONLY=1"})
                expect(fatal(occurrences(cxx, w) == 0u)) << "C: " << w << " occurs " << occurrences(cxx, w) << " times in lib.cpp (-p " << position << ")";
            expect(fatal(precedes(cxx, "-DWS_CXXFLAG=1", "-DMEMBER_CXXFLAG=1"))) << "A: the workspace cxxflags must precede the member's (-p " << position << ")";
            expect(fatal(precedes(c, "-DWS_CFLAG=1", "-DMEMBER_CFLAG=1"))) << "A: the workspace cflags must precede the member's (-p " << position << ")";
        }
        auto r = ws.mcpp({"run", "-p", "app"}, cdb_at_root);
        expect(fatal(e2e::succeeded(r))) << "A/B: the program did not return 0: " << s::text(r);

        // D: a member of a git-hosted workspace.
        s::project repo(s::case_dir() / "repo", {
            {"mcpp.toml", "[workspace]\nmembers = [\"glib\"]\n\n[workspace.package]\nversion = \"0.2.0\"\n\n[workspace.build]\ncxxflags = [\"-DREPO_FLAG=1\"]\n"},
            {"glib/mcpp.toml", "[package]\nnamespace = \"probe770\"\nname = \"glib\"\n\n[targets.glib]\nkind = \"lib\"\n\n[build]\nsources = [\"g.cpp\"]\n"},
            {"glib/g.cpp", "#ifndef REPO_FLAG\n#error \"the repository [workspace.build] did not reach its git-consumed member\"\n#endif\nint g_v() { return REPO_FLAG; }\n"},
        });
        commit_all(repo.dir());
        s::project consumer(s::case_dir() / "consumer", {
            {"mcpp.toml", std::format("[package]\nname = \"consumer\"\nversion = \"0.1.0\"\n\n[dependencies]\n"
                                      "\"probe770.glib\" = {{ git = \"file://{}\", branch = \"main\" }}\n\n"
                                      "[targets.consumer]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n",
                                      repo.dir().generic_string())},
            {"src/main.cpp", "int g_v();\nint main() { return g_v() == 1 ? 0 : 1; }\n"},
        });
        r = consumer.mcpp({"build"}, cdb_at_root);
        expect(fatal(e2e::succeeded(r))) << "D: consumer of a git-hosted member: " << s::text(r);
        const auto g = entry(consumer.at("compile_commands.json"), "glib/g.cpp");
        expect(fatal(occurrences(g, "-DREPO_FLAG=1") == 1u)) << "D: -DREPO_FLAG=1 occurs " << occurrences(g, "-DREPO_FLAG=1") << " times in the git member's g.cpp";
    };

    "772 a published member is self contained"_test[s::gcc] = [] {
        // `publish` of a member writes the workspace layer into the archived manifest and keeps
        // the original as mcpp.toml.orig, reproducibly; a consumer of the unpacked archive (and
        // a released client, when $MCPP_BOOT names one) builds it; a versioned sibling edge is
        // published as a version edge, a path-only one is refused naming the version to add;
        // `emit xpkg` points at the archived manifest; a package that needs no normalisation is
        // archived as `git archive HEAD`.
        const auto tmp = s::case_dir();
        s::project repo(tmp / "repo", {
            {"mcpp.toml", R"([workspace]
members = ["util", "lib", "edgeless"]

[workspace.package]
version = "0.3.0"
license = "MIT"
repo    = "https://github.com/example/probe"

[workspace.build]
cxxflags = ["-DWS_FLAG=1"]
)"},
            {"util/mcpp.toml", "[package]\nnamespace = \"probe\"\nname = \"util\"\n\n[targets.probe_util]\nkind = \"lib\"\n"},
            {"util/src/util.cppm", "module;\n#ifndef WS_FLAG\n#error \"WS_FLAG missing: the published manifest lost [workspace.build]\"\n#endif\n"
                                   "export module probe.util;\nexport int util_value() { return WS_FLAG + 41; }\n"},
            {"lib/mcpp.toml", "[package]\nnamespace = \"probe\"\nname = \"lib\"\n\n[dependencies]\n\"probe.util\" = { path = \"../util\", version = \"0.3.0\" }\n\n[targets.probe_lib]\nkind = \"lib\"\n"},
            {"lib/src/lib.cppm", "export module probe.lib;\nimport probe.util;\nexport int lib_value() { return util_value(); }\n"},
            {"edgeless/mcpp.toml", "[package]\nnamespace = \"probe\"\nname = \"edgeless\"\n\n[dependencies]\n\"probe.util\" = { path = \"../util\" }\n\n[targets.probe_edgeless]\nkind = \"lib\"\n"},
            {"edgeless/src/edgeless.cppm", "export module probe.edgeless;\nexport int edgeless_value() { return 1; }\n"},
        });
        commit_all(repo.dir());

        auto r = repo.mcpp_in("util", {"publish", "--dry-run", "--allow-dirty"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "publish of a member that omits version failed: " << s::text(r);
        const auto archive = repo.at("util/target/dist/probe.util-0.3.0.tar.gz");
        expect(fatal(fs::is_regular_file(archive))) << "no archive at " << archive << ": " << s::text(r);
        const auto members = s::tar_list(archive);
        expect(fatal(std::ranges::find(members, "probe.util-0.3.0/mcpp.toml.orig") != members.end())) << "the archive does not keep the original manifest";
        const auto util_toml = archived(archive, "probe.util-0.3.0/mcpp.toml");
        for (std::string_view want : {R"(version = "0.3.0")", R"(license = "MIT")", R"(cxxflags = ["-DWS_FLAG=1"])"})
            expect(fatal(e2e::contains(util_toml, want))) << "the archived mcpp.toml lacks: " << want << "\n" << util_toml;
        expect(fatal(archived(archive, "probe.util-0.3.0/mcpp.toml.orig") == repo.read("util/mcpp.toml"))) << "mcpp.toml.orig is not the file as written";
        const auto sha1 = s::sha256(s::read_file(archive));
        fs::remove_all(repo.at("util/target"));
        r = repo.mcpp_in("util", {"publish", "--dry-run", "--allow-dirty"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "second publish failed: " << s::text(r);
        const auto sha2 = s::sha256(s::read_file(archive));
        expect(fatal(sha1 == sha2)) << "two runs produced different archives (" << sha1 << " vs " << sha2 << ")";

        fs::create_directories(tmp / "unpacked");
        s::tar_extract(archive, tmp / "unpacked");
        s::project app(tmp / "app", {
            {"mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\n"
                                      "\"probe.util\" = {{ path = \"{}\" }}\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n",
                                      (tmp / "unpacked/probe.util-0.3.0").generic_string())},
            {"src/main.cpp", "import std;\nimport probe.util;\nint main() { std::println(\"value={}\", util_value()); return util_value() == 42 ? 0 : 1; }\n"},
        });
        r = app.mcpp({"run"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "a consumer of the published archive does not build: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "value=42"))) << "wrong value: " << s::text(r);

        // B: a released client builds the normalised archive, when $MCPP_BOOT names one.
        const auto boot = s::env("MCPP_BOOT");
        std::string boot_ver;
        if (boot && fs::is_regular_file(*boot)) boot_ver = s::run(*boot, {"--version"}).out;
        const auto new_ver = s::mcpp({"--version"}, tmp).out;
        if (std::regex_search(boot_ver, std::regex{"^mcpp [0-9]"}) && boot_ver != new_ver) {
            fs::remove_all(app.at("target"));
            e2e::command old{.argv = {*boot, "run"}, .cwd = app.dir(), .env = {{"MCPP_HOME", s::case_home(s::home::inherited).string()}},
                             .timeout = std::chrono::minutes{10}};
            auto o = e2e::run(old);
            expect(fatal(e2e::succeeded(o))) << boot_ver << " cannot build the normalised archive: " << s::text(o);
            expect(fatal(e2e::contains(s::text(o), "value=42"))) << "old client: wrong value: " << s::text(o);
        }

        r = repo.mcpp_in("lib", {"publish", "--dry-run", "--allow-dirty"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "publish of a member with a versioned sibling edge failed: " << s::text(r);
        const auto lib_toml = archived(repo.at("lib/target/dist/probe.lib-0.3.0.tar.gz"), "probe.lib-0.3.0/mcpp.toml");
        expect(fatal(e2e::contains(lib_toml, R"("probe.util" = { version = "0.3.0" })"))) << "the sibling edge is not a version edge:\n" << lib_toml;
        expect(fatal(s::lacks(lib_toml, "path"))) << "a path survived";
        r = repo.mcpp_in("lib", {"emit", "xpkg"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "emit xpkg: " << s::text(r);
        // mcpp#960: the descriptor points at the archived manifest, which carries the sibling's
        // version edge (checked above); it states no dependency of its own.
        expect(fatal(e2e::contains(s::text(r), R"(mcpp = "*/mcpp.toml")"))) << "the descriptor does not point at the manifest: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), R"(["probe.util"])"))) << "the descriptor states a dependency beside its manifest: " << s::text(r);
        r = repo.mcpp_in("edgeless", {"publish", "--dry-run", "--allow-dirty"}, inherited);
        // mcpp#947 (1): a sibling edge that states no version takes the sibling's own.
        expect(fatal(e2e::succeeded(r))) << "a path-only sibling edge was not published under the sibling's version: " << s::text(r);
        const auto edgeless_toml = archived(repo.at("edgeless/target/dist/probe.edgeless-0.3.0.tar.gz"), "probe.edgeless-0.3.0/mcpp.toml");
        expect(fatal(e2e::contains(edgeless_toml, R"(version = "0.3.0")") && s::lacks(edgeless_toml, "path")))
            << "the path-only sibling edge is not a version edge:\n" << edgeless_toml;

        // mcpp#947 (2)-(5): every member in one run, into the workspace's dist: archives by
        // identity, a descriptor of the index's shape each, the checksums, and the tag.
        r = repo.mcpp({"publish", "--workspace", "--dry-run", "--allow-dirty", "--tag", "probe-{version}"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "publish --workspace failed: " << s::text(r);
        for (std::string_view f : {"probe.util-0.3.0.tar.gz", "probe.lib-0.3.0.tar.gz", "probe.edgeless-0.3.0.tar.gz",
                                   "probe.util.lua", "probe.lib.lua", "probe.edgeless.lua", "SHA256SUMS"})
            expect(fatal(repo.exists(std::format("target/dist/{}", f)))) << "publish --workspace did not write " << f << ": " << s::text(r);
        const auto sums = repo.read("target/dist/SHA256SUMS");
        expect(fatal(s::count_lines(sums, "tar.gz") == 3u)) << "SHA256SUMS does not list the three archives:\n" << sums;
        expect(fatal(e2e::contains(sums, s::sha256(s::read_file(repo.at("target/dist/probe.util-0.3.0.tar.gz"))))))
            << "SHA256SUMS does not state an archive's checksum:\n" << sums;
        const auto lua = repo.read("target/dist/probe.util.lua");
        expect(fatal(e2e::contains(lua, R"(namespace = "probe")") && e2e::contains(lua, R"(    name = "util")")))
            << "the descriptor is not namespace and one-segment name:\n" << lua;
        expect(fatal(e2e::contains(lua, "/releases/download/probe-0.3.0/probe.util-0.3.0.tar.gz")))
            << "the descriptor's url does not carry the tag and the identity:\n" << lua;
        r = repo.mcpp({"publish", "-p", "probe.util", "--dry-run", "--allow-dirty", "--tag", "{verison}"}, inherited);
        expect(fatal(failed(r) && e2e::contains(s::text(r), "is not a placeholder"))) << "an unknown placeholder was accepted: " << s::text(r);
        r = repo.mcpp_in("util", {"emit", "xpkg"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "emit xpkg in a member that omits version: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "['0.3.0']"))) << "descriptor version: " << s::text(r);

        // A package that needs no normalisation is archived as `git archive HEAD`.
        s::project solo(tmp / "solo", {
            {"mcpp.toml", "[package]\nname    = \"solo\"\nversion = \"1.0.0\"\nrepo    = \"https://github.com/example/solo\"\n\n[targets.solo]\nkind = \"lib\"\n"},
            {"src/solo.cppm", "export module solo; export int solo_v() { return 1; }\n"},
        });
        commit_all(solo.dir());
        r = solo.mcpp({"publish", "--dry-run", "--allow-dirty"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "publish outside a workspace: " << s::text(r);
        git(solo.dir(), {"archive", "--format=tar.gz", "--prefix=solo-1.0.0/", "-o", (tmp / "direct.tar.gz").string(), "HEAD"});
        expect(fatal(s::read_file(tmp / "direct.tar.gz") == s::read_file(solo.at("target/dist/solo-1.0.0.tar.gz"))))
            << "a package that needs no normalisation is not archived as git archive HEAD";
        expect(fatal(s::lacks(s::text(r), "Manifest"))) << "an unchanged manifest was reported as normalised: " << s::text(r);
    };

    "773 commands outside the build read the effective manifest"_test[s::gcc] = [] {
        // `toolchain list` in a member without [toolchain] reports the workspace's (the root and
        // a member with its own are the controls); `pack` in a member that leaves version to the
        // workspace is not refused for it.
        s::project ws(s::case_dir() / "ws", {
            {"mcpp.toml", "[workspace]\nmembers = [\"app\", \"pinned\"]\n\n[workspace.package]\nversion = \"0.2.0\"\n\n[toolchain]\ndefault = \"gcc@16.1.0\"\n"},
            {"app/mcpp.toml", "[package]\nname = \"app\"\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
            {"app/src/main.cpp", "int main() { return 0; }\n"},
            {"pinned/mcpp.toml", "[package]\nname = \"pinned\"\nversion = \"0.2.0\"\n\n[toolchain]\ndefault = \"gcc@16.1.0\"\n\n[targets.pinned]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
            {"pinned/src/main.cpp", "int main() { return 0; }\n"},
        });
        constexpr std::string_view legend = "effective toolchain from project mcpp.toml";
        const auto root = s::text(ws.mcpp({"toolchain", "list"}, inherited));
        // A home with no toolchain payload visible lists none; the assertions need one.
        if (!e2e::contains(root, "no toolchains installed")) {
            expect(fatal(e2e::contains(root, legend))) << "control: the workspace root's [toolchain] is not reported: " << root;
            const auto pinned = s::text(ws.mcpp_in("pinned", {"toolchain", "list"}, inherited));
            expect(fatal(e2e::contains(pinned, legend))) << "control: a member's own [toolchain] is not reported: " << pinned;
            const auto app = s::text(ws.mcpp_in("app", {"toolchain", "list"}, inherited));
            expect(fatal(e2e::contains(app, legend))) << "a member without [toolchain] does not report the workspace's: " << app;
        }
        const auto pack = s::text(ws.mcpp_in("app", {"pack"}, inherited));
        expect(fatal(s::lacks(pack, "missing required field 'package.version'"))) << "pack in a member that leaves version to the workspace was refused: " << pack;
    };

    "774 an index member inherits its archive workspace"_test[s::python3] = [] {
        // A package whose descriptor names a member inside a workspace archive
        // (`mcpp = "*/libs/wlib/mcpp.toml"`) receives the archive's [workspace.build], once.
        const auto home = s::case_home(s::home::inherited);
        const auto x = home / "registry/data/xpkgs/probe774-x-wlib/1.0.0/wrepo-1.0.0";
        s::project archive(x, {
            {"mcpp.toml", "[workspace]\nmembers = [\"libs/wlib\"]\n\n[workspace.package]\nversion = \"1.0.0\"\n\n[workspace.build]\ndefines = [\"REPO_DEF=4\"]\n"},
            {"libs/wlib/mcpp.toml", "[package]\nnamespace = \"probe774\"\nname = \"wlib\"\n\n[targets.wlib]\nkind = \"lib\"\n\n[build]\nsources = [\"w.cpp\"]\n"},
            {"libs/wlib/w.cpp", "#ifndef REPO_DEF\n#error \"the archive workspace did not reach its member\"\n#endif\nint w_v() { return REPO_DEF; }\n"},
        });
        constexpr std::string_view zero = "0000000000000000000000000000000000000000000000000000000000000000";
        s::project idx(s::case_dir() / "idx", {
            {"pkgs/p/probe774.wlib.lua", std::format(R"(package = {{
    spec = "1",
    namespace = "probe774",
    name = "probe774.wlib",
    description = "Form A member inside a workspace archive",
    licenses = {{"MIT"}},
    type = "package",
    xpm = {{
        linux   = {{ ["1.0.0"] = {{ url = "https://example.invalid/wrepo-1.0.0.tar.gz", sha256 = "{0}" }} }},
        macosx  = {{ ["1.0.0"] = {{ url = "https://example.invalid/wrepo-1.0.0.tar.gz", sha256 = "{0}" }} }},
        windows = {{ ["1.0.0"] = {{ url = "https://example.invalid/wrepo-1.0.0.tar.gz", sha256 = "{0}" }} }},
    }},
    mcpp = "*/libs/wlib/mcpp.toml",
}}
)", zero)},
        });
        s::project app(s::case_dir() / "app", {
            {"mcpp.toml", "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n[indices]\nprobe774 = { path = \"../idx\" }\n\n"
                          "[dependencies.probe774]\nwlib = \"1.0.0\"\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
            {"src/main.cpp", "int w_v();\nint main() { return w_v() == 4 ? 0 : 1; }\n"},
        });
        const s::options o{.where = s::home::inherited, .env = {{"MCPP_COMPILE_COMMANDS", "compile_commands.json"}}};
        auto r = app.mcpp({"run"}, o);
        expect(fatal(e2e::succeeded(r))) << "the member inside the archive did not build or run: " << s::text(r);
        const auto w = entry(app.at("compile_commands.json"), "libs/wlib/w.cpp");
        expect(fatal(occurrences(w, "-DREPO_DEF=4") == 1u)) << "-DREPO_DEF=4 occurs " << occurrences(w, "-DREPO_DEF=4") << " times in the member's unit";
    };

    "805 rooted workspace reaches its own path dependency"_test[s::gcc][s::python3] = [] {
        // A rooted workspace ([package] beside [workspace]) whose root depends on a member by
        // path: the member gets its version from [workspace.package], its `workspace = true`
        // dependency the workspace's pinned (not latest) version, the workspace's cxxflags, and
        // the root's [toolchain] (the same compiler as the root's unit).
        const auto gcc = s::newest_payload("xim-x-gcc");
        expect(fatal(gcc.has_value())) << "no gcc payload found to pin [toolchain]";
        s::project ws(s::case_dir() / "ws", {
            {"local-index/pkgs/f/fx.pinned.lua", R"lua(package = {
    spec = "1",
    namespace = "fx",
    name = "fx.pinned",
    description = "fixture package pinned to a non-latest version (#725)",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux = {
            ["0.0.1"] = {
                url = "https://example.invalid/fx-pinned-0.0.1.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
            ["0.0.2"] = {
                url = "https://example.invalid/fx-pinned-0.0.2.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
    },
    mcpp = {
        language = "c++23",
        import_std = false,
        sources = { "src/pinned.cppm" },
        targets = { ["pinned"] = { kind = "lib" } },
        deps = {},
    },
}
)lua"},
            {".mcpp/.xlings/data/xpkgs/fx.pinned/0.0.1/src/pinned.cppm", "export module pinned;\nexport int pinned_value() { return 1; }\n"},
            {"mcpp.toml", std::format(R"([package]
name    = "ws"
version = "0.1.0"

[workspace]
members = ["a"]

[workspace.package]
version = "0.2.0"

[workspace.dependencies.fx]
pinned = "0.0.1"

[workspace.build]
cxxflags = ["-DWS_FLAG=1"]

[indices]
fx = {{ path = "local-index" }}

[toolchain]
default = "gcc@{}"

[dependencies]
a = {{ path = "a" }}

[targets.ws]
kind = "bin"
main = "src/main.cpp"
)", *gcc)},
            {"src/main.cpp", "import a;\nint main() { return a_value() == 2 ? 0 : 1; }\n"},
            {"a/mcpp.toml", "[package]\nname = \"a\"\n\n[dependencies.fx]\npinned = { workspace = true }\n\n[targets.a]\nkind = \"lib\"\n\n[build]\nsources = [\"src/a.cppm\"]\n"},
            {"a/src/a.cppm", "export module a;\nimport pinned;\n#if !defined(WS_FLAG)\n#error \"a.cppm: [workspace.build] cxxflags did not reach this member (#725)\"\n#endif\n"
                             "export int a_value() { return pinned_value() + WS_FLAG; }\n"},
        });
        const s::options o{.where = s::home::inherited, .env = {{"MCPP_COMPILE_COMMANDS", "compile_commands.json"}}};
        auto r = ws.mcpp({"build"}, o);
        expect(fatal(e2e::succeeded(r))) << "a rooted workspace's own path dependency did not build: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), "missing required field 'package.version'"))) << "a's omitted package.version was not supplied by [workspace.package]";
        const auto lock = ws.read("mcpp.lock");
        expect(fatal(e2e::contains(lock, R"(version = "0.0.1")"))) << "mcpp.lock does not record the pinned version 0.0.1:\n" << lock;
        expect(fatal(s::lacks(lock, R"(version = "0.0.2")"))) << "mcpp.lock records 0.0.2 -- the pin (not \"latest\") should have won";
        const auto a = entry(ws.at("compile_commands.json"), "a/src/a.cppm");
        const auto root = entry(ws.at("compile_commands.json"), "src/main.cpp");
        expect(fatal(occurrences(a, "-DWS_FLAG=1") >= 1u)) << "[workspace.build] cxxflags did not reach a.cppm's compile command";
        expect(fatal(!a.empty() && !root.empty() && a[0] == root[0])) << "member 'a' and the root were not compiled with the same toolchain";
        expect(fatal(e2e::contains(a[0], *gcc))) << "member 'a' was not compiled with the workspace's [toolchain] gcc@" << *gcc << " (" << a[0] << ")";
        r = ws.mcpp({"run"}, o);
        expect(fatal(e2e::succeeded(r))) << "the program did not return 0 (pinned_value() + WS_FLAG != 2): " << s::text(r);
    };

    "1115 a package has one command line under every member"_test = [] {
        // mcpp#945. Members of a workspace share a build directory, and a package's units were
        // given its dependencies' include directories in the order the ROOT's graph was walked:
        // under one member `-Imid -Ized -Ia`, under another `-Imid -Ia -Ized` (what `mid` carries
        // of `a` had or had not arrived when `shared` read it). The set was the same,
        // the command line was not, and every member compiled the package again. Built for one
        // member, then for another whose graph reaches the dependencies in another order, the
        // package's unit is compiled once.
        const auto tmp = s::case_dir();
        const auto lib = [](std::string_view name, std::string_view deps) {
            return std::format("[package]\nnamespace = \"ord\"\nname = \"{0}\"\nversion = \"0.1.0\"\n\n[build]\ninclude_dirs = [\"inc\"]\n"
                               "{1}\n[targets.ord_{0}]\nkind = \"lib\"\n", name, deps);
        };
        const auto bin = [](std::string_view name, std::string_view deps) {
            return std::format("[package]\nnamespace = \"ord\"\nname = \"{0}\"\nversion = \"0.1.0\"\n\n{1}\n[targets.{0}]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", name, deps);
        };
        const std::string main_cpp = "import ord.shared;\nint main() { return shared_value() == 3 ? 0 : 1; }\n";
        s::project ws(tmp / "ws", {
            {"mcpp.toml", "[workspace]\nmembers = [\"a\", \"zed\", \"mid\", \"shared\", \"one\", \"two\"]\n"},
            {"a/mcpp.toml", lib("a", "")},
            {"a/inc/a.h", "#define ORD_A 1\n"},
            {"a/src/a.cppm", "export module ord.a;\nexport int a_value() { return 1; }\n"},
            {"zed/mcpp.toml", lib("zed", "")},
            {"zed/inc/zed.h", "#define ORD_ZED 1\n"},
            {"zed/src/zed.cppm", "export module ord.zed;\nexport int zed_value() { return 1; }\n"},
            {"mid/mcpp.toml", lib("mid", "[dependencies]\n\"ord.a\" = { path = \"../a\" }\n")},
            {"mid/inc/mid.h", "#define ORD_MID 1\n"},
            {"mid/src/mid.cppm", "export module ord.mid;\nimport ord.a;\nexport int mid_value() { return a_value(); }\n"},
            {"shared/mcpp.toml", lib("shared", "[dependencies]\n\"ord.mid\" = { path = \"../mid\" }\n\"ord.zed\" = { path = \"../zed\" }\n")},
            {"shared/inc/shared.h", "#define ORD_SHARED 1\n"},
            {"shared/src/shared.cppm", "module;\n#include \"a.h\"\n#include \"zed.h\"\n#include \"mid.h\"\nexport module ord.shared;\nimport ord.mid;\nimport ord.zed;\n"
                                       "export int shared_value() { return mid_value() + zed_value() + ORD_A; }\n"},
            {"one/mcpp.toml", bin("one", "[dependencies]\n\"ord.shared\" = { path = \"../shared\" }\n")},
            {"one/src/main.cpp", main_cpp},
            {"two/mcpp.toml", bin("two", "[dependencies]\n\"ord.mid\" = { path = \"../mid\" }\n\"ord.shared\" = { path = \"../shared\" }\n")},
            {"two/src/main.cpp", main_cpp},
        });
        auto r = ws.mcpp({"build", "-p", "one"});
        expect(fatal(e2e::succeeded(r))) << "the first member did not build: " << s::text(r);
        r = ws.mcpp({"build", "-p", "two", "--verbose"});
        expect(fatal(e2e::succeeded(r))) << "the second member did not build: " << s::text(r);
        expect(s::lacks(s::text(r), "shared.cppm")) << "A: the package was compiled again for the second member: " << s::text(r);
        r = ws.mcpp({"build", "-p", "one", "--verbose"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(s::lacks(s::text(r), "shared.cppm") && s::lacks(s::text(r), "mid.cppm")) << "B: back at the first member a package was compiled again: " << s::text(r);
    };
    "953 a package compiled under two feature sets keeps both instances"_test[s::gcc][s::elf] = [] {
        // mcpp#953 D5, design 2026-11-19 §11.1. `mcpp test` enables a feature of a shared package
        // that `mcpp build` does not. Each instance has its own directory, `obj/<package>/<digest>/`,
        // so alternating the two commands compiles each instance once (A). `clean --report` lists
        // the instances (B); `clean --stale --older-than 0` removes the ones the last plan did not
        // use and keeps the others (C).
        const auto tmp = s::case_dir();
        s::project ws(tmp / "ws", {
            {"mcpp.toml", "[workspace]\nmembers = [\"util\", \"core\", \"app\"]\n"},
            {"util/mcpp.toml", "[package]\nname = \"util\"\nversion = \"0.1.0\"\n\n[features]\nextra = { defines = [\"UTIL_EXTRA=1\"] }\n\n[targets.util]\nkind = \"lib\"\n"},
            {"util/src/util.cppm", "export module ws953.util;\nexport int util_value() {\n#ifdef UTIL_EXTRA\n  return 2;\n#else\n  return 1;\n#endif\n}\n"},
            {"core/mcpp.toml", "[package]\nname = \"core\"\nversion = \"0.1.0\"\n\n[dependencies]\nutil = { path = \"../util\" }\n\n[targets.core]\nkind = \"lib\"\n"},
            {"core/src/core.cppm", "export module ws953.core;\nimport ws953.util;\nexport int core_value() { return util_value() + 10; }\n"},
            {"app/mcpp.toml", "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\ncore = { path = \"../core\" }\n\n"
                              "[dev-dependencies]\nutil = { path = \"../util\", features = [\"extra\"] }\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
            {"app/src/main.cpp", "import ws953.core;\nint main() { return core_value() == 11 ? 0 : 1; }\n"},
            {"app/tests/t.cpp", "import ws953.core;\nint main() { return core_value() == 12 ? 0 : 1; }\n"},
        });
        auto stamps = [&] {
            std::vector<std::pair<std::string, long long>> out;
            for (auto const& f : s::find_files(ws.at("target"), R"(util\.m\.o)"))
                out.emplace_back(f.generic_string(), fs::last_write_time(f).time_since_epoch().count());
            std::ranges::sort(out);
            return out;
        };
        expect(fatal(e2e::succeeded(ws.mcpp({"build", "-p", "app"})))) << "A: the first build failed";
        auto r = ws.mcpp({"test", "-p", "app"});
        expect(fatal(e2e::succeeded(r))) << "A: the first test failed (the feature did not reach the test build): " << s::text(r);
        const auto before = stamps();
        expect(fatal(before.size() == 2u)) << "A: the shared package does not have two instances: " << before.size();
        for (int round = 0; round < 2; ++round) {
            expect(fatal(e2e::succeeded(ws.mcpp({"build", "-p", "app"})))) << "A: a build failed";
            expect(fatal(e2e::succeeded(ws.mcpp({"test", "-p", "app"})))) << "A: a test failed";
        }
        expect(stamps() == before) << "A: alternating build and test compiled an instance again";
        r = ws.mcpp({"clean", "--report"});
        expect(fatal(e2e::succeeded(r))) << "B: clean --report failed: " << s::text(r);
        expect(s::count_lines(s::text(r), "obj/util/") == 2u) << "B: the report does not list both instances: " << s::text(r);
        r = ws.mcpp({"clean", "--stale", "--older-than", "0"});
        expect(fatal(e2e::succeeded(r))) << "C: clean --stale failed: " << s::text(r);
        expect(stamps().size() == 1u) << "C: the instance the last plan did not use was kept, or the used one removed: " << s::text(r);
        expect(fatal(e2e::succeeded(ws.mcpp({"test", "-p", "app"})))) << "C: the build after the sweep failed";
    };

    "954 the workspace's abi table is the root's statement"_test[s::gcc][s::elf] = [] {
        // mcpp#954. A workspace whose root is only `[workspace]` states the build's abi once, in
        // `[workspace.target.<selector>.abi]`; its members inherit the row and no member is told
        // that it declares a table only the root decides (A). A member whose own manifest has the
        // table is reported once, by name, when another member depends on it (B).
        const auto tmp = s::case_dir();
        const auto lib = [](std::string_view own) {
            return std::format("[package]\nname = \"core\"\nversion = \"0.1.0\"\n\n{}[targets.core]\nkind = \"lib\"\n", own);
        };
        const std::string app = "[package]\nname = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\ncore = { path = \"../core\" }\n\n"
                                "[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n";
        s::project ws(tmp / "ws", {
            {"mcpp.toml", "[workspace]\nmembers = [\"core\", \"app\"]\n\n[workspace.target.'cfg(os = \"linux\")'.abi]\nthreads = true\n"},
            {"core/mcpp.toml", lib("")},
            {"core/src/core.cppm", "export module ws954.core;\nexport int core_value() { return 3; }\n"},
            {"app/mcpp.toml", app},
            {"app/src/main.cpp", "import ws954.core;\nint main() { return core_value() == 3 ? 0 : 1; }\n"},
        });
        auto r = ws.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "A: the workspace did not build: " << s::text(r);
        expect(s::lacks(s::text(r), "declares [target.<selector>.abi]")) << "A: an inherited abi row was reported as a member's own table: " << s::text(r);
        ws.write("core/mcpp.toml", lib("[target.'cfg(os = \"linux\")'.abi]\nthreads = true\n\n"));
        r = ws.mcpp({"build", "-p", "app"});
        expect(fatal(e2e::succeeded(r))) << "B: the workspace did not build: " << s::text(r);
        const auto text = s::text(r);
        std::size_t n = 0;
        for (auto pos = text.find("`core` declares [target.<selector>.abi]"); pos != std::string::npos;
             pos = text.find("`core` declares [target.<selector>.abi]", pos + 1))
            ++n;
        expect(n == 1u) << "B: the member's own abi table was reported " << n << " times: " << text;
        expect(s::lacks(text, "`app` declares")) << "B: the member without a table was reported: " << text;
    };
}};

}  // namespace

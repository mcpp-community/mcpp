// A project's inputs carried to a machine without a network (design 2026-11-21, offline and
// toolchain defaults §3.2, F1 and F2): `mcpp fetch --to` writes what the build used -- the
// toolchain's closure, an index package, a git dependency and a build program's git dependency --
// into a directory; on a fresh home, offline, `mcpp fetch --from` takes it in, and the build and
// the program run. Where the machine gives a process a network namespace of its own
// (`unshare -rn`), the offline runs are in one, so no download can succeed whatever the engine
// decides; where it does not (Ubuntu 24.04 restricts unprivileged namespaces), the engine's
// offline mode is the fence, and ci-offline.yml builds the repository in a namespace. What the
// offline run lacks is refused by its cause, and a bundle older than the lock is refused before
// anything is installed.
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

// A git repository at `dir` holding a library package `name` whose module `name` exports
// `<name>_v()`, tagged `v1`.
void library_repository(const fs::path& git, const fs::path& dir, std::string_view name, int value) {
    s::project lib(dir, {});
    lib.write("mcpp.toml", std::format("[package]\nname = \"{0}\"\nversion = \"0.1.0\"\n\n[targets.{0}]\nkind = \"lib\"\n", name));
    lib.write(std::format("src/{}.cppm", name),
              std::format("export module {0};\nexport int {0}_v() {{ return {1}; }}\n", name, value));
    for (std::vector<std::string> args : {std::vector<std::string>{"init", "-q"}, {"add", "-A"},
                                          {"-c", "user.name=e2e", "-c", "user.email=e2e@localhost", "commit", "-qm", "v1"},
                                          {"tag", "v1"}}) {
        auto r = s::run(git, args, dir);
        expect(fatal(e2e::succeeded(r))) << "git " << args.front() << " failed: " << s::text(r);
    }
}

E2E bundle{"bundle", s::home_fixture, [] {
    "990 a project's inputs are carried to a machine without a network"_test[s::gcc] = [] {
        const auto git = s::on_path("git");
        auto unshare = s::on_path("unshare");
        if (!git) skip("990 -- no git on PATH");
        if (unshare && !e2e::succeeded(s::run(*unshare, {"-rn", "true"}))) unshare.reset();
        const auto xlings = s::shared_home() / "registry" / "bin" / "xlings";
        if (!e2e::contains(s::run(xlings, {"download", "--help"}).out + s::run(xlings, {"download", "--help"}).err, "--to"))
            skip("990 -- the shared home's xlings writes no bundle (xlings 2026.11.21.1 does)");

        const auto tmp = s::case_dir();
        library_repository(*git, tmp / "gd", "gd", 7);
        library_repository(*git, tmp / "gb", "gb", 5);
        const auto manifest = std::format(
            "[package]\nname = \"carried\"\nversion = \"0.1.0\"\n\n"
            "[toolchain]\ndefault = \"gcc@{}\"\n\n"
            "[dependencies]\nmcpplibs.cmdline = \"0.0.5\"\ngd = {{ git = \"file://{}\", tag = \"v1\" }}\n\n"
            "[build-dependencies]\ngb = {{ git = \"file://{}\", tag = \"v1\" }}\n",
            s::gcc_version(), (tmp / "gd").generic_string(), (tmp / "gb").generic_string());
        const std::string main_cpp = "import std;\nimport gd;\nint main() { std::println(\"carried {} {}\", gd_v(), CARRIED_GB); }\n";
        const std::string build_mcpp = "import std;\nimport mcpp;\nimport gb;\n"
                                       "int main() { mcpp::define(std::format(\"CARRIED_GB={}\", gb_v()).c_str()); return 0; }\n";
        s::project p(tmp / "p", {});
        p.write("mcpp.toml", manifest);
        p.write("src/main.cpp", main_cpp);
        p.write("build.mcpp", build_mcpp);

        // F1. Written where the payloads are, then the sources are gone.
        const auto dir = tmp / "bundle";
        auto r = p.mcpp({"fetch", "--to", dir.string()});
        expect(fatal(e2e::succeeded(r))) << "mcpp fetch --to failed\n" << s::text(r);
        expect(fatal(s::has_line(s::text(r), R"(^ *Fetched [0-9]+ packages, 2 git clones \()"))) << "no summary of what was written\n" << s::text(r);
        const auto manifestText = s::read_file(dir / "bundle.toml");
        for (auto item : {"[mcpp]", "mcpplibs:cmdline@0.0.5", "[[git]]", "[[index]]", "[[archive]]"})
            expect(e2e::contains(manifestText, item)) << "bundle.toml lacks " << item << "\n" << manifestText;
        fs::rename(tmp / "gd", tmp / "gd.gone");
        fs::rename(tmp / "gb", tmp / "gb.gone");

        // A fresh home, its xlings the shared home's (as a release ships one), offline and, where
        // the machine allows it, in a network namespace of its own.
        const auto home = s::another_home(s::home::fresh);
        s::project q(tmp / "q", {});
        q.write("mcpp.toml", manifest);
        q.write("src/main.cpp", main_cpp);
        q.write("build.mcpp", build_mcpp);
        q.write("mcpp.lock", p.read("mcpp.lock"));
        const s::options there{.env = {{"MCPP_HOME", home.string()}, {"MCPP_VENDORED_XLINGS", xlings.string()},
                                       {"MCPP_OFFLINE", std::nullopt}, {"MCPP_ARCHIVES", std::nullopt}}};
        auto without_network = [&](std::vector<std::string> args, s::options o) {
            auto c = s::mcpp_command(std::move(args), q.dir(), o);
            if (unshare) c.argv.insert(c.argv.begin(), {unshare->string(), "-rn"});
            return s::as_text(e2e::run(c));
        };
        r = without_network({"fetch", "--from", dir.string(), "--offline"}, there);
        expect(fatal(e2e::succeeded(r))) << "mcpp fetch --from failed without a network\n" << s::text(r);
        expect(fatal(s::has_line(s::text(r), R"(^ *Imported [0-9]+ packages, 2 git clones)"))) << "no summary of what was imported\n" << s::text(r);
        r = without_network({"run", "--offline"}, there);
        expect(fatal(e2e::succeeded(r))) << "the build without a network failed\n" << s::text(r);
        expect(fatal(e2e::contains(r.out, "carried 7 5"))) << "the program did not run with both dependencies\n" << s::text(r);

        // F2. Offline, a payload no local source holds: without an archive directory nothing is
        // asked; with one, the sources are asked and the refusal says none holds it.
        r = without_network({"build", "--offline", "--toolchain", "gcc@15.1.0"}, there);
        expect(fatal(r.started && r.status == 5)) << "an offline build that needs a download did not stop at the denied effect\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "no archive directory is named"))) << "the refusal does not say no source was named\n" << s::text(r);
        auto withArchives = there;
        withArchives.env.push_back({"MCPP_ARCHIVES", dir.string()});
        r = without_network({"build", "--offline", "--toolchain", "gcc@15.1.0"}, withArchives);
        expect(fatal(r.started && r.status == 5)) << "an offline build whose sources lack an archive was not refused\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "neither the download cache nor an archive directory")))
            << "the refusal does not say the local sources were asked\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "mcpp fetch --to"))) << "the refusal does not name the remedy\n" << s::text(r);

        // The offline key of the manifest, and its way back named.
        q.write("mcpp.toml", manifest + "\n[build]\noffline = true\n");
        r = without_network({"build", "--toolchain", "gcc@15.1.0"}, there);
        expect(fatal(failed(r))) << "[build] offline = true reached for the network\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "`offline = false` in mcpp.toml [build]"))) << "the refusal does not name the manifest's key\n" << s::text(r);
        q.write("mcpp.toml", manifest);

        // A lock the bundle is older than: refused before anything is installed.
        auto lock = q.read("mcpp.lock");
        const auto at = lock.find("version = \"0.0.5\"");
        expect(fatal(at != std::string::npos)) << "the lock records no mcpplibs.cmdline 0.0.5\n" << lock;
        lock.replace(at, std::string_view("version = \"0.0.5\"").size(), "version = \"0.0.4\"");
        q.write("mcpp.lock", lock);
        r = without_network({"fetch", "--from", dir.string(), "--offline"}, there);
        expect(fatal(failed(r))) << "a bundle older than the lock was taken\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "does not carry what mcpp.lock records"))) << "the refusal does not name the lock\n" << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "0.0.4"))) << "the refusal does not name the version\n" << s::text(r);
    };
}};

}  // namespace

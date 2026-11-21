// The commands and flags of the surface that no other case reaches (design 2026-11-20 §5.16): each
// is run here once, in a home of the case, and answers as its help states. The surface's ledger
// (tests/e2e/mtest.toml) counts an item as covered when a passing case used it; these were listed
// as missing by the first run that kept the ledger.
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

E2E surface{"surface", s::home_fixture, [] {
    "1125 the maintenance commands answer"_test = [] {
        s::own_home();
        const auto dir = s::case_dir();
        auto run = [&](std::vector<std::string> args) {
            auto r = s::mcpp(std::move(args), dir);
            expect(fatal(e2e::succeeded(r))) << s::text(r);
            return s::text(r);
        };
        for (auto args : {std::vector<std::string>{"version"}, std::vector<std::string>{"self", "version"}})
            expect(s::has_line(run(args), R"(^mcpp [0-9]+\.[0-9]+)")) << "no version line";
        for (auto args : {std::vector<std::string>{"explain", "E0001"}, std::vector<std::string>{"self", "explain", "E0001"}})
            expect(e2e::contains(run(args), "E0001: dependency name mismatch"));
        // A registry added is listed until it is removed.
        expect(e2e::contains(run({"index", "add", "acme1125", "https://example.invalid/acme-index.git"}), "Added registry `acme1125`"));
        expect(s::has_line(run({"index", "list"}), R"(^  acme1125 +https://example\.invalid/acme-index\.git)")) << "an added registry is not listed";
        expect(e2e::contains(run({"index", "remove", "acme1125"}), "Removed registry `acme1125`"));
        expect(s::lacks(run({"index", "list"}), "acme1125")) << "a removed registry is still listed";
        // A payload this home holds that an upstream also has is listed, and --apply replaces it
        // with a link; then nothing is left to replace. A home that installed every payload by
        // link answers so at once (a host whose provider copies one answers it after --apply).
        const auto listed = run({"self", "store", "dedupe"});
        expect(e2e::contains(listed, "no payload here is also in an upstream") || e2e::contains(listed, "could be links to an upstream's")) << listed;
        (void)run({"self", "store", "dedupe", "--apply"});
        expect(e2e::contains(run({"self", "store", "dedupe"}), "no payload here is also in an upstream")) << "--apply left a payload that an upstream has";
        expect(e2e::contains(run({"cache", "clean", "--deps"}), "Cleaned package entries"));
        expect(e2e::contains(run({"cache", "clean", "--tools"}), "Cleaned host tool entries"));
        expect(e2e::contains(run({"self", "config", "--cache-unused-days", "30"}), "cache.unused_days = 30 days"));
    };

    "1126 a test run's selection flags"_test[s::gcc] = [] {
        // `--seed` reaches the program, `--shard 1/1` runs every test, `--split none` keeps a
        // program one process, `--failed` runs what failed last, and `--frozen` builds a project with nothing to lock.
        s::own_home();
        auto p = s::project::scaffold("flags1126");
        p.write("tests/cases.cpp", R"(import std;
import mcpplibs.mtest;
using namespace mcpplibs::mtest;
UTest seeded{"seeded", [] { expect(std::getenv("MTEST_SEED") != nullptr && std::string_view(std::getenv("MTEST_SEED")) == "1126"); }};
)");
        auto r = p.mcpp({"test", "--seed", "1126"});
        expect(fatal(e2e::succeeded(r))) << "the seed did not reach the program: " << s::text(r);
        r = p.mcpp({"test", "--seed", "1126", "--shard", "1/1", "--split", "none"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(e2e::contains(s::text(r), "cases ... ok")) << "the one shard did not run every test: " << s::text(r);
        p.write("tests/broken.cpp", "int main() { return 1; }\n");
        r = p.mcpp({"test", "--seed", "1126"});
        expect(fatal(r.started && r.status != 0)) << "a failing test passed: " << s::text(r);
        r = p.mcpp({"test", "--failed", "--seed", "1126"});
        expect(fatal(r.started && r.status != 0)) << s::text(r);
        expect(e2e::contains(s::text(r), "broken ... FAIL")) << "--failed did not run the failed test";
        expect(s::lacks(s::text(r), "cases ... ok")) << "--failed ran a test that passed";
        p.remove("tests/broken.cpp");
        // A project whose only dependencies are its tests' resolves nothing a lock holds: under
        // --frozen it builds as it is.
        expect(fatal(e2e::succeeded(p.mcpp({"build"}))));
        expect(!fs::exists(p.at("mcpp.lock"))) << "a project with no dependency wrote a lock";
        r = p.mcpp({"--frozen", "build"});
        expect(e2e::succeeded(r)) << "a build with nothing to lock was refused under --frozen: " << s::text(r);
    };
}};

}  // namespace

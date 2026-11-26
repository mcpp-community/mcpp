// The outputs whose form is itself the contract, compared whole (design 2026-11-20 §5.21): the
// machine protocol a client reads before anything else (docs/50 §4) and mcpp's surface (§5.16).
// A change of either is a change of what clients and the surface's ledger read, and its golden file
// changes with it in the same pull request: `MTEST_GOLDEN=update` writes the files and prints what
// it changed, which the review reads.
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

fs::path golden_file(std::string_view name) { return s::repository() / "tests/e2e/golden" / name; }

E2E golden{"golden", s::home_fixture, [] {
    "1123 the machine protocol is the golden one"_test = [] {
        auto r = s::mcpp({"--protocol-version"}, s::case_dir());
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(e2e::golden(golden_file("protocol-version.json"), r.out,
                           {{R"("version": "[0-9][0-9.]*")", R"("version": "<version>")"}}));
    };

    "1124 the surface is the golden one"_test = [] {
        auto r = s::mcpp({"emit", "surface"}, s::case_dir());
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        expect(e2e::golden(golden_file("surface.txt"), r.out));
    };
}};

}  // namespace

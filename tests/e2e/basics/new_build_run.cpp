// mcpp new, build and run: the scaffold of a program (formerly tests/e2e/02_new_build_run.sh).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;

namespace {

E2E basics{"basics", tag{"requires:fresh-sandbox"}, [] {
    "02 a new program is scaffolded, built and run with its arguments"_test = [] {
        s::project root;
        expect(fatal(e2e::succeeded(root.mcpp({"new", "hello"}))));
        s::project p(root.at("hello"), {});
        for (auto f : {"mcpp.toml", "src/main.cpp", ".gitignore"})
            expect(p.exists(f)) << f;
        const auto main_cpp = p.read("src/main.cpp");
        expect(e2e::contains(main_cpp, "import std"));
        expect(e2e::contains(main_cpp, "std::println"));

        expect(fatal(e2e::succeeded(p.mcpp({"build"}))));
        const auto binary = p.artifact(s::exe("hello"));
        expect(fatal(binary.has_value())) << "no hello under target/";
        expect(std::filesystem::is_regular_file(*binary));

        expect(e2e::contains(s::text(p.mcpp({"run"})), "Hello from hello"));
        const auto args = s::text(p.mcpp({"run", "--", "foo", "bar"}));
        expect(e2e::contains(args, "foo"));
        expect(e2e::contains(args, "bar"));
    };
}};

}  // namespace

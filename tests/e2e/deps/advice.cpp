// What a build edge says on success: its advice, reported once, on the full and the fast path
// (formerly tests/e2e/821).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

E2E advice{"advice", [] {
    "821 an edge states its advice on both paths"_test[tag{"requires:unix-shell"}] = [] {
        // An edge that has something to say on success writes its advice file, and mcpp reports
        // it once after a successful build, by one function the full build path and the fast
        // path both call; a build in which the edge did not run reports nothing.
        s::needs({"unix-shell"});
        s::project p{{{"mcpp.toml", "[package]\nname    = \"advice821\"\nversion = \"0.1.0\"\n"},
                      {"src/main.cpp", "int main() { return 0; }\n"},
                      {"data/probe.in", "first\n"},
                      {"build.mcpp", R"cpp(import std;
import mcpp;
int main() {
    const std::string in  = std::string(mcpp::manifest_dir()) + "/data/probe.in";
    const std::string out = std::string(mcpp::out_dir()) + "/probe.stamp";
    mcpp::action a;
    a.id   = "advise";
    a.role = mcpp::roles::check;
    // The advice file of this edge, in the build directory the edge runs in.
    a.arg("sh").arg("-c")
     .arg("mkdir -p .mcpp-advice && printf 'note\\tprobe input reads %s\\n' \"$(cat \"$1\")\" > .mcpp-advice/probe.stamp.advice")
     .arg("sh").arg(in.c_str())
     .input(in.c_str())
     .output(out.c_str())
     .submit();
    return 0;
}
)cpp"}}};
        auto b1 = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(b1))) << "the first build failed";
        expect(eq(s::count_lines(s::text(b1), "probe input reads first"), std::size_t{1})) << "A1: the full path did not report the edge's advice exactly once";

        p.write(".before-b2", "");
        std::this_thread::sleep_for(std::chrono::seconds{1});  // a coarse file system clock must see the input as newer
        p.write("data/probe.in", "second\n");
        auto b2 = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(b2))) << "the second build failed";
        const auto marker = fs::last_write_time(p.at(".before-b2"));
        bool planned = false;
        for (const auto& f : s::find_files(p.at("target"), R"(build\.ninja)"))
            if (f.generic_string().find("/.build-mcpp/") == std::string::npos && fs::last_write_time(f) > marker) planned = true;
        if (planned) {
            if (s::on_linux())
                expect(fatal(false)) << "A2: the second build planned again, so the fast path was not exercised";
            else
                std::println("READING 821: the fast path declines on this system (#400); A2 reads the full path");
        }
        expect(eq(s::count_lines(s::text(b2), "probe input reads second"), std::size_t{1})) << "A2: the second build did not report the edge's advice exactly once";
        expect(s::lacks(s::text(b2), "probe input reads first")) << "A2: the second build reported the previous run's advice";

        auto b3 = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(b3))) << "the third build failed";
        expect(s::lacks(s::text(b3), "probe input reads")) << "A3: a build in which the edge did not run reported its advice";
    };
}};

}  // namespace

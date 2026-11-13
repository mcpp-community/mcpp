// Check actions: the stamp the engine writes and moves, and `blocking` (formerly tests/e2e/313,
// 315, 780).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

std::optional<fs::path> ninja_file(const fs::path& target) {
    for (const auto& f : s::find_files(target, R"(build\.ninja)"))
        if (f.generic_string().find("/.build-mcpp/") == std::string::npos) return f;
    return std::nullopt;
}

E2E checks{"checks", [] {
    "313 check stamp on every platform"_test = [] {
        // A `role = "check"` whose command never touches its stamp, on every platform: the
        // engine writes the stamp (one), and a no-op rebuild does not run the check again.
        s::project p{s::files{{"mcpp.toml", "[package]\nname    = \"stampprobe\"\nversion = \"0.1.0\"\n"},
                      {"src/main.cpp", "#include <cstdio>\n#ifndef CHECK_DECLARED\n#error \"the build program never ran\"\n#endif\nint main() { std::printf(\"STAMP_PROBE_OK\\n\"); }\n"}}};
        p.write("build.mcpp", std::format(R"cpp(import std;
import mcpp;
int main() {{
    mcpp::define("CHECK_DECLARED");
    mcpp::action a;
    a.id          = "probe";
    a.role        = "check";
    a.description = "a check whose command writes no stamp";
    // The engine itself: present on every platform this suite runs on, exits 0,
    // and touches nothing. That is exactly the shape of a real analyser.
    a.arg("{}").arg("--version")
     .output((std::string(mcpp::out_dir()) + "/probe.stamp").c_str())
     .submit();
}}
)cpp", fs::path(s::mcpp_path()).generic_string()));
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "build failed";
        expect(e2e::contains(s::last_line(p.mcpp({"run"}).out), "STAMP_PROBE_OK")) << "the program did not run";
        const auto stamps = s::find_files(p.at("target"), R"(probe\.stamp)");
        expect(eq(stamps.size(), std::size_t{1})) << "expected exactly one stamp written by the engine, found " << stamps.size();
        auto b2 = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(b2))) << "no-op rebuild failed";
        expect(s::lacks(s::text(b2), "CHECK")) << "the check re-ran with nothing changed — its stamp is not satisfying the edge";
    };

    "315 blocking check gates compilation"_test[tag{"requires:gcc"}] = [] {
        // `blocking = true` on a check orders every compile edge after it (its stamp joins the
        // package's action phony), so a failing blocking check stops the compile; a
        // non-blocking one does not.
        s::needs({"gcc"});
        s::project p{s::files{{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n"},
                      {"src/main.cpp", "#include <cstdio>\nint main(){ std::printf(\"ok\\n\"); }\n"}}};
        s::write_executable(p.at("check.sh"), "#!/usr/bin/env bash\nsleep 2\necho \"the check says no\" >&2\nexit 1\n");
        auto emit = [&](std::string_view blocking) {
            p.write("build.mcpp", std::format(R"cpp(#include <string>
import mcpp;
int main() {{
    const std::string root = mcpp::manifest_dir();
    mcpp::action a;
    a.id       = "gate";
    a.role     = "check";
    a.blocking = {};
    a.arg((root + "/check.sh").c_str())
     .output("${{mcpp.out_dir}}/gate.stamp")
     .submit();
}}
)cpp", blocking));
            p.remove("target");
        };
        auto objects = [&] { return s::find_files(p.at("target"), R"(main.*\.o)"); };
        const std::string gated = R"(^build mcpp-actions-app : phony.*gate\.stamp)";
        emit("true");
        expect(fatal(failed(p.mcpp({"build"})))) << "a failing blocking check let the build succeed";
        expect(fatal(objects().empty())) << "a failing BLOCKING check did not stop the compile — the object exists";
        const auto nj = ninja_file(p.at("target"));
        expect(fatal(nj.has_value())) << "no build.ninja";
        expect(s::has_line(s::read_file(*nj), gated)) << "the blocking check's stamp is not in the package's action phony";
        expect(s::count_matching(s::read_file(*nj), R"(^build [^:]*main[^:]*\.o *:.*\|\|.*mcpp-actions-app)") >= 1) << "the compile edge does not wait for the blocking check";
        emit("false");
        p.mcpp({"build"});  // still fails: the check still fails
        expect(fatal(!objects().empty())) << "a NON-blocking check also stopped the compile — 'blocking' is not distinguishing anything, it is just always on";
        const auto nj2 = ninja_file(p.at("target"));
        expect(fatal(nj2.has_value())) << "no build.ninja";
        expect(s::no_line(s::read_file(*nj2), gated)) << "a NON-blocking check joined the ordering phony";
    };

    "780 a passing check moves its stamp past a changed input"_test = [] {
        // A check whose command writes no stamp, after one of its inputs changed: the engine
        // moves the stamp when the command passes, so the build after does not run it again.
        s::project p{s::files{{"mcpp.toml", "[package]\nname    = \"stampmove\"\nversion = \"0.1.0\"\n"},
                      {"src/main.cpp", "#include <cstdio>\nint main() { std::printf(\"STAMP_MOVE_OK\\n\"); }\n"},
                      {"input.txt", "one\n"}}};
        p.write("build.mcpp", std::format(R"cpp(import std;
import mcpp;
int main() {{
    mcpp::action a;
    a.id          = "probe";
    a.role        = "check";
    a.description = "a check whose command writes no stamp";
    a.arg("{}").arg("--version")
     .input("{}")
     .output((std::string(mcpp::out_dir()) + "/probe.stamp").c_str())
     .submit();
}}
)cpp", fs::path(s::mcpp_path()).generic_string(), p.at("input.txt").generic_string()));
        const std::string ran = R"(__action(-stamp)? )";
        auto b1 = p.mcpp({"build", "-v"});
        expect(fatal(e2e::succeeded(b1))) << "build failed";
        expect(fatal(s::has_line(s::text(b1), ran))) << "the first build did not run the check";
        auto b2 = p.mcpp({"build", "-v"});
        expect(fatal(e2e::succeeded(b2))) << "no-op rebuild failed";
        expect(s::no_line(s::text(b2), ran)) << "the check re-ran with nothing changed";
        std::this_thread::sleep_for(std::chrono::seconds{1});
        p.write("input.txt", "two\n");
        auto b3 = p.mcpp({"build", "-v"});
        expect(fatal(e2e::succeeded(b3))) << "rebuild after the edit failed";
        expect(s::has_line(s::text(b3), ran)) << "a changed input did not re-run the check";
        auto b4 = p.mcpp({"build", "-v"});
        expect(fatal(e2e::succeeded(b4))) << "rebuild after the check failed";
        expect(s::no_line(s::text(b4), ran)) << "the check re-ran on the build after it passed -- its stamp stayed older than the input";
        expect(e2e::contains(s::last_line(p.mcpp({"run"}).out), "STAMP_MOVE_OK")) << "the program did not run";
    };
}};

}  // namespace

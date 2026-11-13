// The `prepare` role and the roles an action may name (formerly tests/e2e/790, 791, 792, 793).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

constexpr std::string_view prep_manifest = "[package]\nname    = \"prep\"\nversion = \"0.1.0\"\n\n[toolchain]\nlinux = \"gcc@16.1.0\"\n\n[targets.prep]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n";

E2E prepare{"prepare", [] {
    "790 a prepare action populates an unknown directory"_test[s::gcc] = [] {
        // A `prepare` action declares only the directory a command fills; the build reads it by
        // directory. It runs on the first build, not on an unchanged one (nor does the compile),
        // again when an input changes, and not on the build after it passed.
        s::project p{s::files{{"mcpp.toml", prep_manifest},
                      {"recipe.txt", "recipe v1\n"},
                      {"src/main.cpp", "#include <greet.h>\nint main() { return prep_greet() == 99 ? 0 : 1; }\n"},
                      {"build.mcpp", R"cpp(import mcpp;
#include <string>
int main() {
    const std::string root   = mcpp::manifest_dir();
    const std::string prefix = std::string(mcpp::out_dir()) + "/prep-install";

    mcpp::action a;
    a.id   = "prep:install";
    a.role = mcpp::roles::prepare;
    a.arg((root + "/install.sh").c_str()).arg(prefix.c_str())
     .input((root + "/install.sh").c_str())
     .input((root + "/recipe.txt").c_str())
     .output((prefix + ".stamp").c_str())
     .output_dir(prefix.c_str())
     .submit();

    mcpp::include_dir((prefix + "/include").c_str());
    mcpp::link_lib("prepgreet");
    mcpp::link_search((prefix + "/lib").c_str());
    mcpp::runtime_search_dir((prefix + "/lib").c_str());
    return 0;
}
)cpp"}}};
        s::write_executable(p.at("install.sh"), R"cpp(#!/usr/bin/env bash
set -e
PREFIX="$1"
mkdir -p "$PREFIX/include" "$PREFIX/lib"
cat > "$PREFIX/include/greet.h" <<HDR
#ifdef __cplusplus
extern "C" {
#endif
int prep_greet(void);
#ifdef __cplusplus
}
#endif
HDR
cat > "$PREFIX/lib/greet.c" <<SRC
int prep_greet(void) { return 99; }
SRC
gcc -shared -fPIC "$PREFIX/lib/greet.c" -o "$PREFIX/lib/libprepgreet.so"
)cpp");
        const std::string ran = R"(PREPARE prep:install|__action(-stamp)? .*install\.sh)";
        auto b1 = p.mcpp({"build", "-v"});
        expect(fatal(e2e::succeeded(b1))) << "first build failed";
        expect(fatal(s::has_line(s::text(b1), ran))) << "the prepare action did not run on the first build";
        expect(fatal(e2e::succeeded(p.mcpp({"run"})))) << "run failed after the first build";
        auto b2 = p.mcpp({"build", "-v"});
        expect(fatal(e2e::succeeded(b2))) << "second build failed";
        expect(s::no_line(s::text(b2), ran)) << "the prepare action re-ran on an unchanged second build";
        expect(s::no_line(s::text(b2), R"(\bmain\.o\b)")) << "the compile re-ran on an unchanged second build";
        std::this_thread::sleep_for(std::chrono::seconds{1});
        p.write("recipe.txt", "recipe v2\n");
        auto b3 = p.mcpp({"build", "-v"});
        expect(fatal(e2e::succeeded(b3))) << "third build (after the input changed) failed";
        expect(s::has_line(s::text(b3), ran)) << "the action did not re-run after its input changed";
        auto b4 = p.mcpp({"build", "-v"});
        expect(fatal(e2e::succeeded(b4))) << "fourth build failed";
        expect(s::no_line(s::text(b4), ran)) << "the action re-ran on the build after it passed -- R2's stamp rule did not hold";
        expect(e2e::succeeded(p.mcpp({"run"}))) << "run failed after the recipe changed";
    };

    "791 a prepare action that creates nothing fails"_test[s::gcc] = [] {
        // A prepare command that succeeds and leaves its directory absent or holding only its
        // stamps fails the edge naming the directory, and writes no stamp; the stamp beside the
        // directory or inside it.
        s::project p{s::files{{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n"},
                      {"src/main.cpp", "#include <cstdio>\nint main(){ std::printf(\"ok\\n\"); }\n"}}};
        for (std::string_view where : {"beside", "inside"}) {
            p.remove("target");
            p.write("build.mcpp", std::format(R"cpp(import mcpp;
#include <string>
int main() {{
    const std::string prefix = std::string(mcpp::out_dir()) + "/never-created";
    const std::string stamp = std::string("{}") == "inside"
        ? prefix + "/installed.stamp" : prefix + ".stamp";
    mcpp::action a;
    a.id   = "empty:prepare";
    a.role = mcpp::roles::prepare;
    a.arg("true").output(stamp.c_str()).output_dir(prefix.c_str()).submit();
    return 0;
}}
)cpp", where));
            auto b = p.mcpp({"build"});
            expect(fatal(failed(b))) << "(" << where << ") a prepare action whose command created nothing let the build succeed";
            expect(e2e::contains(s::text(b), "never-created")) << "(" << where << ") the failure does not name the declared directory";
            expect(s::contains_nocase(s::text(b), "does not exist or holds no file")) << "(" << where << ") the failure does not state the post-condition";
            bool stamped = false;
            for (const auto& f : s::find_files(p.at("target"), R"(.*\.stamp)"))
                stamped = stamped || f.generic_string().find("never-created") != std::string::npos;
            expect(!stamped) << "(" << where << ") a stamp was written although the directory holds nothing";
        }
    };

    "792 a rerun input inside a prepare directory is warned"_test[s::gcc] = [] {
        // A build program's re-run input inside a prepare action's directory (a file or a glob)
        // is warned about, naming the path, the directory and the package; one outside is not.
        s::project p{s::files{{"mcpp.toml", prep_manifest},
                      {"src/main.cpp", "#include <cstdio>\nint main(){ std::printf(\"ok\\n\"); }\n"}}};
        s::write_executable(p.at("install.sh"), "#!/usr/bin/env bash\nset -e\nPREFIX=\"$1\"\nmkdir -p \"$PREFIX/include\"\necho '#define GREETING 1' > \"$PREFIX/include/greet.h\"\n");
        auto build = [&](std::string_view call) {
            p.write("build.mcpp", std::format(R"cpp(import mcpp;
#include <string>
int main() {{
    const std::string root   = mcpp::manifest_dir();
    const std::string prefix = std::string(mcpp::out_dir()) + "/prep-install";

    mcpp::action a;
    a.id   = "prep:install";
    a.role = mcpp::roles::prepare;
    a.arg((root + "/install.sh").c_str()).arg(prefix.c_str())
     .input((root + "/install.sh").c_str())
     .output((prefix + ".stamp").c_str())
     .output_dir(prefix.c_str())
     .submit();

    {}

    return 0;
}}
)cpp", call));
            p.remove("target");
            return p.mcpp({"build"});
        };
        auto b1 = build("mcpp::rerun_if_changed((prefix + \"/include/greet.h\").c_str());");
        expect(fatal(e2e::succeeded(b1))) << "build with rerun_if_changed inside the prepare dir failed";
        expect(s::contains_nocase(s::text(b1), "prepare")) << "no warning mentions the prepare action";
        expect(e2e::contains(s::text(b1), "include/greet.h")) << "the warning does not name the re-run path";
        expect(e2e::contains(s::text(b1), "prep-install")) << "the warning does not name the prepare directory";
        expect(e2e::contains(s::text(b1), "prep")) << "the warning does not name the declaring package";
        auto b2 = build("mcpp::rerun_if_changed_glob((prefix + \"/include/*.h\").c_str());");
        expect(fatal(e2e::succeeded(b2))) << "build with rerun_if_changed_glob inside the prepare dir failed";
        expect(s::contains_nocase(s::text(b2), "prepare")) << "no warning mentions the prepare action (glob case)";
        expect(e2e::contains(s::text(b2), "prep-install")) << "the glob warning does not name the prepare directory";
        auto b3 = build("mcpp::rerun_if_changed((root + \"/install.sh\").c_str());");
        expect(fatal(e2e::succeeded(b3))) << "control build failed";
        expect(!s::contains_nocase(s::text(b3), "prepare")) << "a re-run input OUTSIDE any prepare directory was warned about";
    };

    "793 an unknown action role is refused"_test[s::gcc] = [] {
        // A role naming none of the five is refused naming the value and the list; each of the
        // five is accepted.
        s::project p{s::files{{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n"},
                      {"src/main.cpp", "#include <cstdio>\nint main(){ std::printf(\"ok\\n\"); }\n"}}};
        auto program = [](std::string_view role) {
            return std::format(R"cpp(import mcpp;
#include <string>
int main() {{
    mcpp::action a;
    a.id   = "{}";
    a.role = "{}";
    a.arg("true")
     .output((std::string(mcpp::out_dir()) + "/{}.stamp").c_str())
     .submit();
    return 0;
}}
)cpp", role == "prepar" ? "typo" : "ctrl", role, role == "prepar" ? "typo" : "ctrl");
        };
        p.write("build.mcpp", program("prepar"));
        auto b = p.mcpp({"build"});
        expect(fatal(failed(b))) << "an action with an unrecognised role let the build succeed";
        expect(e2e::contains(s::text(b), "\"prepar\"")) << "the refusal does not name the misspelt role";
        for (std::string_view role : {"source", "check", "object", "artifact", "prepare"})
            expect(e2e::contains(s::text(b), role)) << "the refusal does not list role '" << role << "'";
        p.write("extra.c", "int ctrl_unused_symbol(void) { return 0; }\n");
        for (std::string_view role : {"source", "check", "artifact"}) {
            p.write("build.mcpp", program(role));
            p.remove("target");
            expect(e2e::succeeded(p.mcpp({"build"}))) << "a well-known role '" << role << "' was refused";
        }
        p.write("build.mcpp", R"cpp(import mcpp;
#include <string>
int main() {
    const std::string root = mcpp::manifest_dir();
    const std::string obj  = std::string(mcpp::out_dir()) + "/extra.o";
    mcpp::action a;
    a.id   = "ctrl-object";
    a.role = "object";
    a.arg("gcc").arg("-c").arg((root + "/extra.c").c_str()).arg("-o").arg(obj.c_str())
     .input((root + "/extra.c").c_str())
     .output(obj.c_str())
     .submit();
    return 0;
}
)cpp");
        p.remove("target");
        expect(e2e::succeeded(p.mcpp({"build"}))) << "the well-known role 'object' was refused";
    };
}};

}  // namespace

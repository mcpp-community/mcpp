// mcpp and signals: no build process outlives a signalled mcpp, a closed reader ends mcpp by
// SIGPIPE and no program it starts inherits the signal ignored, a killed build does not race the
// next one (formerly tests/e2e/340, 941, 1016).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

// mcpp `args` in `dir`, held by its pid; its streams as `sp` says.
s::process start(std::vector<std::string> args, const fs::path& dir, s::spawn sp = {}) {
    auto c = s::mcpp_command(std::move(args), dir);
    sp.argv = c.argv;
    sp.cwd = dir;
    sp.env.insert(sp.env.begin(), c.env.begin(), c.env.end());
    return s::process::start(std::move(sp));
}

// The ninja processes working under `dir` (by executable name and working directory).
std::size_t ninjas_under(const fs::path& dir) {
    std::size_t n = 0;
    for (const auto& p : s::processes())
        if (p.name == "ninja" && p.cwd.string().starts_with(dir.string())) ++n;
    return n;
}

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds{ms}); }

E2E signals{"signals", [] {
    "340 no orphan survives a killed mcpp"_test[tag{"requires:gcc"}][tag{"requires:unix-shell"}] = [] {
        // A serial build is running ninja when mcpp alone is sent SIGTERM; ten seconds later no
        // ninja of the project is alive.
        s::needs({"gcc", "unix-shell"});
        s::project p({{"src/main.cpp", "int main() { return 0; }\n"},
                      {"mcpp.toml", "[package]\nname    = \"orphan\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/*.cpp\"]\n"}});
        for (int i = 0; i < 60; ++i)
            p.write(std::format("src/u{}.cpp", i), std::format(R"(#include <vector>
#include <string>
#include <map>
#include <set>
#include <algorithm>
template<int N> struct T_{0} {{ static int v(){{ return N + T_{0}<N-1>::v(); }} }};
template<> struct T_{0}<0> {{ static int v(){{ return 0; }} }};
int f{0}() {{
  std::map<std::string, std::vector<std::pair<int,std::string>>> m;
  std::set<std::string> s;
  for (int k = 0; k < 800; ++k) {{ m[std::to_string(k)].push_back({{k, std::to_string(k*2)}}); s.insert(std::to_string(k*3)); }}
  std::vector<std::string> keys;
  for (auto& kv : m) keys.push_back(kv.first);
  std::sort(keys.begin(), keys.end());
  return (int)m.size() + T_{0}<400>::v() + (int)keys.size() + (int)s.size();
}}
)", i));
        const auto work = p.dir();
        const auto baseline = ninjas_under(work);
        auto mcpp = start({"build", "--jobs", "1"}, work, {.out = s::stream::null, .err = s::stream::null});
        bool seen = false;
        for (int i = 0; i < 60 && !seen; ++i) {
            seen = ninjas_under(work) > baseline;
            if (!seen) sleep_ms(250);
        }
        if (!seen) mcpp.signal(s::sig::kill);
        expect(fatal(seen)) << "ninja never started: this test measured nothing";
        sleep_ms(2000);
        mcpp.signal(s::sig::terminate);
        (void)mcpp.wait();
        sleep_ms(8000);
        const auto after = ninjas_under(work);
        std::vector<std::string> orphans;
        for (const auto& e : s::processes())
            if (e.name == "ninja") orphans.push_back(std::format("orphan {} cwd={}", e.pid, e.cwd.string()));
        expect(after <= baseline) << "ninja survived a killed mcpp: " << after << " alive, baseline " << baseline << orphans;
    };

    "941 a closed reader ends mcpp and no child inherits the ignore"_test[tag{"requires:gcc"}][tag{"requires:unix-shell"}][tag{"requires:python3"}] = [] {
        // A: a program `mcpp run` starts begins with SIGPIPE at its default; B: `mcpp --help`
        // whose standard output has no reader ends by SIGPIPE, not by an exception or an exit.
        // mcpp itself starts with SIGPIPE at its default.
        s::needs({"gcc", "unix-shell", "python3"});
        s::project p({{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                      {"src/main.cpp", R"(#include <signal.h>
#include <stdio.h>
int main() {
    struct sigaction now {};
    sigaction(SIGPIPE, nullptr, &now);
    puts(now.sa_handler == SIG_IGN ? "pipe=ignored" : "pipe=default");
    return 0;
}
)"}});
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "the program does not build\n" << s::text(r);
        auto run = start({"run"}, p.dir(), {.out = s::stream::pipe, .err = s::stream::file, .err_file = p.at("run.err")});
        const int code = run.wait();
        expect(fatal(code == 0 && e2e::contains(run.piped(), "pipe=default")))
            << "A: the program mcpp ran began with SIGPIPE ignored\n" << run.piped() << s::read_file(p.at("run.err"));
        auto help = start({"--help"}, p.dir(), {.out = s::stream::closed, .err = s::stream::file, .err_file = p.at("pipe.err")});
        (void)help.wait();
        const auto err = s::read_file(p.at("pipe.err"));
        expect(fatal(help.ended_by() == s::sig::pipe && s::lacks(err, "terminate")))
            << "B: mcpp did not end by SIGPIPE when its reader was gone (signal " << help.ended_by() << ")\n" << err;
    };

    "1016 a killed build does not race the next one"_test[tag{"requires:gcc"}] = [] {
        // Under both schedules, twelve rounds: a build whose mcpp is killed (SIGKILL) while a
        // second build waits; the waiting build succeeds, the next does nothing, the program runs.
        s::needs({"gcc"});
        for (std::string_view schedule : {"default", "split"}) {
            s::project p(s::case_dir() / schedule, {});
            std::string manifest = "[package]\nname = \"kg\"\nversion = \"0.1.0\"\nstandard = \"c++23\"\n[toolchain]\ndefault = \"gcc@16.1.0\"\n";
            if (schedule == "split") manifest += "[build]\nbmi_schedule = \"on\"\n";
            p.write("mcpp.toml", manifest);
            std::string main_cpp = "import std;\n";
            for (int i = 1; i <= 24; ++i) {
                p.write(std::format("src/m{}.cppm", i), std::format("export module m{0};\nimport std;\nexport int f{0}(){{ std::vector<int> v({0}); return (int)v.size(); }}\n", i));
                main_cpp += std::format("import m{};\n", i);
            }
            p.write("src/main.cpp", main_cpp + "int main(){ std::println(\"{}\", f1()+f24()); }\n");
            auto r = p.mcpp({"build"});
            expect(fatal(e2e::succeeded(r))) << s::text(r);
            for (int round = 1; round <= 12; ++round) {
                p.remove("target");
                auto a = start({"build"}, p.dir(), {.out = s::stream::null, .err = s::stream::null});
                sleep_ms(200);
                auto b = start({"build"}, p.dir());
                sleep_ms(700);
                a.signal(s::sig::kill);
                (void)a.wait();
                const int b_rc = b.wait();
                expect(fatal(b_rc == 0)) << schedule << " round " << round << ": the build after the killed one failed\n" << b.piped();
                r = p.mcpp({"build"});
                expect(fatal(e2e::succeeded(r))) << s::text(r);
                expect(fatal(s::lacks(s::text(r), "Compiling"))) << schedule << " round " << round << ": the following build compiled again\n" << s::text(r);
                const auto out = s::last_line(p.mcpp({"run"}).out);
                expect(fatal(out == "25")) << schedule << " round " << round << ": the program printed '" << out << "'";
            }
        }
    };
}};

}  // namespace

// An action's command: a depfile it writes, no size limit on its declaration, the engine named
// through `${mcpp.self}`, its environment and directory (formerly tests/e2e/631, 659, 660, 799).
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

long long mtime(const fs::path& p) {
    // Compared with one another only: the file clock's own epoch serves, and
    // file_clock::to_sys is optional (MSVC's library has to_utc instead).
    return std::chrono::duration_cast<std::chrono::seconds>(fs::last_write_time(p).time_since_epoch()).count();
}

std::string mcpp_host() { return fs::absolute(s::mcpp_path()).generic_string(); }

E2E commands{"commands", [] {
    "631 action depfile tracks a runtime discovered include"_test[s::gcc] = [] {
        // `mcpp::action`'s `depfile`: a file the command discovers by running, named only in the
        // depfile it writes, reruns the action when touched; nothing else does.
        s::project p{s::files{{"mcpp.toml", "[package]\nname    = \"shaderdep\"\nversion = \"0.1.0\"\n"},
                      {"src/main.cpp", "#include <cstdio>\nint generated_value();\nint main() { std::printf(\"VALUE=%d\\n\", generated_value()); }\n"},
                      {"included.glsl", "float unused = 1.0;\n"},
                      {"build.mcpp", R"cpp(#include <cstdio>
#include <string>
import mcpp;
int main() {
    const std::string root  = mcpp::manifest_dir();
    const std::string out   = std::string(mcpp::out_dir()) + "/generated.cpp";
    const std::string dep   = std::string(mcpp::out_dir()) + "/generated.cpp.d";
    const std::string count = root + "/run_count.txt";
    mcpp::action a;
    a.id      = "genshader";
    a.role    = "source";
    a.depfile = dep.c_str();
    // Deliberately NOT declared as `.input(...)`: the whole point is that
    // ninja learns about it from the depfile the command writes, not from
    // anything build.mcpp told the engine in advance.
    a.arg((root + "/gen.sh").c_str())
     .arg((root + "/included.glsl").c_str())
     .arg(out.c_str())
     .arg(dep.c_str())
     .arg(count.c_str())
     .output(out.c_str())
     .submit();
}
)cpp"}}};
        s::write_executable(p.at("gen.sh"), R"cpp(#!/usr/bin/env bash
n=0
[ -f "$4" ] && n="$(cat "$4")"
n=$((n + 1))
echo "$n" > "$4"
printf 'int generated_value() { return %s; }\n' "$n" > "$2"
# Make syntax, the shape `-MD -MF`/`--depfile` produce: OUTPUT: PREREQUISITES.
printf '%s: %s\n' "$2" "$1" > "$3"
)cpp");
        auto count = [&] { return p.exists("run_count.txt") ? s::last_line(p.read("run_count.txt")) : std::string{"<missing>"}; };
        auto value = [&] { return s::last_starting(p.mcpp({"run"}).out, "VALUE="); };
        auto touch = [&] { fs::last_write_time(p.at("included.glsl"), fs::file_time_type::clock::now()); };
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "initial build failed";
        expect(fatal(eq(count(), std::string{"1"}))) << "the action did not run on a fresh build";
        expect(fatal(eq(value(), std::string{"VALUE=1"}))) << "unexpected initial output";
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "no-op rebuild failed";
        expect(fatal(eq(count(), std::string{"1"}))) << "the action reran although nothing it tracks changed";
        touch();
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "rebuild after touch failed";
        expect(fatal(eq(count(), std::string{"2"}))) << "touching a file the depfile named did not rerun the action";
        expect(fatal(eq(value(), std::string{"VALUE=2"}))) << "the action reran but the artifact was not regenerated";
        touch();
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "second touch rebuild failed";
        expect(fatal(eq(count(), std::string{"3"}))) << "the second touch did not rerun the action";
        expect(eq(value(), std::string{"VALUE=3"})) << "unexpected output after the second touch";
    };

    "659 an action declaration has no size limit"_test = [] {
        // An action's inputs, outputs and command have no declared size limit: 600 inputs and
        // outputs reach the edge (and its replayed record), a 19200-byte command runs, and a
        // command over the host's limit is refused naming the edge, briefly.
        constexpr int n = 600;
        const std::string pad = "a-directory-name-long-enough-to-make-the-edge-line-cross-the-per-argument-limit-of-the-host-os-xxxx";
        s::project app{s::files{{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                        {"src/main.cpp", "#include <cstdio>\nint main() { std::puts(\"LARGE_DECLARATION_OK\"); }\n"}}};
        for (int i = 1; i <= n; ++i) app.write(std::format("in/{}/input-{}.txt", pad, i), "");
        std::string true_host;
        if (!s::on_windows())
            if (auto t = s::on_path("true")) true_host = t->generic_string();
        app.write("build.mcpp", std::format(R"cpp(import std;
import mcpp;
int main() {{
    const std::string root = "{0}";
    const std::string pad  = "{1}";
    mcpp::action a;
    a.id          = "wide";
    a.role        = "source";
    a.description = "an action with {2} inputs and {2} outputs";
    a.arg("{3}").arg("--version");
    for (int i = 1; i <= {2}; ++i) {{
        a.input((root + "/in/" + pad + "/input-" + std::to_string(i) + ".txt").c_str());
        a.output((std::string(mcpp::out_dir()) + "/out/" + pad + "/output-" + std::to_string(i) + ".txt").c_str());
    }}
    a.submit();
    const std::string t = "{4}";
    if (!t.empty()) {{
        mcpp::action c;
        c.id          = "long-command";
        c.role        = "check";
        c.description = "a command past sixteen kilobytes";
        c.arg(t.c_str());
        // 300 arguments of 64 bytes: 19200 bytes of argv, past the old
        // 16384-byte bound of the command list.
        const std::string word(64, 'x');
        for (int i = 0; i < 300; ++i) c.arg(word.c_str());
        c.output((std::string(mcpp::out_dir()) + "/long-command.stamp").c_str());
        c.submit();
    }}
}}
)cpp", app.dir().generic_string(), pad, n, mcpp_host(), true_host));
        auto b1 = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(b1))) << "a build with a wide action was refused";
        expect(fatal(e2e::contains(s::last_line(app.mcpp({"run"}).out), "LARGE_DECLARATION_OK"))) << "the program did not run";
        // `grep ': mcpp_action_' | tr -s ' ' '\n' | grep -c <pattern>`
        auto count_in_edge = [&](std::string_view pattern) {
            const auto nj = ninja_file(app.at("target"));
            expect(fatal(nj.has_value())) << "no build.ninja";
            const std::regex re{std::string(pattern)};
            std::size_t c = 0;
            for (const auto& line : s::lines_with(s::read_file(*nj), ": mcpp_action_")) {
                std::istringstream words{line};
                for (std::string w; words >> w;) c += std::regex_search(w, re) ? 1 : 0;
            }
            return c;
        };
        expect(eq(count_in_edge(R"(output-[0-9]*\.txt)"), std::size_t{n})) << "build.ninja does not name every declared output";
        expect(eq(count_in_edge(R"(input-[0-9]*\.txt)"), std::size_t{n})) << "build.ninja does not name every declared input";
        if (!true_host.empty())
            expect(eq(s::find_files(app.at("target"), R"(long-command\.stamp)").size(), std::size_t{1})) << "the long-command check did not run";
        expect(s::lacks(s::text(b1), "\"overflow\":true")) << "the overflow marker appeared";
        app.write("src/extra.cpp", "int extra() { return 1; }\n");
        expect(fatal(e2e::succeeded(app.mcpp({"build"})))) << "rebuild after an unrelated source failed";
        expect(eq(count_in_edge(R"(output-[0-9]*\.txt)"), std::size_t{n})) << "after a replay build.ninja does not name every declared output";

        s::project too_long{s::case_dir() / "too-long", {{"mcpp.toml", app.read("mcpp.toml")}, {"src/main.cpp", app.read("src/main.cpp")}}};
        too_long.write("build.mcpp", std::format(R"cpp(import std;
import mcpp;
int main() {{
    mcpp::action c;
    c.id   = "too-long";
    c.role = "check";
    c.arg("{}");
    const std::string word(64, 'y');
    for (int i = 0; i < 2200; ++i) c.arg(word.c_str());
    c.output((std::string(mcpp::out_dir()) + "/too-long.stamp").c_str());
    c.submit();
}}
)cpp", mcpp_host()));
        auto b4 = too_long.mcpp({"build"});
        expect(fatal(failed(b4))) << "a 140800-byte literal command was accepted";
        expect(s::has_line(s::text(b4), "over the .* byte limit")) << "the refusal is not the command-length guard's";
        expect(e2e::contains(s::text(b4), "too-long.stamp")) << "the refusal does not name the edge";
        expect(lt(s::text(b4).size(), std::size_t{4000})) << "the refusal printed the whole edge line";
    };

    "660 an action names the engine through mcpp self"_test = [] {
        // `${mcpp.self}` in an action's argv is the engine's own executable, substituted before
        // build.ninja, and `mcpp stage` through it is a portable copy; `${mcpp.*}` path tokens are
        // spelled with forward slashes; a no-op rebuild copies nothing.
        s::project p{s::files{{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n"},
                      {"src/main.cpp", "#include <cstdio>\nint main() { std::puts(\"SELF_COPY_OK\"); }\n"},
                      {"build.mcpp", R"cpp(import std;
import mcpp;
int main() {
    mcpp::action a;
    a.id          = "copy";
    a.role        = "artifact";
    a.description = "the linked program, copied by the engine";
    a.arg("${mcpp.self}").arg("stage").arg("--verify").arg("content")
     .arg("--output").arg("${mcpp.out_dir}/copied/deeper/app-copy")
     .arg("${mcpp.target_file:app}")
     .input("${mcpp.target_file:app}")
     .output("${mcpp.out_dir}/copied/deeper/app-copy")
     .submit();
}
)cpp"}}};
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "build failed";
        expect(fatal(e2e::contains(s::last_line(p.mcpp({"run"}).out), "SELF_COPY_OK"))) << "the program did not run";
        const auto nj = ninja_file(p.at("target"));
        expect(fatal(nj.has_value())) << "no build.ninja";
        const auto ninja = s::read_file(*nj);
        expect(fatal(s::lacks(ninja, "mcpp.self"))) << "${mcpp.self} reached build.ninja unsubstituted";
        std::vector<std::string> commands;
        {
            std::istringstream in{ninja};
            bool seen = false;
            for (std::string line; std::getline(in, line);) {
                if (line.starts_with("rule mcpp_action_")) { seen = true; continue; }
                if (seen && line.starts_with("  command = ")) { commands.push_back(line.substr(12)); seen = false; }
            }
        }
        expect(fatal(!commands.empty())) << "no mcpp_action_ rule in build.ninja";
        for (const auto& line : commands) {
            const auto at = line.find(" -- ");
            const auto argv = at == std::string::npos ? line : line.substr(at + 4);
            expect(argv.find('\\') == std::string::npos) << "a ${mcpp.*} path token expanded to a native spelling:\n  " << argv;
        }
        std::optional<fs::path> copy, bin;
        for (const auto& f : s::find_files(p.at("target"), "app-copy"))
            if (!copy && f.generic_string().ends_with("/copied/deeper/app-copy")) copy = f;
        expect(fatal(copy.has_value())) << "the copy action produced nothing";
        for (const auto& f : s::find_files(p.at("target"), R"(app(\.exe)?)"))
            if (!bin && f.parent_path().filename() == "bin") bin = f;
        expect(fatal(bin.has_value())) << "no linked program under target/";
        expect(s::read_file(*bin) == s::read_file(*copy)) << "the copy differs from the program";
        const auto before = mtime(*copy);
        std::this_thread::sleep_for(std::chrono::seconds{1});
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "no-op rebuild failed";
        expect(eq(mtime(*copy), before)) << "the copy was rewritten on a no-op rebuild";
    };

    "799 an action runs with its env and cwd"_test = [] {
        // `mcpp::action::env` and `::cwd`: the command sees the variable and runs in the
        // directory, its declared output is not resolved against that directory, a changed
        // value re-runs it; the wrapper puts a prepended PATH before a declared or inherited one.
        s::project p{s::files{{"mcpp.toml", "[package]\nname    = \"envcwd\"\nversion = \"0.1.0\"\n"},
                      {"src/main.cpp", "#include <cstdio>\nint main() { std::printf(\"ENV_CWD_OK\\n\"); }\n"}}};
        fs::create_directories(p.at("tools"));
        auto program = [&](std::string_view greeting) {
            p.write("build.mcpp", std::format(R"cpp(#include <string>
import mcpp;
int main() {{
    const std::string out = std::string(mcpp::out_dir()) + "/probe.txt";
    mcpp::action a;
    a.id   = "probe";
    a.role = mcpp::roles::check;
    a.env("GREETING", "{}")
     .cwd("tools")
     .arg("sh").arg("-c")
     .arg("printf '%s\\n' \"$GREETING\" > \"$1\"; pwd -P >> \"$1\"")
     .arg("sh").arg(out.c_str())
     .output(out.c_str())
     .submit();
}}
)cpp", greeting));
        };
        auto probe = [&] {
            const auto f = s::find_file(p.at("target"), "probe.txt");
            expect(fatal(f.has_value())) << "B: the declared output was not written";
            std::vector<std::string> lines;
            std::istringstream in{s::read_file(*f)};
            for (std::string l; std::getline(in, l);) lines.push_back(l);
            lines.resize(2);
            return lines;
        };
        program("hello");
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "build failed";
        const auto first = probe();
        expect(eq(first[0], std::string{"hello"})) << "A: the command did not see GREETING";
        // On Windows the command's `sh` (MSYS) prints `C:\x` as `/c/x`.
        auto where = first[1];
        if (s::on_windows() && where.size() > 2 && where[0] == '/' && where[2] == '/')
            where = std::string(1, where[1]) + ":" + where.substr(2);
        std::error_code ec;
        expect(fs::equivalent(where, p.at("tools"), ec)) << "A: the command did not run in tools: " << first[1];
        expect(!p.exists("tools/probe.txt")) << "B: the output landed under cwd";
        program("goodbye");
        expect(fatal(e2e::succeeded(p.mcpp({"build"})))) << "rebuild failed";
        expect(eq(probe()[0], std::string{"goodbye"})) << "C: a changed value did not re-run the action";
        if (s::on_windows()) return;  // D is not read on this host; the graph half is unit-tested
        const auto tmp = s::case_dir();
        auto d1 = p.mcpp({"__action", "--env", "PATH=/declared/bin", "--path-prepend", "/first/bin", "--", "/bin/sh", "-c", "printf \"%s\" \"$PATH\" > \"$1\"", "sh", (tmp / "d1.txt").string()});
        expect(fatal(e2e::succeeded(d1))) << "D: the wrapper failed";
        expect(eq(s::read_file(tmp / "d1.txt"), std::string{"/first/bin:/declared/bin"})) << "D: with a declared PATH";
        auto d2 = p.mcpp({"__action", "--path-prepend", "/first/bin", "--", "/bin/sh", "-c", "printf \"%s\" \"$PATH\" > \"$1\"", "sh", (tmp / "d2.txt").string()});
        expect(fatal(e2e::succeeded(d2))) << "D: the wrapper failed";
        expect(eq(s::read_file(tmp / "d2.txt"), "/first/bin:" + s::env("PATH").value_or(""))) << "D: without a declared PATH";
    };
}};

}  // namespace

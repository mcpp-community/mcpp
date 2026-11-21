// What a build program's compile read is an input of the program (mcpp#991), what a project is told
// about it, and how a name several dependencies answer is judged (mcpp#852): design 2026-11-21,
// build programs' inputs.
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

const s::options inherited{.where = s::home::inherited};

std::string emit_header(std::string_view value) {
    return std::format("#pragma once\n#include <cstdio>\ninline void emit() {{ std::puts(\"mcpp:cfg=VALUE=\\\"{}\\\"\"); }}\n",
                       value);
}

const std::string kMain = "#include <cstdio>\nint main() { std::puts(VALUE); }\n";

std::string package(std::string_view name) {
    return std::format("[package]\nname = \"{0}\"\nversion = \"0.1.0\"\n\n[targets.{0}]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", name);
}

// The program's output, the last line of `mcpp run`.
std::string ran(const s::project& p) {
    auto r = p.mcpp({"run"}, inherited);
    expect(e2e::succeeded(r)) << s::text(r);
    return s::last_line(r.out);
}

// The edit lands in a later second than the record's, as a person's edit does.
void later() { std::this_thread::sleep_for(std::chrono::milliseconds(1100)); }

// The git repository `dir`, committed with what it holds.
void commit_all(const fs::path& dir) {
    const auto git = s::on_path("git");
    expect(fatal(git.has_value())) << "git is not on PATH";
    const std::vector<std::pair<std::string, std::optional<std::string>>> who{
        {"GIT_AUTHOR_NAME", "fixture"}, {"GIT_AUTHOR_EMAIL", "fixture@example.invalid"},
        {"GIT_COMMITTER_NAME", "fixture"}, {"GIT_COMMITTER_EMAIL", "fixture@example.invalid"}};
    if (!fs::exists(dir / ".git")) expect(fatal(e2e::succeeded(s::run(*git, {"init", "-q", "."}, dir, who))));
    expect(fatal(e2e::succeeded(s::run(*git, {"add", "-A"}, dir, who))));
    expect(fatal(e2e::succeeded(s::run(*git, {"commit", "-qm", "fixture", "--allow-empty"}, dir, who))));
}

E2E inputs{"inputs", s::home_fixture, [] {
    "1128 a header a build program includes is an input of its compile and its run"_test[s::gcc] = [] {
        // mcpp#991 T1, T2, T3, T6: an edit of a header build.mcpp includes compiles and runs the
        // program again, through the fast path and after target/.build-mcpp is deleted; an edit of
        // a source the program does not read neither compiles nor runs it; a record of an earlier
        // engine (no `reads`) compiles the program once.
        s::project p{{{"mcpp.toml", package("repro")},
                      {"src/main.cpp", kMain},
                      {"support/emit.hpp", emit_header("one")},
                      {"build.mcpp", "#include \"support/emit.hpp\"\nimport std;\nint main() { emit(); }\n"}}};
        expect(eq(ran(p), std::string("one"))) << "T1: the first run";
        later();
        p.write("support/emit.hpp", emit_header("two"));
        expect(eq(ran(p), std::string("two"))) << "T1/T3: the header edit was not an input";
        fs::remove_all(p.at("target/.build-mcpp"));
        expect(eq(ran(p), std::string("two"))) << "T1: after target/.build-mcpp was deleted";
        later();
        p.write("support/emit.hpp", emit_header("three"));
        expect(eq(ran(p), std::string("three"))) << "T1: the edit after the record was made again";
        // T2.
        later();
        p.write("src/main.cpp", kMain + "// an edit the program does not read\n");
        auto b = p.mcpp({"build", "-v"}, inherited);
        expect(fatal(e2e::succeeded(b))) << s::text(b);
        expect(s::has_line(s::text(b), "build\\.mcpp repro +cached")) << "T2: the program compiled or ran";
        // T6: a group record without `reads`, as an earlier engine wrote it.
        const auto records = s::glob(p.at("target/.build-mcpp/host"), "*/programs.json");
        expect(fatal(eq(records.size(), 1u)));
        auto doc = s::json_file(records.front());
        for (auto row = doc.begin(); row != doc.end(); ++row) row->erase("reads");
        std::ofstream(records.front(), std::ios::trunc) << doc.dump(2);
        later();
        p.write("src/main.cpp", kMain);   // a plan, not the fast path
        auto again = p.mcpp({"build", "-v"}, inherited);
        expect(s::has_line(s::text(again), "identity .* compiled")) << "T6: the record without reads was current";
        later();
        p.write("src/main.cpp", kMain + "\n");
        auto then = p.mcpp({"build", "-v"}, inherited);
        expect(s::has_line(s::text(then), "identity .* current")) << "T6: compiled twice";
    };

    "1129 a project is told once of the headers its build programs include"_test[s::gcc] = [] {
        // T4: the note is said on the first build, not on the second, again after `mcpp clean`;
        // the members of a workspace with the same usage are one note.
        s::project ws{{{"mcpp.toml", "[workspace]\nmembers = [\"a\", \"b\"]\n"}}};
        for (auto m : {"a", "b"}) {
            s::project member{ws.dir() / m, {{"mcpp.toml", package(m)}, {"src/main.cpp", kMain},
                                             {"build.mcpp", "#include <cstdio>\nint main() { std::puts(\"mcpp:cfg=VALUE=\\\"x\\\"\"); }\n"}}};
        }
        const std::string note = "includes standard or system headers";
        auto first = ws.mcpp({"build", "--workspace"}, inherited);
        expect(fatal(e2e::succeeded(first))) << s::text(first);
        expect(eq(s::count_lines(first.err, note), 1u)) << "one note for both members: " << first.err;
        expect(s::has_line(first.err, "build\\.mcpp of (a and b|b and a) includes")) << first.err;
        auto second = ws.mcpp({"build", "--workspace"}, inherited);
        expect(s::lacks(second.err, note)) << "said again";
        // A no-op build plans nothing and says nothing (docs/30); a source edit plans again.
        later();
        ws.write("a/src/main.cpp", kMain + "\n");
        auto verbose = ws.mcpp({"build", "--workspace", "-v"}, inherited);
        expect(s::has_line(verbose.err, "includes standard or system headers.*\\(noted before\\)")) << verbose.err;
        expect(e2e::succeeded(ws.mcpp({"clean"}, inherited)));
        auto third = ws.mcpp({"build", "--workspace"}, inherited);
        expect(s::has_line(third.err, note)) << "not said after mcpp clean";
    };

    "1130 a file a build program reads outside its package is warned once and is not published"_test[s::gcc] = [] {
        // T5, T9: a file outside the package is warned on the first build only, and the build
        // database keeps the notice with `shown` false; `mcpp publish` refuses it, and a file HEAD
        // does not track.
        const auto root = s::case_dir();
        s::project shared{root / "shared", {{"emit.hpp", emit_header("outside")}}};
        s::project p{root / "repro", {{"mcpp.toml", package("repro")}, {"src/main.cpp", kMain},
                                      {"build.mcpp", "#include \"../shared/emit.hpp\"\nimport std;\nint main() { emit(); }\n"}}};
        const std::string warning = "reads files outside its package";
        auto first = p.mcpp({"build"}, inherited);
        expect(fatal(e2e::succeeded(first))) << s::text(first);
        expect(s::has_line(first.err, warning)) << first.err;
        later();
        shared.write("emit.hpp", emit_header("outside2"));
        auto second = p.mcpp({"build"}, inherited);
        expect(s::lacks(second.err, warning)) << "warned twice";
        later();
        shared.write("emit.hpp", emit_header("outside3"));
        auto db = p.mcpp({"emit", "build-database", "--format", "json"}, inherited);
        expect(fatal(e2e::succeeded(db))) << s::text(db);
        bool kept = false;
        for (auto const& d : s::json(db.out)["diagnostics"])
            if (d.value("code", "") == "MCPP_BUILD_PROGRAM_READS_OUTSIDE")
                kept = d.contains("shown") && d["shown"] == false;
        expect(kept) << "T9: the envelope lost the notice or showed it: " << db.out;
        // T5.
        commit_all(p.dir());
        auto outside = p.mcpp({"publish", "--dry-run", "--allow-dirty"}, inherited);
        expect(!e2e::succeeded(outside)) << "published a program that reads outside its package";
        expect(s::has_line(outside.err, "emit\\.hpp \\(outside the package\\)")) << outside.err;
        p.write("inc/emit.hpp", emit_header("inside"));
        p.write("build.mcpp", "#include \"inc/emit.hpp\"\nimport std;\nint main() { emit(); }\n");
        commit_all(p.dir());
        p.write(".gitignore", "inc/\n");
        const auto git = s::on_path("git");
        expect(e2e::succeeded(s::run(*git, {"rm", "-q", "--cached", "inc/emit.hpp"}, p.dir())));
        commit_all(p.dir());
        auto untracked = p.mcpp({"publish", "--dry-run", "--allow-dirty"}, inherited);
        expect(!e2e::succeeded(untracked)) << "published a program that reads an untracked file";
        expect(s::has_line(untracked.err, "inc/emit\\.hpp \\(not tracked by HEAD\\)")) << untracked.err;
    };

    "1131 a contract variable two dependencies answer is no spelling's"_test[s::gcc] = [] {
        // mcpp#852 T10: `foo-bar` and `foo_bar` are one variable. The build says nothing about it;
        // a program that asks receives the first answer under the deprecated reading, warned on
        // every run, listed by `mcpp why compat` and refused by --strict. Qualified names that do
        // differ are the spellings the warning names.
        const auto root = s::case_dir();
        for (auto [dir, ns, mod] : {std::tuple{"foo-bar", "x", "fa"}, std::tuple{"foo_bar", "y", "fb"}})
            s::project{root / dir, {{"mcpp.toml", std::format("[package]\nnamespace = \"{}\"\nname = \"{}\"\nversion = \"0.1.0\"\n", ns, dir)},
                                    {"src/m.cppm", std::format("export module {};\nexport int v() {{ return 1; }}\n", mod)}}};
        s::project app{root / "app", {{"mcpp.toml", package("app") + "\n[dependencies]\nx.foo-bar = { path = \"../foo-bar\" }\ny.foo_bar = { path = \"../foo_bar\" }\n"},
                                      {"src/main.cpp", kMain},
                                      {"build.mcpp", "import std;\nimport mcpp;\nint main() { std::println(\"mcpp:cfg=VALUE=\\\"{}\\\"\", mcpp::dep_dir(\"foo_bar\")); }\n"}}};
        auto b = app.mcpp({"build"}, inherited);
        expect(fatal(e2e::succeeded(b))) << s::text(b);
        expect(s::lacks(b.err, "collides on")) << "the build warned of a name nobody asked for";
        expect(s::has_line(b.err, "asks dep_dir\\(\"foo_bar\"\\), which several dependencies answer.*Write one of x\\.foo-bar,y\\.foo_bar instead")) << b.err;
        expect(s::has_line(ran(app), "foo-bar$")) << "the first answer, as before the rule";
        later();
        app.write("src/main.cpp", kMain + "\n");   // a replay of the program, not a no-op build
        auto replay = app.mcpp({"build"}, inherited);
        expect(s::has_line(replay.err, "asks dep_dir\\(\"foo_bar\"\\)")) << "a replay did not warn";
        auto why = app.mcpp({"why", "compat"}, inherited);
        expect(s::has_line(s::text(why), "ambiguous-contract-name: dep_dir\\(\"foo_bar\"\\)")) << s::text(why);
        auto strict = app.mcpp({"build", "--strict"}, inherited);
        expect(!e2e::succeeded(strict)) << "--strict accepted the deprecated reading";
        // Asked by a qualified name, the answer is that package's, and nothing is said.
        app.write("build.mcpp", "import std;\nimport mcpp;\nint main() { std::println(\"mcpp:cfg=VALUE=\\\"{}\\\"\", mcpp::dep_dir(\"y.foo_bar\")); }\n");
        auto exact = app.mcpp({"build"}, inherited);
        expect(s::lacks(exact.err, "several dependencies answer")) << exact.err;
        expect(s::has_line(ran(app), "foo_bar$")) << "the qualified name";
    };
}};

}  // namespace

// The linker's run-path rule from mcpp's side (xlings design 2026-11-27 §8.3, C1): xlings' `ld`
// wrapper appends `-rpath "$XLINGS_SUBOS_LIB"` to a link of a non-system loader unless
// XLINGS_SUBOS_LD_PATHS=0. mcpp declares the 0 for everything it starts, whatever its caller set,
// so an artifact's run path is the one mcpp itself derived from the binding it selected, and a
// pack holds no path of the machine that built it.
//
// What these cases hold without the wrapper (binutils before its r1 carries none): the variable
// each program mcpp starts receives, and the run path the artifact ends with. With the wrapper
// installed the second would fail were the declaration missing.
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

std::vector<std::string> split(std::string_view text, char sep) {
    std::vector<std::string> out;
    for (auto part : std::views::split(text, sep))
        if (part.begin() != part.end()) out.emplace_back(part.begin(), part.end());
    return out;
}

// The first build.ninja of the project's target, outside the build program's tree.
std::string graph_of(const s::project& p) {
    for (const auto& f : s::find_files(p.at("target"), R"(build\.ninja)"))
        if (f.generic_string().find("/.build-mcpp/") == std::string::npos) return s::read_file(f);
    expect(fatal(false)) << "no build.ninja generated";
    return {};
}

// The directories the link line gives the linker as run path (`-Wl,-rpath,<dir>`), as the linker
// reads them (ninja's `$$` is a `$`).
std::set<std::string> rpath_of_graph(const std::string& graph) {
    std::set<std::string> out;
    static const std::regex rpath{R"(-Wl,-rpath,([^\s"']+))"};
    for (std::sregex_iterator it{graph.begin(), graph.end(), rpath}, end; it != end; ++it) {
        std::string dir = (*it)[1].str();
        for (auto at = dir.find("$$"); at != std::string::npos; at = dir.find("$$", at + 1)) dir.erase(at, 1);
        out.insert(std::move(dir));
    }
    return out;
}

std::set<std::string> rpath_of_artifact(const fs::path& file) {
    const auto parts = split(s::read_header(file).runpath, ':');
    return {parts.begin(), parts.end()};
}

std::string joined(const std::set<std::string>& entries) {
    std::string out;
    for (const auto& e : entries) out += (out.empty() ? "" : ":") + e;
    return out;
}

std::optional<fs::path> program_under(const fs::path& root, std::string_view name) {
    for (const auto& f : s::find_files(root, std::string(name)))
        if (f.generic_string().find("/.build-mcpp/") == std::string::npos && f.generic_string().find("/bin/") != std::string::npos)
            return f;
    return std::nullopt;
}

std::string output_of(const fs::path& program) {
    auto out = s::run(program).out;
    while (out.ends_with('\n') || out.ends_with('\r')) out.pop_back();
    return out;
}

// The regular files under `root`, not the links.
std::vector<fs::path> plain_files(const fs::path& root) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (fs::is_directory(root, ec))
        for (const auto& e : fs::recursive_directory_iterator(root))
            if (!e.is_symlink() && e.is_regular_file()) out.push_back(e.path());
    std::ranges::sort(out);
    return out;
}

// What a caller that meets the wrapper without mcpp's declaration would give it: a directory of
// the caller's own as the active SubOS's library directory, and the variable that lets the
// wrapper add it.
std::vector<std::pair<std::string, std::optional<std::string>>> callers_env(const fs::path& subos_lib) {
    return {{"XLINGS_SUBOS_LIB", subos_lib.string()}, {"XLINGS_SUBOS_LD_PATHS", std::string("1")}};
}

E2E linker_declaration{"linker_declaration", s::home_fixture, [] {
    "1177 a link under a caller's SubOS keeps the run path mcpp derived"_test[s::gcc][s::elf][s::unix_shell] = [] {
        // The caller's XLINGS_SUBOS_LD_PATHS=1 and XLINGS_SUBOS_LIB do not reach the link as
        // they were given: the programs the link starts (the compiler driver, and what it starts
        // for the link) and a build program read XLINGS_SUBOS_LD_PATHS=0, and the artifact's run
        // path is exactly the `-Wl,-rpath,` of the link line, without the caller's directory.
        const auto tmp = s::case_dir();
        const auto subos_lib = tmp / "callers-subos" / "lib";
        fs::create_directories(subos_lib);
        const auto log = tmp / "driver-env.log";
        s::write_executable(tmp / "observe.sh", std::format(R"sh(#!/bin/sh
# gcc's -wrapper runs each program of the link (collect2, which starts the linker) as `$@`.
printf '%s LD_PATHS=%s LIB=%s\n' "$(basename "$1")" "${{XLINGS_SUBOS_LD_PATHS-unset}}" "${{XLINGS_SUBOS_LIB-unset}}" >> '{}'
exec "$@"
)sh", log.string()));
        s::project p{tmp / "p", {{"mcpp.toml", std::format(R"([package]
name    = "rp"
version = "0.1.0"

[build]
ldflags = ["-wrapper", "{}"]
)", (tmp / "observe.sh").string())},
                                 {"src/main.cpp", R"cpp(#include <cstdio>
int main() { std::printf("%d\n", LD_PATHS_SEEN_BY_BUILD_PROGRAM); }
)cpp"},
                                 {"build.mcpp", R"cpp(import mcpp;
import std;
int main() {
    const char* v = std::getenv("XLINGS_SUBOS_LD_PATHS");
    mcpp::cxxflag(std::format("-DLD_PATHS_SEEN_BY_BUILD_PROGRAM={}", v && *v == '0' && !v[1] ? 0 : 7).c_str());
}
)cpp"}}};
        auto r = p.mcpp({"build"}, {.env = callers_env(subos_lib)});
        expect(fatal(e2e::succeeded(r))) << "the build failed\n" << s::text(r);
        const auto bin = program_under(p.at("target"), "rp");
        expect(fatal(bin.has_value())) << "no program was produced";
        expect(output_of(*bin) == "0") << "the build program saw XLINGS_SUBOS_LD_PATHS other than 0 (7 is anything else, the caller's 1 included): " << output_of(*bin);

        const auto seen = s::read_file(log);
        expect(fatal(e2e::contains(seen, "collect2 "))) << "the compiler driver started no collect2 under -wrapper: the link was not observed\n" << seen;
        for (const auto& l : split(seen, '\n')) {
            expect(e2e::contains(l, "LD_PATHS=0 ")) << "a program of the link was not told XLINGS_SUBOS_LD_PATHS=0: " << l;
            expect(e2e::contains(l, "LIB=" + subos_lib.string())) << "the caller's XLINGS_SUBOS_LIB was dropped on the way (the case would prove nothing about the opt-out): " << l;
        }

        const auto intent = rpath_of_graph(graph_of(p));
        const auto actual = rpath_of_artifact(*bin);
        expect(fatal(actual == intent)) << "the artifact's run path is not the one mcpp put on the link line\n  link line: " << joined(intent) << "\n  artifact:  " << joined(actual);
        expect(fatal(!joined(actual).contains(subos_lib.string()))) << "the caller's SubOS directory is in the run path: " << joined(actual);
        expect(fatal(!joined(actual).contains("callers-subos"))) << "the caller's SubOS is in the run path: " << joined(actual);
    };

    "1178 a pack holds no run path outside its bundle"_test[s::pack][s::gcc][s::elf] = [] {
        // Under a caller's XLINGS_SUBOS_LD_PATHS=1 and XLINGS_SUBOS_LIB, no ELF object of the
        // packed program names a directory other than one under its own $ORIGIN: not the
        // caller's SubOS, not the home's store, not the registry's farm.
        const auto tmp = s::case_dir();
        const auto subos_lib = tmp / "callers-subos" / "lib";
        fs::create_directories(subos_lib);
        auto app = s::project::scaffold("bundled");
        auto r = app.mcpp({"pack"}, {.env = callers_env(subos_lib)});
        expect(fatal(e2e::succeeded(r))) << "pack failed\n" << s::text(r);
        std::optional<fs::path> tarball;
        for (const auto& f : s::find_files(app.at("target/dist"), R"(.*\.tar\.gz)")) tarball = f;
        expect(fatal(tarball.has_value())) << "no tarball\n" << s::text(r);
        s::tar_extract(*tarball, tmp / "x");
        std::size_t objects = 0;
        for (const auto& obj : plain_files(tmp / "x")) {
            const auto h = s::read_header(obj);
            if (h.kind != s::format::elf || !h.wide) continue;
            ++objects;
            const auto rel = fs::relative(obj, tmp / "x").generic_string();
            for (const auto& entry : split(h.runpath, ':'))
                expect(entry.starts_with("$ORIGIN")) << rel << " names a run-path directory outside the bundle: " << entry << " (run path " << h.runpath << ")";
        }
        expect(fatal(objects > 0u)) << "the bundle holds no ELF object: nothing was checked";
    };
}};

}  // namespace

// Target sets and the legs of a pack: a set builds every member and runs where it is reached,
// tests built once run from an archive, a pack holds the legs it names, a set of programs is a
// package per leg, a format list writes each container of one tree, single files cover the legs
// and systems, and the universal spellings are migrated (formerly tests/e2e/963, 964, 983, 984,
// 985, 986, 987).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

#if defined(__x86_64__) || defined(_M_X64)
constexpr std::string_view host_arch = "x86_64", other_arch = "aarch64";
#elif defined(__aarch64__) || defined(_M_ARM64)
constexpr std::string_view host_arch = "aarch64", other_arch = "x86_64";
#else
constexpr std::string_view host_arch = "", other_arch = "";
#endif

// The script's `case "$(uname -m)"`: a machine with no ok-image row skips.
void needs_image_row() {
    if (host_arch.empty()) skip("no ok-image row for this machine");
}

// The system's word of a POSIX launcher (`case "$(uname -s)"`), or "".
std::string posix_system() { return s::on_linux() ? "linux" : s::on_macos() ? "macos" : ""; }

std::uint32_t machine_of(std::string_view arch) { return arch == "x86_64" ? s::elf_machine::x86_64 : s::elf_machine::aarch64; }

// `env -i HOME="$HOME" PATH=/usr/bin:/bin <program> <args>`: the program with nothing installed.
e2e::outcome run_bare(const fs::path& program, std::vector<std::string> args) {
    e2e::command c{.argv = {s::on_path("env").value_or("/usr/bin/env").string(), "-i", "HOME=" + s::env("HOME").value_or(""), "PATH=/usr/bin:/bin", program.string()}};
    for (auto& a : args) c.argv.push_back(std::move(a));
    return e2e::run(c);
}

// `PATH="<dir>:$PATH" <program> <args>`.
e2e::outcome run_with_path(const fs::path& dir, const fs::path& program, std::vector<std::string> args) {
    return s::run(program, std::move(args), {}, {{"PATH", dir.string() + ":" + s::env("PATH").value_or("")}});
}

// The platform's runner as `self env --target <t> --format json` locates it.
fs::path runner_of(const s::project& p, std::string_view target) {
    auto r = p.mcpp({"self", "env", "--target", std::string(target), "--format", "json"});
    expect(fatal(e2e::succeeded(r))) << "self env: " << s::text(r);
    const auto d = s::json(r.out)["data"];
    expect(fatal(d["runner"].is_object() && d["runner"]["program"].is_string())) << "the platform's runner was not located: " << r.out;
    return d["runner"]["program"].get<std::string>();
}

// The names of a directory's entries, sorted (`ls`).
std::vector<std::string> names_in(const fs::path& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    if (fs::is_directory(dir, ec))
        for (const auto& e : fs::directory_iterator(dir)) out.push_back(e.path().filename().string());
    std::ranges::sort(out);
    return out;
}

std::vector<std::string> strings(const nlohmann::json& a) {
    std::vector<std::string> out;
    for (const auto& x : a) out.push_back(x.get<std::string>());
    return out;
}

constexpr std::string_view smoke_test = "import std;\nint main() { std::vector<int> v{2, 1}; std::ranges::sort(v); return v.front() == 1 ? 0 : 1; }\n";

// Every member of a target set built: about 17 minutes on a Linux runner beside three other
// programs, measured 2026-11-14.
E2E sets{"sets", timeout{std::chrono::minutes{30}}, [] {
    "963 a target set builds every member and runs where it is reached"_test[s::llvm][s::python3] = [] {
        // `--target openkal`: A builds both legs under target/<triple>/ with one Finished; B the
        // JSON envelope is the first leg's record with both legs, each artifact event naming its
        // leg; C run runs this machine's leg and says which, and --print-launch describes the
        // selected leg; D test runs this machine's leg and reports the other built, not run; E
        // legs that ran no test fail; F self env reports both legs; G refusals: `none`,
        // `linux-msvc`, a set in [build] target.
        needs_image_row();
        const std::string ht = std::string(host_arch) + "-openkal", ot = std::string(other_arch) + "-openkal";
        const auto tmp = s::case_dir();
        s::project app(tmp / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n"},
                                     {"src/main.cpp", "import std;\nint main() { std::println(\"image of the set\"); return 0; }\n"},
                                     {"tests/smoke.cpp", smoke_test}});
        // A
        auto r = app.mcpp({"build", "--target", "openkal"});
        expect(fatal(e2e::succeeded(r))) << "A: mcpp build --target openkal: " << s::text(r);
        for (const auto& t : {ht, ot})
            expect(fatal(!s::glob(app.at("target"), t + "/*/bin/app.oke").empty())) << "A: no image for " << t << ": " << s::text(r);
        expect(fatal(s::count_lines(s::text(r), "Finished") == 1u)) << "A: not exactly one Finished: " << s::text(r);
        // B
        r = app.mcpp({"build", "--target", "openkal", "--message-format", "json"});
        expect(fatal(e2e::succeeded(r))) << "B: json build: " << s::text(r);
        const auto lines = s::json_lines(r.out);
        expect(fatal(!lines.empty())) << "B: no JSON lines: " << r.out;
        const auto& env = lines.back();
        expect(fatal(env["kind"] == "mcpp.build")) << "B: " << env["kind"].dump();
        const auto& legs = env["data"]["legs"];
        expect(fatal(legs.size() == 2u)) << "B: " << legs.dump();
        std::vector<std::string> triples;
        for (const auto& l : legs) triples.push_back(l["configuration"]["triple"].get<std::string>());
        std::ranges::sort(triples);
        std::vector<std::string> want{"x86_64-openkal", "aarch64-openkal"};
        std::ranges::sort(want);
        expect(fatal(triples == want)) << "B: " << legs.dump();
        expect(fatal(env["data"]["configuration"]["triple"] == "x86_64-openkal")) << "B: " << env["data"]["configuration"].dump();
        expect(fatal(env["data"]["status"] == "ok")) << "B: " << env["data"]["status"].dump();
        std::size_t artifacts = 0;
        for (std::size_t i = 0; i + 1 < lines.size(); ++i)
            if (lines[i].value("event", "") == "artifact") {
                ++artifacts;
                expect(fatal(lines[i].contains("triple"))) << "B: an artifact event names no leg: " << lines[i].dump();
            }
        expect(fatal(artifacts > 0)) << "B: no artifact events: " << r.out;
        // C
        r = app.mcpp({"run", "--target", "openkal"});
        expect(fatal(e2e::succeeded(r))) << "C: mcpp run --target openkal: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "image of the set"))) << "C: the program did not run: " << s::text(r);
        expect(fatal(s::has_line(s::text(r), "Leg.*" + ht))) << "C: the run does not say which leg: " << s::text(r);
        // The launch describes the leg run selected, not the first self-env report.
        r = app.mcpp({"run", "--target", ot, "--target", ht, "--print-launch"});
        expect(fatal(e2e::succeeded(r))) << "C: selected leg launch: " << s::text(r);
        const auto data = s::json(r.out)["data"];
        expect(fatal(data["configuration"]["triple"] == ht && data["configuration"]["profile"] == "dev")) << "C: launch configuration is not the selected leg: " << r.out;
        expect(fatal(data["configuration"].contains("platform") && !data["configuration"]["platform"].is_null() && data["configuration"]["platform"] != "" && data["configuration"]["platform"] != false))
            << "C: the launch names no platform: " << r.out;
        // D
        r = app.mcpp({"test", "--target", "openkal"});
        auto out = s::text(r);
        expect(fatal(e2e::succeeded(r))) << "D: mcpp test --target openkal: " << out;
        expect(fatal(e2e::contains(out, "smoke ... ok"))) << "D: this machine's leg did not pass: " << out;
        expect(fatal(e2e::contains(out, "built, not run"))) << "D: the other leg is not reported built, not run: " << out;
        expect(fatal(e2e::contains(out, "legs result"))) << "D: no result over the legs: " << out;
        // E
        r = app.mcpp({"test", "--target", "openkal"}, {.env = {{"MCPP_TARGET_X86_64_OPENKAL_RUN_UNDER", "/nonexistent/execution-host"}, {"MCPP_TARGET_AARCH64_OPENKAL_RUN_UNDER", "/nonexistent/execution-host"}}});
        expect(fatal(r.started && r.status == 1)) << "E: a test over legs that ran nothing exited " << r.status << ", not 1: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "no test ran on any of the 2 legs"))) << "E: the failure does not say so: " << s::text(r);
        // F
        r = app.mcpp({"self", "env", "--target", "openkal", "--format", "json"});
        expect(fatal(e2e::succeeded(r))) << "F: self env: " << s::text(r);
        expect(fatal(s::json(r.out)["data"]["legs"].size() == 2u)) << "F: self env does not report both legs: " << r.out;
        // G
        r = app.mcpp({"build", "--target", "none"});
        expect(fatal(r.started && r.status == 2 && e2e::contains(s::text(r), "none-elf"))) << "G: `none` is not refused naming the environments: " << s::text(r);
        r = app.mcpp({"build", "--target", "linux-msvc"});
        expect(fatal(r.started && r.status == 2 && e2e::contains(s::text(r), "names no target"))) << "G: an empty set is not refused: " << s::text(r);
        s::project bt(tmp / "bt", {{"mcpp.toml", "[package]\nname = \"bt\"\nversion = \"0.1.0\"\n[build]\ntarget = \"openkal\"\n"}, {"src/main.cpp", "int main() {}\n"}});
        r = bt.mcpp({"build"});
        expect(fatal(failed(r) && e2e::contains(s::text(r), "names a target set"))) << "G: a set in [build] target is not refused: " << s::text(r);
    };

    "964 tests built once run from an archive"_test[s::llvm][s::python3] = [] {
        // A `test --no-run --archive` writes every leg's tests with mcpp-tests.json; B in a
        // checkout without target/ `test --archive` runs this machine's leg, reports the other
        // not run, builds nothing; C a failing archived test fails the run; D an archive of no
        // leg this machine reaches fails; E the archive is compressed as its name states, and
        // a name stating no tar archive is refused before a build.
        needs_image_row();
        const std::string ot = std::string(other_arch) + "-openkal";
        const auto tmp = s::case_dir();
        s::project app(tmp / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n"},
                                     {"src/main.cpp", "import std;\nint main() { std::println(\"app\"); }\n"},
                                     {"tests/smoke.cpp", smoke_test}});
        // A
        auto r = app.mcpp({"test", "--target", "openkal", "--no-run", "--archive", (tmp / "t.tar.gz").string()});
        expect(fatal(e2e::succeeded(r))) << "A: --no-run --archive: " << s::text(r);
        expect(fatal(fs::is_regular_file(tmp / "t.tar.gz") && fs::file_size(tmp / "t.tar.gz") > 0)) << "A: no archive written: " << s::text(r);
        s::tar_extract(tmp / "t.tar.gz", tmp / "x");
        const auto manifests = s::glob(tmp / "x", "*/mcpp-tests.json");
        expect(fatal(!manifests.empty())) << "A: no mcpp-tests.json in the archive";
        const auto m = s::json_file(manifests.front());
        expect(fatal(m["schema"] == 1 && m["legs"].size() == 2u)) << "A: the manifest: " << m.dump();
        for (const auto& leg : m["legs"]) {
            bool smoke = false;
            for (const auto& t : leg["tests"]) {
                smoke = smoke || t["name"] == "smoke";
                expect(fatal(fs::is_regular_file(manifests.front().parent_path() / t["program"].get<std::string>()))) << "A: " << t["program"].dump();
            }
            expect(fatal(smoke)) << "A: " << leg["triple"].dump() << " has no smoke test";
        }
        // B: a checkout with nothing built
        s::project co(tmp / "co", {});
        for (std::string_view e : {"src", "tests", "mcpp.toml"}) fs::copy(app.at(e), co.at(e), fs::copy_options::recursive);
        r = co.mcpp({"test", "--archive", (tmp / "t.tar.gz").string()});
        expect(fatal(e2e::succeeded(r))) << "B: mcpp test --archive: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "smoke ... ok"))) << "B: this machine's leg did not pass: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "not run here"))) << "B: the other leg is not reported not run: " << s::text(r);
        // Nothing of the package is built here: no test program and no image of it.
        if (fs::is_directory(co.at("target")))
            for (auto it = fs::recursive_directory_iterator(co.at("target")); it != fs::recursive_directory_iterator(); ++it) {
                if (it->path().filename() == ".test-archive") { it.disable_recursion_pending(); continue; }
                const auto name = it->path().filename().string();
                expect(fatal(!name.starts_with("smoke") && name != "app.oke")) << "B: the archive's tests were built again: " << it->path();
            }
        // C
        app.write("tests/broken.cpp", "int main() { return 3; }\n");
        r = app.mcpp({"test", "--target", "openkal", "--no-run", "--archive", (tmp / "c.tar.gz").string()});
        expect(fatal(e2e::succeeded(r))) << "C: archive with a failing test: " << s::text(r);
        r = co.mcpp({"test", "--archive", (tmp / "c.tar.gz").string()});
        expect(fatal(r.started && r.status == 1)) << "C: a failing archived test exited " << r.status << ", not 1: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "broken ... FAIL"))) << "C: the failure is not named: " << s::text(r);
        // D
        app.remove("tests/broken.cpp");
        r = app.mcpp({"test", "--target", ot, "--no-run", "--archive", (tmp / "d.tar.gz").string()});
        expect(fatal(e2e::succeeded(r))) << "D: archive of " << ot << ": " << s::text(r);
        r = co.mcpp({"test", "--archive", (tmp / "d.tar.gz").string()});
        expect(fatal(r.started && r.status == 1)) << "D: an archive this machine reaches no leg of exited " << r.status << ", not 1: " << s::text(r);
        expect(fatal(s::has_line(s::text(r), "no test of .* ran on this machine"))) << "D: the failure does not say so: " << s::text(r);
        // E
        r = app.mcpp({"test", "--target", std::string(host_arch) + "-openkal", "--no-run", "--archive", (tmp / "e.tar.zst").string()});
        expect(fatal(e2e::succeeded(r))) << "E: --archive e.tar.zst: " << s::text(r);
        expect(fatal(s::read_file(tmp / "e.tar.zst").starts_with("\x28\xb5\x2f\xfd"))) << "E: e.tar.zst is no zstd frame";
        r = co.mcpp({"test", "--archive", (tmp / "e.tar.zst").string()});
        expect(fatal(e2e::succeeded(r))) << "E: mcpp test --archive e.tar.zst: " << s::text(r);
        fs::remove_all(app.at("target"));
        r = app.mcpp({"test", "--no-run", "--archive", (tmp / "e.zip").string()});
        expect(fatal(r.started && r.status == 2)) << "E: --archive e.zip exited " << r.status << ", not 2: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "states no tar archive"))) << "E: the refusal does not say why: " << s::text(r);
        expect(fatal(!fs::is_directory(app.at("target")))) << "E: something was built before the name was refused";
    };

    "983 a pack holds the legs it names"_test[s::llvm][s::python3][s::unix_shell] = [] {
        // A `--target <host>-openkal` is one leg in the tree layout, and the summary says how to
        // pack them all; B `--target openkal` is both legs; C `--systems <os>-<arch>` carries
        // that runner alone and says where it runs; D the JSON envelope describes legs, formats
        // and where it runs; E `--merge` of two one-leg packages writes both legs' launchers.
        needs_image_row();
        const auto os = posix_system();
        if (os.empty()) skip("a POSIX launcher");
        const std::string ht = std::string(host_arch) + "-openkal", pair = os + "-" + std::string(host_arch);
        s::project app({{"mcpp.toml", "[package]\nname    = \"legs\"\nversion = \"0.1.0\"\n"}, {"src/main.cpp", "import std;\nint main(int argc, char**) { std::println(\"legs {} args\", argc); }\n"}});
        // A
        auto r = app.mcpp({"pack", "--target", ht, "--pkg-format", "dir"});
        expect(fatal(e2e::succeeded(r))) << "A: pack: " << s::text(r);
        const auto p = app.at("target/dist/legs-0.1.0-" + ht);
        for (const auto& f : {std::format("lib/{}/legs.oke", host_arch), std::string("bin/legs"), std::string("bin/legs.exe"), std::string("mcpp-package.json")})
            expect(fatal(fs::is_regular_file(p / f))) << "A: the package has no " << f << ": " << s::text(r);
        expect(fatal(!fs::is_directory(p / "lib" / other_arch))) << "A: one leg named, and the package holds " << other_arch;
        expect(fatal(!fs::is_directory(app.at("target/dist/legs-0.1.0-openkal")))) << "A: one leg named, and a tree of the platform was written";
        auto d = s::json_file(p / "mcpp-package.json");
        expect(fatal(strings(d["legs"]) == std::vector<std::string>{ht})) << "A: " << d["legs"].dump();
        expect(fatal(d["launch"]["runner"][0].get<std::string>().starts_with("okrun"))) << "A: " << d["launch"].dump();
        expect(fatal(e2e::contains(s::text(r), std::format("only {}; `--target openkal` or `[pack] targets = [\"openkal\"]` packs every architecture", host_arch))))
            << "A: the summary does not say how to pack every architecture: " << s::text(r);
        // B
        r = app.mcpp({"pack", "--target", "openkal", "--pkg-format", "dir"});
        expect(fatal(e2e::succeeded(r))) << "B: --target openkal: " << s::text(r);
        const auto b = app.at("target/dist/legs-0.1.0-openkal");
        for (std::string_view f : {"lib/x86_64/legs.oke", "lib/aarch64/legs.oke", "bin/legs", "bin/legs.exe"})
            expect(fatal(fs::is_regular_file(b / f))) << "B: the package has no " << f << ": " << s::text(r);
        expect(fatal(s::lacks(s::text(r), std::format("only {}", host_arch)))) << "B: the summary of both legs says one: " << s::text(r);
        const auto okrun = runner_of(app, ht);
        auto ran = run_with_path(okrun.parent_path(), b / "bin/legs", {"x", "y"});
        expect(fatal(e2e::succeeded(ran))) << "B: the launcher: " << s::text(ran);
        expect(fatal(e2e::contains(s::text(ran), "legs 3 args"))) << "B: the launcher's output: " << s::text(ran);
        // C
        r = app.mcpp({"pack", "--target", "openkal", "--mode", "self-contained", "--systems", pair, "--pkg-format", "dir", "-o", "one-runner"});
        expect(fatal(e2e::succeeded(r))) << "C: --systems " << pair << ": " << s::text(r);
        const auto c = app.at("target/dist/one-runner");
        expect(fatal(fs::is_directory(c / "libexec/okrun" / pair))) << "C: no runner for " << pair << ": " << s::text(r);
        expect(fatal(names_in(c / "libexec/okrun") == std::vector<std::string>{pair})) << "C: runners other than " << pair;
        expect(fatal(e2e::contains(s::text(r), "Runs on " + pair))) << "C: the summary does not say where it runs: " << s::text(r);
        d = s::json_file(c / "mcpp-package.json");
        expect(fatal(strings(d["runs_on"]["self"]) == std::vector<std::string>{pair})) << "C: " << d["runs_on"].dump();
        expect(fatal(strings(d["systems"]) == std::vector<std::string>{pair})) << "C: " << d["systems"].dump();
        ran = run_bare(c / "bin/legs", {"z"});
        expect(fatal(e2e::succeeded(ran))) << "C: the self-contained launcher: " << s::text(ran);
        expect(fatal(e2e::contains(s::text(ran), "legs 2 args"))) << "C: the self-contained package's output: " << s::text(ran);
        // D
        r = app.mcpp({"pack", "--target", "openkal", "--mode", "self-contained", "--systems", os, "--message-format", "json"});
        expect(fatal(e2e::succeeded(r))) << "D: json: " << s::text(r);
        const auto e = s::json(r.out)["data"];
        expect(fatal(strings(e["legs"]) == std::vector<std::string>{"x86_64-openkal", "aarch64-openkal"})) << "D: " << e["legs"].dump();
        expect(fatal(e["pkgFormat"] == "tar" && strings(e["pkgFormats"]) == std::vector<std::string>{"tar"})) << "D: " << r.out;
        expect(fatal(std::ranges::contains(strings(e["runsOn"]["self"]), pair) && e["runsOn"]["with"].is_object())) << "D: " << e["runsOn"].dump();
        // E
        r = app.mcpp({"pack", "--target", ht, "--mode", "self-contained", "--pkg-format", "dir"});
        expect(fatal(e2e::succeeded(r))) << "E: pack " << ht << ": " << s::text(r);
        r = app.mcpp({"pack", "--target", std::string(other_arch) + "-openkal", "--mode", "self-contained", "--pkg-format", "dir"});
        expect(fatal(e2e::succeeded(r))) << "E: pack " << other_arch << "-openkal: " << s::text(r);
        r = app.mcpp({"pack", "--merge", std::format("target/dist/legs-0.1.0-{}-bundle-all", ht), "--merge", std::format("target/dist/legs-0.1.0-{}-openkal-bundle-all", other_arch), "--pkg-format", "dir"});
        expect(fatal(e2e::succeeded(r))) << "E: --merge of two one-leg packages: " << s::text(r);
        const auto merged = app.at("legs-0.1.0-openkal-bundle-all");
        expect(fatal(fs::is_directory(merged))) << "E: the merged tree is not named after the platform: " << s::text(r);
        const auto launcher = s::read_file(merged / "bin/legs");
        expect(fatal(e2e::contains(launcher, "'x86_64') entry='legs.oke'") && e2e::contains(launcher, "'aarch64') entry='legs.oke'"))) << "E: the merged launcher does not name both legs: " << launcher;
        ran = run_bare(merged / "bin/legs", {"q"});
        expect(fatal(e2e::succeeded(ran))) << "E: the merged launcher: " << s::text(ran);
        expect(fatal(e2e::contains(s::text(ran), "legs 2 args"))) << "E: the merged package's output: " << s::text(ran);
    };

    "984 a set of programs is a package per leg"_test[s::gcc][s::python3] = [] {
        // `--target linux-musl`: A a package per leg named by its triple below the `-o`
        // directory, each holding the program of its architecture; B `-o` naming one file is
        // refused before a build; C the JSON envelope lists both packages and legs.
        if (!s::on_linux()) skip("the linux-musl rows are built on Linux");
        s::project app({{"mcpp.toml", "[package]\nname    = \"per\"\nversion = \"0.1.0\"\n"}, {"src/main.cpp", "import std;\nint main() { std::println(\"per leg\"); }\n"}});
        // A
        auto r = app.mcpp({"pack", "--target", "linux-musl", "-o", "out/"});
        if (!e2e::succeeded(r) && s::has_line(s::text(r), "cannot be built on this host|install_packages failed|not found in the synced index"))
            skip("a linux-musl row cannot be built here");
        expect(fatal(e2e::succeeded(r))) << "A: pack --target linux-musl: " << s::text(r);
        for (std::string_view arch : {"x86_64", "aarch64"}) {
            const auto f = app.at(std::format("out/per-0.1.0-{}-linux-musl-static.tar.gz", arch));
            expect(fatal(fs::is_regular_file(f))) << "A: no " << f << ": " << s::text(r);
            std::string member;
            for (const auto& m : s::tar_list(f))
                if (m.ends_with("/bin/per")) { member = m; break; }
            expect(fatal(!member.empty())) << "A: the " << arch << " package has no bin/per";
            const auto into = s::case_dir() / std::format("x-{}", arch);
            s::tar_extract(f, into, {member});
            const auto h = s::read_header(into / member);
            expect(fatal(h.kind == s::format::elf && h.machine == machine_of(arch))) << "A: the " << arch << " package's program is machine " << h.machine;
        }
        // B
        r = app.mcpp({"pack", "--target", "linux-musl", "-o", "one.tar.gz"});
        expect(fatal(!e2e::succeeded(r))) << "B: -o one file was accepted: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "use: -o <directory>/"))) << "B: the refusal does not name a directory: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), "Compiling"))) << "B: refused after compiling: " << s::text(r);
        // C
        r = app.mcpp({"pack", "--target", "linux-musl", "--message-format", "json"});
        expect(fatal(e2e::succeeded(r))) << "C: json: " << s::text(r);
        const auto d = s::json(r.out)["data"];
        auto legs = strings(d["legs"]);
        std::ranges::sort(legs);
        expect(fatal(legs == std::vector<std::string>{"aarch64-linux-musl", "x86_64-linux-musl"})) << "C: " << d["legs"].dump();
        expect(fatal(d["artifacts"].size() == 2u)) << "C: " << d["artifacts"].dump();
    };

    "985 a format list writes each container of one tree"_test[s::llvm][s::python3][s::unix_shell] = [] {
        // A `--pkg-format tar,zip -o out/` writes both containers of one tree, compiled once;
        // B `dir` or `single-file` in a list, `-o` naming one file with a list and `mcpp run`
        // with a list are refused before a build; C `tar,okb` writes the tree's tar and the
        // platform's okb, and okb alone is refused under self-contained; D `-o target/dist/`
        // keeps what is written there.
        s::project app({{"mcpp.toml", "[package]\nname    = \"lst\"\nversion = \"0.1.0\"\n"}, {"src/main.cpp", "import std;\nint main() { std::println(\"list\"); }\n"}});
        // A
        auto r = app.mcpp({"pack", "--pkg-format", "tar,zip", "-o", "out/", "--message-format", "json"});
        expect(fatal(e2e::succeeded(r))) << "A: tar,zip: " << s::text(r);
        const auto d = s::json(r.out)["data"];
        expect(fatal(d["pkgFormat"] == "tar" && strings(d["pkgFormats"]) == std::vector<std::string>{"tar", "zip"})) << "A: " << r.out;
        std::vector<std::string> paths;
        for (const auto& a : d["artifacts"]) paths.push_back(a["path"].get<std::string>());
        expect(fatal(paths.size() == 2u && std::ranges::all_of(paths, [](const std::string& p) { return fs::path(p).parent_path().generic_string().ends_with("/out"); }))) << "A: " << d["artifacts"].dump();
        const auto tgz = std::ranges::find_if(paths, [](const std::string& p) { return p.ends_with(".tar.gz"); });
        const auto zp = std::ranges::find_if(paths, [](const std::string& p) { return p.ends_with(".zip"); });
        expect(fatal(tgz != paths.end() && zp != paths.end())) << "A: " << d["artifacts"].dump();
        std::vector<std::string> in_tar, in_zip;
        for (const auto& m : s::tar_list(*tgz))
            if (!m.ends_with('/')) in_tar.push_back(m);
        for (const auto& z : s::zip_entries(*zp))
            if (!z.name.ends_with('/')) in_zip.push_back(z.name);
        std::ranges::sort(in_tar);
        std::ranges::sort(in_zip);
        expect(fatal(in_tar == in_zip)) << "A: the tar and the zip hold different files";
        expect(fatal(s::count_lines(r.err, "Compiling lst") <= 1u)) << "A: the program was compiled more than once: " << r.err;
        // B
        const auto refused = [&](std::string_view label, std::vector<std::string> args) {
            auto x = app.mcpp(std::move(args));
            expect(fatal(!e2e::succeeded(x))) << "B: " << label << " was accepted: " << s::text(x);
            expect(fatal(s::lacks(s::text(x), "Compiling"))) << "B: " << label << " was refused after compiling: " << s::text(x);
            return s::text(x);
        };
        auto out = refused("dir in a list", {"pack", "--pkg-format", "tar,dir"});
        expect(fatal(e2e::contains(out, "lists the containers of one staged tree"))) << "B: the refusal of dir: " << out;
        refused("single-file in a list", {"pack", "--pkg-format", "single-file,tar"});
        out = refused("-o one file with a list", {"pack", "--pkg-format", "tar,zip", "-o", "one.tar.gz"});
        expect(fatal(e2e::contains(out, "use: -o <directory>/"))) << "B: the refusal of -o: " << out;
        out = refused("mcpp run with a list", {"run", "--pkg-format", "tar,zip"});
        expect(fatal(e2e::contains(out, "names one format"))) << "B: the refusal of run: " << out;
        // C (the script passed here on a machine with no ok-image row)
        if (host_arch.empty()) return;
        r = app.mcpp({"pack", "--target", "openkal", "--mode", "self-contained", "--pkg-format", "tar,okb", "-o", "kit/"});
        expect(fatal(e2e::succeeded(r))) << "C: tar,okb: " << s::text(r);
        expect(fatal(fs::is_regular_file(app.at("kit/lst-0.1.0-openkal-bundle-all.tar.gz")))) << "C: no tar: " << s::text(r);
        expect(fatal(!s::glob(app.at("kit"), "*.okb").empty())) << "C: no okb: " << s::text(r);
        r = app.mcpp({"pack", "--target", "openkal", "--mode", "self-contained", "--pkg-format", "okb"});
        expect(fatal(!e2e::succeeded(r))) << "C: okb alone was accepted under self-contained: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "the runner would be dropped"))) << "C: the refusal: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "use: --pkg-format tar,okb"))) << "C: the refusal names no remedy: " << s::text(r);
        // D
        r = app.mcpp({"pack", "--target", "openkal", "--mode", "self-contained", "--systems", "all", "--pkg-format", "tar,okb", "-o", "target/dist/"});
        expect(fatal(e2e::succeeded(r))) << "D: -o target/dist/: " << s::text(r);
        expect(fatal(fs::is_regular_file(app.at("target/dist/lst-0.1.0-openkal-bundle-all.tar.gz")))) << "D: no tar in target/dist/: " << s::text(r);
        expect(fatal(fs::is_regular_file(app.at("target/dist/lst-0.1.0.okb")))) << "D: no okb in target/dist/: " << s::text(r);
    };

    "986 single files cover the legs and systems"_test[s::llvm][s::python3][s::unix_shell] = [] {
        // `--target openkal --pkg-format single-file`: A a file per architecture on this
        // system, this machine's runs; B `--systems <os>-<host>` makes one; C (Linux)
        // `--systems all` makes every file this mcpp can and says the rest; D `-o` naming one
        // file with two is refused before a build.
        needs_image_row();
        const auto os = posix_system();
        if (os.empty()) skip("a POSIX host");
        s::project app({{"mcpp.toml", "[package]\nname    = \"sf\"\nversion = \"0.1.0\"\n"}, {"src/main.cpp", "import std;\nint main(int argc, char**) { std::println(\"single {} args\", argc); }\n"}});
        // A
        auto r = app.mcpp({"pack", "--target", "openkal", "--pkg-format", "single-file", "-o", "files/"});
        expect(fatal(e2e::succeeded(r))) << "A: pack: " << s::text(r);
        for (std::string_view arch : {"x86_64", "aarch64"})
            expect(fatal(fs::is_regular_file(app.at(std::format("files/sf-0.1.0-{}-{}", os, arch))))) << "A: no file for " << os << "-" << arch << ": " << s::text(r);
        auto ran = s::run(app.at(std::format("files/sf-0.1.0-{}-{}", os, host_arch)), {"x"});
        expect(fatal(e2e::succeeded(ran))) << "A: this machine's file: " << s::text(ran);
        expect(fatal(e2e::contains(s::text(ran), "single 2 args"))) << "A: the file's output: " << s::text(ran);
        // B
        const auto pair = std::format("{}-{}", os, host_arch);
        r = app.mcpp({"pack", "--target", "openkal", "--pkg-format", "single-file", "--systems", pair, "-o", "one/"});
        expect(fatal(e2e::succeeded(r))) << "B: --systems " << pair << ": " << s::text(r);
        expect(fatal(names_in(app.at("one")) == std::vector<std::string>{"sf-0.1.0-" + pair})) << "B: the files are not the one pair: " << s::text(r);
        // C: `all` is the system table; FreeBSD's runner states the identity it is built as.
        if (os == "linux") {
            r = app.mcpp({"pack", "--target", "openkal", "--pkg-format", "single-file", "--systems", "all", "-o", "all/"});
            expect(fatal(e2e::succeeded(r))) << "C: --systems all: " << s::text(r);
            for (std::string_view p : {"linux-x86_64", "linux-aarch64", "windows-x86_64", "macos-aarch64", "freebsd-x86_64", "freebsd-aarch64"})
                expect(fatal(!s::glob(app.at("all"), std::format("sf-0.1.0-{}*", p)).empty())) << "C: no file for " << p << ": " << s::text(r);
            expect(fatal(e2e::contains(s::text(r), "Not covered macos-x86_64 (no carrier row"))) << "C: the pair without a row is not said: " << s::text(r);
        }
        // D
        r = app.mcpp({"pack", "--target", "openkal", "--pkg-format", "single-file", "-o", "one-file"});
        expect(fatal(!e2e::succeeded(r))) << "D: -o one file with two files was accepted: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "use: -o <directory>/"))) << "D: the refusal: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), "Compiling"))) << "D: refused after compiling: " << s::text(r);
    };

    "987 the universal spellings are migrated"_test[s::llvm][s::python3] = [] {
        // A `--no-universal` with one target packs the leg and notes the layout; B with a set it
        // is refused; C `[pack] universal = true` is refused naming `[pack] targets`, `false`
        // is noted; D `[pack] targets = ["openkal"]` packs both legs; E `pack --help` puts the
        // common options first.
        needs_image_row();
        const std::string ht = std::string(host_arch) + "-openkal";
        s::project app({{"mcpp.toml", "[package]\nname    = \"mig\"\nversion = \"0.1.0\"\n"}, {"src/main.cpp", "import std;\nint main() { std::println(\"mig\"); }\n"}});
        // A
        auto r = app.mcpp({"pack", "--no-universal", "--target", ht, "--pkg-format", "dir"});
        expect(fatal(e2e::succeeded(r))) << "A: --no-universal: " << s::text(r);
        expect(fatal(fs::is_regular_file(app.at(std::format("target/dist/mig-0.1.0-{}/lib/{}/mig.oke", ht, host_arch))))) << "A: the one leg's tree: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "note: --no-universal is what a pack of one target does"))) << "A: the note: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "--pkg-format single-file"))) << "A: the note names no single file: " << s::text(r);
        // B
        r = app.mcpp({"pack", "--no-universal", "--target", "openkal"});
        expect(fatal(!e2e::succeeded(r))) << "B: --no-universal with a set was accepted: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "contradict"))) << "B: the refusal: " << s::text(r);
        // C
        const auto base = app.read("mcpp.toml");
        app.write("mcpp.toml", base + "\n[pack]\nuniversal = true\n");
        r = app.mcpp({"pack", "--target", ht});
        expect(fatal(!e2e::succeeded(r))) << "C: [pack] universal = true was accepted: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), R"(use: [pack] targets = ["openkal"])"))) << "C: the refusal names no remedy: " << s::text(r);
        app.write("mcpp.toml", base + "\n[pack]\nuniversal = false\n");
        r = app.mcpp({"pack", "--target", ht, "--pkg-format", "dir"});
        expect(fatal(e2e::succeeded(r))) << "C: [pack] universal = false: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "note: [pack] universal = false is what a pack of one target does; it is ignored"))) << "C: the note: " << s::text(r);
        // D
        app.write("mcpp.toml", base + "\n[pack]\ntargets = [\"openkal\"]\n");
        r = app.mcpp({"pack", "--pkg-format", "dir"});
        expect(fatal(e2e::succeeded(r))) << "D: [pack] targets: " << s::text(r);
        for (std::string_view arch : {"x86_64", "aarch64"})
            expect(fatal(fs::is_regular_file(app.at(std::format("target/dist/mig-0.1.0-openkal/lib/{}/mig.oke", arch))))) << "D: no " << arch << " leg: " << s::text(r);
        // E
        r = app.mcpp({"pack", "--help"});
        expect(fatal(e2e::succeeded(r))) << "E: pack --help: " << s::text(r);
        const auto text = s::text(r);
        const auto common = text.find("Options:"), advanced = text.find("Advanced options:");
        expect(fatal(common != std::string::npos && advanced != std::string::npos && common < advanced)) << "E: the groups are not in order: " << text;
        for (std::string_view flag : {"--target", "--mode", "--pkg-format", "--systems", "--linkage", "--output"}) {
            const auto at = text.find(flag);
            expect(fatal(at != std::string::npos && common < at && at < advanced)) << "E: " << flag << " is not among the common options: " << text;
        }
        for (std::string_view flag : {"--compress", "--merge", "--no-universal"}) {
            const auto at = text.find(flag);
            expect(fatal(at != std::string::npos && at > advanced)) << "E: " << flag << " is not among the advanced options: " << text;
        }
    };
}};

}  // namespace

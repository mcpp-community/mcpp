// The case of e2e/openkal that holds the machine (nothing else of this program runs beside it):
// an ok image needs no declaration, down to `toolchain install --target` and an offline run
// (formerly tests/e2e/951). It is a program of its own so that the resource serialises it.
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

// `grep -A5 '"<name>"' <lock> | grep -q 'origin  = "platform"'`: a line naming the package, or
// one of the five after it, records the platform as its origin.
bool locked_as_platforms(std::string_view lock, std::string_view name) {
    std::vector<std::string> lines;
    std::istringstream in{std::string(lock)};
    for (std::string l; std::getline(in, l);) lines.push_back(l);
    const auto quoted = std::format("\"{}\"", name);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (!lines[i].contains(quoted)) continue;
        for (std::size_t j = i; j < lines.size() && j <= i + 5; ++j)
            if (lines[j].contains("origin  = \"platform\"")) return true;
    }
    return false;
}

E2E machine{"machine", [] {
    "951 an ok image needs no declaration"_test[tag{"requires:llvm"}][tag{"requires:python3"}][resource{"machine"}] = [] {
        // The target table pins openkal-platform for the ok-image rows: A a project naming only
        // its sources runs as an image; B the report names the platform and what it added; C
        // the lock records those packages as the platform's, and a build for this machine
        // keeps it; D test and a self-contained pack work; E self env and toolchain list state
        // the platform; F a version outside its group is refused; G `--platform none` selects
        // none; H after `toolchain install --target` a project runs `--offline`.
        s::needs({"llvm", "python3"});
#if defined(__x86_64__) || defined(_M_X64)
        const std::string t = "x86_64-openkal";
#elif defined(__aarch64__) || defined(_M_ARM64)
        const std::string t = "aarch64-openkal";
#else
        skip("no ok-image row for this machine");
        const std::string t;
#endif
        const auto tmp = s::case_dir();
        const auto main_cpp = "import std;\nint main(int argc, char** argv) {\n    try { throw std::runtime_error(\"caught\"); }\n    catch (const std::exception& e) { std::println(\"image: {} ({} args)\", e.what(), argc); }\n    return 0;\n}\n";
        s::project app(tmp / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n"},
                                     {"src/main.cpp", main_cpp},
                                     {"tests/smoke.cpp", "import std;\nint main() { std::vector<int> v{2, 1}; std::ranges::sort(v); return v.front() == 1 ? 0 : 1; }\n"}});
        // A
        auto r = app.mcpp({"run", "--target", t, "--", "one", "two"});
        expect(fatal(e2e::succeeded(r))) << "A: the image did not build or run: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "image: caught (3 args)"))) << "A: the image's output: " << s::text(r);
        // B
        r = app.mcpp({"build", "--target", t, "-v"});
        expect(fatal(e2e::succeeded(r))) << "B: rebuild: " << s::text(r);
        fs::remove_all(app.at("target"));
        r = app.mcpp({"build", "--target", t});
        auto out = s::text(r);
        expect(fatal(e2e::succeeded(r))) << "B: fresh build: " << out;
        // The source is the target table's column, or the variable a run before the platform's
        // release selects it with.
        expect(fatal(s::has_line(out, R"(Platform +openkal-platform [0-9.]+ \((built-in|MCPP_TARGET_[A-Z0-9_]+_PLATFORM)\))"))) << "B: no Platform line naming the source: " << out;
        expect(fatal(s::has_line(out, R"(c-abi +musl +\(openkal-musl@[0-9.]+, platform\))"))) << "B: the C library is not reported as the platform's: " << out;
        expect(fatal(s::has_line(out, R"(c\+\+-abi +libc\+\+ +\(openkal-llvm-runtime@[0-9.]+, platform\))"))) << "B: the C++ run-time is not the platform's: " << out;
        expect(fatal(s::has_line(out, R"(tools +okld, okprof, okrun \(openkal-tools@)"))) << "B: the tools are not reported: " << out;
        // C
        const auto lock = app.read("mcpp.lock");
        for (std::string_view name : {"openkal-musl", "openkal-llvm-runtime", "openkal-tools"})
            expect(fatal(locked_as_platforms(lock, name))) << "C: " << name << " is not locked as the platform's: " << lock;
        r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "C: the build for this machine: " << s::text(r);
        expect(fatal(app.read("mcpp.lock") == lock)) << "C: the build for this machine rewrote the lock: " << app.read("mcpp.lock");
        // D
        r = app.mcpp({"test", "--target", t});
        expect(fatal(e2e::succeeded(r))) << "D: mcpp test: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "smoke ... ok"))) << "D: the test image did not pass: " << s::text(r);
        r = app.mcpp({"pack", "--target", t, "--mode", "self-contained", "--pkg-format", "dir", "-o", "m3"});
        expect(fatal(e2e::succeeded(r))) << "D: pack: " << s::text(r);
        const auto launcher = app.at("target/dist/m3/bin/app");
        expect(fatal(fs::is_regular_file(launcher) && (fs::status(launcher).permissions() & fs::perms::owner_exec) != fs::perms::none)) << "D: the package has no launcher: " << s::text(r);
        // `env -i HOME="$HOME" PATH=/usr/bin:/bin <launcher> x`
        auto ran = e2e::run({.argv = {s::on_path("env").value_or("/usr/bin/env").string(), "-i", "HOME=" + s::env("HOME").value_or(""), "PATH=/usr/bin:/bin", launcher.string(), "x"}});
        expect(fatal(e2e::succeeded(ran))) << "D: the launcher did not run: " << s::text(ran);
        expect(fatal(e2e::contains(s::text(ran), "image: caught (2 args)"))) << "D: the package's image: " << s::text(ran);
        // E
        r = app.mcpp({"self", "env", "--target", t, "--format", "json"});
        expect(fatal(e2e::succeeded(r))) << "E: self env: " << s::text(r);
        auto d = s::json(r.out)["data"];
        const auto& p = d["platform"];
        expect(fatal(p.is_object() && p["name"].get<std::string>().ends_with("openkal-platform"))) << "E: " << p.dump();
        const auto source = p["source"].get<std::string>();
        expect(fatal(source == "built-in" || source.starts_with("MCPP_TARGET_"))) << "E: " << source;
        expect(fatal(p["fields"]["runner"]["source"] == "platform")) << "E: " << p["fields"].dump();
        expect(fatal(p["fields"]["layers.c-abi"]["source"] == "platform")) << "E: " << p["fields"].dump();
        auto program = d["runner"].is_object() && d["runner"]["program"].is_string() ? d["runner"]["program"].get<std::string>() : std::string{};
        std::ranges::replace(program, '\\', '/');
        expect(fatal(!program.empty() && fs::path(program).filename().string().starts_with("okrun"))) << "E: " << d["runner"].dump();
        r = app.mcpp({"toolchain", "list", "--format", "json"});
        expect(fatal(e2e::succeeded(r))) << "E: toolchain list: " << r.out;
        std::optional<nlohmann::json> row;
        for (const auto& x : s::json(r.out)["data"]["targets"])
            if (x["target"] == t) { row = x; break; }
        expect(fatal(row && (*row)["platform"].get<std::string>().substr(0, (*row)["platform"].get<std::string>().find('@')).ends_with("openkal-platform")))
            << "E: toolchain list's platform column: " << r.out;
        // F
        s::project own(tmp / "own", {{"mcpp.toml", "[package]\nname    = \"own\"\nversion = \"0.1.0\"\n\n[dependencies]\nopenkal-musl = { version = \"0.22.2\", default-features = false, features = [\"posix\"] }\n"},
                                     {"src/main.cpp", main_cpp}});
        r = own.mcpp({"build", "--target", t});
        out = s::text(r);
        expect(fatal(!e2e::succeeded(r))) << "F: a version outside the platform's group was built: " << out;
        expect(fatal(e2e::contains(out, "version group"))) << "F: the refusal does not name the group: " << out;
        expect(fatal(s::has_line(out, R"(names openkal-musl [0-9]+\.[0-9]+\.[0-9]+)"))) << "F: the refusal does not name the group's version: " << out;
        // G
        r = own.mcpp({"self", "env", "--target", t, "--platform", "none", "--format", "json"});
        expect(fatal(e2e::succeeded(r))) << "G: self env --platform none: " << s::text(r);
        expect(fatal(s::json(r.out)["data"]["platform"].is_null())) << "G: --platform none still selected a platform: " << r.out;
        // H
        s::project off(tmp / "off", {{"mcpp.toml", "[package]\nname    = \"off\"\nversion = \"0.1.0\"\n"}, {"src/main.cpp", "import std;\nint main() { std::println(\"offline image\"); }\n"}});
        r = off.mcpp({"toolchain", "install", "--target", t});
        expect(fatal(e2e::succeeded(r))) << "H: toolchain install --target: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "Installed openkal-platform"))) << "H: the install does not name the platform: " << s::text(r);
        r = off.mcpp({"run", "--offline", "--target", t});
        expect(fatal(e2e::succeeded(r))) << "H: run --offline after the install: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "offline image"))) << "H: the offline run's output: " << s::text(r);
    };
}};

}  // namespace

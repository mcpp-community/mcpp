// What a project asks of xlings: declared [xlings] dependencies and their failures, the one
// [xlings.workspace] table, tool tiers by verb and feature, a feature's tool directory, the
// declared runtime payload, and a borrowed registry (formerly tests/e2e/327, 331, 335, 614, 737,
// 1067). 614 holds the machine; it is the one case of this program that does.
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

const s::options no_auto{.env = {{"MCPP_NO_AUTO_INSTALL", "1"}}};

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

E2E xlings{"xlings", [] {
    "327 declared xlings deps report their own failure"_test[s::unix_shell] = [] {
        // An unprovisionable [xlings] dependency: MCPP_NO_AUTO_INSTALL refuses naming the knob
        // and the package (announcing no provisioning); MCPP_OFFLINE refuses naming the offline
        // knob; a failed provisioning fails two builds in a row; a pre-upgrade stamp keeps an
        // auto-install-off build working and does not leak to another project.
        const auto tmp = s::case_dir();
        constexpr std::string_view pkg = "definitely-not-a-real-package-540";
        const auto manifest = [&](std::string_view name) {
            return std::format("[package]\nname    = \"{}\"\nversion = \"0.1.0\"\n\n[xlings]\ndeps = [\"{}\"]\n", name, pkg);
        };
        s::project proj(tmp / "proj", {{"mcpp.toml", manifest("provproj")}, {"src/main.cpp", "int main() { return 0; }\n"}});
        auto r = proj.mcpp({"build"}, no_auto);
        auto t = s::text(r);
        expect(fatal(failed(r))) << "MCPP_NO_AUTO_INSTALL did not refuse: " << t;
        expect(fatal(e2e::contains(t, "MCPP_NO_AUTO_INSTALL"))) << "the refusal does not name MCPP_NO_AUTO_INSTALL: " << t;
        expect(fatal(e2e::contains(t, pkg))) << "the refusal does not name the package to install: " << t;
        expect(fatal(s::lacks(t, "Provisioning"))) << "it announced provisioning and then refused: " << t;
        r = proj.mcpp({"build"}, {.env = {{"MCPP_OFFLINE", "1"}}});
        t = s::text(r);
        expect(fatal(failed(r))) << "MCPP_OFFLINE did not refuse: " << t;
        expect(fatal(e2e::contains(t, "MCPP_OFFLINE") || e2e::contains(t, "--offline"))) << "the refusal does not name the offline knob: " << t;
        auto first = proj.mcpp({"build"});
        auto second = proj.mcpp({"build"});
        expect(fatal(failed(first))) << "an unprovisionable [xlings] dep built successfully: " << s::text(first);
        expect(fatal(failed(second))) << "the second build succeeded: the failed provisioning was recorded as done: " << s::text(second);
        for (const auto* o : {&first, &second})
            expect(fatal(e2e::contains(s::text(*o), pkg))) << "a build does not name the package that could not be provisioned: " << s::text(*o);

        s::project legacy(tmp / "legacy", {{"mcpp.toml", manifest("legacyproj")}, {"src/main.cpp", "int main() { return 0; }\n"},
                                           {".mcpp/.xlings-deps.stamp", std::string(pkg) + "\n"}});
        r = legacy.mcpp({"build"}, no_auto);
        expect(fatal(e2e::succeeded(r))) << "moving the stamp refused a build that a pre-upgrade mcpp allowed: " << s::text(r);
        r = proj.mcpp({"build"}, no_auto);
        expect(fatal(failed(r))) << "a project with no legacy stamp was let through by another project's: " << s::text(r);
    };

    "331 xlings workspace one table"_test[s::unix_shell] = [] {
        // [xlings.workspace] assembles addresses (a version, a namespace on the value or a quoted
        // key, none); an unquoted namespaced key is an error; `deps` is provisioned with an
        // advisory naming the table and the line to write; an entry in both is a conflict naming
        // both versions; [xlings.envs] is refused; a project with no [xlings] hears nothing of it.
        auto p = s::project::scaffold("app");
        const auto base = p.read("mcpp.toml");
        const auto declare_and_build = [&](std::string_view block) {
            p.write("mcpp.toml", base + "\n" + std::string(block) + "\n");
            return s::text(p.mcpp({"build"}, no_auto));
        };
        auto out = declare_and_build("[xlings.workspace]\nmcpp-e2e-absent = \"1.0\"");
        expect(fatal(e2e::contains(out, "mcpp-e2e-absent@1.0"))) << "the address the entry assembles into is missing: " << out;
        for (std::string_view block : {"[xlings.workspace]\nmcpp-e2e-absent = \"xim:1.0\"", "[xlings.workspace]\n\"xim:mcpp-e2e-absent\" = \"1.0\""}) {
            out = declare_and_build(block);
            expect(fatal(e2e::contains(out, "xim:mcpp-e2e-absent@1.0"))) << "namespaced address missing: " << out;
        }
        out = declare_and_build("[xlings.workspace]\nxim:mcpp-e2e-absent = \"1.0\"");
        expect(fatal(s::contains_nocase(out, "error"))) << "an unquoted namespaced key was accepted: " << out;
        out = declare_and_build("[xlings.workspace]\nmcpp-e2e-absent = \"\"");
        expect(fatal(e2e::contains(out, "mcpp-e2e-absent"))) << "unconstrained entry missing: " << out;
        expect(fatal(s::lacks(out, "mcpp-e2e-absent@"))) << "unconstrained entry carries a version: " << out;
        out = declare_and_build("[xlings]\ndeps = [\"xim:mcpp-e2e-absent@1.0\"]");
        expect(fatal(e2e::contains(out, "xim:mcpp-e2e-absent@1.0"))) << "deps entry not provisioned: " << out;
        expect(fatal(e2e::contains(out, "[xlings.workspace]"))) << "no advisory naming the new table: " << out;
        expect(fatal(e2e::contains(out, "\"xim:mcpp-e2e-absent\" = \"1.0\""))) << "the advisory does not show the recommended line to write: " << out;
        out = declare_and_build("[xlings]\ndeps = [\"mcpp-e2e-absent@2.0\"]\n\n[xlings.workspace]\nmcpp-e2e-absent = \"1.0\"");
        expect(fatal(e2e::contains(out, "named in both"))) << "the conflict was accepted: " << out;
        expect(fatal(e2e::contains(out, "1.0") && e2e::contains(out, "2.0"))) << "the conflict message omits a version: " << out;
        out = declare_and_build("[xlings.envs]\nOPENBLAS_NUM_THREADS = \"1\"");
        expect(fatal(e2e::contains(out, "[xlings.envs]"))) << "envs was accepted: " << out;
        p.write("mcpp.toml", base);
        auto r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "a project with no [xlings] section: " << s::text(r);
        expect(fatal(!s::contains_nocase(s::text(r), "xlings"))) << "an undeclared project heard about xlings: " << s::text(r);
    };

    "335 xlings tool tiers"_test[s::gcc][s::unix_shell] = [] {
        // An entry without `when` is asked by build and run; `when = "build"` by build only;
        // `when = "run"` by run only; [feature-xlings.<f>] only with the feature.
        s::project p(s::case_dir() / "app", {{"mcpp.toml", R"([package]
name    = "tiers"
version = "0.1.0"

[build]
sources = ["src/main.cpp"]

[features]
default  = ["emulator"]
emulator = {}
hardware = {}

[xlings.workspace]
"xim:tier-always" = "1.0.0"
"xim:tier-build"  = { version = "1.0.0", when = "build" }
"xim:tier-run"    = { version = "1.0.0", when = "run" }

[feature-xlings.hardware]
"xim:tier-hardware" = "1.0.0"
)"}, {"src/main.cpp", "int main() { return 0; }\n"}});
        const auto build = s::text(p.mcpp({"build"}, no_auto));
        const auto run = s::text(p.mcpp({"run"}, no_auto));
        const auto hw = s::text(p.mcpp({"build", "--features", "hardware"}, no_auto));
        expect(fatal(e2e::contains(build, "not provisioned"))) << "mcpp build never reached the provisioning gate: " << build;
        expect(fatal(e2e::contains(run, "not provisioned"))) << "mcpp run never reached the provisioning gate: " << run;
        expect(fatal(e2e::contains(build, "tier-always"))) << "an entry with no 'when' was not provisioned by mcpp build: " << build;
        expect(fatal(e2e::contains(run, "tier-always"))) << "an entry with no 'when' was not provisioned by mcpp run: " << run;
        expect(fatal(e2e::contains(build, "tier-build"))) << "when = 'build' was not provisioned by mcpp build: " << build;
        expect(fatal(s::lacks(build, "tier-run"))) << "when = 'run' was provisioned by mcpp build: " << build;
        expect(fatal(e2e::contains(run, "tier-run"))) << "when = 'run' was NOT provisioned by mcpp run: " << run;
        expect(fatal(s::lacks(build, "tier-hardware") && s::lacks(run, "tier-hardware"))) << "[feature-xlings.hardware] applied without the feature: " << build << run;
        expect(fatal(e2e::contains(hw, "tier-hardware"))) << "[feature-xlings.hardware] did not apply with --features hardware: " << hw;
    };

    "737 the declared runtime payload is installed"_test[s::gcc] = [] {
        // The default SubOS declares a glibc runtime. With that payload gone from the cache: A an
        // offline build fails naming xim:<runtime>; B an online build installs it (its loader)
        // and the program runs; C `toolchain install` installs it too.
        const auto tmp = s::case_dir();
        const auto home = s::case_home(s::home::fresh);
        const s::options o{.where = s::home::fresh, .env = {{"MCPP_NO_AUTO_INSTALL", "1"}}};
        const auto set_mirror = [&] {
            if (auto m = s::env("MCPP_E2E_TOOLCHAIN_MIRROR"); m && !m->empty())
                (void)s::mcpp({"self", "config", "--mirror", *m}, tmp, o);
        };
        const auto declared_runtime = [&] {
            std::smatch m;
            const auto info = home / "registry/subos/default/.xlings.json";
            const auto text = fs::exists(info) ? s::read_file(info) : "";
            return std::regex_search(text, m, std::regex{R"rx("runtime": *"(glibc@[^"]*)")rx"}) ? m[1].str() : std::string{};
        };
        const auto cache_shape = [&](const std::string& version) {
            const auto keep = tmp / "xpkgs-kept";
            fs::rename(home / "registry/data/xpkgs", keep);
            for (const auto& e : fs::directory_iterator(home)) fs::remove_all(e.path());
            fs::create_directories(home / "registry/data");
            fs::rename(keep, home / "registry/data/xpkgs");
            const auto glibc = home / "registry/data/xpkgs/xim-x-glibc";
            expect(fatal(fs::is_directory(glibc / version))) << "cache shape: no payload for " << version << " under " << glibc;
            // `mv a b` moves a into b when b is already a directory (the second shaping).
            auto to = glibc / (version + "-cached-revision");
            if (fs::is_directory(to)) to /= version;
            fs::rename(glibc / version, to);
        };
        const auto has_loader = [](const fs::path& payload) {
            return !s::glob(payload, "lib64/ld-linux-*").empty() || !s::glob(payload, "lib/ld-linux-*").empty();
        };
        s::project proj(tmp / "proj", {{"mcpp.toml", "[package]\nname    = \"runtimeprobe\"\nversion = \"0.1.0\"\n"},
                                       {"src/main.cpp", "import std;\nint main() { std::println(\"runtime ok\"); return 0; }\n"}});
        set_mirror();
        auto r = s::mcpp({"toolchain", "install", "gcc", "16.1.0"}, tmp, o);
        expect(fatal(e2e::succeeded(r))) << "initial toolchain install: " << s::text(r);
        const auto declared = declared_runtime();
        expect(fatal(!declared.empty())) << "the default SubOS declares no glibc runtime";
        const auto version = declared.substr(std::string_view("glibc@").size());
        cache_shape(version);
        set_mirror();
        r = s::mcpp({"toolchain", "default", "gcc@16.1.0"}, tmp, o);
        expect(fatal(e2e::succeeded(r))) << "toolchain default in the cache shape: " << s::text(r);
        expect(fatal(declared_runtime() == declared)) << "the re-initialised SubOS declares " << declared_runtime() << ", not " << declared;
        const auto payload = home / "registry/data/xpkgs/xim-x-glibc" / version;
        expect(fatal(!fs::exists(payload))) << "A: the declared payload exists before the build; the shape is wrong";
        s::options offline = o;
        offline.env.emplace_back("MCPP_OFFLINE", "1");
        r = proj.mcpp({"build"}, offline);
        expect(fatal(failed(r))) << "A: an offline build without the declared payload succeeded: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "xim:" + declared))) << "A: the offline failure does not name xim:" << declared << ": " << s::text(r);
        fs::remove_all(proj.at("target"));
        r = proj.mcpp({"build"}, o);
        expect(fatal(e2e::succeeded(r))) << "B: online build: " << s::text(r);
        expect(fatal(has_loader(payload))) << "B: no loader under " << payload << " after the build: " << s::text(r);
        r = proj.mcpp({"run"}, o);
        expect(fatal(e2e::succeeded(r) && e2e::contains(s::text(r), "runtime ok"))) << "B: run: " << s::text(r);
        cache_shape(version);
        set_mirror();
        r = s::mcpp({"toolchain", "install", "gcc", "16.1.0"}, tmp, o);
        expect(fatal(e2e::succeeded(r))) << "C: toolchain install in the cache shape: " << s::text(r);
        expect(fatal(has_loader(payload))) << "C: no loader under " << payload << " after toolchain install: " << s::text(r);
    };

    "1067 a borrowed registry keeps its owners places"_test = [] {
        // A home whose [xlings] home is another home's registry starts without rewriting that
        // registry's configuration or naming itself in it.
        const auto owner = s::another_home(s::home::fresh);
        auto r = s::mcpp({"index", "list"}, s::case_dir(), {.env = {{"MCPP_HOME", owner.string()}}});
        expect(fatal(e2e::succeeded(r))) << "the owner home did not start: " << s::text(r);
        const auto config = owner / "registry/.xlings.json";
        expect(fatal(fs::is_regular_file(config))) << "the owner home wrote no registry configuration";
        const auto before = s::read_file(config);
        expect(fatal(e2e::contains(before, "\"store\""))) << "the owner recorded no store for its index: " << before;
        const auto borrower = s::another_home(s::home::fresh);
        s::project(borrower, {{"config.toml", std::format("[xlings]\nhome = \"{}\"\n", (owner / "registry").generic_string())}});
        r = s::mcpp({"index", "list"}, s::case_dir(), {.env = {{"MCPP_HOME", borrower.string()}}});
        expect(fatal(e2e::succeeded(r))) << "the borrowing home did not start: " << s::text(r);
        expect(fatal(s::read_file(config) == before)) << "the borrowing home rewrote the registry's index places:\n" << before << "\n---\n" << s::read_file(config);
        expect(fatal(s::lacks(s::read_file(config), borrower.filename().string()))) << "the registry names the borrowing home: " << s::read_file(config);
        // A home whose `registry` is a link to the owner's borrows it the same way.
        const auto linked = s::another_home(s::home::fresh);
        std::error_code ec;
        fs::create_directory_symlink(owner / "registry", linked / "registry", ec);
        if (ec) skip(std::format("no directory link here: {}", ec.message()));
        r = s::mcpp({"index", "list"}, s::case_dir(), {.env = {{"MCPP_HOME", linked.string()}}});
        expect(fatal(e2e::succeeded(r))) << "the linking home did not start: " << s::text(r);
        expect(fatal(s::read_file(config) == before)) << "the linking home rewrote the registry's index places:\n" << before << "\n---\n" << s::read_file(config);
        expect(fatal(s::lacks(s::read_file(config), linked.filename().string()))) << "the registry names the linking home: " << s::read_file(config);
    };
}};

// The cases that hold the machine (P7.1): a suite of their own, which the runner runs with no
// other test beside it.
E2E xlings_machine{"xlings-machine", resource{"machine"}, [] {
    "614 feature xlings reaches xpkg dir"_test[s::gcc][s::unix_shell] = [] {
        // A [feature-xlings.<f>] tool's MCPP_XPKG_<NS>_<NAME>_DIR is its payload with the feature
        // on and empty with it off.
        auto st = s::mcpp({"index", "status"}, s::case_dir());
        fs::path registry;
        std::smatch m;
        if (std::regex_search(st.out, m, std::regex{R"((/[^ \n]*/registry))"})) registry = m[1].str();
        if (registry.empty()) registry = s::shared_home() / "registry";
        const auto store = registry / "data/xpkgs/xim-x-ninja";
        if (!fs::is_directory(store)) skip(std::format("{} is absent, so there is no installed payload to look up", store.string()));
        std::vector<std::string> versions;
        for (const auto& e : fs::directory_iterator(store)) versions.push_back(e.path().filename().string());
        // `sort -V -r`: by numeric components, highest first.
        const auto key = [](const std::string& v) {
            std::vector<long> k;
            for (auto part : std::views::split(v, '.')) {
                long n = 0;
                std::from_chars(&*part.begin(), &*part.begin() + std::ranges::distance(part), n);
                k.push_back(n);
            }
            return k;
        };
        std::ranges::sort(versions, [&](const std::string& a, const std::string& b) { return key(a) > key(b); });
        std::string version;
        for (const auto& v : versions)
            if (fs::is_directory(store / v) && !fs::is_empty(store / v)) { version = v; break; }
        if (version.empty()) skip(std::format("every version under {} is empty", store.string()));
        s::project p(s::case_dir() / "app", {
            {"mcpp.toml", std::format("[package]\nname    = \"featurexlings\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/main.cpp\"]\n\n[features]\ndefault = {{}}\ngpu     = {{}}\n\n[feature-xlings.gpu]\n\"xim:ninja\" = \"{}\"\n", version)},
            {"src/main.cpp", "int main() { return 0; }\n"},
            {"build.mcpp", "#include <cstdio>\n#include <cstdlib>\nint main() {\n    const char* dir = std::getenv(\"MCPP_XPKG_XIM_NINJA_DIR\");\n    std::printf(\"mcpp:warning=xpkg_dir=[%s]\\n\", (dir && *dir) ? dir : \"\");\n    return 0;\n}\n"}});
        const auto with_gpu = s::text(p.mcpp({"build", "--features", "gpu"}));
        fs::remove_all(p.at("target"));
        const auto without = s::text(p.mcpp({"build"}));
        expect(e2e::contains(with_gpu, std::format("xpkg_dir=[{}]", (store / version).string()))) << "the feature's tool does not resolve to its payload: " << with_gpu;
        expect(e2e::contains(without, "xpkg_dir=[]")) << "the tool does not answer empty when the feature is off: " << without;
    };
}};

}  // namespace

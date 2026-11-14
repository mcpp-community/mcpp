// The cases of e2e/self that hold the machine (nothing else of this program runs beside them):
// a SubOS selection is root-local, and a suggested command quotes its path (formerly
// tests/e2e/205, 931). They are one program so that the resource serialises them.
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

// The last `[11] <hash>` line of a `--print-fingerprint` output.
std::string field11(std::string_view out) {
    std::string last;
    const std::regex row{R"(^\s*\[11\]\s*(.*)$)"};
    for (auto part : std::views::split(out, '\n')) {
        const std::string line(part.begin(), part.end());
        std::smatch m;
        if (std::regex_match(line, m, row)) last = m[1];
    }
    return last;
}

E2E machine{"machine", resource{"machine"}, [] {
    "205 root local subos"_test[s::elf][s::gcc] = [] {
        // The default SubOS's binding ignores XLINGS_ACTIVE_SUBOS, and an explicit "default" is
        // not its absence; a workspace root owns the members' SubOS (materialised at the root
        // only); the member alone selects its own; a dependency's SubOS does not reach its
        // consumer.
        const auto tmp = s::case_dir();
        const auto default_manifest = s::shared_home() / "registry/subos/default/.xlings.json";
        expect(fatal(fs::is_regular_file(default_manifest))) << "mcpp's default SubOS is missing";
        const auto default_subos = default_manifest.parent_path();
        std::smatch m;
        const auto text = s::read_file(default_manifest);
        expect(fatal(std::regex_search(text, m, std::regex{R"rx("runtime"\s*:\s*"([^"]*)")rx"}))) << "the default SubOS has no runtime identity";
        const auto runtime = m[1].str();
        const auto write_contract = [&](const fs::path& dir, std::string_view marker) {
            fs::create_directories(dir);
            fs::create_directory_symlink(default_subos / "lib", dir / "lib");
            s::project(dir, {{".xlings.json", std::format(R"({{ "workspace": {{}}, "subos_info": {{ "schema_version": 1,
  "runtime": "{0}", "envs": {{ "{1}@1": [
    {{ "var": "MCPP_ROOT_LOCAL_MARKER", "op": "set", "value": "{1}" }}
  ] }} }} }}
)", runtime, marker)}});
        };
        const auto fingerprint = [](const s::project& p, std::vector<std::string> args, std::string_view what, s::options o = {}) {
            args.insert(args.begin(), "build");
            args.push_back("--print-fingerprint");
            auto r = p.mcpp(args, o);
            expect(fatal(e2e::succeeded(r))) << what << ": " << s::text(r);
            return field11(s::text(r));
        };
        s::project def(tmp / "default", {{"mcpp.toml", "[package]\nname = \"default-app\"\nversion = \"0.1.0\"\n"}, {"src/main.cpp", "int main() { return 0; }\n"}});
        const auto plain = fingerprint(def, {}, "default build");
        const auto active = fingerprint(def, {}, "the active state influenced the default build", {.env = {{"XLINGS_ACTIVE_SUBOS", "definitely-missing"}}});
        expect(fatal(!plain.empty() && plain == active)) << "the McppDefault fingerprint changed with XLINGS_ACTIVE_SUBOS";
        def.write("mcpp.toml", def.read("mcpp.toml") + "\n[xlings]\nsubos = \"default\"\n");
        const auto explicit_hash = fingerprint(def, {}, "explicit default build");
        expect(fatal(!explicit_hash.empty() && explicit_hash != plain)) << "an explicit default collapsed into absence";

        s::project ws(tmp / "workspace", {{"mcpp.toml", "[workspace]\nmembers = [\"app\"]\n\n[xlings]\nsubos = \"root-el8\"\n"},
                                          {"app/mcpp.toml", "[package]\nname = \"workspace-app\"\nversion = \"0.1.0\"\n\n[xlings]\nsubos = \"missing-member-subos\"\n"},
                                          {"app/src/main.cpp", "int main() { return 0; }\n"}});
        write_contract(ws.at(".mcpp/.xlings/subos/root-el8"), "root");
        const auto workspace_hash = fingerprint(ws, {"-p", "app"}, "the workspace root did not own the SubOS");
        expect(fatal(!workspace_hash.empty())) << "the workspace binding hash is missing";
        expect(fatal(ws.exists(".mcpp/.xlings.json") && e2e::contains(ws.read(".mcpp/.xlings.json"), "\"subos\": \"root-el8\""))) << "the workspace root's xlings config was not materialized";
        expect(fatal(!ws.exists("app/.mcpp/.xlings.json"))) << "the member's xlings config was materialized during the workspace build";
        fs::copy(ws.at("app"), tmp / "standalone", fs::copy_options::recursive | fs::copy_options::copy_symlinks);
        const s::project standalone(tmp / "standalone", {});
        write_contract(standalone.at(".mcpp/.xlings/subos/missing-member-subos"), "member");
        const auto standalone_hash = fingerprint(standalone, {}, "standalone member selection");
        expect(fatal(!standalone_hash.empty() && standalone_hash != workspace_hash)) << "the standalone member reused the workspace RuntimeBinding";

        s::project dep(tmp / "dep", {{"mcpp.toml", "[package]\nname = \"localdep\"\nversion = \"0.1.0\"\n\n[modules]\nsources = [\"src/**/*.cppm\"]\n\n[targets.localdep]\nkind = \"lib\"\n\n[xlings]\nsubos = \"missing-dependency-subos\"\n"},
                                     {"src/value.cppm", "export module localdep.value;\nexport int local_value() { return 7; }\n"}});
        s::project app(tmp / "consumer", {{"mcpp.toml", std::format("[package]\nname = \"consumer\"\nversion = \"0.1.0\"\n\n[dependencies.localdep]\npath = \"{}\"\n", dep.dir().generic_string())},
                                          {"src/main.cpp", "import localdep.value;\nint main() { return local_value() == 7 ? 0 : 1; }\n"}});
        auto r = app.mcpp({"run"});
        expect(fatal(e2e::succeeded(r))) << "the dependency's SubOS leaked into the consumer: " << s::text(r);
    };

    "931 a suggested command quotes its path"_test[s::unix_shell][s::fresh_sandbox] = [] {
        // With a home selected, `self init --home '<spaced path>'` suggests a command that quotes
        // the path, and that command, pasted, succeeds.
        const auto work = s::case_dir();
        fs::create_directories(work / "user");
        const s::options o{.env = {{"NO_COLOR", "1"}, {"HOME", (work / "user").string()}, {"MCPP_HOME", std::nullopt}}};
        auto r = s::mcpp({"self", "init", "--home", (work / "first home").string()}, work, o);
        expect(fatal(e2e::succeeded(r))) << "the first home could not be initialised: " << s::text(r);
        r = s::mcpp({"self", "init", "--home", (work / "second home").string()}, work, o);
        const auto out = s::text(r);
        expect(fatal(s::has_line(out, "--home '[^']*/second home'"))) << "the suggested command does not quote the spaced path: " << out;
        std::smatch m;
        expect(fatal(std::regex_search(out, m, std::regex{"use `(mcpp self init --force --home [^\\n]*)` to select"}))) << "no suggested command found: " << out;
        const auto cmd = m[1].str().substr(std::string_view("mcpp ").size());
        auto pasted = e2e::run({.argv = {"bash", "-c", std::format("\"{}\" {}", s::mcpp_path(), cmd)}, .cwd = work,
                                .env = {{"NO_COLOR", "1"}, {"HOME", (work / "user").string()}, {"MCPP_HOME", std::nullopt}}});
        expect(fatal(pasted.started && pasted.status == 0)) << "the suggested command, pasted, failed: " << pasted.out << pasted.err;
    };
}};

}  // namespace

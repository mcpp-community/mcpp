// A home's SubOS and index tables: a SubOS without its description degrades and says so, a
// missing one fails, the doctor offline initialises nothing, [index.repos.<name>] reaches an
// existing home and is restored, and the shell's active SubOS redirects nothing (formerly
// tests/e2e/221, 237, 604, 681, 686).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

bool failed(const e2e::outcome& r) { return r.started && r.status != 0; }

std::string entry_url(const fs::path& xj, std::string_view name) {
    for (const auto& r : s::json_file(xj).value("index_repos", nlohmann::json::array()))
        if (r.value("name", "") == name) return r.value("url", "");
    return "absent";
}

void rewrite(const fs::path& file, const std::string& pattern) {
    s::project(file.parent_path(), {{file.filename().string(), std::regex_replace(s::read_file(file), std::regex{pattern}, "\n")}});
}

E2E subos{"subos", [] {
    "221 subos without info still builds"_test = [] {
        // A project SubOS that does not describe itself costs at most hermeticity (never "cannot
        // provide a RuntimeBinding"), builds with allow_host_libs and reports the degradation; a
        // SubOS that does not exist fails saying so.
        s::project p(s::case_dir() / "proj", {{".mcpp/.xlings/subos/bare/.keep", ""}, {"src/main.cpp", "int main() { return 0; }\n"},
                                              {"mcpp.toml", "[package]\nname = \"bare\"\nversion = \"0.1.0\"\n\n[xlings]\nsubos = \"bare\"\n"}});
        p.remove(".mcpp/.xlings/subos/bare/.keep");
        auto r = p.mcpp({"build"});
        const auto out = s::text(r);
        expect(fatal(s::lacks(out, "cannot provide a RuntimeBinding"))) << "an undeclared SubOS still invalidates the build: " << out;
        if (r.status != 0) expect(fatal(e2e::contains(out, "hermetic link check failed"))) << "mcpp build exited " << r.status << " for a reason this test does not recognise: " << out;
        p.write("mcpp.toml", "[package]\nname = \"bare\"\nversion = \"0.1.0\"\n\n[xlings]\nsubos = \"bare\"\n\n[build]\nallow_host_libs = true\n");
        r = p.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "even with allow_host_libs, an undeclared SubOS blocks the build: " << s::text(r);
        expect(fatal(e2e::contains(out, "does not describe itself"))) << "the build never reported the degradation: " << out;
        p.write("mcpp.toml", "[package]\nname = \"bare\"\nversion = \"0.1.0\"\n\n[xlings]\nsubos = \"definitely-not-created\"\n");
        r = p.mcpp({"build"});
        expect(fatal(failed(r))) << "naming a SubOS that does not exist was accepted: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "does not exist"))) << "the missing-SubOS error no longer says what is wrong: " << s::text(r);
    };

    "237 default subos without info"_test[s::elf] = [] {
        // The default SubOS with no description: the build costs at most hermeticity; described
        // (runtime glibc@<v>), the same home builds; `toolchain install` exits 0 undescribed.
        const fs::path real = s::env("MCPP_HOME_REAL").value_or((fs::path(s::env("HOME").value_or("")) / ".mcpp").string());
        const auto home = s::case_dir() / "home";
        fs::create_directories(home / "registry/data");
        fs::create_directories(home / "registry/subos/default");
        if (!fs::is_directory(real / "registry/data/xpkgs")) skip(std::format("no xpkgs payloads to inherit from {}", real.string()));
        fs::create_directory_symlink(real / "registry/data/xpkgs", home / "registry/data/xpkgs");
        for (const auto& d : fs::directory_iterator(real / "registry")) {
            const auto b = d.path().filename();
            if (b == "data" || b == "subos") continue;
            std::error_code ec;
            if (d.is_directory()) fs::create_directory_symlink(d.path(), home / "registry" / b, ec);
            else fs::create_symlink(d.path(), home / "registry" / b, ec);
        }
        const s::options o{.env = {{"MCPP_HOME", home.string()}}};
        const auto llvm = s::llvm_version();
        s::project p(s::case_dir() / "proj", {{"mcpp.toml", std::format("[package]\nname    = \"nosubosinfo\"\nversion = \"0.1.0\"\n\n[toolchain]\ndefault = \"gcc@16.1.0\"\nmacos   = \"llvm@{0}\"\nwindows = \"llvm@{0}\"\n", llvm)},
                                              {"src/main.cpp", "int main() { return 0; }\n"}});
        const auto info = home / "registry/subos/default/.xlings.json";
        s::project(info.parent_path(), {{".xlings.json", R"({"workspace":{}})"}});
        auto r = p.mcpp({"build"}, o);
        const auto out = s::text(r);
        expect(fatal(s::lacks(out, "has no RuntimeBinding identity"))) << "an undescribed DEFAULT SubOS still kills the build: " << out;
        if (r.status != 0) expect(fatal(e2e::contains(out, "hermetic link check failed"))) << "mcpp build exited " << r.status << " for a reason this test does not recognise: " << out;

        const auto glibc_dir = home / "registry/data/xpkgs/xim-x-glibc";
        std::string ver;
        try {
            const auto rt = s::json_file(real / "registry/subos/default/.xlings.json")["subos_info"]["runtime"].get<std::string>();
            if (const auto at = rt.find('@'); at != std::string::npos) ver = rt.substr(at + 1);
        } catch (...) {}
        if (ver.empty() || !fs::is_directory(glibc_dir / ver)) {
            // `ls | head -1`: the first name in sorted order.
            std::vector<std::string> names;
            if (fs::is_directory(glibc_dir))
                for (const auto& e : fs::directory_iterator(glibc_dir)) names.push_back(e.path().filename().string());
            std::ranges::sort(names);
            ver = names.empty() ? "" : names.front();
        }
        if (ver.empty()) skip("no glibc payload to bind to");
        s::project(info.parent_path(), {{".xlings.json", nlohmann::json{{"workspace", nlohmann::json::object()}, {"subos_info", {{"schema_version", 1}, {"runtime", "glibc@" + ver}, {"host_glibc", ver}, {"envs", nlohmann::json::object()}}}}.dump()}});
        fs::remove_all(p.at("target"));
        r = p.mcpp({"build"}, o);
        expect(fatal(e2e::succeeded(r))) << "the same home builds nothing even once the SubOS describes itself (runtime glibc@" << ver << "): " << s::text(r);
        s::project(info.parent_path(), {{".xlings.json", R"({"workspace":{}})"}});
        r = s::mcpp({"toolchain", "install", "gcc@16.1.0"}, p.dir(), o);
        expect(fatal(e2e::succeeded(r))) << "`mcpp toolchain install` exited " << r.status << " with an undescribed SubOS: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), "has no RuntimeBinding identity"))) << "the install path still turns an absent description into an error: " << s::text(r);
    };

    "604 offline skips sandbox bootstrap"_test = [] {
        // Offline, `self doctor` on a fresh home creates nothing (no index, no sandbox) and goes
        // on to the build policy; on a bootstrapped home it announces no skipped bootstrap.
        const auto home = s::case_dir() / "home";
        auto r = s::mcpp({"self", "doctor"}, s::case_dir(), {.env = {{"MCPP_HOME", home.string()}, {"MCPP_OFFLINE", "1"}}});
        expect(fatal(!fs::exists(home))) << "an offline diagnostic initialized the home: " << s::text(r);
        expect(fatal(!fs::exists(home / "registry/data/xim-pkgindex"))) << "the package index was cloned under offline mode";
        expect(fatal(!fs::exists(home / "registry/subos/default/.xlings.json"))) << "the sandbox was initialised under offline mode";
        expect(fatal(e2e::contains(s::text(r), "Checking build policy"))) << "the doctor stopped at the registry check: " << s::text(r);
        r = s::mcpp({"self", "doctor"}, s::case_dir(), {.env = {{"MCPP_OFFLINE", "1"}}});
        expect(fatal(s::lacks(s::text(r), "Skipping sandbox bootstrap"))) << "a bootstrapped home announced a skip: " << s::text(r);
    };

    "681 index repos reach an existing home"_test = [] {
        // [index.repos.xim] in config.toml reaches .xlings.json once (a line, a record), is
        // restored when removed, and leaves an entry someone else changed; the default mcpplibs
        // entry is not rewritten without a table, is replaced by one, and restored after it.
        const auto tmp = s::case_dir();
        const auto home = tmp / "mcpp-home";
        const auto cfg = home / "config.toml", xj = home / "registry/.xlings.json", rec = home / "registry/.mcpp-index-overrides.json";
        const auto checkout = tmp / "xim-checkout";
        fs::create_directories(checkout);
        const auto config = [&] { return s::text(s::mcpp({"self", "config", "--mirror", "auto"}, tmp, {.env = {{"MCPP_HOME", home.string()}}})); };
        const auto append = [&](const std::string& text) { s::project(home, {{"config.toml", s::read_file(cfg) + text}}); };
        (void)config();
        expect(fatal(fs::exists(xj))) << "the registry was not seeded";
        expect(fatal(entry_url(xj, "xim") == "absent")) << "a fresh home already names xim: " << s::read_file(xj);
        append(std::format("\n[index.repos.xim]\nurl = \"{}\"\n", checkout.generic_string()));
        auto log = config();
        expect(fatal(entry_url(xj, "xim") == checkout.generic_string())) << "A: the table did not reach .xlings.json: " << s::read_file(xj) << log;
        expect(fatal(e2e::contains(log, std::format("xim -> {} ([index.repos.xim] in config.toml)", checkout.generic_string())))) << "A: no line names the change: " << log;
        expect(fatal(fs::exists(rec))) << "A: the change was not recorded";
        const auto before = s::read_file(xj);
        log = config();
        expect(fatal(s::read_file(xj) == before)) << "B: a second run changed .xlings.json: " << s::read_file(xj);
        expect(fatal(s::lacks(log, "xim -> "))) << "B: a second run printed the index line again: " << log;
        const std::string xim_table = R"(\n\[index\.repos\.xim\]\nurl = "[^"]*"\n)";
        rewrite(cfg, xim_table);
        log = config();
        expect(fatal(entry_url(xj, "xim") == "absent")) << "C: removing the table left the entry: " << s::read_file(xj) << log;
        expect(fatal(e2e::contains(log, "xim restored"))) << "C: no line names the restoration: " << log;
        expect(fatal(!fs::exists(rec))) << "C: the record outlived the table";
        append(std::format("\n[index.repos.xim]\nurl = \"{}\"\n", checkout.generic_string()));
        (void)config();
        auto d = s::json_file(xj);
        for (auto& r : d["index_repos"]) if (r.value("name", "") == "xim") r["url"] = "/somewhere/else";
        s::project(xj.parent_path(), {{".xlings.json", d.dump(2)}});
        rewrite(cfg, xim_table);
        (void)config();
        expect(fatal(entry_url(xj, "xim") == "/somewhere/else")) << "D: an entry someone else changed was overwritten: " << s::read_file(xj);
        expect(fatal(s::has_line(s::read_file(cfg), R"(^\[index\.repos\."mcpplibs"\])"))) << "E: config.toml was not seeded with the mcpplibs table: " << s::read_file(cfg);
        rewrite(cfg, R"(\n\[index\.repos\."mcpplibs"\]\n(?:[^\[\n][^\n]*\n)*)");
        expect(fatal(s::no_line(s::read_file(cfg), R"(index\.repos\."mcpplibs")"))) << "E: the mcpplibs table is still in config.toml: " << s::read_file(cfg);
        d = s::json_file(xj);
        for (auto& r : d["index_repos"]) if (r.value("name", "") == "mcpplibs") r["source"] = "git";
        s::project(xj.parent_path(), {{".xlings.json", d.dump(2)}});
        const auto xj_e = s::read_file(xj);
        fs::remove(rec);
        log = config();
        expect(fatal(s::read_file(xj) == xj_e)) << "E: the default entry was rewritten without a table: " << s::read_file(xj) << log;
        expect(fatal(s::lacks(log, "mcpplibs -> "))) << "E: a table was named for the default entry: " << log;
        expect(fatal(!fs::exists(rec))) << "E: a change was recorded for the default entry";
        const auto mirror = tmp / "mcpplibs-checkout";
        fs::create_directories(mirror);
        append(std::format("\n[index.repos.mcpplibs]\nurl = \"{}\"\n", mirror.generic_string()));
        log = config();
        expect(fatal(entry_url(xj, "mcpplibs") == mirror.generic_string())) << "F: the table did not replace the default entry: " << s::read_file(xj) << log;
        expect(fatal(e2e::contains(log, std::format("mcpplibs -> {} ([index.repos.mcpplibs] in config.toml)", mirror.generic_string())))) << "F: no line names the change: " << log;
        rewrite(cfg, R"(\n\[index\.repos\.mcpplibs\]\nurl = "[^"]*"\n)");
        log = config();
        expect(fatal(s::json_file(xj) == nlohmann::json::parse(xj_e))) << "F: removing the table did not restore the entry it replaced: " << s::read_file(xj) << log;
        expect(fatal(e2e::contains(log, "mcpplibs restored"))) << "F: no line names the restoration: " << log;
    };

    "686 an activated xlings subos does not redirect the registry"_test[s::fresh_sandbox] = [] {
        // `self init` under XLINGS_ACTIVE_SUBOS initialises subos/default with its tools and
        // never acts on the shell's SubOS.
        const auto home = s::case_dir() / "mcpp-home";
        constexpr std::string_view shell_subos = "mcpp-e2e-shell-subos";
        auto r = s::mcpp({"self", "init"}, s::case_dir(), {.env = {{"MCPP_HOME", home.string()}, {"XLINGS_ACTIVE_SUBOS", std::string(shell_subos)}}});
        const auto subos = home / "registry/subos";
        expect(fatal(fs::is_directory(subos / "default"))) << "the registry was not initialised: " << s::text(r);
        expect(fatal(!fs::exists(subos / shell_subos))) << "A: the registry acted on the shell's SubOS: " << s::text(r);
        std::vector<std::string> tools;
        if (fs::is_directory(subos / "default/bin"))
            for (const auto& e : fs::directory_iterator(subos / "default/bin")) {
                auto n = e.path().filename().string();
                std::string lower = n;
                for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (!lower.starts_with("xlings")) tools.push_back(n);
            }
        expect(fatal(!tools.empty())) << "B: the bootstrap placed no tool in subos/default/bin: " << s::text(r);
    };
}};

}  // namespace

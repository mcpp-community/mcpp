// When an index is refreshed and what a failed resolution says: refresh by local resolvability,
// an index's floor, a bare compat name, the bounded and owned refresh, the envelope's network
// record, auto_refresh, the offline codes, a recorded payload that is gone, a floor as a closing
// tip, and a version the index lacks (formerly tests/e2e/173, 185, 730, 732, 733, 734, 735, 803,
// 812, 949).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

const s::options inherited{.where = s::home::inherited};

void age(const fs::path& file, std::chrono::hours by) { fs::last_write_time(file, fs::file_time_type::clock::now() - by); }

bool alive(const std::string& pid) {
    auto r = e2e::run({.argv = {"sh", "-c", "kill -0 " + pid + " 2>/dev/null"}});
    return r.started && r.status == 0;
}

std::string trimmed(std::string t) {
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
    return t;
}

constexpr std::string_view handshake = R"HS([[ "${1:-}" == "interface" && "${2:-}" == "--version" ]] && { echo '{"protocol_version":"1.13","version":"9999.1.1.1","capabilities":["install_packages"],"home_layouts":[2],"features":["index_dir","index_store","upstreams","payload_status","sync_index","payload_currency","binding_state","network_diagnostics","download_socket_budgets","read_only_queries"]}'; exit 0; })HS";

// A stub xlings that answers the handshake and records every other command in STUB_LOG.
void stub_logging_xlings(const fs::path& file) {
    s::write_executable(file, std::string("#!/usr/bin/env bash\n") + std::string(handshake) + "\necho \"$*\" >> \"${STUB_LOG:?}\"\nexit 0\n");
}

// The home's config.toml with its `binary` lines and `[xlings]` headers dropped (`grep -v
// '^binary' | sed '/^\[xlings\]/d'`), then `[xlings] binary` naming `stub`. With `index_tail`, the
// `auto_refresh` lines and `[index]` headers are dropped too and `index_tail` follows.
void replace_xlings_binary(const fs::path& home, const fs::path& stub, std::string_view index_tail) {
    std::string out;
    if (fs::exists(home / "config.toml"))
        for (auto part : std::views::split(s::read_file(home / "config.toml"), '\n')) {
            const std::string line(part.begin(), part.end());
            if (line.starts_with("binary") || line.starts_with("[xlings]")) continue;
            if (!index_tail.empty() && (line.starts_with("auto_refresh") || line == "[index]")) continue;
            out += line + "\n";
        }
    out += std::format("\n[xlings]\nbinary = \"{}\"\n{}", stub.generic_string(), index_tail);
    s::project(home, {{"config.toml", out}});
}

bool has_word(const nlohmann::json& list, std::string_view word) {
    const std::regex w{std::format("(^|[^A-Za-z0-9_]){}([^A-Za-z0-9_]|$)", word)};
    for (const auto& e : list)
        if (e.is_string() && std::regex_search(e.get<std::string>(), w)) return true;
    return false;
}

E2E refresh{"refresh", [] {
    "173 index refresh policy"_test[s::gcc][s::fresh_sandbox] = [] {
        // A 30-day-old marker refreshes nothing when the index answers (an exact version and a
        // constraint); offline, a miss says so and names the revision and `mcpp index update`;
        // a miss under a namespace no index can refute is inconclusive; a miss just after a sync
        // is debounced; `auto_refresh = false` names itself; `index update` refuses offline;
        // `index status` shows the revision.
        const auto home = s::case_home(s::home::inherited);
        const auto index = home / "indices/mcpplibs";
        s::project(index, {{"pkgs/w/widget.lua", R"(package = {
    spec = "1",
    namespace = "mcpplibs",
    name = "widget",
    description = "Present in the local index",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux   = { ["1.2.0"] = { url = "https://example.invalid/w.tar.gz", sha256 = "0000000000000000000000000000000000000000000000000000000000000000" } },
        macosx  = { ["1.2.0"] = { url = "https://example.invalid/w.tar.gz", sha256 = "0000000000000000000000000000000000000000000000000000000000000000" } },
        windows = { ["1.2.0"] = { url = "https://example.invalid/w.zip",    sha256 = "0000000000000000000000000000000000000000000000000000000000000000" } },
    },
}
)"}, {".xlings-index-version", "abc1234\n"}, {".mcpp-index-updated", ""}});
        const auto marker = index / ".mcpp-index-updated";
        age(marker, std::chrono::hours{24 * 30});
        s::project p(s::case_dir() / "proj", {{"src/main.cpp", "int main() { return 0; }\n"}});
        const auto build = [&](std::string_view dep, bool offline) {
            p.write("mcpp.toml", std::format("[package]\nname    = \"proj\"\nversion = \"0.1.0\"\n\n[dependencies]\n{}\n\n[targets.proj]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", dep));
            s::options o = inherited;
            o.env.emplace_back("MCPP_VERBOSE", "1");
            if (offline) o.env.emplace_back("MCPP_OFFLINE", "1");
            return s::text(p.mcpp({"build"}, o));
        };
        auto log = build("widget = \"1.2.0\"", true);
        expect(fatal(s::has_line(log, "widget@1.2.0: resolvable locally"))) << "a locally resolvable dependency should be reported as such: " << log;
        expect(fatal(s::no_line(log, "Refreshing +package index|Updating +package index"))) << "a 30-day-old marker must NOT trigger a refresh when the index can answer: " << log;
        log = build("widget = \"^1.2\"", true);
        expect(fatal(s::has_line(log, R"(widget@\^1.2: resolvable locally)"))) << "a satisfiable constraint should not be a version miss: " << log;
        expect(fatal(s::no_line(log, "Refreshing +package index"))) << "a satisfiable constraint must not trigger a refresh: " << log;
        log = build("absentpkg = \"1.0.0\"", true);
        expect(fatal(s::has_line(log, "absentpkg@1.0.0: offline mode"))) << "offline mode must suppress the refresh and say so: " << log;
        expect(fatal(s::no_line(log, "Refreshing +package index"))) << "offline mode must not reach the network: " << log;
        expect(fatal(s::has_line(log, "index: local index abc1234"))) << "the resolution failure should name the index revision: " << log;
        expect(fatal(s::has_line(log, "mcpp index update"))) << "the resolution failure should point at the explicit refresh command: " << log;
        for (std::string_view dep : {"xim.nasm = \"2.16.03\"", "somevendor.thing = \"1.0.0\""}) {
            log = build(dep, false);
            expect(fatal(s::has_line(log, "no index can refute this"))) << "a miss under an unrefutable namespace must be inconclusive: " << dep << "\n" << log;
            expect(fatal(s::no_line(log, "Refreshing +package index"))) << "an unrefutable miss must never trigger a refresh: " << dep << "\n" << log;
        }
        s::project(index, {{".mcpp-index-updated", ""}});
        log = build("absentpkg = \"1.0.0\"", false);
        expect(fatal(s::has_line(log, "index was just refreshed"))) << "a miss moments after a sync must be debounced, not re-synced: " << log;
        expect(fatal(s::no_line(log, "Refreshing +package index"))) << "debounce must prevent the second sync: " << log;
        age(marker, std::chrono::hours{24 * 30});
        s::project(home, {{"config.toml", s::read_file(home / "config.toml") + "\n[index]\nauto_refresh = false\n"}});
        log = build("absentpkg = \"1.0.0\"", false);
        expect(fatal(s::has_line(log, "auto_refresh = false"))) << "the config opt-out must suppress the refresh and name itself: " << log;
        expect(fatal(s::no_line(log, "Refreshing +package index"))) << "auto_refresh = false must prevent the sync: " << log;
        s::options off = inherited;
        off.env.emplace_back("MCPP_OFFLINE", "1");
        log = s::text(p.mcpp({"index", "update"}, off));
        expect(fatal(s::has_line(log, "offline mode is on"))) << "an explicit index update must refuse audibly when offline: " << log;
        log = s::text(p.mcpp({"index", "status"}, off));
        expect(fatal(s::has_line(log, "revision"))) << "index status should have a revision column: " << log;
        expect(fatal(s::has_line(log, "abc1234"))) << "index status should show the local index revision: " << log;
    };

    "185 index floor degrades"_test[s::gcc][s::fresh_sandbox] = [] {
        // An index whose min_mcpp is beyond this mcpp: a build that needs it fails naming E0006
        // and "this mcpp cannot read" (never a bare "not found"); a build that needs nothing of
        // it succeeds; MCPP_INDEX_FLOOR=ignore bypasses the floor.
        const auto tmp = s::case_dir();
        const auto make_index = [](const fs::path& dir, std::string_view floor, std::string_view pkg) {
            s::project idx(dir, {{std::format("pkgs/{}/{}.lua", pkg.substr(0, 1), pkg), std::format(R"(package = {{
    spec = "1",
    name = "{0}",
    description = "fixture",
    licenses = {{"MIT"}},
    type = "package",
    xpm = {{ linux = {{ ["1.0.0"] = {{
        url = "https://example.invalid/{0}-1.0.0.tar.gz",
        sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
    }} }} }},
    mcpp = {{
        language = "c++23",
        sources = {{ "src/**/*.cppm" }},
        targets = {{ ["{0}"] = {{ kind = "lib" }} }},
        deps = {{}},
    }},
}}
)", pkg)}});
            if (!floor.empty()) idx.write("index.toml", std::format("[index]\nspec     = \"1\"\nmin_mcpp = \"{}\"\n", floor));
        };
        make_index(tmp / "good-index", "", "goodlib");
        make_index(tmp / "too-new-index", "9999.9.9.9", "newlib");
        const auto good = (tmp / "good-index").generic_string(), toonew = (tmp / "too-new-index").generic_string();
        const auto manifest = [](std::string_view indices, std::string_view deps) {
            return std::format("[package]\nname = \"floorproj\"\nversion = \"0.1.0\"\n\n[indices]\n{}\n{}[targets.floorproj]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n", indices, deps);
        };
        s::project p(tmp / "proj", {{"src/main.cpp", "import std;\nint main() { std::println(\"ok\"); return 0; }\n"},
                                    {"mcpp.toml", manifest(std::format("toonew = {{ path = \"{}\" }}\n", toonew), "[dependencies]\n\"toonew.newlib\" = \"1.0.0\"\n\n")}});
        auto r = p.mcpp({"build"}, inherited);
        auto log = s::text(r);
        expect(fatal(r.started && r.status != 0)) << "the build unexpectedly succeeded: " << log;
        expect(fatal(e2e::contains(log, "E0006"))) << "the failure never mentions E0006, the index that needs a newer mcpp: " << log;
        std::vector<std::string> lines;
        for (auto part : std::views::split(log, '\n')) lines.emplace_back(part.begin(), part.end());
        std::string last_error;
        for (const auto& l : lines) if (l.starts_with("error:")) last_error = l;
        if (last_error.contains("not found")) {
            const auto head = last_error.substr(0, 40);
            bool named = false;
            for (std::size_t i = 0; i < lines.size(); ++i)
                if (lines[i].contains(head))
                    for (std::size_t j = i; j < std::min(lines.size(), i + 5); ++j)
                        named = named || lines[j].contains("cannot read") || lines[j].contains("E0006");
            expect(fatal(named)) << "the build stopped on a bare 'not found' with no mention of the index that could not be read: " << log;
        }
        expect(fatal(e2e::contains(log, "this mcpp cannot read"))) << "nothing in the output names the unreadable index as the cause: " << log;

        p.write("mcpp.toml", manifest(std::format("toonew = {{ path = \"{}\" }}\ngood   = {{ path = \"{}\" }}\n", toonew, good), ""));
        r = p.mcpp({"build"}, inherited);
        expect(fatal(e2e::succeeded(r))) << "a project that depends on nothing from the too-new index failed to build: " << s::text(r);
        p.write("mcpp.toml", manifest(std::format("toonew = {{ path = \"{}\" }}\n", toonew), ""));
        s::options ignore = inherited;
        ignore.env.emplace_back("MCPP_INDEX_FLOOR", "ignore");
        r = p.mcpp({"build"}, ignore);
        expect(fatal(e2e::succeeded(r))) << "MCPP_INDEX_FLOOR=ignore no longer bypasses the floor: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), "E0006"))) << "MCPP_INDEX_FLOOR=ignore still reported E0006: " << s::text(r);
    };

    "730 bare compat dependency is not a refresh miss"_test[s::elf] = [] {
        // A bare name the shared home's mcpplibs index files as compat.<name> resolves through
        // the deprecated bare-name rung, and the refresh decision asks no network refresh for it.
        const auto root = s::shared_home() / "indices/mcpplibs/pkgs/c";
        std::string desc;
        for (std::string_view cand : {"cjson", "argparse", "gtest", "ftxui"})
            if (fs::is_regular_file(root / std::format("compat.{}.lua", cand))) { desc = cand; break; }
        if (desc.empty()) skip("no compat descriptor in the local mcpplibs index");
        std::smatch m;
        const auto text = s::read_file(root / std::format("compat.{}.lua", desc));
        expect(fatal(std::regex_search(text, m, std::regex{R"rx(\["([0-9][0-9.]*)"\])rx"}))) << "could not read a version of compat." << desc;
        const auto ver = m[1].str();
        s::project app(s::case_dir() / "app", {{"mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\n{} = \"{}\"\n", desc, ver)},
                                               {"src/main.cpp", "int main() { return 0; }\n"}});
        (void)app.mcpp({"emit", "build-database", "--format", "json"});
        auto r = app.mcpp({"emit", "build-database", "--format", "json", "-v"}, {.env = {{"MCPP_OFFLINE", "1"}}});
        expect(fatal(e2e::contains(r.err, std::format("resolved to 'compat.{}' through the deprecated bare-name search", desc)))) << "A: the resolver did not take the bare-name rung for " << desc << ": " << r.err;
        expect(fatal(s::lacks(r.err, std::format("index: {}@{}: offline mode", desc, ver)))) << "B: the refresh decision asked for a network refresh of a dependency the resolver found: " << r.err;
    };

    "732 xlings refresh is bounded and owned"_test[s::unix_shell] = [] {
        // A: `index update` under refresh_timeout = 3 returns well within 30 s, names the bound,
        // and the hung xlings does not outlive it. B: SIGTERM to mcpp ends the xlings it started.
        const auto tmp = s::case_dir();
        s::write_executable(tmp / "bin/xlings", std::string("#!/usr/bin/env bash\n") + std::string(handshake) + R"SH(
case "${1:-}:${2:-}" in
  update:*|interface:update_packages) echo "$$" > "${STUB_PID:?}"; exec sleep 1000 ;;
esac
exit 0
)SH");
        const auto write_config = [&](int secs) {
            s::project(tmp / "home", {{"config.toml", std::format("[xlings]\nbinary = \"{}\"\n\n[index]\nrefresh_timeout = {}\n", (tmp / "bin/xlings").generic_string(), secs)}});
        };
        write_config(3);
        const auto pid1 = tmp / "stub.pid";
        const auto start = std::chrono::steady_clock::now();
        auto r = s::mcpp({"index", "update"}, tmp, {.env = {{"STUB_PID", pid1.string()}, {"MCPP_HOME", (tmp / "home").string()}}, .timeout = std::chrono::seconds{60}});
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
        expect(fatal(fs::exists(pid1) && fs::file_size(pid1) > 0)) << "A: the stub's update never ran: " << s::text(r);
        const auto stub1 = trimmed(s::read_file(pid1));
        expect(fatal(elapsed < 30)) << "A: index update returned after " << elapsed << "s under a 3 s bound: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "did not finish within 3 seconds"))) << "A: no line names the bound: " << s::text(r);
        std::this_thread::sleep_for(std::chrono::seconds{1});
        expect(fatal(!alive(stub1))) << "A: the stub outlived its bound";
        if (alive(stub1)) (void)e2e::run({.argv = {"sh", "-c", "kill -9 " + stub1}});

        write_config(600);
        const auto pid2 = tmp / "stub2.pid";
        auto cmd = s::mcpp_command({"index", "update"}, tmp, {.env = {{"STUB_PID", pid2.string()}, {"MCPP_HOME", (tmp / "home").string()}}});
        auto mcpp = s::process::start({.argv = cmd.argv, .cwd = tmp, .env = cmd.env});
        for (int i = 0; i < 150 && !(fs::exists(pid2) && fs::file_size(pid2) > 0); ++i) std::this_thread::sleep_for(std::chrono::milliseconds{100});
        expect(fatal(fs::exists(pid2) && fs::file_size(pid2) > 0)) << "B: the stub's update never ran";
        const auto stub2 = trimmed(s::read_file(pid2));
        mcpp.signal(s::sig::terminate);
        (void)mcpp.wait(std::chrono::seconds{60});
        for (int i = 0; i < 30 && alive(stub2); ++i) std::this_thread::sleep_for(std::chrono::milliseconds{100});
        const bool outlived = alive(stub2);
        if (outlived) (void)e2e::run({.argv = {"sh", "-c", "kill -9 " + stub2}});
        expect(fatal(!outlived)) << "B: the stub outlived mcpp after SIGTERM";
    };

    "733 envelope reports observed network access"_test[s::unix_shell][s::python3] = [] {
        // A: a plan that refreshed the index (the stub saw the update) reports a network effect.
        // B: offline, no network effect, and a plan with no index is MCPP_OFFLINE_DOWNLOAD_REQUIRED.
        const auto tmp = s::case_dir();
        const auto home = s::case_home(s::home::inherited);
        fs::remove_all(home / "indices/mcpplibs");
        stub_logging_xlings(tmp / "bin/xlings");
        replace_xlings_binary(home, tmp / "bin/xlings", "");
        s::project app(tmp / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies.mcpplibs]\nprobe-not-published = \"1.0.0\"\n"},
                                     {"src/main.cpp", "int main() { return 0; }\n"}});
        s::options o = inherited;
        o.env.emplace_back("STUB_LOG", (tmp / "online.log").string());
        auto r = app.mcpp({"emit", "build-database", "--format", "json"}, o);
        expect(fatal(!r.out.empty())) << "A: no envelope: " << r.err;
        const auto online = fs::exists(tmp / "online.log") ? s::read_file(tmp / "online.log") : "";
        expect(fatal(s::has_line(online, "^update|^interface update_packages"))) << "A: the plan did not refresh the index: " << r.err;
        auto d = s::json(r.out);
        expect(fatal(has_word(d["effects"], "network"))) << "A: a plan that refreshed the index reports no network effect: " << d["effects"].dump();
        o = inherited;
        o.env.emplace_back("STUB_LOG", (tmp / "offline.log").string());
        o.env.emplace_back("MCPP_OFFLINE", "1");
        r = app.mcpp({"emit", "build-database", "--format", "json"}, o);
        expect(fatal(!r.out.empty())) << "B: no envelope: " << r.err;
        d = s::json(r.out);
        expect(fatal(!has_word(d["effects"], "network"))) << "B: an offline plan reports a network effect: " << d["effects"].dump();
        nlohmann::json codes = nlohmann::json::array();
        for (const auto& x : d["diagnostics"]) codes.push_back(x["code"]);
        expect(fatal(has_word(codes, "MCPP_OFFLINE_DOWNLOAD_REQUIRED"))) << "B: an offline plan with no index is not MCPP_OFFLINE_DOWNLOAD_REQUIRED: " << d["diagnostics"].dump();
    };

    "734 auto refresh false governs first index sync"_test[s::unix_shell] = [] {
        // With auto_refresh = false a custom index never synced is refused (naming `mcpp index
        // update` and the setting) and is not synced.
        const auto tmp = s::case_dir();
        const auto home = s::case_home(s::home::inherited);
        stub_logging_xlings(tmp / "bin/xlings");
        replace_xlings_binary(home, tmp / "bin/xlings", "\n[index]\nauto_refresh = false\n");
        s::project app(tmp / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[indices]\nacme = { url = \"https://example.invalid/acme-index.git\" }\n\n[dependencies.acme]\nwidget = \"1.0.0\"\n"},
                                     {"src/main.cpp", "int main() { return 0; }\n"}});
        s::options o = inherited;
        o.env.emplace_back("STUB_LOG", (tmp / "stub.log").string());
        auto r = app.mcpp({"build"}, o);
        expect(fatal(r.started && r.status != 0)) << "A: the build succeeded with an unsynced custom index: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "mcpp index update"))) << "A: the refusal does not name mcpp index update: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "auto_refresh = false"))) << "A: the refusal does not name the setting: " << s::text(r);
        const auto log = fs::exists(tmp / "stub.log") ? s::read_file(tmp / "stub.log") : "";
        expect(fatal(s::no_line(log, "^update|^interface update_packages"))) << "B: the custom index was synced although auto_refresh = false: " << log;
    };

    "735 offline missing package has its own code"_test[s::python3] = [] {
        // Offline, a package whose archive is not installed exits 5 with
        // MCPP_OFFLINE_DOWNLOAD_REQUIRED naming it and its version; a selector error keeps
        // MCPP_BUILD_DATABASE_PLAN_FAILED.
        const auto tmp = s::case_dir();
        s::project index(tmp / "index", {{"pkgs/w/widget.lua", R"(package = {
    spec = "1",
    namespace = "acme",
    name = "widget",
    description = "a package whose archive is not installed",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux   = { ["1.0.0"] = { url = "https://example.invalid/widget-1.0.0.tar.gz", sha256 = "0000000000000000000000000000000000000000000000000000000000000000" } },
        macosx  = { ["1.0.0"] = { url = "https://example.invalid/widget-1.0.0.tar.gz", sha256 = "0000000000000000000000000000000000000000000000000000000000000000" } },
        windows = { ["1.0.0"] = { url = "https://example.invalid/widget-1.0.0.tar.gz", sha256 = "0000000000000000000000000000000000000000000000000000000000000000" } },
    },
    mcpp = { language = "c++23", sources = { "src/*.cpp" }, targets = { ["widget"] = { kind = "lib" } } },
}
)"}});
        s::project app(tmp / "app", {{"mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[indices]\nacme = {{ path = \"{}\" }}\n\n[dependencies.acme]\nwidget = \"1.0.0\"\n", index.dir().generic_string())},
                                     {"src/main.cpp", "int main() { return 0; }\n"}});
        const s::options offline{.env = {{"MCPP_OFFLINE", "1"}}};
        const auto first = [](const nlohmann::json& d) {
            return d["diagnostics"].empty() ? std::pair<std::string, std::string>{} : std::pair{d["diagnostics"][0]["code"].get<std::string>(), d["diagnostics"][0]["message"].get<std::string>()};
        };
        auto r = app.mcpp({"emit", "build-database", "--format", "json"}, offline);
        expect(fatal(e2e::exited(r, 5))) << "A: exit status " << r.status << ", expected 5: " << r.err;
        auto [code, message] = first(s::json(r.out));
        expect(fatal(code == "MCPP_OFFLINE_DOWNLOAD_REQUIRED")) << "A: wrong code: " << code;
        expect(fatal(message.contains("widget") && message.contains("1.0.0"))) << "A: the message does not name the package and version: " << message;
        app.write("mcpp.toml", std::regex_replace(app.read("mcpp.toml"), std::regex{R"((^|\n)widget = "1.0.0")"}, "$1absent-package = \"1.0.0\""));
        r = app.mcpp({"emit", "build-database", "--format", "json"}, offline);
        expect(fatal(first(s::json(r.out)).first == "MCPP_BUILD_DATABASE_PLAN_FAILED")) << "B: a selector error took the offline code: " << r.out;
    };

    "803 a recorded payload that is gone is refused offline"_test[s::gcc] = [] {
        // Offline, an [xlings.workspace] entry that is not installed is refused naming the
        // declared address; recorded as provisioned (its FNV-1a stamp), it is refused naming the
        // address and the record.
        const auto home = s::case_home(s::home::linked);
        s::project app(s::case_dir() / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[xlings.workspace]\nmcpp-e2e-payload-that-is-gone = \"1.0.0\"\n"},
                                               {"src/main.cpp", "int main() {}\n"}});
        const s::options o{.where = s::home::linked};
        auto r = app.mcpp({"build", "--offline"}, o);
        expect(fatal(r.started && r.status != 0)) << "1: an uninstalled entry was accepted offline: " << s::text(r);
        std::string address;
        std::smatch m;
        const auto t1 = s::text(r);
        if (std::regex_search(t1, m, std::regex{R"((?:^|\n) *declared: ([^\n]*))"})) address = m[1];
        expect(fatal(address.contains("mcpp-e2e-payload-that-is-gone"))) << "1: the refusal does not name the declared entry: " << t1;
        std::uint64_t h = 1469598103934665603ull;
        for (unsigned char c : address + "\n") { h ^= c; h *= 1099511628211ull; }
        const auto stamp = std::format("xlings-deps-{:016x}", h);
        s::project(home / "provisioned", {{stamp, address + "\n"}});
        r = app.mcpp({"build", "--offline"}, o);
        const auto t2 = s::text(r);
        expect(fatal(r.started && r.status != 0)) << "2: a recorded payload that is not installed was accepted: " << t2;
        expect(fatal(t2.contains("recorded as provisioned") && t2.contains(address) && t2.contains(stamp))) << "2: the refusal does not name the address and the record: " << t2;
    };

    "812 an index floor is a closing tip"_test[s::unix_shell] = [] {
        // A: a refresh that brings a floor this mcpp does not meet keeps the usable tree and
        // ends with one tip naming the version asked for. B: `self doctor` lists an index whose
        // floor this mcpp does not meet and points at E0006.
        const auto tmp = s::case_dir();
        const auto home = s::case_home(s::home::inherited);
        const auto data = home / "indices/mcpplibs";
        fs::remove_all(data);
        s::project(data, {{"index.toml", "[index]\nspec = \"1\"\nmin_mcpp = \"0.0.1\"\n"}, {"pkgs/f/fixture.lua", "package = { spec = \"1\", name = \"fixture\", type = \"package\" }\n"}});
        std::string stub = std::string("#!/usr/bin/env bash\n") + std::string(handshake) + R"SH(
case " $* " in
    *" sync_index "*)
        dest=$(printf '%s' "$4" | sed -n 's/.*"dest":"\([^"]*\)".*/\1/p')
        mkdir -p "$dest/pkgs/f"
        cp "@DATA@/pkgs/f/fixture.lua" "$dest/pkgs/f/fixture.lua"
        printf '[index]\nspec = "1"\nmin_mcpp = "9999.9.9.9"\n' > "$dest/index.toml"
        echo '{"kind":"result","exitCode":0}'
        ;;
    *" interface "*) echo '{"kind":"result","exitCode":0}' ;;
esac
exit 0
)SH";
        stub.replace(stub.find("@DATA@"), 6, data.string());
        s::write_executable(tmp / "bin/xlings", stub);
        replace_xlings_binary(home, tmp / "bin/xlings", "");
        auto r = s::mcpp({"index", "update"}, tmp, inherited);
        expect(fatal(e2e::succeeded(r))) << "A: the refresh failed: " << r.err;
        expect(fatal(s::no_line(r.err, "^error:"))) << "A: a refresh that kept a usable index printed an error: " << r.err;
        expect(fatal(s::count_matching(r.err, "^tip:") == 1u)) << "A: expected exactly one tip line: " << r.err;
        expect(fatal(s::has_line(s::last_line(r.err), "^tip: .*requires a newer mcpp"))) << "A: the tip is not the last line of the run: " << r.err;
        expect(fatal(s::has_line(r.err, "^tip: .*requires mcpp >= 9999.9.9.9"))) << "A: the tip does not name the version the index asks for: " << r.err;
        expect(fatal(e2e::contains(s::read_file(data / "index.toml"), "min_mcpp = \"0.0.1\""))) << "A: the guard did not keep the previous, usable tree: " << s::read_file(data / "index.toml");
        s::project(data, {{"index.toml", "[index]\nspec = \"1\"\nmin_mcpp = \"9999.9.9.9\"\n"}});
        r = s::mcpp({"self", "doctor"}, tmp, inherited);
        expect(fatal(e2e::contains(s::text(r), "index 'mcpplibs' requires mcpp >= 9999.9.9.9"))) << "B: doctor does not list the index this mcpp cannot read: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "E0006"))) << "B: doctor does not point at E0006: " << s::text(r);
    };

    "949 a version the index lacks refreshes at install"_test[s::gcc][s::fresh_sandbox] = [] {
        // A version the local index predates: inside the debounce window the refused install
        // reaches the refresh policy and syncs nothing; with auto_refresh = false it is declined;
        // only the one address the descriptor named is asked for.
        const auto home = s::case_home(s::home::inherited);
        const auto index = home / "indices/mcpplibs";
        s::project(index, {{".xlings-index-version", "abc1234\n"}, {"pkgs/l/laterpkg.lua", R"(package = {
    spec = "1",
    namespace = "mcpplibs",
    name = "laterpkg",
    description = "A package whose next version the local index predates",
    licenses = {"MIT"},
    type = "package",
    xpm = {
        linux = {
            ["1.0.0"] = {
                url = "https://example.invalid/laterpkg-1.0.0.tar.gz",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
            },
        },
    },
}
)"}});
        s::project app(s::case_dir() / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[targets.app]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n\n[dependencies]\nlaterpkg = \"1.0.1\"\n"},
                                               {"src/main.cpp", "int main() { return 0; }\n"}});
        s::project(index, {{".mcpp-index-updated", ""}});
        s::options o = inherited;
        o.env.emplace_back("MCPP_VERBOSE", "1");
        auto r = app.mcpp({"build"}, o);
        const auto a = s::text(r);
        expect(fatal(r.started && r.status != 0)) << "A: the build installed a version no index carries: " << a;
        expect(fatal(s::has_line(a, R"(skip refresh for mcpplibs:laterpkg@1\.0\.1: index was just refreshed)"))) << "A: the refused install did not reach the refresh policy: " << a;
        expect(fatal(s::no_line(a, "Refreshing +package index"))) << "A: a sync inside the debounce window: " << a;
        age(index / ".mcpp-index-updated", std::chrono::hours{24 * 30});
        s::project(home, {{"config.toml", s::read_file(home / "config.toml") + "\n[index]\nauto_refresh = false\n"}});
        r = app.mcpp({"build"}, o);
        const auto b = s::text(r);
        expect(fatal(r.started && r.status != 0)) << "B: the build installed a version no index carries: " << b;
        expect(fatal(s::has_line(b, R"(skip refresh for mcpplibs:laterpkg@1\.0\.1: \[index\] auto_refresh = false)"))) << "B: auto_refresh = false did not decide the refused install: " << b;
        expect(fatal(s::no_line(a, "Downloading +compat") && s::no_line(b, "Downloading +compat"))) << "C: another namespace was shown as downloading: " << a << b;
        expect(fatal(s::has_line(a, "wire address tried: mcpplibs:laterpkg@1.0.1$"))) << "C: the refusal does not name the one address asked for: " << a;
    };
}};

}  // namespace

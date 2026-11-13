// Homes and their tools: the vendored xlings probed by an argument vector and replaced when older,
// a home selection remembered once, the doctor's bounded resource checks without a bootstrap, and
// a fresh home at a spaced or non-ASCII path (formerly tests/e2e/687, 919, 920, 932; 919's and
// 920's checks were tests/e2e/_home_selection.py and _doctor_network.py).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

using env_list = std::vector<std::pair<std::string, std::optional<std::string>>>;

fs::path resolved(const fs::path& p) { return fs::weakly_canonical(p); }

// Every path under `root` with its bytes (a directory: nothing), to compare a home before and after.
std::map<std::string, std::optional<std::string>> snapshot(const fs::path& root) {
    std::map<std::string, std::optional<std::string>> out;
    for (const auto& rel : s::listing(root))
        out[rel] = fs::is_regular_file(root / rel) ? std::optional{s::read_file(root / rel)} : std::nullopt;
    return out;
}

E2E homes{"homes", [] {
    "687 the vendored xlings probe is an argument vector"_test = [] {
        // A: the vendored xlings's version probe runs without a shell. B: a vendored binary
        // answering an older version (ninja stands in) is replaced from MCPP_VENDORED_XLINGS.
        // C: with no such variable and no xlings on PATH, the xlings released beside this mcpp
        // replaces it.
        const auto tmp = s::case_dir();
        const auto home = s::case_home(s::home::inherited);
        fs::remove(home / "config.toml");  // MCPP_INHERIT_CONFIG=0
        const s::options o{.where = s::home::inherited, .env = {{"MCPP_OFFLINE", "1"}}};
        const auto vendored = home / s::exe("registry/bin/xlings");
        auto r = s::mcpp({"self", "init"}, tmp, o);
        expect(fatal(fs::is_regular_file(vendored))) << "the first command did not vendor xlings at " << vendored << ": " << s::text(r);
        r = s::mcpp({"self", "init"}, tmp, o);
        expect(fatal(!s::contains_nocase(r.err, "cannot find the path specified"))) << "A: the version probe reached a shell that could not open its redirect: " << r.err;
        std::optional<fs::path> ninja;
        for (std::string_view pattern : {"*/ninja", "*/bin/ninja"}) {
            for (const auto& c : s::glob(home / "registry/data/xpkgs/xim-x-ninja", std::string(pattern) + (s::on_windows() ? ".exe" : "")))
                if (fs::is_regular_file(c)) { ninja = c; break; }
            if (ninja) break;
        }
        expect(fatal(ninja.has_value())) << "B: no ninja binary to stand in for an older xlings under " << home / "registry";
        const auto answer = s::run(*ninja, {"--version"}).out;
        const auto older = answer.substr(0, answer.find('\n'));
        expect(fatal(std::regex_match(older, std::regex{R"([0-9].*\..*)"}))) << "B: the stand-in " << *ninja << " answered '" << older << "', not a dotted version";
        const auto real = tmp / "real" / s::exe("xlings");
        fs::create_directories(real.parent_path());
        fs::copy_file(vendored, real);
        const auto stand_in = [&] {
            fs::remove(vendored);
            fs::copy_file(*ninja, vendored);
            fs::permissions(vendored, fs::perms::owner_exec, fs::perm_options::add);
        };
        stand_in();
        s::options with_real = o;
        with_real.env.emplace_back("MCPP_VENDORED_XLINGS", real.string());
        r = s::mcpp({"self", "init"}, tmp, with_real);
        expect(fatal(e2e::contains(r.err, std::format("vendored xlings {} -> ", older)))) << "B: a vendored xlings answering " << older << " was not replaced: " << s::text(r);
        expect(fatal(s::has_line(s::run(vendored, {"--version"}).out, "^xlings "))) << "B: after the replacement the vendored binary is not xlings: " << r.err;

        stand_in();
        const auto release = tmp / "release";
        fs::create_directories(release / "bin");
        fs::create_directories(release / "registry/bin");
        fs::copy_file(s::mcpp_path(), release / "bin" / s::exe("mcpp"));
        fs::copy_file(real, release / "registry/bin" / s::exe("xlings"));
        constexpr std::string_view c_path = "/usr/bin:/bin";
        expect(fatal(!fs::exists("/usr/bin/xlings") && !fs::exists("/bin/xlings"))) << "C: an xlings is reachable on " << c_path << ", so the criterion cannot tell its sources apart";
        auto c = e2e::run({.argv = {(release / "bin" / s::exe("mcpp")).string(), "self", "init"}, .cwd = tmp,
                           .env = {{"MCPP_HOME", home.string()}, {"MCPP_OFFLINE", "1"}, {"MCPP_VENDORED_XLINGS", std::nullopt}, {"PATH", std::string(c_path)}},
                           .timeout = std::chrono::minutes{10}});
        expect(fatal(e2e::contains(c.err, std::format("vendored xlings {} -> ", older)))) << "C: the released xlings did not replace a vendored xlings answering " << older << ": " << c.out << c.err;
        expect(fatal(s::has_line(s::run(vendored, {"--version"}).out, "^xlings "))) << "C: after the replacement the vendored binary is not xlings: " << c.err;
    };

    "919 a home selection is remembered once"_test[tag{"requires:python3"}] = [] {
        // Real CLI processes: a relative, spaced, Unicode home selected with --force persists and
        // is read through one hop; a locator in the destination and a broken one are handled;
        // a mirror change needs no network; a portable installation owns its locator; concurrent
        // writers publish whole; a failed publication keeps the previous pointer; a bare --force
        // rebuilds the target and keeps the origin.
        s::needs({"python3"});
        const auto root = resolved(s::case_dir());
        const auto original = root / fs::path(u8"original home कखग");
        const auto target = root / fs::path(u8"selected home 日本語 \U0001f642");
        const auto third = root / "third";
        s::project(original, {{"config.toml", "# Existing home\n"}});
        const auto user = root / "unselected user";
        const auto user_locator = user / ".mcpp/home.toml";
        s::project(user / ".mcpp", {{"home.toml", "home = [\n"}});
        env_list env{{"MCPP_HOME", original.string()}, {"MCPP_OFFLINE", "1"}, {"HOME", user.string()}, {"USERPROFILE", user.string()}};
        const auto run = [&](std::vector<std::string> args, std::optional<int> expected = std::nullopt, bool offline = true) {
            auto e = env;
            if (!offline) e.emplace_back("MCPP_OFFLINE", std::nullopt);
            args.insert(args.begin(), s::mcpp_path());
            auto p = e2e::run({.argv = args, .cwd = root, .env = e, .timeout = std::chrono::seconds{15}});
            if (expected) expect(fatal(p.started && p.status == *expected)) << args[1] << " " << (args.size() > 2 ? args[2] : "") << ": exit " << p.status << ", expected " << *expected << "\n" << p.out << p.err;
            return p;
        };
        const auto home_of = [&] { return s::json(run({"self", "env", "--format", "json"}, 0).out)["data"]; };
        const auto as_path = [](const nlohmann::json& v) { return resolved(fs::path(v.get<std::string>())); };

        const auto relative_target = target.lexically_relative(root).string();
        auto p = run({"self", "init", "--home", relative_target}, 1);
        expect(fatal(e2e::contains(p.err, "--force") && !fs::exists(target))) << p.out << p.err;
        p = run({"self", "init", "--force", "--home", relative_target});
        expect(fatal(p.status == 0 || p.status == 1)) << p.out << p.err;
        expect(fatal(fs::is_regular_file(target / "config.toml"))) << p.out << p.err;
        expect(fatal(fs::is_regular_file(original / "config.toml") && fs::is_regular_file(original / "home.toml"))) << "the origin lost its config or has no locator";
        auto data = home_of();
        expect(fatal(as_path(data["mcppHome"]) == target && as_path(data["originalHome"]) == original)) << data.dump();

        s::project(target, {{"home.toml", "home = " + nlohmann::json(third.generic_string()).dump() + "\n"}});
        data = home_of();
        expect(fatal(as_path(data["mcppHome"]) == target)) << "a locator in the destination was followed: " << data.dump();
        expect(fatal(!fs::exists(third))) << "the destination's locator target was made";
        const auto sentinel = target / "registry/retain-sentinel";
        s::project(target / "registry", {{"retain-sentinel", "keep"}});
        (void)run({"self", "init", "--force", "--home", target.string()});
        expect(fatal(fs::is_regular_file(sentinel))) << "an explicit force-home deleted existing data";

        for (std::string_view v : {"HTTPS_PROXY", "HTTP_PROXY", "ALL_PROXY"}) env.emplace_back(v, "http://127.0.0.1:9");
        env.emplace_back("NO_PROXY", "");
        p = run({"self", "config", "--mirror", "auto"}, 0, false);
        for (std::string_view w : {"Initialize", "Bootstrap", "Fetching"}) expect(fatal(s::lacks(p.err, w))) << p.err;
        expect(fatal(s::json_file(target / "registry/.xlings.json")["mirror"] == "auto")) << s::read_file(target / "registry/.xlings.json");
        expect(fatal(fs::is_regular_file(sentinel))) << "the mirror change removed the sentinel";

        s::project(original, {{"home.toml", "home = [\n"}});
        p = run({"self", "env", "--format", "json"}, 4);
        expect(fatal(e2e::contains(p.err, "locator") && s::lacks(p.err, "internal:"))) << p.err;
        (void)run({"self", "init", "--force", "--home", target.string()});
        expect(fatal(fs::is_regular_file(sentinel))) << "a repair removed the sentinel";
        expect(fatal(as_path(home_of()["mcppHome"]) == target)) << "the repaired locator does not select the target";
        (void)run({"self", "init", "--force", "--home", original.string()});
        expect(fatal(!fs::exists(original / "home.toml") && fs::is_regular_file(sentinel))) << "selecting the origin kept its locator or lost the sentinel";
        expect(fatal(as_path(home_of()["mcppHome"]) == original)) << "the origin is not selected";

        const auto portable = root / "portable";
        const auto portable_bin = portable / "bin" / fs::path(s::mcpp_path()).filename();
        fs::create_directories(portable_bin.parent_path());
        fs::copy_file(s::mcpp_path(), portable_bin, fs::copy_options::overwrite_existing);
        s::project(portable, {{"home.toml", "home = \"" + target.generic_string() + "\"\n"}});
        auto penv = env;
        penv.emplace_back("MCPP_HOME", std::nullopt);
        auto pp = e2e::run({.argv = {portable_bin.string(), "self", "env", "--format", "json"}, .cwd = root, .env = penv, .timeout = std::chrono::seconds{15}});
        expect(fatal(pp.started && pp.status == 0)) << pp.status << pp.out << pp.err;
        data = s::json(pp.out)["data"];
        expect(fatal(as_path(data["originalHome"]) == resolved(portable) && as_path(data["mcppHome"]) == target)) << data.dump();
        expect(fatal(s::read_file(user_locator) == "home = [\n")) << "the user's locator was written";

        std::vector<fs::path> raced;
        for (int i = 0; i < 4; ++i) raced.push_back(root / ("concurrent home " + std::to_string(i)));
        {
            std::vector<s::process> writers;
            for (const auto& d : raced)
                writers.push_back(s::process::start({.argv = {s::mcpp_path(), "self", "init", "--force", "--home", d.string()}, .cwd = root, .env = env}));
            for (int i = 0; i < 4; ++i) {
                const auto h = as_path(home_of()["mcppHome"]);
                expect(fatal(h == original || std::ranges::contains(raced, h))) << "a reader saw " << h;
            }
            for (auto& w : writers) {
                const int st = w.wait(std::chrono::seconds{15});
                expect(fatal(st == 0 || st == 1)) << "a writer exited " << st << ": " << w.piped();
            }
        }
        expect(fatal(std::ranges::contains(raced, as_path(home_of()["mcppHome"])))) << "the last selection is none of the writers'";
        expect(fatal(fs::is_regular_file(sentinel) && fs::is_regular_file(original / "config.toml"))) << "the race removed data";
        expect(fatal(s::glob(original, "home.toml.*.tmp").empty())) << "a normal writer left a staged locator";

        const auto locator = original / "home.toml";
        const auto before = s::read_file(locator);
        const auto blocked = root / "blocked publication";
        if (s::on_windows()) {
            fs::permissions(locator, fs::perms::owner_read, fs::perm_options::replace);
            p = run({"self", "init", "--force", "--home", blocked.string()}, 1);
            fs::permissions(locator, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
            expect(fatal(s::read_file(locator) == before)) << p.out << p.err;
        } else {
            const auto mode = fs::status(original).permissions();
            fs::permissions(original, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);
            std::ofstream control(original / "write-fault-control");
            const bool denied = !control;
            control.close();
            if (denied) {
                p = run({"self", "init", "--force", "--home", blocked.string()}, 1);
                fs::permissions(original, mode, fs::perm_options::replace);
                expect(fatal(s::read_file(locator) == before)) << p.out << p.err;
            } else {
                // This account bypasses directory write permissions: no POSIX write-fault proof.
                fs::remove(original / "write-fault-control");
                fs::permissions(original, mode, fs::perm_options::replace);
            }
        }
        expect(fatal(s::glob(original, "home.toml.*.tmp").empty())) << "a failed publication left a staged locator";

        (void)run({"self", "init", "--force", "--home", target.string()});
        s::project(original / "registry", {{"retained-old-data", "keep"}});
        std::vector<fs::path> markers;
        for (std::string_view name : {"registry", "cache", "build-cache", "bmi"}) {
            s::project(target / name, {{"bare-force-marker", "derived"}});
            markers.push_back(target / name / "bare-force-marker");
        }
        p = run({"self", "init", "--force"});
        expect(fatal(p.status == 0 || p.status == 1)) << p.out << p.err;
        expect(fatal(e2e::contains(p.err, "--force rebuilds") && e2e::contains(p.err, target.filename().string()))) << p.err;
        for (const auto& m : markers) expect(fatal(!fs::exists(m))) << "a bare --force kept " << m;
        expect(fatal(s::read_file(original / "registry/retained-old-data") == "keep" && fs::is_regular_file(locator))) << "a bare --force touched the origin";
        expect(fatal(as_path(home_of()["mcppHome"]) == target)) << "a bare --force changed the selection";
    };

    "920 the doctor checks resources without bootstrap"_test[tag{"requires:python3"}] = [] {
        // The provider's bounded probe_url names an HTTP refusal, a stall, a trickle and a TLS
        // refusal within 2 s each and changes nothing in the home; `self doctor` reaches the
        // resource server (exit 1, the corp rows, the build policy) within 7 s and changes
        // nothing; offline it skips the resources, makes no request and bootstraps nothing.
        s::needs({"python3"});
        if (s::on_windows()) skip("the resource oracle is a POSIX socket server (tests/support local_server)");
        fs::path provider;
        for (std::string_view v : {"MCPP_NETWORK_PROVIDER", "MCPP_VENDORED_XLINGS", "XLINGS_BIN"})
            if (auto x = s::env(v); x && !x->empty() && fs::is_regular_file(*x)) { provider = *x; break; }
        if (provider.empty() && fs::exists(s::shared_home() / "registry/bin/xlings")) provider = s::shared_home() / "registry/bin/xlings";
        if (provider.empty()) skip("no network provider (MCPP_NETWORK_PROVIDER, MCPP_VENDORED_XLINGS or XLINGS_BIN naming a file, or the home's xlings)");
        provider = resolved(provider);

        std::atomic<int> requests{0}, client_hellos{0};
        s::local_server http([&](s::connection& c) {
            const auto head = c.receive_until("\r\n\r\n");
            if (head.empty()) return;
            ++requests;
            const auto path = head.substr(head.find(' ') + 1, head.find(' ', head.find(' ') + 1) - head.find(' ') - 1);
            const std::string body = R"({"indexes":{}})";
            const int code = path == "/denied" ? 403 : path.starts_with("/cn/") ? 404 : 200;
            const auto reason = code == 403 ? "Forbidden" : code == 404 ? "Not Found" : "OK";
            if (!c.send(std::format("HTTP/1.0 {} {}\r\nContent-Length: {}\r\n\r\n", code, reason, body.size()))) return;
            if (path == "/stall") std::this_thread::sleep_for(std::chrono::seconds{2});
            if (path == "/trickle") {
                for (char b : body) {
                    if (!c.send(std::string_view(&b, 1))) return;
                    std::this_thread::sleep_for(std::chrono::milliseconds{100});
                }
            } else {
                (void)c.send(body);
            }
        });
        s::local_server tls([&](s::connection& c) {
            const auto hello = c.receive(1024, std::chrono::seconds{2});
            if (!hello.empty() && static_cast<unsigned char>(hello[0]) == 0x16) ++client_hellos;
            (void)c.send(std::string("\x15\x03\x03\x00\x02\x02\x28", 7));  // fatal handshake_failure
        });

        const auto root = s::case_dir();
        const auto home = root / "home", registry = home / "registry";
        const auto base = std::format("http://127.0.0.1:{}", http.port());
        s::project(home, {{"config.toml", "[xlings]\nbinary = " + nlohmann::json(provider.generic_string()).dump() + "\n"},
                          {"registry/.xlings.json", nlohmann::json{{"mirror", "auto"}, {"index_repos", {{{"name", "corp"}, {"url", base + "/corp.git"},
                              {"artifact", {{"GLOBAL", base + "/global"}, {"CN", base + "/cn"}}}}}}}.dump()}});
        env_list env{{"MCPP_HOME", home.string()}, {"XLINGS_HOME", registry.string()}, {"XLINGS_INDEX_BASE_URL", base + "/official"}, {"MCPP_OFFLINE", "0"}};
        auto hs = e2e::run({.argv = {provider.string(), "interface", "--version"}, .cwd = root, .env = env, .timeout = std::chrono::seconds{5}});
        expect(fatal(hs.started && hs.status == 0)) << hs.err;
        const auto features = s::json(hs.out)["features"];
        for (std::string_view f : {"network_diagnostics", "download_socket_budgets", "read_only_queries"})
            expect(fatal(std::ranges::contains(features, nlohmann::json(f)))) << hs.out;
        const auto before = snapshot(home);
        auto local_env = env;
        for (std::string_view v : {"HTTPS_PROXY", "https_proxy", "ALL_PROXY", "all_proxy"}) local_env.emplace_back(v, "");
        local_env.emplace_back("NO_PROXY", "*");
        local_env.emplace_back("no_proxy", "*");
        struct probe { std::string url; std::vector<std::string> expected; int http_status; };
        for (const auto& [url, expected, http_status] : {probe{base + "/denied", {"http"}, 403}, probe{base + "/stall", {"timeout"}, 200}, probe{base + "/trickle", {"timeout"}, 200},
                                                          probe{std::format("https://127.0.0.1:{}/resource", tls.port()), {"tls", "connect"}, 0}}) {
            const auto started = std::chrono::steady_clock::now();
            auto r = e2e::run({.argv = {provider.string(), "interface", "probe_url", "--args", nlohmann::json{{"url", url}, {"socket_timeout_ms", 500}}.dump()},
                               .cwd = root, .env = local_env, .timeout = std::chrono::seconds{3}});
            const auto took = std::chrono::steady_clock::now() - started;
            expect(fatal(r.started && r.status == 0 && !r.timed_out)) << url << ": " << r.out << r.err;
            nlohmann::json data;
            for (const auto& line : s::json_lines(r.out))
                if (line.value("dataKind", "") == "network_probe") { data = line["payload"]; break; }
            expect(fatal(!data.is_null())) << url << ": no network_probe record: " << r.out;
            expect(fatal(std::ranges::contains(expected, data["status"].get<std::string>()) && !data["reachable"].get<bool>())) << data.dump();
            expect(fatal(data["http_status"] == http_status)) << data.dump();
            expect(fatal(took < std::chrono::seconds{2})) << url << " took too long: " << data.dump();
            expect(fatal(snapshot(home) == before)) << "a resource probe changed the home";
        }
        expect(fatal(client_hellos.load() == 1)) << "the TLS probe never reached the handshake oracle (" << client_hellos.load() << ")";
        const int before_doctor = requests.load();
        const auto started = std::chrono::steady_clock::now();
        auto online = e2e::run({.argv = {s::mcpp_path(), "self", "doctor"}, .cwd = root, .env = env, .timeout = std::chrono::seconds{8}});
        expect(fatal(std::chrono::steady_clock::now() - started < std::chrono::seconds{7})) << online.err;
        expect(fatal(online.started && online.status == 1)) << online.status << online.out << online.err;
        expect(fatal(e2e::contains(online.err, "corp GLOBAL") && e2e::contains(online.err, ": reachable"))) << online.err;
        expect(fatal(e2e::contains(online.err, "corp CN") && e2e::contains(online.err, ": http"))) << online.err;
        expect(fatal(e2e::contains(online.err, "Checking build policy"))) << online.err;
        expect(fatal(snapshot(home) == before)) << "the online doctor changed the home";
        const int after_doctor = requests.load();
        expect(fatal(after_doctor - before_doctor >= 3)) << "the doctor never reached the resource server";
        env.emplace_back("MCPP_OFFLINE", "1");
        auto offline = e2e::run({.argv = {s::mcpp_path(), "self", "doctor"}, .cwd = root, .env = env, .timeout = std::chrono::seconds{8}});
        expect(fatal(offline.started && offline.status == 0)) << offline.status << offline.out << offline.err;
        expect(fatal(e2e::contains(offline.err, "Skipped (offline mode)"))) << offline.err;
        expect(fatal(requests.load() == after_doctor)) << "the offline doctor made a resource request";
        expect(fatal(snapshot(home) == before)) << "the offline doctor changed the home";
        expect(fatal(!fs::exists(registry / "subos"))) << "the doctor bootstrapped the provider";
    };

    "932 a fresh home with a space and non ascii builds"_test[tag{"requires:gcc"}][tag{"requires:unix-shell"}][tag{"requires:fresh-sandbox"}] = [] {
        // A fresh home at a spaced path initialises and builds and runs the default project; at a
        // non-ASCII path it builds too, or is refused naming gcc's module mapper.
        s::needs({"gcc", "unix-shell", "fresh-sandbox"});
        const auto work = s::case_dir();
        const auto run_in = [&](const fs::path& home) {
            fs::create_directories(home);
            const s::options o{.env = {{"NO_COLOR", "1"}, {"MCPP_HOME", home.string()}}};
            auto r = s::mcpp({"self", "init"}, work, o);
            expect(fatal(e2e::succeeded(r))) << "self init in " << home << " failed: " << s::text(r);
            return o;
        };
        auto o = run_in(work / "mc probe/home");
        auto r = s::mcpp({"new", "app"}, work, o);
        expect(fatal(e2e::succeeded(r))) << "mcpp new failed: " << s::text(r);
        r = s::mcpp({"run"}, work / "app", o);
        expect(fatal(e2e::succeeded(r))) << "the default build in a spaced home failed: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "Hello"))) << "the program did not run: " << s::text(r);
        o = run_in(work / fs::path(u8"mc probe/家"));
        fs::remove_all(work / "app/target");
        r = s::mcpp({"run"}, work / "app", o);
        if (e2e::succeeded(r))
            expect(fatal(e2e::contains(s::text(r), "Hello"))) << "the program did not run: " << s::text(r);
        else
            expect(fatal(e2e::contains(s::text(r), "gcc's module mapper cannot carry a path outside ASCII"))) << "the non-ASCII home failed without naming the cause: " << s::text(r);
    };
}};

}  // namespace

// A home's configuration: `self env` and `self init`, the mirror chosen without a bootstrap, the
// index migrated and healed in config.toml and the registry's .xlings.json, and a project's
// xlings environment materialised (formerly tests/e2e/10, 38, 39, 46, 88, 151).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

// A home of the case that does not exist yet (the scripts' `$TMP/mcpp-home`) and mcpp run in it.
struct cold_home {
    fs::path dir = s::case_dir() / "mcpp-home";
    s::options o{.env = {{"MCPP_HOME", dir.string()}}};
    e2e::outcome mcpp(std::vector<std::string> args) const { return s::mcpp(std::move(args), s::case_dir(), o); }
    std::string read(std::string_view rel) const { return fs::exists(dir / rel) ? s::read_file(dir / rel) : ""; }
};

nlohmann::json mcpplibs_entry(const fs::path& xj) {
    for (const auto& r : s::json_file(xj)["index_repos"])
        if (r["name"] == "mcpplibs") return r;
    fail("no mcpplibs entry in " + xj.string());
}

E2E config{"config", [] {
    "10 env command"_test = [] {
        // `self env` does not initialise a cold home; `self init --offline` lays it out (bin,
        // registry, build-cache/v1, cache, config.toml, a seeded .xlings.json with mcpplibs and
        // no awesome, the xlings binary); `self env` names MCPP_HOME, the xlings binary, mcpplibs.
        const cold_home h;
        const auto env = h.mcpp({"self", "env"});
        expect(fatal(!fs::exists(h.dir))) << "self env initialized a cold home";
        auto r = h.mcpp({"--offline", "self", "init"});
        expect(fatal(r.started && (r.status == 0 || r.status == 1))) << s::text(r);
        for (std::string_view d : {"bin", "registry", "build-cache/v1", "cache"})
            expect(fatal(fs::is_directory(h.dir / d))) << "missing " << d << "/";
        expect(fatal(fs::is_regular_file(h.dir / "config.toml"))) << "missing config.toml";
        expect(fatal(fs::is_regular_file(h.dir / "registry/.xlings.json"))) << "missing seeded .xlings.json";
        expect(fatal(fs::is_regular_file(h.dir / s::exe("registry/bin/xlings")))) << "xlings binary not acquired";
        const auto xj = h.read("registry/.xlings.json");
        expect(fatal(e2e::contains(xj, "\"name\": \"mcpplibs\""))) << "seeded .xlings.json missing mcpplibs";
        expect(fatal(s::lacks(xj, "\"name\": \"awesome\""))) << "seeded .xlings.json should NOT contain awesome";
        for (std::string_view w : {"MCPP_HOME", "xlings binary", "mcpplibs"})
            expect(fatal(e2e::contains(s::text(env), w))) << "no " << w << " in env output: " << s::text(env);
    };

    "38 self config mirror"_test = [] {
        // Reading the configuration initialises nothing; auto, GLOBAL and cn are written to the
        // registry's .xlings.json; an invalid mirror is refused.
        const cold_home h;
        auto r = h.mcpp({"self", "config"});
        expect(fatal(e2e::succeeded(r))) << "reading a fresh configuration failed: " << s::text(r);
        expect(fatal(!fs::exists(h.dir))) << "reading configuration initialized the home";
        for (auto [arg, want] : {std::pair{"auto", "auto"}, {"GLOBAL", "GLOBAL"}, {"cn", "CN"}}) {
            r = h.mcpp({"self", "config", "--mirror", arg});
            expect(fatal(e2e::succeeded(r))) << "self config --mirror " << arg << " failed: " << s::text(r);
            expect(fatal(e2e::contains(h.read("registry/.xlings.json"), std::format("\"mirror\": \"{}\"", want)))) << "mirror should be " << want << ": " << h.read("registry/.xlings.json");
        }
        r = h.mcpp({"self", "config", "--mirror", "BAD"});
        expect(fatal(r.started && r.status != 0)) << "an invalid mirror unexpectedly succeeded: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "invalid mirror"))) << "invalid mirror diagnostic missing: " << s::text(r);
    };

    "39 xlings index migration"_test = [] {
        // A home naming the legacy mcpp-index has its config.toml and .xlings.json migrated to
        // mcpplibs by `self config --mirror auto`, and `self config` shows mcpplibs only.
        const auto home = s::case_home(s::home::inherited);
        s::options o{.where = s::home::inherited};
        if (!s::env("MCPP_VENDORED_XLINGS") && fs::exists(s::shared_home() / "registry/bin/xlings"))
            o.env.emplace_back("MCPP_VENDORED_XLINGS", (s::shared_home() / "registry/bin/xlings").string());
        s::project(home, {{"config.toml", "[xlings]\nbinary = \"bundled\"\nhome   = \"\"\n\n[index]\ndefault = \"mcpp-index\"\n\n[index.repos.\"mcpp-index\"]\nurl = \"https://github.com/mcpp-community/mcpp-index.git\"\n\n[cache]\nsearch_ttl_seconds = 3600\n\n[build]\ndefault_jobs    = 0\n"},
                          {"registry/.xlings.json", R"({
  "activeSubos": "default",
  "index_repos": [
    {
      "name": "mcpp-index",
      "url": "https://github.com/mcpp-community/mcpp-index.git"
    }
  ],
  "lang": "en",
  "mirror": "GLOBAL",
  "subos": {
    "default": {
      "dir": ""
    }
  }
}
)"}});
        auto r = s::mcpp({"self", "config", "--mirror", "auto"}, s::case_dir(), o);
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        const auto cfg = s::read_file(home / "config.toml");
        expect(fatal(e2e::contains(cfg, "default = \"mcpplibs\""))) << "config.toml default index was not migrated: " << cfg;
        expect(fatal(e2e::contains(cfg, "[index.repos.\"mcpplibs\"]"))) << "config.toml repo table was not migrated: " << cfg;
        expect(fatal(s::lacks(cfg, "default = \"mcpp-index\"") && s::lacks(cfg, "[index.repos.\"mcpp-index\"]"))) << "config.toml still contains a legacy index key: " << cfg;
        const auto xj = s::read_file(home / "registry/.xlings.json");
        expect(fatal(e2e::contains(xj, "\"name\": \"mcpplibs\""))) << ".xlings.json repo name was not migrated: " << xj;
        expect(fatal(s::lacks(xj, "\"name\": \"mcpp-index\""))) << ".xlings.json still contains the legacy index name: " << xj;
        r = s::mcpp({"self", "config"}, s::case_dir(), o);
        expect(fatal(e2e::succeeded(r) && e2e::contains(r.out, "mcpplibs"))) << "self config did not show mcpplibs: " << s::text(r);
        expect(fatal(s::no_line(r.out, R"(index-repo.*mcpp-index[[:space:]]*:)"))) << "self config still shows the legacy index key: " << r.out;
    };

    "46 self config mirror no bootstrap"_test = [] {
        // On a fresh home, `self config --mirror GLOBAL` writes the mirror without a bootstrap or
        // a sandbox, and CN round-trips.
        const cold_home h;
        expect(fatal(!fs::exists(h.dir / "registry/.xlings.json"))) << "precondition: registry/.xlings.json already exists";
        auto r = h.mcpp({"self", "config", "--mirror", "GLOBAL"});
        expect(fatal(e2e::succeeded(r))) << "'self config --mirror GLOBAL' exited non-zero on a fresh MCPP_HOME: " << s::text(r);
        expect(fatal(e2e::contains(h.read("registry/.xlings.json"), "\"mirror\": \"GLOBAL\""))) << h.read("registry/.xlings.json");
        expect(fatal(s::no_line(s::text(r), "Initialize|Bootstrap|Fetching"))) << "mirror selection attempted a bootstrap: " << s::text(r);
        expect(fatal(!fs::exists(h.dir / "registry/subos/default"))) << "mirror selection created a sandbox";
        r = h.mcpp({"self", "config", "--mirror", "CN"});
        expect(fatal(e2e::succeeded(r))) << "switching back to CN failed: " << s::text(r);
        expect(fatal(e2e::contains(h.read("registry/.xlings.json"), "\"mirror\": \"CN\""))) << "the round-trip to CN did not stick: " << h.read("registry/.xlings.json");
    };

    "88 xlings environment"_test = [] {
        // A project's [xlings] deps and subos and its [xlings.workspace] are materialised once in
        // .mcpp/.xlings.json (no envs), with an advisory naming [xlings.workspace].
        s::project app(s::case_dir() / "app", {
            {"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[xlings]\ndeps = [\"ninja@1.12.1\"]\nsubos = \"dev\"\n\n[xlings.workspace]\nninja = \"1.12.1\"\n"},
            {"src/main.cpp", "int main() { return 0; }\n"},
            {".mcpp/.xlings/subos/dev/.xlings.json", s::read_file(s::shared_home() / "registry/subos/default/.xlings.json")}});
        auto r = app.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << "build errored: " << s::text(r);
        expect(fatal(app.exists(".mcpp/.xlings.json"))) << ".mcpp/.xlings.json was not written";
        const auto j = app.read(".mcpp/.xlings.json");
        for (std::string_view w : {"\"deps\"", "ninja@1.12.1", "\"subos\": \"dev\"", "\"workspace\"", "\"ninja\": \"1.12.1\""})
            expect(fatal(e2e::contains(j, w))) << "not materialized: " << w << "\n" << j;
        expect(fatal(s::lacks(j, "\"envs\""))) << "envs is materialized and nothing reads it: " << j;
        std::size_t n = 0;
        for (auto at = j.find("ninja@1.12.1"); at != std::string::npos; at = j.find("ninja@1.12.1", at + 1)) ++n;
        expect(fatal(n == 1u)) << "the same statement was materialized " << n << " times";
        expect(fatal(e2e::contains(s::text(r), "[xlings.workspace]"))) << "no advisory for [xlings] deps: " << s::text(r);
    };

    "151 index url artifact migration"_test[tag{"requires:python3"}] = [] {
        // A fresh seed names the new index url and its region artifact in both files; an old org
        // url is healed (idempotently, unrelated keys kept); a compact legacy entry is renamed and
        // gains both artifact halves; a flat default artifact becomes the region table; a user's
        // own artifact base is kept as written.
        s::needs({"python3"});
        const cold_home h;
        const std::string new_url = "https://github.com/mcpplibs/mcpp-index.git", old_url = "https://github.com/mcpp-community/mcpp-index.git";
        const std::string art = "https://github.com/xlings-res/mcpp-index", art_cn = "https://gitcode.com/xlings-res/mcpp-index";
        const nlohmann::json region{{"GLOBAL", art}, {"CN", art_cn}};
        const auto cfg = h.dir / "config.toml", xj = h.dir / "registry/.xlings.json";
        const auto heal = [&] {
            auto r = h.mcpp({"self", "config", "--mirror", "auto"});
            expect(fatal(e2e::succeeded(r))) << s::text(r);
        };
        heal();
        expect(fatal(e2e::contains(s::read_file(cfg), std::format("url      = \"{}\"", new_url)))) << "config.toml missing the new url: " << s::read_file(cfg);
        expect(fatal(e2e::contains(s::read_file(cfg), std::format("artifact = {{ GLOBAL = \"{}\", CN = \"{}\" }}", art, art_cn)))) << "config.toml missing the region artifact: " << s::read_file(cfg);
        expect(fatal(e2e::contains(s::read_file(xj), std::format("\"url\": \"{}\"", new_url)))) << "seeded .xlings.json missing the new url: " << s::read_file(xj);
        expect(fatal(mcpplibs_entry(xj)["artifact"] == region)) << "seeded .xlings.json missing the region artifact: " << s::read_file(xj);
        expect(fatal(s::lacks(s::read_file(cfg) + s::read_file(xj), "mcpp-community/mcpp-index"))) << "the old org URL leaked into a fresh seed";

        s::project(h.dir, {{"config.toml", std::format("[xlings]\nbinary = \"bundled\"\nhome   = \"\"\n\n[index]\ndefault = \"mcpplibs\"\n\n[index.repos.\"mcpplibs\"]\nurl = \"{}\"\n", old_url)},
                           {"registry/.xlings.json", std::format("{{\n  \"index_repos\": [\n    {{ \"name\": \"mcpplibs\", \"url\": \"{}\" }}\n  ],\n  \"subos\": \"default\",\n  \"lang\": \"en\",\n  \"mirror\": \"auto\"\n}}\n", old_url)}});
        heal();
        expect(fatal(e2e::contains(s::read_file(cfg), std::format("url = \"{}\"", new_url)))) << "config.toml url not healed: " << s::read_file(cfg);
        auto e = mcpplibs_entry(xj);
        expect(fatal(e.value("url", "") == new_url && e.value("artifact", nlohmann::json{}) == region)) << ".xlings.json not healed: " << s::read_file(xj);
        expect(fatal(e2e::contains(s::read_file(xj), "\"subos\": \"default\""))) << "an unrelated .xlings.json key was lost";
        expect(fatal(s::lacks(s::read_file(cfg) + s::read_file(xj), "mcpp-community/mcpp-index"))) << "the old org URL survived the heal";
        const auto cfg1 = s::read_file(cfg), xj1 = s::read_file(xj);
        heal();
        expect(fatal(s::read_file(cfg) == cfg1)) << "the config.toml heal is not idempotent";
        expect(fatal(s::read_file(xj) == xj1)) << "the .xlings.json heal is not idempotent";

        s::project(h.dir, {{"registry/.xlings.json", std::format(R"({{"index_repos":[{{"name":"mcpp-index","url":"{}"}}],"mirror":"auto"}})", old_url)}});
        heal();
        (void)mcpplibs_entry(xj);
        expect(fatal(e2e::contains(s::read_file(xj), "xlings-res/mcpp-index"))) << "the compact artifact was not injected: " << s::read_file(xj);
        expect(fatal(e2e::contains(s::read_file(xj), "gitcode.com/xlings-res/mcpp-index"))) << "the compact artifact has no CN half: " << s::read_file(xj);

        s::project(h.dir, {{"registry/.xlings.json", std::format("{{\n  \"index_repos\": [\n    {{ \"name\": \"mcpplibs\", \"url\": \"{}\", \"artifact\": \"{}\" }}\n  ],\n  \"mirror\": \"CN\"\n}}\n", new_url, art)}});
        heal();
        expect(fatal(mcpplibs_entry(xj).value("artifact", nlohmann::json{}) == region)) << "the flat default artifact was not upgraded: " << s::read_file(xj);
        const auto xj4 = s::read_file(xj);
        heal();
        expect(fatal(s::read_file(xj) == xj4)) << "the region upgrade is not idempotent:\n" << xj4 << "\n---\n" << s::read_file(xj);

        s::project(h.dir, {{"config.toml", std::format("[index]\ndefault = \"mcpplibs\"\n\n[index.repos.\"mcpplibs\"]\nurl = \"{}\"\nartifact = \"https://mirror.example/idx\"\n", new_url)}});
        heal();
        expect(fatal(e2e::contains(s::read_file(xj), "https://mirror.example/idx"))) << "a user artifact base was rewritten: " << s::read_file(xj);
        expect(fatal(s::lacks(s::read_file(xj), "gitcode.com/xlings-res/mcpp-index"))) << "a user artifact base gained a CN half: " << s::read_file(xj);
        expect(fatal(s::lacks(s::read_file(xj), "mcpp-community"))) << "the compact old org URL survived";
    };
}};

}  // namespace

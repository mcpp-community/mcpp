#include <gtest/gtest.h>

import std;
import mcpp.manifest;
import mcpp.platform;
import mcpp.build.prepare_inputs;
import mcpp.toolchain.triple;

// SPEC-013 (design 2026-10-28): what a platform states, how it is read in its
// three places, which platform a graph selects, how the tiers merge, and the
// two templates of the link (artifact names, seal steps). Tables, so each rule
// is one row.

namespace mm = mcpp::manifest;
namespace mp = mcpp::manifest::platform;
namespace pf = mcpp::platform;
namespace cfgpred = mcpp::build::cfgpred;

namespace {

mm::Manifest parse(std::string_view text) {
    auto m = mm::parse_string(text);
    if (!m) ADD_FAILURE() << m.error().format();
    return m ? *m : mm::Manifest{};
}

std::string parse_error(std::string_view text) {
    auto m = mm::parse_string(text);
    return m ? std::string{} : m.error().message;
}

pf::Matches matcher(std::string_view triple, std::string_view platformPackage = {}) {
    auto c = cfgpred::context_for(triple, platformPackage);
    return [c](std::string_view sel) { return cfgpred::matches(std::string(sel), c); };
}

constexpr std::string_view kPlatform = R"(
[package]
name = "board-platform"
version = "1.2.0"

[platform]
targets   = ["x86_64-openkal", "aarch64-openkal"]
toolchain = "llvm@23.1.3"
runner    = { cmd = ["okrun", "{}"], host = "same-arch" }
carrier   = { package = "openkal-run@0.1.1", section = ".oke", main = "carrier/main.cpp" }

[platform.layers]
"c++-abi"        = "openkal-llvm-runtime@0.16.3"
c-abi            = { name = "openkal-musl", version = "0.22.3", default-features = false, features = ["posix"] }
kernel-abi       = false

[platform.tools]
okld  = "openkal-tools@0.1.1"
probe = { program = "probe-rs" }

[platform.build]
cxxflags = ["-fPIC"]
[platform.build.'cfg(arch = "aarch64")']
cxxflags = ["-ffixed-x18"]

[platform.link]
driver = "lld"
start  = "ok_main"
flags  = ["-shared"]
inputs = [{ from = ["okld", "note-source"], as = "ok-notes.S" }, { file = "crt/x.o" }]
exe    = "{name}.oke"
shared = "lib{name}.okso"
seal   = [["okld", "note", "{}"], ["okld", "check", "{}", "--with", "{needed}"]]

[platform.hosted]
toolchain = "llvm@23.1.3"
[platform.hosted.layers]
c-abi = "openkal-musl@0.22.3"

[platform.versions]
openkal-musl = "0.22.3"
)";

} // namespace

// ── The reader ──────────────────────────────────────────────────────────────

TEST(PlatformManifest, EveryGroupIsRead) {
    auto m = parse(kPlatform);
    ASSERT_TRUE(m.platformDeclared);
    auto const& p = m.platform;
    EXPECT_EQ(p.targets.value, (std::vector<std::string>{"x86_64-openkal", "aarch64-openkal"}));
    EXPECT_EQ(p.toolchain.value, "llvm@23.1.3");
    EXPECT_EQ(p.runner.cmd, (std::vector<std::string>{"okrun", "{}"}));
    EXPECT_EQ(p.runner.host, "same-arch");
    EXPECT_EQ(p.carrier.package.spec.shortName, "openkal-run");
    EXPECT_EQ(p.carrier.section, ".oke");
    EXPECT_TRUE(p.carrier.main.ends_with("carrier/main.cpp"));
    ASSERT_TRUE(p.layers.contains("c++-abi"));
    EXPECT_EQ(p.layers.at("c++-abi").ref.spec.version, "0.16.3");
    EXPECT_EQ(p.layers.at("c-abi").ref.spec.shortName, "openkal-musl");
    EXPECT_FALSE(p.layers.at("c-abi").ref.spec.defaultFeatures);
    EXPECT_EQ(p.layers.at("c-abi").ref.spec.features, (std::vector<std::string>{"posix"}));
    EXPECT_TRUE(p.layers.at("kernel-abi").ref.off);
    EXPECT_EQ(p.tools.at("okld").ref.spec.shortName, "openkal-tools");
    EXPECT_EQ(p.tools.at("probe").ref.program, "probe-rs");
    EXPECT_EQ(p.build.cxxflags, (std::vector<std::string>{"-fPIC"}));
    ASSERT_EQ(p.buildCfg.size(), 1u);
    EXPECT_EQ(p.link.driver.value, "lld");
    EXPECT_EQ(p.link.start.value, "ok_main");
    ASSERT_EQ(p.link.inputs.size(), 2u);
    EXPECT_EQ(p.link.inputs[0].as, "ok-notes.S");
    EXPECT_TRUE(p.link.inputs[1].file.ends_with("crt/x.o"));
    EXPECT_EQ(p.link.seal.size(), 2u);
    EXPECT_EQ(p.hosted.toolchain.value, "llvm@23.1.3");
    EXPECT_TRUE(p.hosted.layers.contains("c-abi"));
    EXPECT_EQ(p.versions.at("openkal-musl"), "0.22.3");
    EXPECT_EQ(p.runner.origin.tier, mp::Tier::Platform);
    EXPECT_GT(p.runner.origin.line, 0);
}

TEST(PlatformManifest, RefusalsNameTheKey) {
    const std::string head = "[package]\nname = \"p\"\nversion = \"1.0.0\"\n";
    struct Row { std::string body; std::string_view says; };
    const Row rows[] = {
        { "[platform]\ntoolchain = \"llvm@23.1.3\"\n", "names the targets it serves" },
        { "[platform]\ntargets = [\"x86_64-openkal\"]\n[platform.layers]\nc-lib = \"x@1\"\n",
          "not a target-side layer" },
        { "[platform]\ntargets = [\"x86_64-openkal\"]\n[platform.link]\ndriver = \"gold\"\n",
          "\"compiler\"" },
        { "[platform]\ntargets = [\"x86_64-openkal\"]\n[platform.link]\nexe = \"x.oke\"\n",
          "{name}" },
        { "[platform]\ntargets = [\"x86_64-openkal\"]\n[platform.link]\nexe = \"lib{name}.oke\"\n",
          "begins with the target's name" },
        { "[platform]\ntargets = [\"x86_64-openkal\"]\nrunner = []\n", "empty command" },
        { "[platform]\ntargets = [\"x86_64-openkal\"]\nrunner = { cmd = [\"x\"], host = \"far\" }\n",
          "same-arch" },
        { "[platform]\ntargets = [\"x86_64-openkal\"]\n[platform.layers]\nc-abi = { version = \"1\" }\n",
          "names the package too" },
        { "[platform]\ntargets = [\"x86_64-openkal\"]\n[platform.layers]\nc-abi = { program = \"x\" }\n",
          "for a tool only" },
        { "[platform]\ntargets = [\"x86_64-openkal\"]\n[platform.link]\nshared = \"{name}{name}\"\n",
          "exactly once" },
        { "[platform]\ntargets = [\"x86_64-openkal\"]\n[platform.link]\nshared = \"lib/{name}.okso\"\n",
          "names no directory" },
        { "[platform]\ntargets = [\"x86_64-openkal\"]\nplugin = \"gen.mcpp\"\n",
          "must be \"build.mcpp\"" },
        { "[platform]\ntargets = [\"x86_64-openkal\"]\n[platform.layers]\n"
          "c-abi = { git = \"https://x/y.git\", tag = \"v1\" }\n",
          "a git source names the package too" },
        { "[target.'cfg(platform = \"openkal\")']\nplatform = \"x@1.0.0\"\n",
          "cannot be selected under a selector" },
        { "[target.x86_64-openkal]\nlayers = \"x\"\n", "must be a table" },
    };
    for (auto const& r : rows) {
        auto e = parse_error(head + r.body);
        EXPECT_NE(e.find(r.says), std::string::npos) << r.body << "\n=> " << e;
    }
}

// An unknown key, in `[platform]`, its tables or `[host]`, is a warning: a
// manifest written for a later mcpp still reads (SPEC-013 §18.1).
TEST(PlatformManifest, UnknownKeysWarn) {
    auto m = parse(R"(
[package]
name = "p"
version = "1.0.0"

[platform]
targets = ["x86_64-openkal"]
runner  = { cmd = ["okrun", "{}"], later = true }
[platform.hosted]
runner = ["x"]
[platform.link]
exe   = "{name}.oke"
later = "x"
[platform.build]
rustflags = ["-x"]

[host]
runner = ["x"]
)");
    std::string all;
    for (auto const& w : m.schemaWarnings) all += w + "\n";
    for (std::string_view key : { "'later' is not a key of a runner", "[platform.hosted] has unsupported key 'runner'",
                                  "[platform.link] has unsupported key 'later'",
                                  "[platform.build] has unsupported key 'rustflags'",
                                  "[host] has unsupported key 'runner'" })
        EXPECT_NE(all.find(key), std::string::npos) << key << "\n=> " << all;
    EXPECT_EQ(m.platform.link.exe.value, "{name}.oke");
}

TEST(PlatformManifest, OverridesAndHostAreRead) {
    auto m = parse(R"(
[package]
name = "app"
version = "0.1.0"

[host]
platform = "openkal-platform@0.17.1"

[target.'cfg(platform = "openkal")']
runner = { cmd = ["my-okrun", "{}"], exclusive = true }
[target.'cfg(platform = "openkal")'.layers]
c-abi = { path = "../openkal-musl" }
[target.'cfg(platform = "openkal")'.tools]
okld = { path = "../openkal-image/tools" }
[target.'cfg(platform = "openkal")'.link]
flags = ["--build-id"]

[target.x86_64-openkal]
platform = "none"
)");
    for (auto const& w : m.schemaWarnings) ADD_FAILURE() << "unexpected warning: " << w;
    EXPECT_TRUE(m.hostPlatform.platformSet);
    EXPECT_EQ(m.hostPlatform.platform.spec.shortName, "openkal-platform");
    ASSERT_EQ(m.platformOverrides.size(), 2u);
    auto const& o = m.platformOverrides[0];
    EXPECT_EQ(o.fields.runner.cmd.front(), "my-okrun");
    EXPECT_TRUE(o.fields.runner.exclusive);
    EXPECT_TRUE(o.fields.layers.at("c-abi").ref.spec.path.ends_with("openkal-musl"));
    EXPECT_TRUE(o.fields.tools.at("okld").ref.spec.path.ends_with("openkal-image/tools"));
    EXPECT_EQ(o.fields.link.flags.value, (std::vector<std::string>{"--build-id"}));
    EXPECT_EQ(o.fields.runner.origin.tier, mp::Tier::Manifest);
    EXPECT_TRUE(m.platformOverrides[1].platformNone);
    // The runner's command is the row's template as well.
    EXPECT_EQ(m.targetOverrides.at("cfg(platform = \"openkal\")").runner.front(), "my-okrun");
}

TEST(PlatformManifest, ReferencesInEveryForm) {
    struct Row { std::string_view text; std::string_view ns, name, version; };
    const Row rows[] = {
        { "openkal-platform@0.17.1",          "mcpplibs", "openkal-platform", "0.17.1" },
        { "mcpplibs.openkal-platform@0.17.1", "mcpplibs", "openkal-platform", "0.17.1" },
        { "acme.board@1.0.0",                 "acme",     "board",            "1.0.0" },
    };
    for (auto const& r : rows) {
        auto ref = mp::parse_ref_text(r.text);
        ASSERT_TRUE(ref) << r.text;
        EXPECT_EQ(ref->spec.namespace_, r.ns);
        EXPECT_EQ(ref->spec.shortName, r.name);
        EXPECT_EQ(ref->spec.version, r.version);
    }
    EXPECT_FALSE(mp::parse_ref_text("openkal-platform"));
    EXPECT_FALSE(mp::parse_ref_text("@1.0"));
    EXPECT_EQ(mp::platform_name("openkal-platform"), "openkal");
    EXPECT_EQ(mp::platform_name("stm32-bsp"), "stm32-bsp");
}

// ── Selection: the first tier that answers decides ─────────────────────────

TEST(PlatformSelect, TheTiersInOrder) {
    auto m = parse(R"(
[package]
name = "app"
version = "0.1.0"
[target.'cfg(os = "openkal")']
platform = "project-platform@1.0.0"
)");
    auto cfg = parse(R"(
[package]
name = "cfg"
version = "0.1.0"
[target.x86_64-openkal]
platform = "machine-platform@1.0.0"
)");
    auto t = matcher("x86_64-openkal");
    auto pick = [&](std::string_view flag, std::string_view env, bool project, bool config,
                    std::string_view column) {
        pf::ChoiceInputs in{ flag, env, "MCPP_TARGET_X86_64_OPENKAL_PLATFORM",
                             project ? std::span<const mp::Override>(m.platformOverrides)
                                     : std::span<const mp::Override>{},
                             config ? std::span<const mp::Override>(cfg.platformOverrides)
                                    : std::span<const mp::Override>{},
                             column };
        auto s = pf::select_target(in, t);
        EXPECT_TRUE(s) << (s ? "" : s.error());
        return s ? (s->none ? std::string("none") : s->ref.spec.shortName) + "|"
                       + std::string(mp::tier_name(s->origin.tier))
                 : std::string("error");
    };
    EXPECT_EQ(pick("flag-platform@1.0.0", "env-platform@1.0.0", true, true, "row-platform@1.0.0"),
              "flag-platform|flag");
    EXPECT_EQ(pick("", "env-platform@1.0.0", true, true, "row-platform@1.0.0"), "env-platform|env");
    EXPECT_EQ(pick("", "", true, true, "row-platform@1.0.0"), "project-platform|mcpp.toml");
    EXPECT_EQ(pick("", "", false, true, "row-platform@1.0.0"), "machine-platform|mcpp.toml");
    EXPECT_EQ(pick("", "", false, false, "row-platform@1.0.0"), "row-platform|built-in");
    EXPECT_EQ(pick("", "", false, false, ""), "none|built-in");
    EXPECT_EQ(pick("none", "", true, true, "row-platform@1.0.0"), "none|flag");
}

TEST(PlatformSelect, TheMostSpecificSelectorDecides) {
    auto m = parse(R"(
[package]
name = "app"
version = "0.1.0"
[target.x86_64-openkal]
platform = "triple@1.0.0"
[target.'cfg(arch = "x86_64")']
platform = "arch@1.0.0"
)");
    pf::ChoiceInputs in{ "", "", "", m.platformOverrides, {}, "" };
    auto s = pf::select_target(in, matcher("x86_64-openkal"));
    ASSERT_TRUE(s);
    EXPECT_EQ(s->ref.spec.shortName, "triple");
    auto a = pf::select_target(in, matcher("aarch64-openkal"));
    ASSERT_TRUE(a);
    EXPECT_TRUE(a->none);
}

TEST(PlatformSelect, TheHostGraph) {
    mp::Override project, config;
    config.platformSet = true;
    config.platform = *mp::parse_ref_text("machine@1.0.0");
    auto s = pf::select_host("", false, project, config);
    ASSERT_TRUE(s);
    EXPECT_EQ(s->ref.spec.shortName, "machine");
    project.platformSet = true;
    project.platformNone = true;
    s = pf::select_host("", false, project, config);
    ASSERT_TRUE(s);
    EXPECT_TRUE(s->none);
    s = pf::select_host("flag@2.0.0", true, project, config);
    ASSERT_TRUE(s);
    EXPECT_EQ(s->ref.spec.shortName, "flag");
    EXPECT_EQ(s->origin.tier, mp::Tier::Flag);
}

TEST(PlatformSelect, ADirectoryIsAPath) {
    auto s = pf::parse_choice("path:/opt/board", {});
    ASSERT_TRUE(s);
    // Absolute on this machine: `D:/opt/board` on a Windows one.
    EXPECT_TRUE(s->ref.spec.path.ends_with("/opt/board")) << s->ref.spec.path;
    s = pf::parse_choice("../board", {});
    ASSERT_TRUE(s);
    EXPECT_FALSE(s->ref.spec.path.empty());
    EXPECT_FALSE(pf::parse_choice("not-a-reference", {}));
}

// ── Merge: per field, flags appended, sequences replaced ───────────────────

TEST(PlatformMerge, TheRulesOfSection45) {
    auto base = parse(kPlatform).platform;
    mp::Statement over;
    over.runner.cmd = {"my-okrun", "{}"};
    over.runner.set = true;                      // argv form: keeps host
    over.build.cxxflags = {"-DUSER"};
    over.link.flags = { {"--build-id"}, {}, true };
    over.link.seal = { {"sign", "{}"} };
    over.link.sealSet = true;
    over.layers["c-abi"] = { mp::PackageRef{ .off = true }, {} };
    pf::merge(base, over);
    EXPECT_EQ(base.runner.cmd.front(), "my-okrun");
    EXPECT_EQ(base.runner.host, "same-arch");
    EXPECT_EQ(base.build.cxxflags, (std::vector<std::string>{"-fPIC", "-DUSER"}));
    EXPECT_EQ(base.link.flags.value, (std::vector<std::string>{"-shared", "--build-id"}));
    ASSERT_EQ(base.link.seal.size(), 1u);
    EXPECT_EQ(base.link.seal.front().front(), "sign");
    EXPECT_EQ(base.link.inputs.size(), 2u);      // not stated: kept
    EXPECT_TRUE(base.layers.at("c-abi").ref.off);
    EXPECT_FALSE(base.layers.at("c++-abi").ref.off);
    EXPECT_EQ(base.link.start.value, "ok_main");
}

TEST(PlatformMerge, AHostTripleTakesTheHostedPartOnly) {
    auto p = parse(kPlatform).platform;
    auto h = pf::hosted_view(p);
    EXPECT_EQ(h.toolchain.value, "llvm@23.1.3");
    EXPECT_TRUE(h.layers.contains("c-abi"));
    EXPECT_FALSE(h.layers.contains("c++-abi"));
    EXPECT_FALSE(h.runner.set);
    EXPECT_FALSE(h.link.driver.set);
    EXPECT_TRUE(h.build.empty());
    auto served = pf::serves(p, "board-platform", *mcpp::toolchain::triple::parse("x86_64-linux-gnu"));
    ASSERT_TRUE(served);
    EXPECT_TRUE(*served);
    auto target = pf::serves(p, "board-platform", *mcpp::toolchain::triple::parse("x86_64-openkal"));
    ASSERT_TRUE(target);
    EXPECT_FALSE(*target);
    p.hosted = {};
    auto refused = pf::serves(p, "board-platform", *mcpp::toolchain::triple::parse("x86_64-linux-gnu"));
    ASSERT_FALSE(refused);
    EXPECT_NE(refused.error().find("[platform.hosted]"), std::string::npos);
}

TEST(PlatformMerge, CfgTablesFollowTheTarget) {
    auto p = parse(kPlatform).platform;
    auto x = pf::evaluate_flags(p, matcher("x86_64-openkal"));
    auto a = pf::evaluate_flags(p, matcher("aarch64-openkal"));
    EXPECT_EQ(x.cxxflags, (std::vector<std::string>{"-fPIC"}));
    EXPECT_EQ(a.cxxflags, (std::vector<std::string>{"-fPIC", "-ffixed-x18"}));
}

TEST(PlatformMerge, CfgPlatformMatchesTheNameOrThePackage) {
    EXPECT_TRUE(cfgpred::matches("cfg(platform = \"openkal\")",
                                 cfgpred::context_for("x86_64-openkal", "openkal-platform")));
    EXPECT_TRUE(cfgpred::matches("cfg(platform = \"openkal-platform\")",
                                 cfgpred::context_for("x86_64-openkal", "openkal-platform")));
    EXPECT_FALSE(cfgpred::matches("cfg(platform = \"openkal\")",
                                  cfgpred::context_for("x86_64-openkal")));
    EXPECT_GT(mcpp::manifest::cfg::specificity("cfg(os = \"openkal\")"),
              mcpp::manifest::cfg::specificity("cfg(platform = \"openkal\")"));
}

// ── The link's templates ────────────────────────────────────────────────────

TEST(PlatformLink, NamePatterns) {
    auto e = pf::split_name_pattern("{name}.oke");
    ASSERT_TRUE(e);
    EXPECT_EQ(e->first, "");
    EXPECT_EQ(e->second, ".oke");
    auto s = pf::split_name_pattern("lib{name}.okso");
    ASSERT_TRUE(s);
    EXPECT_EQ(s->first, "lib");
    EXPECT_EQ(s->second, ".okso");
    EXPECT_FALSE(pf::split_name_pattern("name.oke"));
    EXPECT_FALSE(pf::split_name_pattern("{name}{name}"));
    EXPECT_FALSE(pf::split_name_pattern("bin/{name}"));
}

TEST(PlatformLink, SealSteps) {
    const std::vector<std::string> check{"okld", "check", "{}", "--with", "{needed}"};
    const std::vector<std::string> two{"a.okso", "b.okso"};
    EXPECT_EQ(pf::expand_step(check, "x.oke.tmp", two),
              (std::vector<std::string>{"okld", "check", "x.oke.tmp", "--with", "a.okso",
                                        "--with", "b.okso"}));
    EXPECT_EQ(pf::expand_step(check, "x.oke.tmp", {}),
              (std::vector<std::string>{"okld", "check", "x.oke.tmp"}));
    EXPECT_EQ(pf::expand_step({"sign", "--image={}", "{needed}"}, "x", two),
              (std::vector<std::string>{"sign", "--image=x", "a.okso", "b.okso"}));
}

// ── The run (§10) ───────────────────────────────────────────────────────────

// A `same-arch` runner runs an artifact on a machine of its architecture:
// this one when they agree; the execution host, with that architecture's
// program, when one is named; refused otherwise. `--no-runner` and every
// other runner are this machine's.
TEST(PlatformRun, TheRunnerReachesTheArtifactsArchitecture) {
    using R = pf::RunnerReach;
    mp::Runner same;
    same.set = true;
    same.cmd = {"okrun", "{}"};
    same.host = "same-arch";
    mp::Runner any = same;
    any.host = "any";
    mp::Runner bare = same;
    bare.host.clear();
    mp::Runner none;
    struct Row { const mp::Runner* runner; const char* target; bool executionHost; bool noRunner; R want; };
    const Row rows[] = {
        { &same, "x86_64",  false, false, R::Here },
        { &same, "x86_64",  true,  false, R::Here },
        { &same, "aarch64", false, false, R::Refused },
        { &same, "aarch64", true,  false, R::TargetArchitecture },
        { &same, "aarch64", true,  true,  R::Here },
        { &same, "aarch64", false, true,  R::Here },
        { &any,  "aarch64", true,  false, R::Here },
        { &any,  "aarch64", false, false, R::Here },
        { &bare, "aarch64", true,  false, R::Here },
        { &none, "aarch64", true,  false, R::Here },
    };
    for (auto const& r : rows)
        EXPECT_EQ(pf::runner_reach(*r.runner, r.target, "x86_64", r.executionHost, r.noRunner), r.want)
            << r.runner->host << " " << r.target << " executionHost=" << r.executionHost
            << " noRunner=" << r.noRunner;
}

// ── The report ──────────────────────────────────────────────────────────────

TEST(PlatformReport, SourcesAndJson) {
    pf::Resolved r;
    r.active = true;
    r.package = "mcpplibs.openkal-platform";
    r.version = "0.17.1";
    r.fields = parse(kPlatform).platform;
    r.selection.origin = { mp::Tier::BuiltIn, {}, 0, "the target table" };
    r.layers["c-abi"] = { "openkal-musl@0.22.3", mp::Tier::Platform };
    r.layers["c++-abi"] = { "acme-runtime@1.0.0", mp::Tier::Graph };
    EXPECT_EQ(r.display(), "openkal-platform 0.17.1");
    EXPECT_EQ(r.identity(), "mcpplibs.openkal-platform@0.17.1");
    auto rows = pf::report_rows(r);
    ASSERT_FALSE(rows.empty());
    bool graph = false;
    for (auto const& [k, v] : rows)
        if (k == "c++-abi") graph = v.find("(graph)") != std::string::npos;
    EXPECT_TRUE(graph);
    auto json = pf::to_json(r);
    EXPECT_NE(json.find("\"name\":\"mcpplibs.openkal-platform\""), std::string::npos);
    EXPECT_NE(json.find("\"source\":\"built-in\""), std::string::npos);
    EXPECT_NE(json.find("\"layers.c++-abi\""), std::string::npos);
    EXPECT_EQ(pf::to_json(pf::Resolved{}), "null");
}

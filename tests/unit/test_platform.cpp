
import std;
import mcpplibs.mtest;
import mcpp.manifest;
import mcpp.platform;
import mcpp.build.prepare_inputs;
import mcpp.toolchain.triple;

using namespace mcpplibs::mtest;

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
    if (!m) expect(false) << m.error().format();
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

UTest every_group_is_read{"PlatformManifest/EveryGroupIsRead", [] {
    auto m = parse(kPlatform);
    expect(fatal(bool(m.platformDeclared)));
    auto const& p = m.platform;
    expect(eq(p.targets.value, (std::vector<std::string>{"x86_64-openkal", "aarch64-openkal"})));
    expect(eq(p.toolchain.value, "llvm@23.1.3"));
    expect(eq(p.runner.cmd, (std::vector<std::string>{"okrun", "{}"})));
    expect(eq(p.runner.host, "same-arch"));
    expect(eq(p.carriers.at(0).package.spec.shortName, "openkal-run"));
    expect(eq(p.carriers.at(0).section, ".oke"));
    expect(p.carriers.at(0).main.ends_with("carrier/main.cpp"));
    expect(fatal(p.layers.contains("c++-abi")));
    expect(eq(p.layers.at("c++-abi").ref.spec.version, "0.16.3"));
    expect(eq(p.layers.at("c-abi").ref.spec.shortName, "openkal-musl"));
    expect(!(p.layers.at("c-abi").ref.spec.defaultFeatures));
    expect(eq(p.layers.at("c-abi").ref.spec.features, (std::vector<std::string>{"posix"})));
    expect(bool(p.layers.at("kernel-abi").ref.off));
    expect(eq(p.tools.at("okld").ref.spec.shortName, "openkal-tools"));
    expect(eq(p.tools.at("probe").ref.program, "probe-rs"));
    expect(eq(p.build.cxxflags, (std::vector<std::string>{"-fPIC"})));
    expect(fatal(eq(p.buildCfg.size(), 1u)));
    expect(eq(p.link.driver.value, "lld"));
    expect(eq(p.link.start.value, "ok_main"));
    expect(fatal(eq(p.link.inputs.size(), 2u)));
    expect(eq(p.link.inputs[0].as, "ok-notes.S"));
    expect(p.link.inputs[1].file.ends_with("crt/x.o"));
    expect(eq(p.link.seal.size(), 2u));
    expect(eq(p.hosted.toolchain.value, "llvm@23.1.3"));
    expect(p.hosted.layers.contains("c-abi"));
    expect(eq(p.versions.at("openkal-musl"), "0.22.3"));
    expect(eq(p.runner.origin.tier, mp::Tier::Platform));
    expect(gt(p.runner.origin.line, 0));
}};

UTest refusals_name_the_key{"PlatformManifest/RefusalsNameTheKey", [] {
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
        expect(ne(e.find(r.says), std::string::npos)) << r.body << "\n=> " << e;
    }
}};

// An unknown key, in `[platform]`, its tables or `[host]`, is a warning: a
// manifest written for a later mcpp still reads (SPEC-013 §18.1).
UTest unknown_keys_warn{"PlatformManifest/UnknownKeysWarn", [] {
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
        expect(ne(all.find(key), std::string::npos)) << key << "\n=> " << all;
    expect(eq(m.platform.link.exe.value, "{name}.oke"));
}};

UTest overrides_and_host_are_read{"PlatformManifest/OverridesAndHostAreRead", [] {
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
    for (auto const& w : m.schemaWarnings) expect(false) << "unexpected warning: " << w;
    expect(bool(m.hostPlatform.platformSet));
    expect(eq(m.hostPlatform.platform.spec.shortName, "openkal-platform"));
    expect(fatal(eq(m.platformOverrides.size(), 2u)));
    auto const& o = m.platformOverrides[0];
    expect(eq(o.fields.runner.cmd.front(), "my-okrun"));
    expect(bool(o.fields.runner.exclusive));
    expect(o.fields.layers.at("c-abi").ref.spec.path.ends_with("openkal-musl"));
    expect(o.fields.tools.at("okld").ref.spec.path.ends_with("openkal-image/tools"));
    expect(eq(o.fields.link.flags.value, (std::vector<std::string>{"--build-id"})));
    expect(eq(o.fields.runner.origin.tier, mp::Tier::Manifest));
    expect(bool(m.platformOverrides[1].platformNone));
    // The runner's command is the row's template as well.
    expect(eq(m.targetOverrides.at("cfg(platform = \"openkal\")").runner.front(), "my-okrun"));
}};

UTest references_in_every_form{"PlatformManifest/ReferencesInEveryForm", [] {
    struct Row { std::string_view text; std::string_view ns, name, version; };
    const Row rows[] = {
        { "openkal-platform@0.17.1",          "mcpplibs", "openkal-platform", "0.17.1" },
        { "mcpplibs.openkal-platform@0.17.1", "mcpplibs", "openkal-platform", "0.17.1" },
        { "acme.board@1.0.0",                 "acme",     "board",            "1.0.0" },
    };
    for (auto const& r : rows) {
        auto ref = mp::parse_ref_text(r.text);
        expect(fatal(bool(ref))) << r.text;
        expect(eq(ref->spec.namespace_, r.ns));
        expect(eq(ref->spec.shortName, r.name));
        expect(eq(ref->spec.version, r.version));
    }
    expect(!mp::parse_ref_text("openkal-platform"));
    expect(!mp::parse_ref_text("@1.0"));
    expect(eq(mp::platform_name("openkal-platform"), "openkal"));
    expect(eq(mp::platform_name("stm32-bsp"), "stm32-bsp"));
}};

// ── Selection: the first tier that answers decides ─────────────────────────

UTest the_tiers_in_order{"PlatformSelect/TheTiersInOrder", [] {
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
        if (auto&& failed = expect(bool(s)); !failed) failed << (s ? "" : s.error());
        return s ? (s->none ? std::string("none") : s->ref.spec.shortName) + "|"
                       + std::string(mp::tier_name(s->origin.tier))
                 : std::string("error");
    };
    expect(eq(pick("flag-platform@1.0.0", "env-platform@1.0.0", true, true, "row-platform@1.0.0"), "flag-platform|flag"));
    expect(eq(pick("", "env-platform@1.0.0", true, true, "row-platform@1.0.0"), "env-platform|env"));
    expect(eq(pick("", "", true, true, "row-platform@1.0.0"), "project-platform|mcpp.toml"));
    expect(eq(pick("", "", false, true, "row-platform@1.0.0"), "machine-platform|mcpp.toml"));
    expect(eq(pick("", "", false, false, "row-platform@1.0.0"), "row-platform|built-in"));
    expect(eq(pick("", "", false, false, ""), "none|built-in"));
    expect(eq(pick("none", "", true, true, "row-platform@1.0.0"), "none|flag"));
}};

UTest the_most_specific_selector_decides{"PlatformSelect/TheMostSpecificSelectorDecides", [] {
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
    expect(fatal(bool(s)));
    expect(eq(s->ref.spec.shortName, "triple"));
    auto a = pf::select_target(in, matcher("aarch64-openkal"));
    expect(fatal(bool(a)));
    expect(bool(a->none));
}};

UTest the_host_graph{"PlatformSelect/TheHostGraph", [] {
    mp::Override project, config;
    config.platformSet = true;
    config.platform = *mp::parse_ref_text("machine@1.0.0");
    auto s = pf::select_host("", false, project, config);
    expect(fatal(bool(s)));
    expect(eq(s->ref.spec.shortName, "machine"));
    project.platformSet = true;
    project.platformNone = true;
    s = pf::select_host("", false, project, config);
    expect(fatal(bool(s)));
    expect(bool(s->none));
    s = pf::select_host("flag@2.0.0", true, project, config);
    expect(fatal(bool(s)));
    expect(eq(s->ref.spec.shortName, "flag"));
    expect(eq(s->origin.tier, mp::Tier::Flag));
}};

UTest a_directory_is_a_path{"PlatformSelect/ADirectoryIsAPath", [] {
    auto s = pf::parse_choice("path:/opt/board", {});
    expect(fatal(bool(s)));
    // Absolute on this machine: `D:/opt/board` on a Windows one.
    expect(s->ref.spec.path.ends_with("/opt/board")) << s->ref.spec.path;
    s = pf::parse_choice("../board", {});
    expect(fatal(bool(s)));
    expect(!(s->ref.spec.path.empty()));
    expect(!pf::parse_choice("not-a-reference", {}));
}};

// ── Merge: per field, flags appended, sequences replaced ───────────────────

UTest the_rules_of_section45{"PlatformMerge/TheRulesOfSection45", [] {
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
    expect(eq(base.runner.cmd.front(), "my-okrun"));
    expect(eq(base.runner.host, "same-arch"));
    expect(eq(base.build.cxxflags, (std::vector<std::string>{"-fPIC", "-DUSER"})));
    expect(eq(base.link.flags.value, (std::vector<std::string>{"-shared", "--build-id"})));
    expect(fatal(eq(base.link.seal.size(), 1u)));
    expect(eq(base.link.seal.front().front(), "sign"));
    expect(eq(base.link.inputs.size(), 2u));      // not stated: kept
    expect(bool(base.layers.at("c-abi").ref.off));
    expect(!(base.layers.at("c++-abi").ref.off));
    expect(eq(base.link.start.value, "ok_main"));
}};

UTest a_host_triple_takes_the_hosted_part_only{"PlatformMerge/AHostTripleTakesTheHostedPartOnly", [] {
    auto p = parse(kPlatform).platform;
    auto h = pf::hosted_view(p);
    expect(eq(h.toolchain.value, "llvm@23.1.3"));
    expect(h.layers.contains("c-abi"));
    expect(!h.layers.contains("c++-abi"));
    expect(!h.runner.set);
    expect(!h.link.driver.set);
    expect(h.build.empty());
    auto served = pf::serves(p, "board-platform", *mcpp::toolchain::triple::parse("x86_64-linux-gnu"));
    expect(fatal(bool(served)));
    expect(bool(*served));
    auto target = pf::serves(p, "board-platform", *mcpp::toolchain::triple::parse("x86_64-openkal"));
    expect(fatal(bool(target)));
    expect(!(*target));
    p.hosted = {};
    auto refused = pf::serves(p, "board-platform", *mcpp::toolchain::triple::parse("x86_64-linux-gnu"));
    expect(fatal(!refused));
    expect(ne(refused.error().find("[platform.hosted]"), std::string::npos));
}};

UTest cfg_tables_follow_the_target{"PlatformMerge/CfgTablesFollowTheTarget", [] {
    auto p = parse(kPlatform).platform;
    auto x = pf::evaluate_flags(p, matcher("x86_64-openkal"));
    auto a = pf::evaluate_flags(p, matcher("aarch64-openkal"));
    expect(eq(x.cxxflags, (std::vector<std::string>{"-fPIC"})));
    expect(eq(a.cxxflags, (std::vector<std::string>{"-fPIC", "-ffixed-x18"})));
}};

UTest cfg_platform_matches_the_name_or_the_package{"PlatformMerge/CfgPlatformMatchesTheNameOrThePackage", [] {
    expect(cfgpred::matches("cfg(platform = \"openkal\")",
                                 cfgpred::context_for("x86_64-openkal", "openkal-platform")));
    expect(cfgpred::matches("cfg(platform = \"openkal-platform\")",
                                 cfgpred::context_for("x86_64-openkal", "openkal-platform")));
    expect(!(cfgpred::matches("cfg(platform = \"openkal\")",
                                  cfgpred::context_for("x86_64-openkal"))));
    expect(gt(mcpp::manifest::cfg::specificity("cfg(os = \"openkal\")"), mcpp::manifest::cfg::specificity("cfg(platform = \"openkal\")")));
}};

// ── The link's templates ────────────────────────────────────────────────────

UTest name_patterns{"PlatformLink/NamePatterns", [] {
    auto e = pf::split_name_pattern("{name}.oke");
    expect(fatal(bool(e)));
    expect(eq(e->first, ""));
    expect(eq(e->second, ".oke"));
    auto s = pf::split_name_pattern("lib{name}.okso");
    expect(fatal(bool(s)));
    expect(eq(s->first, "lib"));
    expect(eq(s->second, ".okso"));
    expect(!pf::split_name_pattern("name.oke"));
    expect(!pf::split_name_pattern("{name}{name}"));
    expect(!pf::split_name_pattern("bin/{name}"));
}};

UTest seal_steps{"PlatformLink/SealSteps", [] {
    const std::vector<std::string> check{"okld", "check", "{}", "--with", "{needed}"};
    const std::vector<std::string> two{"a.okso", "b.okso"};
    expect(eq(pf::expand_step(check, "x.oke.tmp", two), (std::vector<std::string>{"okld", "check", "x.oke.tmp", "--with", "a.okso",
                                        "--with", "b.okso"})));
    expect(eq(pf::expand_step(check, "x.oke.tmp", {}), (std::vector<std::string>{"okld", "check", "x.oke.tmp"})));
    expect(eq(pf::expand_step({"sign", "--image={}", "{needed}"}, "x", two), (std::vector<std::string>{"sign", "--image=x", "a.okso", "b.okso"})));
}};

// ── The run (§10) ───────────────────────────────────────────────────────────

// A `same-arch` runner runs an artifact on a machine of its architecture:
// this one when they agree; the execution host, with that architecture's
// program, when one is named; refused otherwise. `--no-runner` and every
// other runner are this machine's.
UTest the_runner_reaches_the_artifacts_architecture{"PlatformRun/TheRunnerReachesTheArtifactsArchitecture", [] {
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
        expect(eq(pf::runner_reach(*r.runner, r.target, "x86_64", r.executionHost, r.noRunner), r.want))
            << r.runner->host << " " << r.target << " executionHost=" << r.executionHost
            << " noRunner=" << r.noRunner;
}};

// ── The report ──────────────────────────────────────────────────────────────

UTest sources_and_json{"PlatformReport/SourcesAndJson", [] {
    pf::Resolved r;
    r.active = true;
    r.package = "mcpplibs.openkal-platform";
    r.version = "0.17.1";
    r.fields = parse(kPlatform).platform;
    r.selection.origin = { mp::Tier::BuiltIn, {}, 0, "the target table" };
    r.layers["c-abi"] = { "openkal-musl@0.22.3", mp::Tier::Platform };
    r.layers["c++-abi"] = { "acme-runtime@1.0.0", mp::Tier::Graph };
    expect(eq(r.display(), "openkal-platform 0.17.1"));
    expect(eq(r.identity(), "mcpplibs.openkal-platform@0.17.1"));
    auto rows = pf::report_rows(r);
    expect(fatal(!rows.empty()));
    bool graph = false;
    for (auto const& [k, v] : rows)
        if (k == "c++-abi") graph = v.find("(graph)") != std::string::npos;
    expect(bool(graph));
    auto json = pf::to_json(r);
    expect(ne(json.find("\"name\":\"mcpplibs.openkal-platform\""), std::string::npos));
    expect(ne(json.find("\"source\":\"built-in\""), std::string::npos));
    expect(ne(json.find("\"layers.c++-abi\""), std::string::npos));
    expect(eq(pf::to_json(pf::Resolved{}), "null"));
}};

// ── Runner hosts, carrier entries, descriptor keys (design 2026-10-29) ──────

UTest runner_hosts_carrier_entries_and_descriptor_keys_are_read{"PlatformManifest/RunnerHostsCarrierEntriesAndDescriptorKeysAreRead", [] {
    auto m = parse(R"(
[package]
name = "p"
version = "1.0.0"

[platform]
targets = ["x86_64-openkal"]
runner  = { cmd = ["okrun", "{}"], host = "same-arch", hosts = ["linux-x86_64", "macos-aarch64"] }
carrier = [
  { package = "openkal-run@0.1.3", section = ".oke", main = "carrier/main.cpp", hosts = ["linux-x86_64"] },
  { package = "openkal-run@0.1.3", section = ".oke", main = "carrier/main.cpp" },
  { package = "openkal-cosmo@0.1.0", section = ".oke", main = "carrier/ape.cpp", build = "ape" },
]
descriptor_keys = ["openkal", "linux"]
)");
    auto const& p = m.platform;
    expect(bool(p.runner.hostsSet));
    expect(eq(p.runner.hosts, (std::vector<std::string>{"linux-x86_64", "macos-aarch64"})));
    // The entry of a model this mcpp does not build is skipped, not refused.
    expect(fatal(eq(p.carriers.size(), 2u)));
    expect(eq(p.carriers[0].hosts, (std::vector<std::string>{"linux-x86_64"})));
    expect(eq(p.carriers[0].build, "row"));
    expect(eq(p.descriptorKeys.value, (std::vector<std::string>{"openkal", "linux"})));
    // The entry naming the pair serves it; another pair takes the open entry.
    expect(eq(mp::carrier_for(p, "linux-x86_64"), &p.carriers[0]));
    expect(eq(mp::carrier_for(p, "windows-x86_64"), &p.carriers[1]));
}};

UTest a_carrier_without_hosts_serves_every_pair_and_none_serves_nothing{"PlatformManifest/ACarrierWithoutHostsServesEveryPairAndNoneServesNothing", [] {
    auto m = parse(kPlatform);
    expect(eq(mp::carrier_for(m.platform, "macos-aarch64"), &m.platform.carriers.at(0)));
    mp::Statement none;
    expect(eq(mp::carrier_for(none, "linux-x86_64"), nullptr));
}};

// A host pair and a descriptor key are words: a platform's rows may name a
// system and read descriptor blocks of their own (SPEC-016 §7).
UTest host_pairs_and_descriptor_keys_are_words{"PlatformManifest/HostPairsAndDescriptorKeysAreWords", [] {
    const auto base = std::string(R"(
[package]
name = "p"
version = "1.0.0"
[platform]
targets = ["x86_64-openkal"]
)");
    expect(ne(parse_error(base + R"(runner = { cmd = ["okrun"], hosts = ["linux"] })" "\n"), ""));
    // A system a platform defines is a host's system as well (mcpp#911 G10).
    expect(eq(parse_error(base + R"(runner = { cmd = ["okrun"], hosts = ["freebsd-x86_64"] })" "\n"), ""));
    expect(ne(parse_error(base + R"(runner = { cmd = ["okrun"], hosts = ["Linux-x86_64"] })" "\n"), ""));
    expect(ne(parse_error(base + R"(descriptor_keys = ["mac os"])" "\n").find("macosx"), std::string::npos));
    expect(eq(parse_error(base + R"(descriptor_keys = ["freebsd", "linux"])" "\n"), ""));
    expect(ne(parse_error(base + R"(descriptor_keys = [])" "\n"), ""));
    expect(eq(parse_error(base + R"(descriptor_keys = ["openkal", "linux"])" "\n"), ""));
}};

UTest an_override_replaces_hosts_carriers_and_descriptor_keys{"PlatformMerge/AnOverrideReplacesHostsCarriersAndDescriptorKeys", [] {
    auto m = parse(kPlatform);
    mp::Statement over;
    over.runner.hosts = {"windows-x86_64"};
    over.runner.hostsSet = true;
    over.descriptorKeys = mp::Words{{"openkal"}, {}, true};
    mp::Carrier c;
    c.section = ".x";
    c.set = true;
    over.carriers = {c};
    auto merged = m.platform;
    pf::merge(merged, over);
    expect(eq(merged.runner.hosts, (std::vector<std::string>{"windows-x86_64"})));
    expect(eq(merged.runner.cmd, m.platform.runner.cmd));
    expect(fatal(eq(merged.carriers.size(), 1u)));
    expect(eq(merged.carriers[0].section, ".x"));
    expect(eq(merged.descriptorKeys.value, (std::vector<std::string>{"openkal"})));
}};

// D20 (design 2026-11-07 §4.10): the target table's default is one release; a
// failed build names a newer one in the index with the line to write, and says
// nothing when the project, the configuration or the command line chose.
UTest a_newer_release_is_named_only_for_the_default{"PlatformDefault/ANewerReleaseIsNamedOnlyForTheDefault", [] {
    namespace mp = mcpp::manifest::platform;
    mp::Origin builtIn{}; builtIn.tier = mp::Tier::BuiltIn;
    auto tip = mcpp::platform::newer_release_tip(builtIn, "openkal-platform", "0.24.3", "0.24.4", "2026.11.7.2");
    expect(fatal(tip.has_value()));
    if (auto&& failed = expect(ne(tip->find("openkal-platform 0.24.3, mcpp 2026.11.7.2's default"), std::string::npos)); !failed) failed << *tip;
    if (auto&& failed = expect(ne(tip->find("platform = \"openkal-platform@0.24.4\""), std::string::npos)); !failed) failed << *tip;
    for (auto tier : {mp::Tier::Manifest, mp::Tier::Config, mp::Tier::Env, mp::Tier::Flag}) {
        mp::Origin chosen{}; chosen.tier = tier;
        expect(!mcpp::platform::newer_release_tip(chosen, "openkal-platform", "0.24.3", "0.24.4", "x"));
    }
    expect(!mcpp::platform::newer_release_tip(builtIn, "openkal-platform", "0.24.3", "0.24.3", "x"));
    expect(!mcpp::platform::newer_release_tip(builtIn, "openkal-platform", "0.24.3", "", "x"));
}};

// THE STRING FORM OF A REPOSITORY (SPEC-016 §8): `git+<url>` with at most one of
// tag, rev, branch after `#`; none is the remote's default branch.
UTest a_repository_reference_has_a_string_form{"PlatformManifest/ARepositoryReferenceHasAStringForm", [] {
    auto tagged = mp::parse_ref_text("git+https://example.org/acme/toy-platform.git#tag=v1");
    expect(fatal(tagged.has_value()));
    expect(eq(tagged->spec.git, "https://example.org/acme/toy-platform.git"));
    expect(eq(tagged->spec.gitRefKind, "tag"));
    expect(eq(tagged->spec.gitRev, "v1"));
    auto deflt = mp::parse_ref_text("git+git@example.org:acme/toy.git");
    expect(fatal(deflt.has_value()));
    expect(eq(deflt->spec.gitRefKind, "branch"));
    expect(deflt->spec.gitRev.empty());
    expect(!mp::parse_ref_text("git+https://example.org/x.git#sha=1").has_value());
    expect(!mp::parse_ref_text("git+").has_value());
}};

// A PLATFORM'S STATEMENT IS CHECKED AGAINST THE ROWS AS IT IS READ (SPEC-016 §5):
// a definition of a built-in row and a row defined twice are errors; a provider
// that serves no row and a tie of priorities are warnings; the served
// identities are ordered by identity, then priority, then provider.
UTest a_platform_statement_is_checked_against_the_rows{"PlatformManifest/AStatementIsCheckedAgainstTheRows", [] {
    constexpr std::string_view kHead = R"(
[package]
name = "toy-platform"
version = "0.1.0"

[platform]
toolchain = "llvm@23.1.3"
)";
    auto redefined = parse_error(std::string(kHead) + R"(
[[platform.define]]
triple = "x86_64-linux-gnu"

[[platform.provider]]
serves = ["x86_64-linux-gnu"]
)");
    if (auto&& failed = expect(ne(redefined.find("'x86_64-linux-gnu' is a built-in row"), std::string::npos)); !failed)
        failed << redefined;
    auto twice = parse_error(std::string(kHead) + R"(
[[platform.define]]
triple = "x86_64-toy"

[[platform.define]]
triple = "x86_64-toy"

[[platform.provider]]
serves = ["x86_64-toy"]
)");
    if (auto&& failed = expect(ne(twice.find("'x86_64-toy' is defined twice"), std::string::npos)); !failed)
        failed << twice;

    auto m = parse(std::string(kHead) + R"(
[[platform.define]]
triple = "x86_64-toy"

[[platform.provider]]
serves = ["x86_64-toy"]

[[platform.provider]]
name   = "second"
serves = ["*-toy"]

[[platform.provider]]
name   = "nothing"
serves = ["riscv64-nosuch"]
)");
    auto has = [&](std::string_view s) {
        return std::ranges::any_of(m.schemaWarnings, [&](auto const& w) { return w.find(s) != std::string::npos; });
    };
    expect(has("'nothing' serves no row"));
    expect(has("'x86_64-toy' is served by 'second' and 'toy' at priority 100"));
    auto served = mp::served_identities(m.platform, "toy");
    expect(fatal(eq(served.size(), std::size_t{2})));
    expect(eq(served[0].identity, "x86_64-toy"));
    expect(eq(served[0].provider, "second"));
    expect(eq(served[1].provider, "toy"));
}};

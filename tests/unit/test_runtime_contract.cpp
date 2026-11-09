
import std;
import mcpplibs.mtest;
import mcpp.build.plan;
import mcpp.manifest.toml;
import mcpp.manifest.types;
import mcpp.manifest.xpkg;
import mcpp.modgraph.scanner;
import mcpplibs.os.platform;
import mcpp.toolchain.platform;
import mcpp.runtime.search;
import mcpp.runtime.binding;
import mcpp.provider.xlings.subos_info;

using namespace mcpplibs::mtest;

namespace build = mcpp::build;
namespace mf = mcpp::manifest;

namespace {

std::string source_without_comments(std::string_view source) {
    enum class State { Code, Line, Block, String, Character } state = State::Code;
    std::string out;
    out.reserve(source.size());
    for (std::size_t i = 0; i < source.size(); ++i) {
        const char c = source[i];
        const char n = i + 1 < source.size() ? source[i + 1] : '\0';
        if (state == State::Code) {
            if (c == '/' && n == '/') { state = State::Line; ++i; out += "  "; }
            else if (c == '/' && n == '*') { state = State::Block; ++i; out += "  "; }
            else {
                out.push_back(c);
                if (c == '"') state = State::String;
                else if (c == '\'') state = State::Character;
            }
        } else if (state == State::Line) {
            if (c == '\n') { state = State::Code; out.push_back(c); }
            else out.push_back(' ');
        } else if (state == State::Block) {
            if (c == '*' && n == '/') { state = State::Code; ++i; out += "  "; }
            else out.push_back(c == '\n' ? '\n' : ' ');
        } else {
            out.push_back(c);
            if (c == '\\' && i + 1 < source.size()) out.push_back(source[++i]);
            else if ((state == State::String && c == '"')
                  || (state == State::Character && c == '\'')) state = State::Code;
        }
    }
    return out;
}

mcpp::modgraph::PackageRoot package(
    const std::filesystem::path& root,
    std::string ns,
    std::string name,
    std::string version,
    std::string source) {
    mcpp::modgraph::PackageRoot out;
    out.root = root;
    out.manifest.package.namespace_ = std::move(ns);
    out.manifest.package.name = std::move(name);
    out.manifest.package.version = std::move(version);
    out.manifest.package.sourceProvenance = std::move(source);
    return out;
}

UTest toml_reads_structured_values_and_keeps_legacy_fields{"RuntimeContract/TomlReadsStructuredValuesAndKeepsLegacyFields", [] {
    auto parsed = mf::parse_string(R"(
[package]
name = "consumer"
version = "1.2.3"

[runtime]
library_dirs = ["legacy/run"]
dlopen_libs = ["liblegacy.so.1"]
capabilities = ["legacy.display"]
provides = ["legacy.provider"]
libraries = ["widget"]
link_library_dirs = ["link"]
transitive_needed_dirs = ["needed"]
runtime_search_dirs = ["run"]
frameworks = ["WindowKit"]
deploy_files = ["bin/widget.dll"]
requirements = [
  { kind = "capability", value = "display.present", phase = "run", required = true },
  { kind = "soname", value = "libwidget.so.1", phase = "link", required = false },
]
artifacts = [
  { role = "library", path = "run/libwidget.so.1", provenance = "payload", abi = "elf-x86_64", digest = "sha256:abc", host_fingerprint = "host-1" },
]
)");
    if (auto&& failed = expect(fatal(bool(parsed))); !failed) failed << parsed.error().format();
    expect(fatal(eq(parsed->runtimeConfig.requirements.size(), 2u)));
    expect(eq(parsed->runtimeConfig.requirements[0].kind, "capability"));
    expect(eq(parsed->runtimeConfig.requirements[0].value, "display.present"));
    expect(bool(parsed->runtimeConfig.requirements[0].required));
    expect(!(parsed->runtimeConfig.requirements[1].required));
    expect(fatal(eq(parsed->runtimeConfig.artifacts.size(), 1u)));
    expect(eq(parsed->runtimeConfig.artifacts[0].role, "library"));
    expect(eq(parsed->runtimeConfig.artifacts[0].hostFingerprint, "host-1"));
    expect(eq(parsed->runtimeConfig.linkIntent.libraries, std::vector<std::string>{"widget"}));
    expect(eq(parsed->runtimeConfig.linkIntent.runtimeSearchDirs, std::vector<std::filesystem::path>{"run"}));
    expect(eq(parsed->runtimeConfig.libraryDirs[0], "legacy/run"));
    expect(eq(parsed->runtimeConfig.capabilities[0], "legacy.display"));
    expect(eq(parsed->runtimeConfig.provides[0], "legacy.provider"));
}};

UTest xpkg_reads_the_same_structured_contract{"RuntimeContract/XpkgReadsTheSameStructuredContract", [] {
    constexpr auto lua = R"(
package = {
  spec = "1",
  namespace = "acme",
  name = "backend",
  xpm = { linux = { ["2.0.0"] = { url = "u", sha256 = "h" } } },
  mcpp = {
    sources = { "src/backend.cpp" },
    runtime = {
      requirements = {
        { kind = "capability", value = "display.present", phase = "run", required = true },
      },
      artifacts = {
        { role = "library", path = "lib/libbackend.so", provenance = "subos_view", abi = "elf-x86_64" },
      },
      libraries = { "backend" },
      link_library_dirs = { "lib" },
      transitive_needed_dirs = { "closure" },
      runtime_search_dirs = { "runtime" },
      frameworks = { "WindowKit" },
      deploy_files = { "bin/backend.dll" },
    },
  },
}
)";
    auto parsed = mf::synthesize_from_xpkg_lua(
        lua, "backend", "2.0.0", mcpp::toolchain::platform::HostPlatform::current());
    if (auto&& failed = expect(fatal(bool(parsed))); !failed) failed << parsed.error().format();
    expect(fatal(eq(parsed->runtimeConfig.requirements.size(), 1u)));
    expect(eq(parsed->runtimeConfig.requirements[0].value, "display.present"));
    expect(fatal(eq(parsed->runtimeConfig.artifacts.size(), 1u)));
    expect(eq(parsed->runtimeConfig.artifacts[0].path, "lib/libbackend.so"));
    expect(eq(parsed->runtimeConfig.linkIntent.transitiveNeededDirs, std::vector<std::filesystem::path>{"closure"}));
}};

UTest requirements_never_self_provide_and_canonical_ids_do_not_collide{"RuntimeContract/RequirementsNeverSelfProvideAndCanonicalIdsDoNotCollide", [] {
    auto base = std::filesystem::temp_directory_path() / "mcpp-runtime-contract";
    auto consumer = package(base / "consumer", "mcpplibs", "consumer", "1.0.0",
                            "path+consumer");
    consumer.manifest.runtimeConfig.requirements.push_back({
        .kind = "capability", .value = "display.present", .phase = "run",
        .required = true,
    });
    // Legacy `capabilities` remains a requirement, never an implicit provider.
    consumer.manifest.runtimeConfig.capabilities.push_back("legacy.display");

    auto alpha = package(base / "alpha", "alpha", "backend", "2.0.0",
                         "index+alpha@rev-a");
    alpha.manifest.runtimeConfig.provides.push_back("display.present");
    alpha.manifest.runtimeConfig.artifacts.push_back({
        .role = "library", .path = "lib/libbackend.so",
        .provenance = "payload", .abi = "elf-x86_64",
    });
    alpha.manifest.runtimeConfig.linkIntent.libraries = {
        "lib/libbackend.a", "backend"};

    auto beta = package(base / "beta", "beta", "backend", "3.0.0",
                        "index+beta@rev-b");
    beta.manifest.runtimeConfig.provides.push_back("display.present");
    beta.manifest.runtimeConfig.artifacts.push_back({
        .role = "library", .path = "lib/libbackend.so",
        .provenance = "host_link", .abi = "elf-x86_64",
    });

    auto contract = build::resolve_runtime_contract({consumer, alpha, beta});
    expect(fatal(eq(contract.requirements.size(), 2u)));
    expect(eq(contract.requirements[0].requester.canonical(), "mcpplibs.consumer@1.0.0"));
    expect(eq(contract.requirements[0].requester.sourceProvenance, "path+consumer"));

    expect(fatal(eq(contract.providers.size(), 2u)));
    expect(eq(contract.providers[0].provider.canonical(), "alpha.backend@2.0.0"));
    expect(eq(contract.providers[1].provider.canonical(), "beta.backend@3.0.0"));
    expect(ne(contract.providers[0].provider, contract.providers[1].provider));
    expect(std::ranges::none_of(contract.providers, [](auto const& provider) {
        return provider.provider.canonical() == "mcpplibs.consumer@1.0.0";
    }));

    expect(fatal(eq(contract.artifacts.size(), 2u)));
    expect(eq(contract.artifacts[0].provider.canonical(), "alpha.backend@2.0.0"));
    expect(eq(contract.artifacts[1].provider.canonical(), "beta.backend@3.0.0"));
    expect(eq(contract.artifacts[0].path, (base / "alpha/lib/libbackend.so").lexically_normal()));
    expect(fatal(eq(contract.linkIntent.libraries.size(), 2u)));
    expect(eq(contract.linkIntent.libraries[0], (base / "alpha/lib/libbackend.a").lexically_normal().string()));
    expect(eq(contract.linkIntent.libraries[1], "backend"));
}};

UTest source_owns_no_provider_specific_selection_or_probe_branch{"RuntimeContract/SourceOwnsNoProviderSpecificSelectionOrProbeBranch", [] {
    auto repo = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    auto src = repo / "src";
    expect(fatal(std::filesystem::is_directory(src))) << src;
    // Intentionally assembled so the ownership gate does not flag its own
    // vocabulary if this test is ever moved under src/.
    const std::vector<std::string> providerWords = {
        "open" "gl", "vul" "kan", "me" "sa", "nvi" "dia",
        "w" "sl", "i" "cd",
    };
    const std::vector<std::string> launchWords = {
        "capture_" "exec", "run_" "exec", "spawn", "system(",
    };
    for (auto it = std::filesystem::recursive_directory_iterator(src);
         it != std::filesystem::recursive_directory_iterator{}; ++it) {
        if (!it->is_regular_file()) continue;
        auto ext = it->path().extension().string();
        if (ext != ".cpp" && ext != ".cppm") continue;
        std::ifstream input(it->path());
        std::string raw((std::istreambuf_iterator<char>(input)), {});
        auto code = source_without_comments(raw);
        std::ranges::transform(code, code.begin(),
            [](unsigned char c) { return std::tolower(c); });

        std::size_t line = 0;
        for (auto text : code | std::views::split('\n')) {
            ++line;
            std::string lineText(text.begin(), text.end());
            const bool branch = lineText.find("if (") != std::string::npos
                || lineText.find("if(") != std::string::npos
                || lineText.find("switch (") != std::string::npos
                || lineText.find("switch(") != std::string::npos
                || lineText.find("case ") != std::string::npos;
            if (!branch) continue;
            for (auto const& word : providerWords) {
                if (auto&& failed = expect(eq(lineText.find(word), std::string::npos)); !failed) failed << it->path() << ':' << line
                    << " contains provider-specific selection logic";
            }
        }
        for (auto const& provider : providerWords) {
            for (auto pos = code.find(provider); pos != std::string::npos;
                 pos = code.find(provider, pos + 1)) {
                const auto begin = pos > 240 ? pos - 240 : 0;
                const auto window = code.substr(begin, 480);
                for (auto const& launch : launchWords) {
                    if (auto&& failed = expect(eq(window.find(launch), std::string::npos)); !failed) failed << it->path()
                        << " couples a provider-specific word to a launched probe";
                }
            }
        }
    }
}};

UTest xlings_selected_facts_precede_descriptor_fallbacks{"RuntimeContract/XlingsSelectedFactsPrecedeDescriptorFallbacks", [] {
    build::BuildPlan plan;
    plan.runtimeProviders.push_back({
        .capability = "render.demo",
        .provider = {.namespace_ = "mcpplibs", .name = "fallback",
                     .version = "1.0.0", .sourceProvenance = "index+mcpplibs"},
    });
    mcpp::runtime::RuntimeBinding binding;
    binding.runtimeProviders.push_back({
        .capability = "render.demo",
        .provider = {.namespace_ = "xim", .name = "selected",
                     .version = "4.0.0", .source = "xim-pkgindex@rev"},
    });
    binding.runtimeArtifacts.push_back({
        .role = "driver",
        .provider = {.namespace_ = "xim", .name = "selected",
                     .version = "4.0.0", .source = "xim-pkgindex@rev"},
        .path = "/runtime/selected.artifact",
        .provenance = "subos_view",
        .abi = "fixture-v1",
        .digest = "sha256:def",
        .hostFingerprint = "host-2",
    });

    build::merge_runtime_binding_contract(plan, binding);
    expect(fatal(eq(plan.runtimeSearch.size(), 0u)))
        << "a binding with no SubOS library view contributes no farm entry";
    expect(fatal(eq(plan.runtimeProviders.size(), 2u)));
    expect(eq(plan.runtimeProviders.front().provider.canonical(), "xim.selected@4.0.0"));
    expect(fatal(eq(plan.runtimeArtifacts.size(), 1u)));
    expect(eq(plan.runtimeArtifacts[0].provider.canonical(), "xim.selected@4.0.0"));
    expect(eq(plan.runtimeArtifacts[0].hostFingerprint, "host-2"));
}};

}  // namespace

// ── the farm's two guards, as a matrix ─────────────────────────────────────
//
// The farm belongs to THIS host's SubOS, so it may only reach an artifact that
// will run here under this runtime. The guards are asserted directly because
// each one costs a full cross toolchain to exercise end to end, and the failure
// they prevent is silent: a path that is inert at best, and points at another
// architecture's or another C library's objects at worst.
namespace {

build::BuildPlan plan_for(std::string targetTriple) {
    build::BuildPlan plan;
    plan.toolchain.targetTriple = std::move(targetTriple);
    return plan;
}

mcpp::runtime::RuntimeBinding host_binding_with_farm() {
    mcpp::runtime::RuntimeBinding b;
    b.platform = "linux";
    b.arch = "x86_64";
    b.declared = true;
    b.runtimeId = "glibc@2.39";
    b.searchDirs = {"/home/u/.mcpp/registry/subos/default/lib"};
    return b;
}

std::size_t farm_entries(const std::vector<mcpp::runtime::search::Dir>& dirs) {
    return static_cast<std::size_t>(std::ranges::count_if(dirs, [](auto const& d) {
        return d.origin == mcpp::runtime::search::Origin::SubosFarm;
    }));
}

}  // namespace

UTest host_target_gets_the_farm{"RuntimeSearchClosure/HostTargetGetsTheFarm", [] {
    // An EXPLICIT Linux target is a farm target from anywhere, including a
    // macOS or Windows runner: the guards read the target, not the runner.
    expect(eq(farm_entries(build::runtime_search_closure(
        plan_for("x86_64-linux-gnu"), host_binding_with_farm())), 1u));

    // An EMPTY triple means "this host", so the answer is the host's format —
    // and that is the point of the ELF guard, not an exception to it. Asserting
    // 1 unconditionally is what failed on the macOS and Windows runners.
    const std::size_t expectedForHost = mcpplibs::os::is_linux ? 1u : 0u;
    expect(eq(farm_entries(build::runtime_search_closure(
        plan_for(""), host_binding_with_farm())), expectedForHost))
        << "an empty triple must resolve to this host's format";
}};

UTest cross_target_gets_no_farm{"RuntimeSearchClosure/CrossTargetGetsNoFarm", [] {
    // Another architecture: the farm holds x86_64 objects.
    expect(eq(farm_entries(build::runtime_search_closure(
        plan_for("aarch64-linux-gnu"), host_binding_with_farm())), 0u));
    // Another C library: a glibc farm on a musl program's search path is the
    // payload mixing rule B exists to prevent.
    expect(eq(farm_entries(build::runtime_search_closure(
        plan_for("x86_64-linux-musl"), host_binding_with_farm())), 0u));
    // Another format: DT_RPATH does not exist there at all.
    expect(eq(farm_entries(build::runtime_search_closure(
        plan_for("x86_64-windows-gnu"), host_binding_with_farm())), 0u));
    expect(eq(farm_entries(build::runtime_search_closure(
        plan_for("aarch64-macos"), host_binding_with_farm())), 0u));
}};

// A MISMATCH must be proven, not assumed. An undeclared SubOS has no runtime
// identity, and refusing its own library view on that basis would be exactly
// the absence-read-as-contradiction this release removes elsewhere. Caught by
// e2e 220, which builds against a SubOS it creates and which therefore has no
// `subos_info` to declare anything.
UTest undeclared_binding_still_gets_its_own_farm{"RuntimeSearchClosure/UndeclaredBindingStillGetsItsOwnFarm", [] {
    auto b = host_binding_with_farm();
    b.declared = false;
    b.runtimeId.clear();
    expect(eq(farm_entries(build::runtime_search_closure(
        plan_for("x86_64-linux-gnu"), b)), 1u));
    // …but the architecture guard still applies: unknown libc is not a licence
    // to ignore everything else.
    expect(eq(farm_entries(build::runtime_search_closure(
        plan_for("aarch64-linux-gnu"), b)), 0u));
}};

// ─── the Windows runtime identity ────────────────────────────────────────
//
// `runtimeId`'s own comment has documented `ucrt@…` since the field was
// introduced, and nothing in the repository ever wrote one. The compiler had
// a version axis and the C runtime it compiles against did not, so two
// Windows SDKs produced one cache key — the version axis simply stopped
// existing one layer down.

UTest provider_is_dispatched_on_not_pattern_matched{"RuntimeIdentity/ProviderIsDispatchedOnNotPatternMatched", [] {
    namespace rt = mcpp::runtime;
    expect(eq(rt::runtime_provider("glibc@2.39"), "glibc"));
    expect(eq(rt::runtime_provider("ucrt@10.0.26100.0"), "ucrt"));
    expect(eq(rt::runtime_provider("macos_sdk@14.0"), "macos_sdk"));
    // No identity at all is a DIFFERENT answer from "an identity belonging to
    // some other provider", and every consumer that spelled
    // `starts_with("glibc@")` collapsed the two.
    expect(rt::runtime_provider("").empty());
    expect(rt::runtime_provider("nonsense").empty());
}};

UTest the_sdk_version_reaches_the_contract_hash{"RuntimeIdentity/TheSdkVersionReachesTheContractHash", [] {
    namespace rt = mcpp::runtime;
    rt::RuntimeBinding a;
    a.platform = "windows";
    a.contractHash = "unset";

    rt::RuntimeBinding b = a;
    rt::bind_windows_ucrt(a, "10.0.22621.0");
    rt::bind_windows_ucrt(b, "10.0.26100.0");

    expect(eq(a.runtimeId, "ucrt@10.0.22621.0"));
    expect(eq(b.runtimeId, "ucrt@10.0.26100.0"));
    expect(ne(a.contractHash, b.contractHash))
        << "two SDKs produced one contract hash, so they produce one build "
           "cache — which is the defect this identity closes";
    expect(ne(a.contractHash, "unset")) << "the hash was not re-derived";
}};

UTest binding_ucrt_is_idempotent_and_skips_an_empty_version{"RuntimeIdentity/BindingUcrtIsIdempotentAndSkipsAnEmptyVersion", [] {
    namespace rt = mcpp::runtime;
    rt::RuntimeBinding b;
    b.platform = "windows";
    rt::bind_windows_ucrt(b, "10.0.26100.0");
    auto once = b.contractHash;
    rt::bind_windows_ucrt(b, "10.0.26100.0");
    expect(eq(b.contractHash, once));

    // A Windows box with no SDK still builds — toolchain SELECTION has to
    // work there — so there is simply nothing to declare.
    rt::RuntimeBinding empty;
    empty.contractHash = "keep";
    rt::bind_windows_ucrt(empty, "");
    expect(empty.runtimeId.empty());
    expect(eq(empty.contractHash, "keep"));
}};

UTest ucrt_is_a_floor_declaration_not_a_private_payload{"RuntimeIdentity/UcrtIsAFloorDeclarationNotAPrivatePayload", [] {
    // The asymmetry with glibc, pinned so it cannot be "tidied up" later.
    //
    // `glibc@2.39` binds a payload: the headers and the .so are both in it and
    // patchelf makes the artifact run on that exact copy, so it is ALSO
    // projected into `libc`, which the loader machinery reads. `ucrtbase.dll`
    // is an OS component from Win10 on — it cannot be swapped and must not be
    // shipped — so mcpp's windows-sdk payload deliberately carries only half
    // of ucrt (headers + import libraries). Projecting it into `libc` would
    // send the private-libc machinery looking for a payload that was never
    // supposed to exist.
    namespace rt = mcpp::runtime;
    rt::RuntimeBinding b;
    b.platform = "windows";
    rt::bind_windows_ucrt(b, "10.0.26100.0");
    expect(b.libc.empty())
        << "ucrt was projected into the private-libc field";
    expect(!b.loader.has_value());
    expect(b.libraryDirs.empty());
}};

// #615: `runtime.deploy` in a descriptor -- the same `{ from, to }` entries as
// mcpp.toml, and the same path rule.
UTest xpkg_reads_runtime_deploy_entries{"RuntimeContract/XpkgReadsRuntimeDeployEntries", [] {
    constexpr auto lua = R"(
package = {
  spec = "1",
  namespace = "acme",
  name = "icd",
  xpm = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
  mcpp = {
    sources = { "src/icd.cpp" },
    runtime = {
      deploy = {
        { from = "share/vulkan/icd.d/lvp_icd.json", to = "vulkan/icd.d" },
        { to = ".", from = "share/readme.txt" },
      },
      deploy_files = { "bin/backend.dll" },
    },
  },
}
)";
    auto parsed = mf::synthesize_from_xpkg_lua(
        lua, "icd", "1.0.0", mcpp::toolchain::platform::HostPlatform::current());
    if (auto&& failed = expect(fatal(bool(parsed))); !failed) failed << parsed.error().format();
    auto const& d = parsed->runtimeConfig.linkIntent.deploy;
    expect(fatal(eq(d.size(), 2u)));
    expect(eq(d[0].from, std::filesystem::path("share/vulkan/icd.d/lvp_icd.json")));
    expect(eq(d[0].to, "vulkan/icd.d"));
    expect(eq(d[1].from, std::filesystem::path("share/readme.txt")));
    expect(eq(d[1].to, "."));
    // The key after it is still read, so the entry loop consumed exactly its table.
    expect(eq(parsed->runtimeConfig.linkIntent.deployFiles, std::vector<std::filesystem::path>{"bin/backend.dll"}));
}};

UTest xpkg_runtime_deploy_refuses_an_escaping_destination{"RuntimeContract/XpkgRuntimeDeployRefusesAnEscapingDestination", [] {
    constexpr auto lua = R"(
package = {
  spec = "1", namespace = "acme", name = "icd",
  xpm = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
  mcpp = {
    sources = { "src/icd.cpp" },
    runtime = { deploy = { { from = "a.json", to = "../x" } } },
  },
}
)";
    auto parsed = mf::synthesize_from_xpkg_lua(
        lua, "icd", "1.0.0", mcpp::toolchain::platform::HostPlatform::current());
    expect(fatal(!parsed));
    if (auto&& failed = expect(ne(parsed.error().message.find("runtime.deploy[1]: `to` has a `.` or `..` component"), std::string::npos)); !failed) failed << parsed.error().message;
}};

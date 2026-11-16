
import std;
import mcpplibs.mtest;
import mcpp.config;
import mcpp.manifest;
import mcpplibs.os.platform;
import mcpp.runtime.binding;
import mcpp.provider.runtime_selection;

using namespace mcpplibs::mtest;

namespace runtime = mcpp::provider::runtime;

namespace {

mcpp::manifest::Manifest named(std::string name) {
    mcpp::manifest::Manifest m;
    m.xlings.subosDeclared = true;
    m.xlings.subos = std::move(name);
    return m;
}

struct RuntimeHome {
    std::filesystem::path dir;
    mcpp::config::GlobalConfig cfg;

    RuntimeHome() {
        dir = std::filesystem::temp_directory_path()
            / std::format("mcpp_runtime_selection_{}", std::random_device{}());
        cfg.registryDir = dir / "xlings-home";
        std::filesystem::create_directories(cfg.registryDir);
    }

    ~RuntimeHome() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    void write(const std::filesystem::path& subos,
               std::string_view runtimeName = "glibc@2.39",
               std::string_view envValue = "${subosdir}/drivers") const {
        std::filesystem::create_directories(subos);
        std::ofstream(subos / ".xlings.json") << std::format(R"({{
          "subos_info": {{
            "schema_version": 1,
            "runtime": "{}",
            "host_glibc": "2.43",
            "envs": {{
              "mesa@25": [{{"var":"DRIVERS","op":"prepend","value":"{}"}}]
            }},
            "runtime_contract": {{
              "providers": [{{
                "capability": "render.demo",
                "provider": {{"namespace":"xim","name":"renderer",
                  "version":"4.0.0","source":"xim-pkgindex@rev"}}
              }}],
              "artifacts": [{{
                "role":"driver",
                "provider": {{"namespace":"xim","name":"renderer",
                  "version":"4.0.0","source":"xim-pkgindex@rev"}},
                "path":"${{subosdir}}/runtime/renderer.bin",
                "provenance":"subos_view","abi":"fixture-v1",
                "digest":"sha256:def","host_fingerprint":"host-2"
              }}]
            }}
          }},
          "workspace": {{}}
        }})", runtimeName, envValue);
    }

    void point_default_glibc_view_at(std::string_view version) const {
        auto payload = install_glibc_payload(version);
        auto view = cfg.xlingsHome() / "subos" / "default" / "lib";
        std::filesystem::create_directories(view);
        std::filesystem::create_symlink(
            payload / "libc.so.6", view / "libc.so.6");
        std::filesystem::create_symlink(
            payload / "ld-linux-x86-64.so.2",
            view / "ld-linux-x86-64.so.2");
    }

    std::filesystem::path install_glibc_payload(
        std::string_view version) const {
        auto payload = cfg.xlingsHome() / "data" / "xpkgs"
                     / "xim-x-glibc" / version / "lib";
        std::filesystem::create_directories(payload);
        std::ofstream(payload / "libc.so.6") << "fixture libc";
        std::ofstream(payload / "ld-linux-x86-64.so.2") << "fixture loader";
        return payload;
    }
};

UTest absence_means_mcpp_default{"RuntimeSelection/AbsenceMeansMcppDefault", [] {
    mcpp::manifest::Manifest root;
    auto selected = runtime::select_runtime(root, std::nullopt, "/repo");
    if (auto&& failed = expect(fatal(selected.has_value())); !failed) failed << selected.error();
    expect(eq(selected->mode, runtime::RuntimeSelection::Mode::McppDefault));
    expect(eq(selected->source, runtime::RuntimeSelection::Source::DefaultPolicy));
    expect(eq(selected->subosName, "default"));
    expect(eq(selected->ownerRoot, std::filesystem::path("/repo")));
}};

UTest explicit_default_remains_named{"RuntimeSelection/ExplicitDefaultRemainsNamed", [] {
    auto root = named("default");
    auto selected = runtime::select_runtime(root, std::nullopt, "/repo");
    if (auto&& failed = expect(fatal(selected.has_value())); !failed) failed << selected.error();
    expect(eq(selected->mode, runtime::RuntimeSelection::Mode::NamedSubos));
    expect(eq(selected->source, runtime::RuntimeSelection::Source::Manifest));
    expect(eq(selected->subosName, "default"));
}};

UTest workspace_root_overrides_member{"RuntimeSelection/WorkspaceRootOverridesMember", [] {
    auto member = named("member-dev");
    auto workspace = named("workspace-el8");
    auto selected = runtime::select_runtime(
        member, std::cref(workspace), "/repo/member", "/repo");
    if (auto&& failed = expect(fatal(selected.has_value())); !failed) failed << selected.error();
    expect(eq(selected->subosName, "workspace-el8"));
    expect(eq(selected->ownerRoot, std::filesystem::path("/repo")));
}};

UTest member_applies_when_it_is_the_independent_root{"RuntimeSelection/MemberAppliesWhenItIsTheIndependentRoot", [] {
    auto member = named("member-dev");
    auto selected = runtime::select_runtime(member, std::nullopt, "/repo/member");
    if (auto&& failed = expect(fatal(selected.has_value())); !failed) failed << selected.error();
    expect(eq(selected->subosName, "member-dev"));
    expect(eq(selected->ownerRoot, std::filesystem::path("/repo/member")));
}};

UTest empty_and_path_like_names_are_rejected{"RuntimeSelection/EmptyAndPathLikeNamesAreRejected", [] {
    for (auto bad : {"", ".", "..", "a/b", "a\\b", "a:b"}) {
        auto root = named(bad);
        auto selected = runtime::select_runtime(root, std::nullopt, "/repo");
        expect(!selected.has_value()) << bad;
    }
}};

UTest dependency_declaration_cannot_affect_consumer_selection{"RuntimeSelection/DependencyDeclarationCannotAffectConsumerSelection", [] {
    mcpp::manifest::Manifest consumer;
    auto dependency = named("dependency-only");
    (void)dependency;
    auto selected = runtime::select_runtime(consumer, std::nullopt, "/consumer");
    if (auto&& failed = expect(fatal(selected.has_value())); !failed) failed << selected.error();
    expect(eq(selected->mode, runtime::RuntimeSelection::Mode::McppDefault));
    expect(eq(selected->subosName, "default"));
}};

UTest default_always_uses_configured_mcpp_home_default{"RuntimeBinding/DefaultAlwaysUsesConfiguredMcppHomeDefault", [] {
    RuntimeHome h;
    const auto expected = h.cfg.xlingsHome() / "subos" / "default";
    h.write(expected);

    mcpp::manifest::Manifest root;
    auto selection = runtime::select_runtime(root, std::nullopt, h.dir / "repo");
    expect(fatal(bool(selection)));
    auto binding = mcpp::runtime::resolve_runtime_binding(
        *selection, {}, h.cfg.xlingsHome());
    if (auto&& failed = expect(fatal(binding.has_value())); !failed) failed << binding.error();
    expect(eq(binding->subosDir, expected));
    expect(eq(binding->runtimeId, "glibc@2.39"));
    if constexpr (mcpplibs::os::is_linux)
        expect(eq(binding->libc, "glibc@2.39"));
    else
        expect(binding->libc.empty());
    expect(eq(binding->provenance, "mcpp_default"));
    expect(!(binding->contractHash.empty()));
    expect(fatal(eq(binding->runtimeProviders.size(), 1u)));
    expect(eq(binding->runtimeProviders[0].provider.name, "renderer"));
    expect(fatal(eq(binding->runtimeArtifacts.size(), 1u)));
    expect(eq(binding->runtimeArtifacts[0].path, (expected / "runtime/renderer.bin").lexically_normal()));
}};

UTest explicit_default_uses_global_default_but_keeps_named_identity{"RuntimeBinding/ExplicitDefaultUsesGlobalDefaultButKeepsNamedIdentity", [] {
    RuntimeHome h;
    const auto expected = h.cfg.xlingsHome() / "subos" / "default";
    h.write(expected);
    auto manifest = named("default");
    auto selection = runtime::select_runtime(manifest, std::nullopt, h.dir / "repo");
    expect(fatal(bool(selection)));
    auto binding = mcpp::runtime::resolve_runtime_binding(
        *selection, {}, h.cfg.xlingsHome());
    if (auto&& failed = expect(fatal(binding.has_value())); !failed) failed << binding.error();
    expect(eq(binding->subosDir, expected));
    expect(eq(binding->provenance, "named_subos"));
    expect(eq(binding->selection.mode, runtime::RuntimeSelection::Mode::NamedSubos));
}};

UTest physical_subos_view_reconciles_a_stale_declared_glibc_identity{"RuntimeBinding/PhysicalSubosViewReconcilesAStaleDeclaredGlibcIdentity", [] {
    if constexpr (!mcpplibs::os::is_linux)
        skip("glibc SubOS views only exist on Linux");

    RuntimeHome h;
    const auto subos = h.cfg.xlingsHome() / "subos" / "default";
    h.write(subos, "glibc@2.39");
    h.point_default_glibc_view_at("2.44");

    mcpp::manifest::Manifest root;
    auto selection = runtime::select_runtime(root, std::nullopt, h.dir / "repo");
    expect(fatal(bool(selection)));
    auto binding = mcpp::runtime::resolve_runtime_binding(
        *selection, {}, h.cfg.xlingsHome());
    if (auto&& failed = expect(fatal(binding.has_value())); !failed) failed << binding.error();

    expect(eq(binding->runtimeId, "glibc@2.44"));
    expect(eq(binding->libc, "glibc@2.44"));
    expect(fatal(eq(binding->libraryDirs.size(), 1u)));
    expect(eq(binding->libraryDirs.front(), h.cfg.xlingsHome() / "data" / "xpkgs"
                  / "xim-x-glibc" / "2.44" / "lib"));
}};

UTest broken_subos_view_falls_back_only_to_the_exact_declared_payload{"RuntimeBinding/BrokenSubosViewFallsBackOnlyToTheExactDeclaredPayload", [] {
    if constexpr (!mcpplibs::os::is_linux)
        skip("glibc SubOS views only exist on Linux");

    RuntimeHome h;
    const auto subos = h.cfg.xlingsHome() / "subos" / "default";
    h.write(subos, "glibc@2.44");
    auto payload = h.install_glibc_payload("2.44");
    auto view = subos / "lib";
    std::filesystem::create_directories(view);
    std::filesystem::create_symlink(
        payload.parent_path() / "lib64" / "libc.so.6",
        view / "libc.so.6");

    mcpp::manifest::Manifest root;
    auto selection = runtime::select_runtime(root, std::nullopt, h.dir / "repo");
    expect(fatal(bool(selection)));
    auto binding = mcpp::runtime::resolve_runtime_binding(
        *selection, {}, h.cfg.xlingsHome());
    if (auto&& failed = expect(fatal(binding.has_value())); !failed) failed << binding.error();

    expect(eq(binding->runtimeId, "glibc@2.44"));
    expect(fatal(eq(binding->libraryDirs.size(), 1u)));
    expect(eq(binding->libraryDirs.front(), payload));
    expect(eq(binding->loader, std::optional<std::filesystem::path>(
                  payload / "ld-linux-x86-64.so.2")));
}};

UTest named_subos_is_owned_by_the_root_and_missing_is_hard_error{"RuntimeBinding/NamedSubosIsOwnedByTheRootAndMissingIsHardError", [] {
    RuntimeHome h;
    auto manifest = named("el8");
    auto selection = runtime::select_runtime(manifest, std::nullopt, h.dir / "repo");
    expect(fatal(bool(selection)));

    auto missing = mcpp::runtime::resolve_runtime_binding(
        *selection, {}, h.cfg.xlingsHome());
    expect(fatal(!missing.has_value()));
    expect(ne(missing.error().find("el8"), std::string::npos));

    const auto expected = h.dir / "repo" / ".mcpp" / ".xlings" / "subos" / "el8";
    h.write(expected);
    auto binding = mcpp::runtime::resolve_runtime_binding(
        *selection, {}, h.cfg.xlingsHome());
    if (auto&& failed = expect(fatal(binding.has_value())); !failed) failed << binding.error();
    expect(eq(binding->subosDir, expected));
}};

UTest named_environments_have_distinct_contracts_and_round_trip{"RuntimeBinding/NamedEnvironmentsHaveDistinctContractsAndRoundTrip", [] {
    RuntimeHome h;
    auto el8Manifest = named("el8");
    auto devManifest = named("dev");
    auto el8Selection = runtime::select_runtime(
        el8Manifest, std::nullopt, h.dir / "repo");
    auto devSelection = runtime::select_runtime(
        devManifest, std::nullopt, h.dir / "repo");
    expect(fatal(bool(el8Selection)));
    expect(fatal(bool(devSelection)));
    h.write(h.dir / "repo" / ".mcpp" / ".xlings" / "subos" / "el8");
    h.write(h.dir / "repo" / ".mcpp" / ".xlings" / "subos" / "dev");

    auto el8 = mcpp::runtime::resolve_runtime_binding(
        *el8Selection, {}, h.cfg.xlingsHome());
    auto dev = mcpp::runtime::resolve_runtime_binding(
        *devSelection, {}, h.cfg.xlingsHome());
    expect(fatal(bool(el8)));
    expect(fatal(bool(dev)));
    expect(ne(el8->contractHash, dev->contractHash));

    auto encoded = mcpp::runtime::serialize_runtime_binding(*el8);
    auto decoded = mcpp::runtime::deserialize_runtime_binding(encoded);
    if (auto&& failed = expect(fatal(decoded.has_value())); !failed) failed << decoded.error();
    expect(eq(decoded->contractHash, el8->contractHash));
    if constexpr (mcpplibs::os::is_linux) {
        expect(fatal(!(decoded->hostLibc.empty())));
        expect(eq(decoded->hostLibc, "2.43"));
    } else {
        expect(decoded->hostLibc.empty());
    }
    expect(eq(decoded->subosDir, el8->subosDir));
    expect(fatal(eq(decoded->environment.size(), 1u)));
    expect(eq(decoded->environment[0].var, "DRIVERS"));
    expect(fatal(eq(decoded->runtimeProviders.size(), 1u)));
    expect(eq(decoded->runtimeProviders[0].provider.namespace_, "xim"));
    expect(fatal(eq(decoded->runtimeArtifacts.size(), 1u)));
    expect(eq(decoded->runtimeArtifacts[0].hostFingerprint, "host-2"));
    expect(bool(decoded->declared));
}};

// ── absence degrades, contradiction fails (openxlings/xlings#543) ──────────
//
// A SubOS that says nothing used to make `resolve_runtime_binding` return an
// error, which every caller of `mcpp build`/`mcpp test` propagated — stopping
// the tool on Windows with a message about GL drivers, on a platform that has
// no ELF, no PT_INTERP and no private libc. The rule the index-floor incident
// already paid for applies here too: data that is missing or newer must not
// invalidate the program that reads it.

UTest undeclared_subos_still_resolves{"RuntimeBindingDegradation/UndeclaredSubosStillResolves", [] {
    RuntimeHome h;
    auto manifest = named("bare");
    auto selection = runtime::select_runtime(manifest, std::nullopt,
                                             h.dir / "repo");
    expect(fatal(bool(selection)));
    // Exists, describes nothing. The common state of a freshly created SubOS,
    // and the permanent state on a platform whose xlings does not write the
    // block at all.
    std::filesystem::create_directories(
        h.dir / "repo" / ".mcpp" / ".xlings" / "subos" / "bare");

    auto binding = mcpp::runtime::resolve_runtime_binding(
        *selection, {}, h.cfg.xlingsHome());
    if (auto&& failed = expect(fatal(binding.has_value())); !failed) failed << "absence must degrade, not fail: " << binding.error();
    expect(!binding->declared);
    expect(!(binding->note.empty()))
        << "a degradation nobody reports is indistinguishable from none";
    expect(binding->runtimeId.empty());
    expect(!binding->hermetic());
}};

// The cache must survive it too. A degraded binding legitimately has schema 0
// and no runtime identity, and a completeness check that demands those fields
// would make every cached degraded binding undecodable — sending the build
// back down the slow path on every single invocation, forever.
UTest undeclared_binding_round_trips{"RuntimeBindingDegradation/UndeclaredBindingRoundTrips", [] {
    RuntimeHome h;
    auto manifest = named("bare");
    auto selection = runtime::select_runtime(manifest, std::nullopt,
                                             h.dir / "repo");
    expect(fatal(bool(selection)));
    std::filesystem::create_directories(
        h.dir / "repo" / ".mcpp" / ".xlings" / "subos" / "bare");
    auto binding = mcpp::runtime::resolve_runtime_binding(
        *selection, {}, h.cfg.xlingsHome());
    expect(fatal(binding.has_value()));

    auto decoded = mcpp::runtime::deserialize_runtime_binding(
        mcpp::runtime::serialize_runtime_binding(*binding));
    if (auto&& failed = expect(fatal(decoded.has_value())); !failed) failed << decoded.error();
    expect(eq(decoded->contractHash, binding->contractHash));
    expect(!decoded->declared);
    expect(eq(decoded->note, binding->note));
}};

// A newer schema is read, not refused. `subos_info::read` already documents
// this ("a HIGHER one on disk is still read"); the consumer used to demand
// equality, so the day xlings writes schema 2 every build on every platform
// would have stopped at once.
UTest newer_schema_is_read_not_refused{"RuntimeBindingDegradation/NewerSchemaIsReadNotRefused", [] {
    RuntimeHome h;
    auto manifest = named("future");
    auto selection = runtime::select_runtime(manifest, std::nullopt,
                                             h.dir / "repo");
    expect(fatal(bool(selection)));
    auto subos = h.dir / "repo" / ".mcpp" / ".xlings" / "subos" / "future";
    std::filesystem::create_directories(subos);
    std::ofstream(subos / ".xlings.json") << R"({
      "subos_info": {
        "schema_version": 99,
        "runtime": "glibc@2.39",
        "envs": {},
        "a_field_from_the_future": true
      }
    })";

    auto binding = mcpp::runtime::resolve_runtime_binding(
        *selection, {}, h.cfg.xlingsHome());
    if (auto&& failed = expect(fatal(binding.has_value())); !failed) failed << "a newer contract must not invalidate an older mcpp: "
        << binding.error();
    expect(bool(binding->declared));
    expect(eq(binding->runtimeId, "glibc@2.39"));
    expect(ne(binding->note.find("newer"), std::string::npos))
        << "reading a newer schema silently is the other half of the defect";
}};

// The boundary. A SubOS the user NAMED and that is not there cannot be
// satisfied under any interpretation, and picking a different one would make
// one mcpp.toml mean different ABIs on different machines.
UTest missing_subos_still_fails{"RuntimeBindingDegradation/MissingSubosStillFails", [] {
    RuntimeHome h;
    auto manifest = named("never-created");
    auto selection = runtime::select_runtime(manifest, std::nullopt,
                                             h.dir / "repo");
    expect(fatal(bool(selection)));
    auto binding = mcpp::runtime::resolve_runtime_binding(
        *selection, {}, h.cfg.xlingsHome());
    expect(fatal(!binding.has_value()))
        << "a contradiction must be reported, not degraded";
    expect(ne(binding.error().find("does not exist"), std::string::npos));
}};

} // namespace

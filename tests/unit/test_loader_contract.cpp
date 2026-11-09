
import std;
import mcpplibs.mtest;
import mcpp.build.loader_contract;
import mcpp.build.graph_shape;
import mcpp.pack.host_requirements;
import mcpp.manifest;
import mcpp.runtime.elf;
import mcpp.build.runtime_validation;
import mcpp.build.plan;
import mcpp.runtime.binding;
import mcpplibs.os.platform;
import nlohmann.json;

using namespace mcpplibs::mtest;

namespace {

// ─── the loader-tag contract ────────────────────────────────────────────────

UTest executables_need_rpath_libraries_need_runpath{"LoaderContract/ExecutablesNeedRpathLibrariesNeedRunpath", [] {
    using namespace mcpp::build::loader;
    expect(eq(required_tag(Form::Executable), RequiredTag::Rpath));
    expect(eq(required_tag(Form::SharedLibrary), RequiredTag::Runpath));
    expect(eq(required_tag(Form::NotElf), RequiredTag::NotApplicable));
}};

// The two producers must not each decide the spelling. If one of them ever
// grows its own string literal, this is the test that notices.
UTest both_producers_spell_the_same_rule_from_one_source{"LoaderContract/BothProducersSpellTheSameRuleFromOneSource", [] {
    using namespace mcpp::build::loader;
    auto exeLink   = link_flag(required_tag(Form::Executable));
    auto exePatch  = patchelf_flag(required_tag(Form::Executable));
    expect(fatal(exeLink.has_value()));
    expect(fatal(exePatch.has_value()));
    expect(eq(*exeLink, "-Wl,--disable-new-dtags"));
    expect(eq(*exePatch, "--force-rpath"));

    // Libraries take the default in BOTH producers -- forcing DT_RPATH onto a
    // library pushes its search path into every lookup below it and breaks
    // eglInitialize (openxlings/xlings#593).
    expect(!(link_flag(required_tag(Form::SharedLibrary)).has_value()));
    expect(!(patchelf_flag(required_tag(Form::SharedLibrary)).has_value()));
}};

// ─── graph shape ────────────────────────────────────────────────────────────

UTest unlabelled_or_unknown_graph_is_never_plain{"GraphShape/UnlabelledOrUnknownGraphIsNeverPlain", [] {
    using namespace mcpp::build;
    auto dir = std::filesystem::temp_directory_path()
             / "mcpp_graph_shape_test";
    std::filesystem::create_directories(dir);
    struct Cleanup { std::filesystem::path d;
        ~Cleanup() { std::error_code ec; std::filesystem::remove_all(d, ec); } }
        cleanup{dir};

    auto write = [&](std::string_view name, std::string_view body) {
        auto p = dir / name;
        std::ofstream out(p, std::ios::trunc);
        out << body;
        return p;
    };

    expect(is_plain_build_graph(
        write("normal.ninja", "# banner\n# mcpp:graph=normal;schedule=none;accel=default;dist=none\nrule x\n")));
    expect(!(is_plain_build_graph(
        write("test.ninja", "# banner\n# mcpp:graph=test;schedule=none;accel=default;dist=none\nrule x\n"))));

    // A plain-shaped graph that `mcpp pack --format <name>` wrote (2026.9.11.1+):
    // it carries an artifact edge consuming a staged tree, which a plain build
    // must not have. These lines are spelled by hand here rather than through
    // `header_line`, which is the point of this copy -- a reader of build.ninja
    // sees the text, and a field added to the producer without being added to
    // the reader would still pass a test that only compared the two.
    expect(!(is_plain_build_graph(
        write("dist.ninja", "# mcpp:graph=normal;schedule=none;accel=default;dist=appimage\n"))));

    // A plain-shaped graph an `--accel` / `--no-accel` build wrote (2026.9.5.3+):
    // the variant a flag chose is not the variant a plain build produces.
    expect(!(is_plain_build_graph(
        write("override.ninja", "# mcpp:graph=normal;schedule=none;accel=override\n"))));

    // A graph from 2026.9.5.2 and earlier: shape and schedule, no selection
    // field. Not known to be the manifest's variant, so a miss, not a guess.
    expect(!(is_plain_build_graph(
        write("no-selection.ninja", "# banner\n# mcpp:graph=normal\nrule x\n"))));

    // A graph from 2026.9.10.2: shape, schedule and selection, no distribution
    // field. Same rule one field later. This assertion is what caught the
    // second copy of these lines when the field was added -- the line above it
    // was the CURRENT spelling in one file and became the LEGACY spelling in
    // both, and only a test that spells it out could say so.
    expect(!(is_plain_build_graph(
        write("no-dist.ninja", "# banner\n# mcpp:graph=normal;schedule=none;accel=default\nrule x\n"))));

    // A build.ninja from before the marker existed. It MUST read as a miss:
    // treating it as plain is precisely the replay #407 is about.
    expect(!(is_plain_build_graph(
        write("legacy.ninja", "# banner\nninja_required_version = 1.11\n"))));

    // A shape this binary does not know. An older mcpp meeting a newer graph
    // must fall back, not guess.
    expect(!(is_plain_build_graph(
        write("future.ninja", "# mcpp:graph=coverage\n"))));

    // Missing file.
    expect(!is_plain_build_graph(dir / "absent.ninja"));
}};

UTest header_and_reader_agree{"GraphShape/HeaderAndReaderAgree", [] {
    using namespace mcpp::build;
    auto dir = std::filesystem::temp_directory_path() / "mcpp_graph_shape_rt";
    std::filesystem::create_directories(dir);
    struct Cleanup { std::filesystem::path d;
        ~Cleanup() { std::error_code ec; std::filesystem::remove_all(d, ec); } }
        cleanup{dir};

    // The line now carries the module-edge schedule too. Round-tripping both
    // fields together is the point: the schedule was added to this line rather
    // than to a second file precisely so the two cannot disagree.
    for (auto shape : {GraphShape::Normal, GraphShape::WithTests}) {
        for (std::string_view sched : {"none", "two-phase", "detach-codegen"}) {
            auto p = dir / "build.ninja";
            { std::ofstream out(p, std::ios::trunc);
              out << header_line(shape, sched) << "\n"; }
            auto read = read_shape(p);
            expect(fatal(read.has_value()));
            expect(eq(*read, shape));
            expect(eq(read_schedule(p), sched));
        }
    }

    // A graph written before the schedule field existed still reads as its
    // shape — an older file must degrade, not become "unknown" — but its
    // schedule reads as empty, which is NOT "none": callers that care have to
    // be able to tell "this file predates the field" from "this file chose to
    // do nothing".
    {
        auto p = dir / "build.ninja";
        { std::ofstream out(p, std::ios::trunc); out << "# mcpp:graph=normal\n"; }
        auto read = read_shape(p);
        expect(fatal(read.has_value()));
        expect(eq(*read, GraphShape::Normal));
        expect(read_schedule(p).empty());
    }
}};

// ─── host requirements: ONE derivation, two projections ─────────────────────

mcpp::manifest::RuntimeConfig runtime_with(std::vector<std::string> capabilities) {
    mcpp::manifest::RuntimeConfig rc;
    rc.capabilities = std::move(capabilities);
    return rc;
}

UTest only_run_phase_capabilities_count{"HostRequirements/OnlyRunPhaseCapabilitiesCount", [] {
    mcpp::manifest::RuntimeConfig rc;
    rc.requirements.push_back({.kind = "capability", .value = "opengl.glx.driver",
                               .phase = "run"});
    // A link-phase requirement is consumed during the build and says nothing
    // about the target machine.
    rc.requirements.push_back({.kind = "capability", .value = "pkg-config",
                               .phase = "link"});
    // A non-capability requirement is not a host capability either.
    rc.requirements.push_back({.kind = "library", .value = "libfoo.so.1",
                               .phase = "run"});

    auto reqs = mcpp::pack::host_requirements_of(rc);
    expect(fatal(eq(reqs.size(), 1u)));
    expect(eq(reqs[0].capability, "opengl.glx.driver"));
}};

// The legacy vector carries the same meaning. A package that has not migrated
// must not silently produce an empty list -- an empty HOST-REQUIREMENTS is a
// CLAIM that nothing is needed.
UTest legacy_capabilities_vector_is_still_read{"HostRequirements/LegacyCapabilitiesVectorIsStillRead", [] {
    auto reqs = mcpp::pack::host_requirements_of(
        runtime_with({"vulkan.icd", "opengl.egl.driver"}));
    expect(fatal(eq(reqs.size(), 2u)));
    expect(eq(reqs[0].capability, "opengl.egl.driver"));   // sorted
    expect(eq(reqs[1].capability, "vulkan.icd"));
}};

UTest duplicates_across_both_forms_collapse{"HostRequirements/DuplicatesAcrossBothFormsCollapse", [] {
    mcpp::manifest::RuntimeConfig rc;
    rc.requirements.push_back({.kind = "capability", .value = "opengl.glx.driver",
                               .phase = "run"});
    rc.capabilities.push_back("opengl.glx.driver");
    expect(eq(mcpp::pack::host_requirements_of(rc).size(), 1u));
}};

// The mechanism is DECLARED, never inferred. mcpp inferring it from the
// capability name would be provider-specific knowledge in mcpp's source --
// gated by test_runtime_contract, and wrong on its merits: the mechanism is
// the provider's property and changes without mcpp.
UTest discovery_is_carried_not_guessed{"HostRequirements/DiscoveryIsCarriedNotGuessed", [] {
    mcpp::manifest::RuntimeConfig rc;
    rc.requirements.push_back({.kind = "capability", .value = "opengl.egl.driver",
                               .phase = "run", .discovery = "json-dir"});
    auto reqs = mcpp::pack::host_requirements_of(rc);
    expect(fatal(eq(reqs.size(), 1u)));
    expect(eq(reqs[0].discovery, "json-dir"));

    // Undeclared stays empty and renders as `unknown` -- saying "we do not
    // know how this is found" is information; guessing would be a claim.
    auto legacy = mcpp::pack::host_requirements_of(
        runtime_with({"opengl.glx.driver"}));
    expect(fatal(eq(legacy.size(), 1u)));
    expect(legacy[0].discovery.empty());
    expect(ne(mcpp::pack::render(legacy).find("discovery=unknown"), std::string::npos));
}};

UTest render_is_greppable{"HostRequirements/RenderIsGreppable", [] {
    mcpp::manifest::RuntimeConfig rc;
    rc.requirements.push_back({.kind = "capability", .value = "opengl.glx.driver",
                               .phase = "run", .discovery = "rpath-of-dispatch"});
    auto text = mcpp::pack::render(mcpp::pack::host_requirements_of(rc));
    expect(ne(text.find("capability=opengl.glx.driver"), std::string::npos));
    expect(ne(text.find("discovery=rpath-of-dispatch"), std::string::npos));
}};

// ─── declared runtime artifact -> identity verdict ──────────────────────────
//
// The graphics failure this exists for: a provider declared at 0.1.2 while the
// symlink on disk still resolved into 0.1.1. Detectable as a pure path fact —
// no knowledge of what the artifact does.

UTest four_valued_and_symlink_aware{"ArtifactIdentity/FourValuedAndSymlinkAware", [] {
    using namespace mcpp::build::runtime_validation;
    namespace fs = std::filesystem;

    auto root = fs::temp_directory_path() / "mcpp_artifact_identity";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "store" / "0.1.1" / "lib");
    fs::create_directories(root / "store" / "0.1.2" / "lib");
    struct Cleanup { fs::path d;
        ~Cleanup() { std::error_code e; fs::remove_all(d, e); } } cleanup{root};

    auto write = [](const fs::path& p) { std::ofstream out(p); out << "x"; };
    write(root / "store" / "0.1.1" / "lib" / "libvendor.so");
    write(root / "store" / "0.1.2" / "lib" / "libvendor.so");

    mcpp::manifest::RuntimeArtifact a;
    a.role = "driver";
    a.provenance = "xim:vendor@0.1.2";

    // Nothing declared / nothing there.
    expect(eq(artifact_identity_verdict(a), ArtifactVerdict::Missing));
    a.path = root / "absent.so";
    expect(eq(artifact_identity_verdict(a), ArtifactVerdict::Missing));

    // Declared version matches the resolved payload.
    a.path = root / "store" / "0.1.2" / "lib" / "libvendor.so";
    expect(eq(artifact_identity_verdict(a), ArtifactVerdict::Ok));

    // THE case. A symlink that still points into the previous payload: reading
    // the declared path alone would confirm the promise against itself.
    fs::create_directory_symlink(root / "store" / "0.1.1", root / "current", ec);
    if (!ec) {
        a.path = root / "current" / "lib" / "libvendor.so";
        expect(eq(artifact_identity_verdict(a), ArtifactVerdict::Mismatch));
    }

    // No version to check against is UNVERIFIED, never Ok — "not checked" and
    // "checked and fine" must not look the same.
    a.path = root / "store" / "0.1.2" / "lib" / "libvendor.so";
    a.provenance = "xim:vendor";
    expect(eq(artifact_identity_verdict(a), ArtifactVerdict::Unverified));
}};

// A component match, not a substring: 0.1.1 must not satisfy 0.1.11.
UTest version_match_is_a_path_component{"ArtifactIdentity/VersionMatchIsAPathComponent", [] {
    using namespace mcpp::build::runtime_validation;
    namespace fs = std::filesystem;
    auto root = fs::temp_directory_path() / "mcpp_artifact_identity_sub";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "0.1.11");
    struct Cleanup { fs::path d;
        ~Cleanup() { std::error_code e; fs::remove_all(d, e); } } cleanup{root};
    { std::ofstream out(root / "0.1.11" / "lib.so"); out << "x"; }

    mcpp::manifest::RuntimeArtifact a;
    a.path = root / "0.1.11" / "lib.so";
    a.provenance = "xim:vendor@0.1.1";
    expect(eq(artifact_identity_verdict(a), ArtifactVerdict::Mismatch));
}};

// ─── a pass with nothing to say does not erase what a pass measured ─────────
//
// The backend runs once per pass and one invocation can drive it more than
// once: `mcpp test` builds the library and then links the test binary, and a
// pass that links only a dependency's shared library has no program, so the
// dlopen surface is not its question to answer.
//
// That pass still decides what the documented place to look contains. The two
// copies of the record have opposite lifetimes -- the sidecar survives an
// invocation, `resolution.json` is regenerated from an empty object at the
// start of one -- so a pass that publishes nothing leaves `resolution.json`
// empty for an answer that was measured, and a pass that publishes its
// non-answer overwrites that answer with a blank.
//
// Stated here because no end-to-end shape reaches it: it needs two drives over
// one output directory where the SECOND one is the one that does not apply.
UTest a_non_answer_republishes_the_answer_already_on_file{"DlopenSurfaceRecord/ANonAnswerRepublishesTheAnswerAlreadyOnFile", [] {
    namespace fs = std::filesystem;
    if constexpr (!mcpplibs::os::is_linux) {
        /* passes */;
        return;
    }

    auto root = fs::temp_directory_path() / "mcpp_dlopen_surface_record";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    struct Cleanup { fs::path d;
        ~Cleanup() { std::error_code e; fs::remove_all(d, e); } } cleanup{root};

    // A plan with no link units: it produces no program, which is exactly the
    // pass whose answer is "not mine to give".
    mcpp::build::BuildPlan plan;
    plan.outputDir = root;
    plan.runtimeBinding.loader = "/nonexistent/xlings-use-rpath-not-default-search";

    const auto sidecar = root / ".mcpp-runtime-verdicts.json";
    auto regenerate_resolution = [&] {
        std::ofstream out(root / "resolution.json");
        out << nlohmann::json{{"runtime", nlohmann::json::object()}}.dump(2) << '\n';
    };
    auto read_json = [](const fs::path& p) {
        std::ifstream in(p);
        return nlohmann::json::parse(in, nullptr, false);
    };

    // Drive one: nothing on file, so the non-answer is published WITH its
    // reason. "did not apply" and "was never run" must not read the same.
    regenerate_resolution();
    mcpp::build::runtime_validation::check_dlopen_surface(plan);
    auto stored = read_json(sidecar);
    expect(fatal(stored.is_object())) << "the sidecar was not written";
    expect(fatal(stored.contains("dlopen_surface")));
    expect(!(stored["dlopen_surface"].value("reason", "").empty()))
        << "a published non-answer carries the reason it did not apply";
    expect(!(read_json(root / "resolution.json")["runtime"]["dlopen_surface"]
                     .value("reason", "").empty()))
        << "the documented place to look carries it too";

    // A pass that DID apply now records a reading, under the key already on
    // file. The key covers the contract, the SubOS stamp and the host-libs
    // policy, none of which this test changes.
    stored["dlopen_surface"] = nlohmann::json{
        {"members", 7}, {"walked", 7}, {"findings", nlohmann::json::array()}};
    { std::ofstream out(sidecar); out << stored.dump(2) << '\n'; }

    // Drive two, with `resolution.json` regenerated as an invocation would:
    // the same plan that does not apply must neither overwrite the reading nor
    // leave the published copy empty.
    regenerate_resolution();
    mcpp::build::runtime_validation::check_dlopen_surface(plan);

    auto after = read_json(sidecar)["dlopen_surface"];
    expect(eq(after.value("members", 0), 7))
        << "a non-answer replaced a reading taken under the same key";
    expect(!after.contains("reason"));

    auto republished = read_json(root / "resolution.json")["runtime"]["dlopen_surface"];
    expect(eq(republished.value("members", 0), 7))
        << "the reading survived in the sidecar but not where it is documented";
    expect(eq(republished.value("walked", 0), 7));
}};

} // namespace

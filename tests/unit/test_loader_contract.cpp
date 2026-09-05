#include <gtest/gtest.h>

import std;
import mcpp.build.loader_contract;
import mcpp.build.graph_shape;
import mcpp.pack.host_requirements;
import mcpp.manifest;
import mcpp.runtime.elf;
import mcpp.build.runtime_validation;

namespace {

// ─── the loader-tag contract ────────────────────────────────────────────────

TEST(LoaderContract, ExecutablesNeedRpathLibrariesNeedRunpath) {
    using namespace mcpp::build::loader;
    EXPECT_EQ(required_tag(Form::Executable),    RequiredTag::Rpath);
    EXPECT_EQ(required_tag(Form::SharedLibrary), RequiredTag::Runpath);
    EXPECT_EQ(required_tag(Form::NotElf),        RequiredTag::NotApplicable);
}

// The two producers must not each decide the spelling. If one of them ever
// grows its own string literal, this is the test that notices.
TEST(LoaderContract, BothProducersSpellTheSameRuleFromOneSource) {
    using namespace mcpp::build::loader;
    auto exeLink   = link_flag(required_tag(Form::Executable));
    auto exePatch  = patchelf_flag(required_tag(Form::Executable));
    ASSERT_TRUE(exeLink.has_value());
    ASSERT_TRUE(exePatch.has_value());
    EXPECT_EQ(*exeLink,  "-Wl,--disable-new-dtags");
    EXPECT_EQ(*exePatch, "--force-rpath");

    // Libraries take the default in BOTH producers -- forcing DT_RPATH onto a
    // library pushes its search path into every lookup below it and breaks
    // eglInitialize (openxlings/xlings#593).
    EXPECT_FALSE(link_flag(required_tag(Form::SharedLibrary)).has_value());
    EXPECT_FALSE(patchelf_flag(required_tag(Form::SharedLibrary)).has_value());
}

// ─── graph shape ────────────────────────────────────────────────────────────

TEST(GraphShape, UnlabelledOrUnknownGraphIsNeverPlain) {
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

    EXPECT_TRUE(is_plain_build_graph(
        write("normal.ninja", "# banner\n# mcpp:graph=normal;schedule=none;accel=default\nrule x\n")));
    EXPECT_FALSE(is_plain_build_graph(
        write("test.ninja", "# banner\n# mcpp:graph=test;schedule=none;accel=default\nrule x\n")));

    // A plain-shaped graph an `--accel` / `--no-accel` build wrote (2026.9.5.3+):
    // the variant a flag chose is not the variant a plain build produces.
    EXPECT_FALSE(is_plain_build_graph(
        write("override.ninja", "# mcpp:graph=normal;schedule=none;accel=override\n")));

    // A graph from 2026.9.5.2 and earlier: shape and schedule, no selection
    // field. Not known to be the manifest's variant, so a miss, not a guess.
    EXPECT_FALSE(is_plain_build_graph(
        write("no-selection.ninja", "# banner\n# mcpp:graph=normal\nrule x\n")));

    // A build.ninja from before the marker existed. It MUST read as a miss:
    // treating it as plain is precisely the replay #407 is about.
    EXPECT_FALSE(is_plain_build_graph(
        write("legacy.ninja", "# banner\nninja_required_version = 1.11\n")));

    // A shape this binary does not know. An older mcpp meeting a newer graph
    // must fall back, not guess.
    EXPECT_FALSE(is_plain_build_graph(
        write("future.ninja", "# mcpp:graph=coverage\n")));

    // Missing file.
    EXPECT_FALSE(is_plain_build_graph(dir / "absent.ninja"));
}

TEST(GraphShape, HeaderAndReaderAgree) {
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
            ASSERT_TRUE(read.has_value());
            EXPECT_EQ(*read, shape);
            EXPECT_EQ(read_schedule(p), sched);
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
        ASSERT_TRUE(read.has_value());
        EXPECT_EQ(*read, GraphShape::Normal);
        EXPECT_TRUE(read_schedule(p).empty());
    }
}

// ─── host requirements: ONE derivation, two projections ─────────────────────

mcpp::manifest::RuntimeConfig runtime_with(std::vector<std::string> capabilities) {
    mcpp::manifest::RuntimeConfig rc;
    rc.capabilities = std::move(capabilities);
    return rc;
}

TEST(HostRequirements, OnlyRunPhaseCapabilitiesCount) {
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
    ASSERT_EQ(reqs.size(), 1u);
    EXPECT_EQ(reqs[0].capability, "opengl.glx.driver");
}

// The legacy vector carries the same meaning. A package that has not migrated
// must not silently produce an empty list -- an empty HOST-REQUIREMENTS is a
// CLAIM that nothing is needed.
TEST(HostRequirements, LegacyCapabilitiesVectorIsStillRead) {
    auto reqs = mcpp::pack::host_requirements_of(
        runtime_with({"vulkan.icd", "opengl.egl.driver"}));
    ASSERT_EQ(reqs.size(), 2u);
    EXPECT_EQ(reqs[0].capability, "opengl.egl.driver");   // sorted
    EXPECT_EQ(reqs[1].capability, "vulkan.icd");
}

TEST(HostRequirements, DuplicatesAcrossBothFormsCollapse) {
    mcpp::manifest::RuntimeConfig rc;
    rc.requirements.push_back({.kind = "capability", .value = "opengl.glx.driver",
                               .phase = "run"});
    rc.capabilities.push_back("opengl.glx.driver");
    EXPECT_EQ(mcpp::pack::host_requirements_of(rc).size(), 1u);
}

// The mechanism is DECLARED, never inferred. mcpp inferring it from the
// capability name would be provider-specific knowledge in mcpp's source --
// gated by test_runtime_contract, and wrong on its merits: the mechanism is
// the provider's property and changes without mcpp.
TEST(HostRequirements, DiscoveryIsCarriedNotGuessed) {
    mcpp::manifest::RuntimeConfig rc;
    rc.requirements.push_back({.kind = "capability", .value = "opengl.egl.driver",
                               .phase = "run", .discovery = "json-dir"});
    auto reqs = mcpp::pack::host_requirements_of(rc);
    ASSERT_EQ(reqs.size(), 1u);
    EXPECT_EQ(reqs[0].discovery, "json-dir");

    // Undeclared stays empty and renders as `unknown` -- saying "we do not
    // know how this is found" is information; guessing would be a claim.
    auto legacy = mcpp::pack::host_requirements_of(
        runtime_with({"opengl.glx.driver"}));
    ASSERT_EQ(legacy.size(), 1u);
    EXPECT_TRUE(legacy[0].discovery.empty());
    EXPECT_NE(mcpp::pack::render(legacy).find("discovery=unknown"),
              std::string::npos);
}

TEST(HostRequirements, RenderIsGreppable) {
    mcpp::manifest::RuntimeConfig rc;
    rc.requirements.push_back({.kind = "capability", .value = "opengl.glx.driver",
                               .phase = "run", .discovery = "rpath-of-dispatch"});
    auto text = mcpp::pack::render(mcpp::pack::host_requirements_of(rc));
    EXPECT_NE(text.find("capability=opengl.glx.driver"), std::string::npos);
    EXPECT_NE(text.find("discovery=rpath-of-dispatch"), std::string::npos);
}

// ─── declared runtime artifact -> identity verdict ──────────────────────────
//
// The graphics failure this exists for: a provider declared at 0.1.2 while the
// symlink on disk still resolved into 0.1.1. Detectable as a pure path fact —
// no knowledge of what the artifact does.

TEST(ArtifactIdentity, FourValuedAndSymlinkAware) {
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
    EXPECT_EQ(artifact_identity_verdict(a), ArtifactVerdict::Missing);
    a.path = root / "absent.so";
    EXPECT_EQ(artifact_identity_verdict(a), ArtifactVerdict::Missing);

    // Declared version matches the resolved payload.
    a.path = root / "store" / "0.1.2" / "lib" / "libvendor.so";
    EXPECT_EQ(artifact_identity_verdict(a), ArtifactVerdict::Ok);

    // THE case. A symlink that still points into the previous payload: reading
    // the declared path alone would confirm the promise against itself.
    fs::create_directory_symlink(root / "store" / "0.1.1", root / "current", ec);
    if (!ec) {
        a.path = root / "current" / "lib" / "libvendor.so";
        EXPECT_EQ(artifact_identity_verdict(a), ArtifactVerdict::Mismatch);
    }

    // No version to check against is UNVERIFIED, never Ok — "not checked" and
    // "checked and fine" must not look the same.
    a.path = root / "store" / "0.1.2" / "lib" / "libvendor.so";
    a.provenance = "xim:vendor";
    EXPECT_EQ(artifact_identity_verdict(a), ArtifactVerdict::Unverified);
}

// A component match, not a substring: 0.1.1 must not satisfy 0.1.11.
TEST(ArtifactIdentity, VersionMatchIsAPathComponent) {
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
    EXPECT_EQ(artifact_identity_verdict(a), ArtifactVerdict::Mismatch);
}

} // namespace

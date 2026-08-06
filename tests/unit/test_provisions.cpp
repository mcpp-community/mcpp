#include <gtest/gtest.h>

import std;
import mcpp.build.provisions;

// A build-time provision — a host tool, a host build rule, a dependency's
// directory — is something a dependency hands to its consumer's build PROGRAM.
// Before #359 each kind invented its own reach, and none could be re-exported,
// so a library could not stand up a toolchain on its user's behalf.
//
// Two properties are load-bearing and both are supply-chain properties, which
// is why they are pinned here rather than left to an e2e:
//   * nothing propagates unless the edge says `reexport = true`;
//   * an unqualified name is bound by a fixed ladder, never by "whoever was
//     appended last" — otherwise two libraries that have never heard of each
//     other could decide which `protoc` runs.

namespace prov = mcpp::build::provisions;

namespace {

struct Edge {
    std::size_t consumerPackageIndex = 0;
    std::size_t dependencyPackageIndex = 0;
    std::vector<std::string> requestedTools;
    bool hostModule = false;
    bool reexport = false;
};

bool sees_tool(const prov::Propagation& p, std::size_t consumer,
               std::size_t provider, std::string_view tool) {
    if (consumer >= p.visible.size()) return false;
    return p.visible[consumer].contains(
        prov::Provision{ prov::Kind::Tool, provider, std::string(tool) });
}

bool sees_dir(const prov::Propagation& p, std::size_t consumer,
              std::size_t provider) {
    if (consumer >= p.visible.size()) return false;
    return p.visible[consumer].contains(
        prov::Provision{ prov::Kind::DepDir, provider, {} });
}

}  // namespace

// ── Table integrity ────────────────────────────────────────────────────────

TEST(Provisions, EveryKindAnswersBothQuestions) {
    // The table exists so a new provision kind cannot be added without someone
    // stating how far it travels. A row that answered neither would compile
    // fine and silently pick a default, which is the exact failure #359 fixed.
    std::set<prov::Kind> seen;
    for (auto const& d : prov::kTable) {
        EXPECT_FALSE(d.name.empty());
        EXPECT_TRUE(d.needsReexport) << d.name;
        seen.insert(d.kind);
    }
    EXPECT_EQ(seen.size(), 3u);
    EXPECT_TRUE(prov::def_of(prov::Kind::Tool).bareAddressable);
    EXPECT_TRUE(prov::def_of(prov::Kind::DepDir).bareAddressable);
    // A host module is addressed by `import <name>;` — the compiler resolves
    // it, so there is no bare-name channel for mcpp to bind.
    EXPECT_FALSE(prov::def_of(prov::Kind::HostModule).bareAddressable);
}

// ── Propagation ────────────────────────────────────────────────────────────

TEST(Provisions, ToolIsVisibleToTheRequesterWithoutAnyReexport) {
    // 0=root, 1=protobuf.  root --tools=[protoc]--> protobuf
    std::vector<Edge> edges{ { 0, 1, { "protoc" }, false, false } };
    auto p = prov::propagate(edges, 2);
    EXPECT_TRUE(sees_tool(p, 0, 1, "protoc"));
    // Nothing to export: the root has no consumers, and it never said it
    // re-exported anything anyway.
    EXPECT_TRUE(p.exported[0].empty());
}

TEST(Provisions, WithoutReexportALibrarysToolStaysWithTheLibrary) {
    // 0=app, 1=lib, 2=protobuf.  app -> lib --tools=[protoc]--> protobuf
    std::vector<Edge> edges{
        { 0, 1, {}, false, false },
        { 1, 2, { "protoc" }, false, /*reexport=*/false },
    };
    auto p = prov::propagate(edges, 3);
    EXPECT_TRUE(sees_tool(p, 1, 2, "protoc"));
    // THE test: an ordinary dependency must not push entries into its
    // consumer's tool namespace. That is a supply-chain rule, not a
    // convenience — which is why `reexport` defaults to false.
    EXPECT_FALSE(sees_tool(p, 0, 2, "protoc"));
}

TEST(Provisions, ReexportHandsTheToolToTheLibrarysConsumer) {
    std::vector<Edge> edges{
        { 0, 1, {}, false, false },
        { 1, 2, { "protoc" }, false, /*reexport=*/true },
    };
    auto p = prov::propagate(edges, 3);
    EXPECT_TRUE(sees_tool(p, 0, 2, "protoc"));
    EXPECT_TRUE(sees_tool(p, 1, 2, "protoc"));
}

TEST(Provisions, ReexportTravelsFurtherOnlyWhenEachHopSaysSo) {
    // 0=app, 1=mid, 2=lib, 3=protobuf.
    // lib re-exports protoc to mid; mid does NOT re-export to app.
    std::vector<Edge> edges{
        { 0, 1, {}, false, /*reexport=*/false },
        { 1, 2, {}, false, /*reexport=*/false },
        { 2, 3, { "protoc" }, false, /*reexport=*/true },
    };
    auto p = prov::propagate(edges, 4);
    EXPECT_TRUE(sees_tool(p, 2, 3, "protoc"));
    EXPECT_TRUE(sees_tool(p, 1, 3, "protoc"));
    EXPECT_FALSE(sees_tool(p, 0, 3, "protoc"));

    // Flip the middle hop and it reaches all the way. Each package decides
    // what IT hands to ITS consumers; nothing decides on someone else's
    // behalf.
    edges[1].reexport = true;
    auto q = prov::propagate(edges, 4);
    EXPECT_TRUE(sees_tool(q, 0, 3, "protoc"));
}

TEST(Provisions, HostModuleFollowsTheSameRule) {
    std::vector<Edge> edges{
        { 0, 1, {}, false, false },
        { 1, 2, {}, /*hostModule=*/true, /*reexport=*/true },
    };
    auto p = prov::propagate(edges, 3);
    const prov::Provision rule{ prov::Kind::HostModule, 2, {} };
    EXPECT_TRUE(p.visible[0].contains(rule));
    EXPECT_TRUE(p.visible[1].contains(rule));
}

TEST(Provisions, EveryEdgeExposesTheDependencyDirectory) {
    // dep_dir() has always worked for a DIRECT dependency without anyone
    // declaring anything; that stays true. Re-export is what extends it.
    std::vector<Edge> edges{
        { 0, 1, {}, false, false },
        { 1, 2, {}, false, /*reexport=*/true },
        { 1, 3, {}, false, /*reexport=*/false },
    };
    auto p = prov::propagate(edges, 4);
    EXPECT_TRUE(sees_dir(p, 0, 1));
    EXPECT_TRUE(sees_dir(p, 0, 2));
    EXPECT_FALSE(sees_dir(p, 0, 3));
    EXPECT_TRUE(sees_dir(p, 1, 3));
}

TEST(Provisions, ACycleTerminates) {
    // Resolution should not produce one, but the fixpoint must not depend on
    // that: sets only grow and are bounded, so a cycle simply stops changing.
    std::vector<Edge> edges{
        { 0, 1, { "a" }, false, true },
        { 1, 0, { "b" }, false, true },
    };
    auto p = prov::propagate(edges, 2);
    EXPECT_TRUE(sees_tool(p, 0, 1, "a"));
    EXPECT_TRUE(sees_tool(p, 1, 0, "b"));
}

// ── Bare-name binding ──────────────────────────────────────────────────────

TEST(Provisions, ALoneCandidateOwnsItsBareName) {
    auto b = prov::bind_bare_names({ "compat.protobuf" });
    ASSERT_TRUE(b.contains("protobuf"));
    EXPECT_EQ(b["protobuf"].owner, "compat.protobuf");
    EXPECT_FALSE(b["protobuf"].contested);
    EXPECT_TRUE(prov::contest_note("protobuf", b["protobuf"]).empty());
}

TEST(Provisions, ANonDefaultNamespaceStillGetsItsBareName) {
    // grpc-m's rule calls dep_bin("grpc-plugin", …); (grpc, grpc-plugin) is on
    // no rung of the package-identity ladder, so without the
    // unique-candidate rung the spelling already in the wild would break.
    auto b = prov::bind_bare_names({ "grpc.grpc-plugin" });
    EXPECT_EQ(b["grpc-plugin"].owner, "grpc.grpc-plugin");
}

TEST(Provisions, TheLadderPicksTheDefaultNamespaceFirst) {
    auto b = prov::bind_bare_names({ "compat.zlib", "mcpplibs.zlib", "acme.zlib" });
    EXPECT_EQ(b["zlib"].owner, "mcpplibs.zlib");
    EXPECT_TRUE(b["zlib"].contested);
    // Contested but bound: the user is told which one won and how to name the
    // other, rather than getting whichever the loop appended last.
    auto note = prov::contest_note("zlib", b["zlib"]);
    EXPECT_NE(note.find("mcpplibs.zlib"), std::string::npos);
    EXPECT_NE(note.find("acme.zlib"), std::string::npos);
}

TEST(Provisions, CompatIsTheSecondRung) {
    auto b = prov::bind_bare_names({ "compat.zlib", "acme.zlib" });
    EXPECT_EQ(b["zlib"].owner, "compat.zlib");
}

TEST(Provisions, TwoUnreachableNamespacesLeaveTheBareNameUnbound) {
    // Neither is on the ladder and neither is unique. Binding either one would
    // be an arbitrary choice that decides which binary runs.
    auto b = prov::bind_bare_names({ "acme.protoc-ish", "other.protoc-ish" });
    EXPECT_TRUE(b["protoc-ish"].owner.empty());
    EXPECT_TRUE(b["protoc-ish"].contested);
    auto note = prov::contest_note("protoc-ish", b["protoc-ish"]);
    EXPECT_NE(note.find("NOT"), std::string::npos);
}

TEST(Provisions, AnUnnamespacedPackageIsTheThirdRung) {
    auto b = prov::bind_bare_names({ "grpcgen", "acme.grpcgen" });
    EXPECT_EQ(b["grpcgen"].owner, "grpcgen");
}

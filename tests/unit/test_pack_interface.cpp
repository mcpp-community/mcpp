
import std;
import mcpplibs.mtest;
import mcpp.pack.interface;
import mcpp.modgraph.graph;

using namespace mcpplibs::mtest;

using namespace mcpp::pack;
using mcpp::modgraph::Graph;
using mcpp::modgraph::ModuleId;
using mcpp::modgraph::SourceUnit;

namespace {

// The shape of the library the design is written against:
//
//   mathkit.cppm   export module mathkit;  export import :api;   ← root
//   api.cppm       export module mathkit:api;                    ← interface partition
//   secret.cppm    module mathkit:secret;   ← implementation partition, PRIVATE
//   impl.cpp       module mathkit;  import :secret;
//   capi.c         (no module at all)
//
// `module M:part;` IS a provider — of `M:part` — and the scanner records it as
// one with `providesInterface = false`. It used to record "requires M:part,
// provides nothing", i.e. a file requiring its own name, which left the graph
// with no edge from the importer to the definer. That is the shape this fixture
// now mirrors, because the closure's warning depends on the distinction.
Graph library_graph(bool interfaceReachesSecret = false) {
    Graph g;
    // `iface` is optional on purpose: nullopt is the state a `scan_overrides`
    // unit is in — it names the module and cannot say whether it is exported.
    auto add = [&](std::string path, std::optional<std::string> provides,
                   std::vector<std::string> requires_,
                   std::optional<bool> iface = true) {
        SourceUnit u;
        u.path        = std::move(path);
        u.packageName = "mathkit";
        if (provides) u.provides = ModuleId{ *provides };
        u.providesInterface = iface;
        for (auto& r : requires_) u.requires_.push_back(ModuleId{ std::move(r) });
        g.units.push_back(std::move(u));
    };

    add("src/mathkit.cppm", "mathkit",
        interfaceReachesSecret ? std::vector<std::string>{ "mathkit:api", "mathkit:secret" }
                               : std::vector<std::string>{ "mathkit:api" });
    add("src/api.cppm",   "mathkit:api", {});
    add("src/secret.cppm", "mathkit:secret", {}, /*iface=*/false);
    add("src/impl.cpp",    std::nullopt,  { "mathkit", "mathkit:secret" });
    add("src/capi.c",      std::nullopt,  {});

    for (std::size_t i = 0; i < g.units.size(); ++i)
        if (g.units[i].provides)
            g.producerOf.emplace(g.units[i].provides->logicalName, i);
    return g;
}

std::vector<std::string> names(const std::vector<std::filesystem::path>& v) {
    std::vector<std::string> out;
    for (auto const& p : v) out.push_back(p.filename().string());
    std::ranges::sort(out);
    return out;
}

}  // namespace

// ─── what travels, and what does not ───────────────────────────────────────

UTest publishes_the_root_and_its_interface_partition_only{"InterfaceClosure/PublishesTheRootAndItsInterfacePartitionOnly", [] {
    auto c = interface_closure(library_graph(), "mathkit", "mathkit");
    if (auto&& failed = expect(fatal(c.has_value())); !failed) failed << (c ? "" : c.error());
    expect(eq(names(c->published), (std::vector<std::string>{"api.cppm", "mathkit.cppm"})));
}};

UTest withholds_the_implementation_partition_source{"InterfaceClosure/WithholdsTheImplementationPartitionSource", [] {
    // The whole point for a closed-source library: `secret.cppm` produces a
    // BMI and an object, and its SOURCE must not be published.
    auto c = interface_closure(library_graph(), "mathkit", "mathkit");
    expect(fatal(c.has_value()));
    expect(eq(names(c->withheld), (std::vector<std::string>{"capi.c", "impl.cpp", "secret.cppm"})));
}};

UTest drop_set_is_the_published_objects_not_every_module_object{"InterfaceClosure/DropSetIsThePublishedObjectsNotEveryModuleObject", [] {
    // Measured: deleting every `.m.o` also deletes `secret.m.o`, which holds
    // real code, and every target then fails to link with an undefined
    // reference that names neither the archive nor the rule that removed it.
    auto c = interface_closure(library_graph(), "mathkit", "mathkit");
    expect(fatal(c.has_value()));
    auto drop = published_object_names(*c);
    std::ranges::sort(drop);
    expect(eq(drop, (std::vector<std::string>{"api.m.o", "mathkit.m.o"})));
    expect(eq(std::ranges::find(drop, "secret.m.o"), drop.end()));
}};

// ─── the loud half of the asymmetry ────────────────────────────────────────

UTest an_interface_that_reaches_an_implementation_partition_publishes_it_and_says_so{"InterfaceClosure/AnInterfaceThatReachesAnImplementationPartitionPublishesItAndSaysSo", [] {
    // Legal, and the consumer cannot build the interface's BMI without that
    // source — so it is published rather than refused. But for a closed-source
    // library it is the one outcome nobody wants by accident, and `import
    // :secret;` in an interface reads like any other import, so it is reported.
    auto c = interface_closure(library_graph(/*interfaceReachesSecret=*/true),
                               "mathkit", "mathkit");
    expect(fatal(c.has_value()));
    expect(eq(names(c->published), (std::vector<std::string>{"api.cppm", "mathkit.cppm", "secret.cppm"})));
    expect(fatal(eq(c->publishedImplementationPartitions.size(), 1u)));
    expect(eq(c->publishedImplementationPartitions[0].filename().string(), "secret.cppm"));
    expect(c->unresolvedImports.empty());
}};

UTest an_interface_only_closure_reports_no_partition_leak{"InterfaceClosure/AnInterfaceOnlyClosureReportsNoPartitionLeak", [] {
    auto c = interface_closure(library_graph(), "mathkit", "mathkit");
    expect(fatal(c.has_value()));
    expect(c->publishedImplementationPartitions.empty());
    expect(c->publishedUndeterminedPartitions.empty());
}};

// ─── the state that used to be spelled "interface" ─────────────────────────

UTest a_published_partition_of_unknown_kind_is_reported_separately{"InterfaceClosure/APublishedPartitionOfUnknownKindIsReportedSeparately", [] {
    // A `[scan_overrides."<glob>"]` entry says which module a file provides and
    // has nowhere to say whether the declaration is exported; a P1689 scanner
    // may omit `is-interface`. That used to arrive as `providesInterface =
    // true` — the value that produces NO warning — so an implementation
    // partition declared that way was published in silence.
    auto g = library_graph(/*interfaceReachesSecret=*/true);
    g.units[2].providesInterface.reset();          // src/secret.cppm, kind unknown
    auto c = interface_closure(g, "mathkit", "mathkit");
    expect(fatal(c.has_value()));
    // Still published — the consumer cannot build the root's BMI without it.
    expect(eq(names(c->published), (std::vector<std::string>{"api.cppm", "mathkit.cppm", "secret.cppm"})));
    // But reported as undetermined, not as a known implementation partition:
    // the sentence to print is a different one.
    expect(c->publishedImplementationPartitions.empty());
    expect(fatal(eq(c->publishedUndeterminedPartitions.size(), 1u)));
    expect(eq(c->publishedUndeterminedPartitions[0].filename().string(), "secret.cppm"));
}};

UTest an_undetermined_primary_interface_is_not_reported{"InterfaceClosure/AnUndeterminedPrimaryInterfaceIsNotReported", [] {
    // Only a PARTITION can be either kind. `module M;` provides nothing, so the
    // only declaration that provides a bare `M` is `export module M;` — asking
    // the question there would warn about every primary interface in every
    // package that uses scan_overrides, and a warning that fires on the normal
    // case is one nobody reads.
    auto g = library_graph();
    g.units[0].providesInterface.reset();          // src/mathkit.cppm, the root
    auto c = interface_closure(g, "mathkit", "mathkit");
    expect(fatal(c.has_value()));
    expect(c->publishedUndeterminedPartitions.empty());
    expect(c->publishedImplementationPartitions.empty());
}};

UTest a_withheld_partition_of_unknown_kind_is_not_reported_either{"InterfaceClosure/AWithheldPartitionOfUnknownKindIsNotReportedEither", [] {
    // The warning is about what is PUBLISHED. `secret.cppm` is unreachable from
    // the interface here, so its kind never mattered.
    auto g = library_graph(/*interfaceReachesSecret=*/false);
    g.units[2].providesInterface.reset();
    auto c = interface_closure(g, "mathkit", "mathkit");
    expect(fatal(c.has_value()));
    expect(c->publishedUndeterminedPartitions.empty());
}};

UTest a_genuinely_missing_partition_is_still_an_error{"InterfaceClosure/AGenuinelyMissingPartitionIsStillAnError", [] {
    // The unresolved path is not dead: a partition nothing provides means the
    // published set is incomplete and the consumer's compile will fail on it.
    auto g = library_graph(/*interfaceReachesSecret=*/true);
    g.producerOf.erase("mathkit:secret");
    auto c = interface_closure(g, "mathkit", "mathkit");
    expect(fatal(c.has_value()));
    expect(fatal(eq(c->unresolvedImports.size(), 1u)));
    expect(eq(c->unresolvedImports[0], "mathkit:secret"));
}};

UTest a_dependencys_module_is_neither_published_nor_unresolved{"InterfaceClosure/ADependencysModuleIsNeitherPublishedNorUnresolved", [] {
    auto g = library_graph();
    g.units[0].requires_.push_back(ModuleId{"compat.zlib"});   // someone else's
    auto c = interface_closure(g, "mathkit", "mathkit");
    expect(fatal(c.has_value()));
    expect(c->unresolvedImports.empty());
    expect(eq(c->published.size(), 2u));
}};

UTest foreign_package_units_are_not_followed{"InterfaceClosure/ForeignPackageUnitsAreNotFollowed", [] {
    auto g = library_graph();
    SourceUnit other;
    other.path        = "vendor/zlib.cppm";
    other.packageName = "compat.zlib";
    other.provides    = ModuleId{"compat.zlib"};
    g.producerOf.emplace("compat.zlib", g.units.size());
    g.units.push_back(std::move(other));
    g.units[0].requires_.push_back(ModuleId{"compat.zlib"});

    auto c = interface_closure(g, "mathkit", "mathkit");
    expect(fatal(c.has_value()));
    expect(eq(names(c->published), (std::vector<std::string>{"api.cppm", "mathkit.cppm"})));
    // Nor does a foreign unit show up as something we withheld.
    auto withheld = names(c->withheld);
    expect(eq(std::ranges::find(withheld, "zlib.cppm"), withheld.end()));
}};

UTest refuses_an_unknown_root{"InterfaceClosure/RefusesAnUnknownRoot", [] {
    auto c = interface_closure(library_graph(), "mathkit", "nosuch");
    expect(fatal(!c.has_value()));
    expect(ne(c.error().find("nosuch"), std::string::npos));
}};

UTest refuses_a_root_owned_by_another_package{"InterfaceClosure/RefusesARootOwnedByAnotherPackage", [] {
    auto g = library_graph();
    g.units[0].packageName = "somebody.else";
    auto c = interface_closure(g, "mathkit", "mathkit");
    expect(fatal(!c.has_value()));
    expect(ne(c.error().find("somebody.else"), std::string::npos));
}};

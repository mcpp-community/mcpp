
import std;
import mcpplibs.mtest;
import mcpp.graph;

using namespace mcpplibs::mtest;

// SUBSYSTEM-LEVEL: this package answers "what order / what depth / what does
// this reach" for an index-addressed directed graph, and nothing else. The
// migrated callers (src/modgraph/graph.cppm, src/build/prepare/features.cpp,
// src/build/prepare/graph.cpp, src/build/dep_graph.cppm,
// src/build/prepare/plan.cpp) restate a subset of these facts through their
// own edge shape in tests/unit/test_modgraph.cpp and in the e2e suite.

namespace g = mcpp::graph;

namespace {
g::AdjacencyList chain(std::size_t n) {
    // 0 <- 1 <- 2 <- ... <- n-1 (node i depends on i-1).
    g::AdjacencyList deps(n);
    for (std::size_t i = 1; i < n; ++i) deps[i] = {i - 1};
    return deps;
}
}

UTest empty_graph_orders_nothing{"TopologicalOrder/EmptyGraphOrdersNothing", [] {
    auto r = g::topological_order({});
    expect(fatal(r.has_value()));
    expect(r->empty());
}};

UTest single_node_no_deps{"TopologicalOrder/SingleNodeNoDeps", [] {
    auto r = g::topological_order({{}});
    expect(fatal(r.has_value()));
    expect(eq(*r, (std::vector<std::size_t>{0})));
}};

UTest unconstrained_nodes_come_out_in_input_index_order{"TopologicalOrder/UnconstrainedNodesComeOutInInputIndexOrder", [] {
    // No edges at all: every node is ready from the start, and ties are
    // broken by index, so the order is the identity permutation.
    g::AdjacencyList deps(5);
    auto r = g::topological_order(deps);
    expect(fatal(r.has_value()));
    expect(eq(*r, (std::vector<std::size_t>{0, 1, 2, 3, 4})));
}};

UTest ties_are_broken_by_input_index_even_against_declaration_order{"TopologicalOrder/TiesAreBrokenByInputIndexEvenAgainstDeclarationOrder", [] {
    // Node 2 depends on nothing but is declared last among the roots; nodes
    // 0 and 1 depend on nothing either. All three become ready at once, so
    // the tie-break (lowest index first) — not the order dependents were
    // declared in — decides: 0, 1, 2, then 3 (which depends on all three).
    g::AdjacencyList deps(4);
    deps[3] = {2, 0, 1};   // declared in a scrambled order on purpose
    auto r = g::topological_order(deps);
    expect(fatal(r.has_value()));
    expect(eq(*r, (std::vector<std::size_t>{0, 1, 2, 3})));
}};

UTest diamond{"TopologicalOrder/Diamond", [] {
    // 3 depends on {1, 2}; 1 and 2 both depend on {0}.
    g::AdjacencyList deps(4);
    deps[1] = {0};
    deps[2] = {0};
    deps[3] = {1, 2};
    auto r = g::topological_order(deps);
    expect(fatal(r.has_value()));
    expect(eq(*r, (std::vector<std::size_t>{0, 1, 2, 3})));
}};

UTest disconnected_components_order_by_index_not_by_component{"TopologicalOrder/DisconnectedComponentsOrderByIndexNotByComponent", [] {
    // Two unrelated chains, 0 -> 2 and 1 -> 3 (node 2 depends on node 0,
    // node 3 depends on node 1). Both roots are ready at once, so the
    // tie-break processes 0 then 1 before either child — the two
    // components' roots interleave by index rather than one component
    // finishing before the other starts.
    g::AdjacencyList deps(4);
    deps[2] = {0};
    deps[3] = {1};
    auto r = g::topological_order(deps);
    expect(fatal(r.has_value()));
    expect(eq(*r, (std::vector<std::size_t>{0, 1, 2, 3})));
}};

UTest long_chain_is_the_identity_permutation{"TopologicalOrder/LongChainIsTheIdentityPermutation", [] {
    auto deps = chain(6);
    auto r = g::topological_order(deps);
    expect(fatal(r.has_value()));
    expect(eq(*r, (std::vector<std::size_t>{0, 1, 2, 3, 4, 5})));
}};

UTest self_loop_is_a_cycle_of_length_one{"TopologicalOrder/SelfLoopIsACycleOfLengthOne", [] {
    g::AdjacencyList deps(1);
    deps[0] = {0};
    auto r = g::topological_order(deps);
    expect(fatal(!r.has_value()));
    expect(eq(r.error().cycle, (std::vector<std::size_t>{0, 0})));
}};

UTest longer_cycle_reports_the_exact_path{"TopologicalOrder/LongerCycleReportsTheExactPath", [] {
    // a(0) -> b(1) -> c(2) -> a(0), plus an unrelated node 3 that depends on
    // nothing and must not appear in the ring.
    g::AdjacencyList deps(4);
    deps[0] = {1};   // a depends on b
    deps[1] = {2};   // b depends on c
    deps[2] = {0};   // c depends on a
    auto r = g::topological_order(deps);
    expect(fatal(!r.has_value()));
    // The path starts wherever the DFS first re-enters the ring; what is
    // pinned down is that it IS the ring a -> b -> c -> a, walked in one
    // direction, closed at both ends by the same node.
    const auto& cycle = r.error().cycle;
    expect(fatal(ge(cycle.size(), 2u)));
    expect(eq(cycle.front(), cycle.back()));
    for (std::size_t i = 0; i + 1 < cycle.size(); ++i) {
        auto u = cycle[i], v = cycle[i + 1];
        expect(std::ranges::find(deps[u], v) != deps[u].end())
            << u << " does not declare a dependency on " << v;
    }
    // Exactly the three ring members, not the unrelated fourth node.
    std::set<std::size_t> members(cycle.begin(), cycle.end());
    expect(eq(members, (std::set<std::size_t>{0, 1, 2})));
}};

UTest cycle_among_otherwise_orderable_nodes{"TopologicalOrder/CycleAmongOtherwiseOrderableNodes", [] {
    // 0 is a normal leaf; 1 and 2 cycle on each other; 3 depends on 1.
    g::AdjacencyList deps(4);
    deps[1] = {2};
    deps[2] = {1};
    deps[3] = {1};
    auto r = g::topological_order(deps);
    expect(fatal(!r.has_value()));
    std::set<std::size_t> members(r.error().cycle.begin(), r.error().cycle.end());
    expect(eq(members, (std::set<std::size_t>{1, 2})));
}};

UTest leaves_are_level_zero{"Levels/LeavesAreLevelZero", [] {
    g::AdjacencyList deps(3);
    auto r = g::levels(deps);
    expect(fatal(r.has_value()));
    expect(eq(*r, (std::vector<std::size_t>{0, 0, 0})));
}};

UTest chain_counts_up{"Levels/ChainCountsUp", [] {
    auto deps = chain(4);
    auto r = g::levels(deps);
    expect(fatal(r.has_value()));
    expect(eq(*r, (std::vector<std::size_t>{0, 1, 2, 3})));
}};

UTest diamond_takes_the_longer_arm{"Levels/DiamondTakesTheLongerArm", [] {
    // 4 depends on {1, 3}; 1 depends on {0}; 3 depends on {2}, {2} depends
    // on {0}. The two arms from 0 to 4 have different lengths (2 and 3), and
    // the level is the LONGER one.
    g::AdjacencyList deps(5);
    deps[1] = {0};
    deps[2] = {0};
    deps[3] = {2};
    deps[4] = {1, 3};
    auto r = g::levels(deps);
    expect(fatal(r.has_value()));
    expect(eq((*r)[0], 0u));
    expect(eq((*r)[1], 1u));
    expect(eq((*r)[2], 1u));
    expect(eq((*r)[3], 2u));
    expect(eq((*r)[4], 3u));
}};

UTest disconnected_nodes_are_independent{"Levels/DisconnectedNodesAreIndependent", [] {
    g::AdjacencyList deps(3);
    deps[2] = {1};
    // node 0 is isolated and must not affect node 2's level.
    auto r = g::levels(deps);
    expect(fatal(r.has_value()));
    expect(eq((*r)[0], 0u));
    expect(eq((*r)[1], 0u));
    expect(eq((*r)[2], 1u));
}};

UTest same_cycle_error_as_topological_order{"Levels/SameCycleErrorAsTopologicalOrder", [] {
    g::AdjacencyList deps(2);
    deps[0] = {1};
    deps[1] = {0};
    auto r = g::levels(deps);
    expect(fatal(!r.has_value()));
    std::set<std::size_t> members(r.error().cycle.begin(), r.error().cycle.end());
    expect(eq(members, (std::set<std::size_t>{0, 1})));
}};

UTest direct_neighbours_are_just_adjacency{"Closure/DirectNeighboursAreJustAdjacency", [] {
    g::AdjacencyList deps(3);
    deps[0] = {1, 2};
    expect(eq(deps[0], (std::vector<std::size_t>{1, 2})));
}};

UTest excludes_roots_by_default{"Closure/ExcludesRootsByDefault", [] {
    // 0 -> 1 -> 2 -> 3 (a chain of dependencies).
    g::AdjacencyList deps(4);
    deps[0] = {1};
    deps[1] = {2};
    deps[2] = {3};
    auto c = g::closure(deps, std::array{std::size_t{0}});
    expect(eq(c, (std::vector<std::size_t>{1, 2, 3})));
}};

UTest include_roots_adds_them_back{"Closure/IncludeRootsAddsThemBack", [] {
    g::AdjacencyList deps(4);
    deps[0] = {1};
    deps[1] = {2};
    deps[2] = {3};
    auto c = g::closure(deps, std::array{std::size_t{0}}, /*include_roots=*/true);
    expect(eq(c, (std::vector<std::size_t>{0, 1, 2, 3})));
}};

UTest multiple_roots_union_and_deduplicate{"Closure/MultipleRootsUnionAndDeduplicate", [] {
    // 0 -> 2, 1 -> 2 -> 3: both roots reach 2, so it appears once.
    g::AdjacencyList deps(4);
    deps[0] = {2};
    deps[1] = {2};
    deps[2] = {3};
    auto c = g::closure(deps, std::array{std::size_t{0}, std::size_t{1}});
    expect(eq(c, (std::vector<std::size_t>{2, 3})));
}};

UTest disconnected_node_is_never_reached{"Closure/DisconnectedNodeIsNeverReached", [] {
    g::AdjacencyList deps(3);
    deps[0] = {1};
    // node 2 is isolated: no root can reach it.
    auto c = g::closure(deps, std::array{std::size_t{0}});
    expect(eq(c, (std::vector<std::size_t>{1})));
}};

UTest tolerant_of_a_cycle_reachable_from_a_root{"Closure/TolerantOfACycleReachableFromARoot", [] {
    // 0 -> 1 -> 2 -> 1 (a cycle downstream of the root). Closure does not
    // reject it; it just returns the reachable set once.
    g::AdjacencyList deps(3);
    deps[0] = {1};
    deps[1] = {2};
    deps[2] = {1};
    auto c = g::closure(deps, std::array{std::size_t{0}});
    expect(eq(c, (std::vector<std::size_t>{1, 2})));
}};

UTest root_with_no_edges_has_an_empty_closure{"Closure/RootWithNoEdgesHasAnEmptyClosure", [] {
    g::AdjacencyList deps(2);
    auto c = g::closure(deps, std::array{std::size_t{0}});
    expect(c.empty());
}};

// ── stack_topological_order: the module graph's unit order ──────────────────
//
// The order the module graph has always had, which is the order of the
// objects on a link line. The reference below is the hand-written loop it
// replaced, run on the same edges.
namespace {
std::vector<std::size_t> reference_stack_order(
        std::size_t n, const std::vector<std::pair<std::size_t, std::size_t>>& edges) {
    std::vector<std::size_t> indeg(n, 0);
    std::vector<std::vector<std::size_t>> adj(n);
    for (auto [c, p] : edges) { indeg[c]++; adj[p].push_back(c); }
    std::vector<std::size_t> order, queue;
    for (std::size_t i = 0; i < n; ++i) if (indeg[i] == 0) queue.push_back(i);
    while (!queue.empty()) {
        auto u = queue.back(); queue.pop_back();
        order.push_back(u);
        for (auto v : adj[u]) if (--indeg[v] == 0) queue.push_back(v);
    }
    return order;
}
}  // namespace

UTest is_the_module_graphs_historical_order{"StackTopologicalOrder/IsTheModuleGraphsHistoricalOrder", [] {
    const std::vector<std::pair<std::size_t, std::size_t>> edges = {
        {3, 0}, {4, 0}, {4, 1}, {5, 3}, {5, 4}, {6, 2}, {7, 6}, {7, 5}, {1, 2},
    };
    auto order = g::stack_topological_order(8, edges);
    expect(fatal(order.has_value()));
    expect(eq(*order, reference_stack_order(8, edges)));
    // It is not the stable order: ties go to the most recently readied node.
    g::AdjacencyList deps(8);
    for (auto [c, p] : edges) deps[c].push_back(p);
    expect(ne(*order, *g::topological_order(deps)));
}};

UTest a_cycle_is_reported_as_its_path{"StackTopologicalOrder/ACycleIsReportedAsItsPath", [] {
    const std::vector<std::pair<std::size_t, std::size_t>> edges = {{0, 1}, {1, 2}, {2, 0}};
    auto order = g::stack_topological_order(3, edges);
    expect(fatal(!order.has_value()));
    auto const& ring = order.error().cycle;
    expect(fatal(ge(ring.size(), 2u)));
    expect(eq(ring.front(), ring.back()));
}};

// ── depth_first_order: the host-module orders ───────────────────────────────

UTest dependencies_first_from_each_root_in_order{"DepthFirstOrder/DependenciesFirstFromEachRootInOrder", [] {
    // 0 -> {2, 1}, 1 -> {3}, 2 -> {3}: from root 0, dependency 2 is visited
    // before 1 because that is the order `deps[0]` lists them in.
    g::AdjacencyList deps(4);
    deps[0] = {2, 1};
    deps[1] = {3};
    deps[2] = {3};
    auto order = g::depth_first_order(deps, std::array{std::size_t{0}});
    expect(fatal(order.has_value()));
    expect(eq(*order, (std::vector<std::size_t>{3, 2, 1, 0})));
}};

UTest only_what_the_roots_reach_is_emitted{"DepthFirstOrder/OnlyWhatTheRootsReachIsEmitted", [] {
    g::AdjacencyList deps(4);
    deps[1] = {0};
    deps[3] = {2};
    auto order = g::depth_first_order(deps, std::array{std::size_t{1}});
    expect(fatal(order.has_value()));
    expect(eq(*order, (std::vector<std::size_t>{0, 1})));
}};

UTest a_cycle_is_an_error_naming_the_ring{"DepthFirstOrder/ACycleIsAnErrorNamingTheRing", [] {
    g::AdjacencyList deps(3);
    deps[0] = {1};
    deps[1] = {2};
    deps[2] = {1};
    auto order = g::depth_first_order(deps, std::array{std::size_t{0}});
    expect(fatal(!order.has_value()));
    expect(eq(order.error().cycle, (std::vector<std::size_t>{1, 2, 1})));
}};

UTest a_cycle_can_be_skipped{"DepthFirstOrder/ACycleCanBeSkipped", [] {
    g::AdjacencyList deps(2);
    deps[0] = {1};
    deps[1] = {0};
    auto order = g::depth_first_order(deps, std::array{std::size_t{0}, std::size_t{1}},
                                      g::Cycles::Skip);
    expect(fatal(order.has_value()));
    expect(eq(*order, (std::vector<std::size_t>{1, 0})));
}};

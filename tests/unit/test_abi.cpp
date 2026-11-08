
import std;
import mcpplibs.mtest;
import mcpp.toolchain.abi;
import mcpp.toolchain.model;
import mcpp.manifest;
import mcpp.build.prepare;

using namespace mcpplibs::mtest;

using namespace mcpp::toolchain;

namespace {

Toolchain make_tc(std::string triple, std::string stdlib, CompilerId cc) {
    Toolchain tc;
    tc.targetTriple = std::move(triple);
    tc.stdlibId     = std::move(stdlib);
    tc.compiler     = cc;
    return tc;
}

}  // namespace

// ─── abi_profile: each dimension from its single canonical source ───────────

UTest clang_libcxx_on_linux_gnu_is_glibc_at_libc_level{"AbiProfile/ClangLibcxxOnLinuxGnuIsGlibcAtLibcLevel", [] {
    // The bug: a clang+libc++ toolchain targeting *-linux-gnu is glibc at the
    // libc level; only its C++ stdlib is libc++. These are independent dims.
    auto p = abi_profile(make_tc("x86_64-unknown-linux-gnu", "libc++", CompilerId::Clang));
    expect(eq(p.libc, "glibc"));
    expect(eq(p.cxxStdlib, "libc++"));
    expect(eq(p.arch, "x86_64"));
    expect(eq(p.os, "linux"));
    expect(eq(p.cxxAbi, "itanium"));
}};

UTest gcc_glibc{"AbiProfile/GccGlibc", [] {
    auto p = abi_profile(make_tc("x86_64-linux-gnu", "libstdc++", CompilerId::GCC));
    expect(eq(p.libc, "glibc"));
    expect(eq(p.cxxStdlib, "libstdc++"));
}};

UTest musl_triple_is_musl{"AbiProfile/MuslTripleIsMusl", [] {
    auto p = abi_profile(make_tc("x86_64-linux-musl", "libstdc++", CompilerId::GCC));
    expect(eq(p.libc, "musl"));
    expect(eq(p.os, "linux"));
}};

// ─── abi_check: the regression + the genuine incompatibility ────────────────

UTest glibc_c_library_under_libcxx_is_compatible{"AbiCheck/GlibcCLibraryUnderLibcxxIsCompatible", [] {
    // Regression lock for the glfw-under-clang false positive.
    auto p = abi_profile(make_tc("x86_64-unknown-linux-gnu", "libc++", CompilerId::Clang));
    auto c = parse_abi_capability("abi:glibc", "compat.glfw");
    expect(fatal(c.has_value()));
    expect(abi_check(p, {*c}).empty());
}};

UTest glibc_dep_under_musl_mismatches_on_libc_dim{"AbiCheck/GlibcDepUnderMuslMismatchesOnLibcDim", [] {
    auto p = abi_profile(make_tc("x86_64-linux-musl", "libstdc++", CompilerId::GCC));
    auto c = parse_abi_capability("abi:glibc", "compat.glfw");
    expect(fatal(c.has_value()));
    auto mm = abi_check(p, {*c});
    expect(fatal(eq(mm.size(), 1u)));
    expect(eq(mm[0].dim, AbiDim::Libc));
    expect(eq(mm[0].need, "glibc"));
    expect(eq(mm[0].got, "musl"));
}};

UTest unspecified_dimension_is_dont_care{"AbiCheck/UnspecifiedDimensionIsDontCare", [] {
    // A cxxstdlib constraint must NOT be implied by a bare libc capability.
    auto p = abi_profile(make_tc("x86_64-unknown-linux-gnu", "libc++", CompilerId::Clang));
    expect(abi_check(p, {{AbiDim::Arch, "x86_64", "x"}}).empty());      // matches
    expect(!(abi_check(p, {{AbiDim::Arch, "aarch64", "x"}}).empty()));    // mismatches
}};

// ─── parse_abi_capability: legacy + dimensional forms ───────────────────────

UTest legacy_bare_form_is_libc{"AbiCapability/LegacyBareFormIsLibc", [] {
    auto c = parse_abi_capability("abi:glibc", "pkg");
    expect(fatal(c.has_value()));
    expect(eq(c->dim, AbiDim::Libc));
    expect(eq(c->value, "glibc"));
    expect(eq(c->source, "pkg"));
}};

UTest dimensional_form{"AbiCapability/DimensionalForm", [] {
    auto c = parse_abi_capability("abi:cxxstdlib=libc++");
    expect(fatal(c.has_value()));
    expect(eq(c->dim, AbiDim::CxxStdlib));
    expect(eq(c->value, "libc++"));
}};

UTest non_abi_capability_ignored{"AbiCapability/NonAbiCapabilityIgnored", [] {
    expect(!(parse_abi_capability("x11.display").has_value()));
    expect(!(parse_abi_capability("opengl.glx.driver").has_value()));
}};

UTest unknown_dimension_ignored{"AbiCapability/UnknownDimensionIgnored", [] {
    expect(!(parse_abi_capability("abi:nonsense=1").has_value()));
}};

// ─── `[target.<sel>.abi] exceptions` and `requires_abi` on the target axis ──
//
// Design 2026-09-12 (a UI framework on Android, iOS and Web), section 2.1
// (A1, the `abi` table's second member) and section 2.6 (A6, `requires_abi`
// moved onto the target axis). Unrelated to the `abi_profile`/`abi_check`
// coverage above -- that is the toolchain's ABI-COMPATIBILITY vocabulary
// (glibc/musl/libc++); this is the MANIFEST's `[target.<selector>.abi]`
// table, a different "abi" that happens to share the word.

namespace manifest_abi {

namespace cfgpred = mcpp::build::cfgpred;

// One matching selector's `ConditionalConfig`, found by predicate text.
// `parse_string` keeps every `[target.<sel>]` section as one entry in
// `conditionalConfigs` regardless of whether any target will ever match it
// (matching happens later, at merge_conditional_config), so a parse-level
// test asks for the entry by name instead of assuming index 0.
const mcpp::manifest::ConditionalConfig* find_cc(
        const mcpp::manifest::Manifest& m, std::string_view predicate) {
    for (auto const& cc : m.conditionalConfigs)
        if (cc.predicate == predicate) return &cc;
    return nullptr;
}

}  // namespace manifest_abi

UTest parses_and_reaches_the_conditional_config{"AbiTableExceptions/ParsesAndReachesTheConditionalConfig", [] {
    constexpr auto src = R"(
[package]
name    = "webrt"
version = "0.1.0"
[target.'cfg(os = "emscripten")'.abi]
exceptions = true
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    auto* cc = manifest_abi::find_cc(*m, "cfg(os = \"emscripten\")");
    expect(fatal(ne(cc, nullptr)));
    expect(bool(cc->abiExceptionsDeclared));
    expect(bool(cc->abiExceptions));
    // `threads` is untouched by a table that only names `exceptions` --
    // the two members are independent switches, not a package deal.
    expect(!cc->abiThreadsDeclared);
}};

UTest both_members_in_one_table_parse_independently{"AbiTableExceptions/BothMembersInOneTableParseIndependently", [] {
    constexpr auto src = R"(
[package]
name    = "webrt"
version = "0.1.0"
[target.'cfg(os = "emscripten")'.abi]
threads    = true
exceptions = false
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    auto* cc = manifest_abi::find_cc(*m, "cfg(os = \"emscripten\")");
    expect(fatal(ne(cc, nullptr)));
    expect(bool(cc->abiThreadsDeclared));
    expect(bool(cc->abiThreads));
    expect(bool(cc->abiExceptionsDeclared));
    expect(!cc->abiExceptions);
}};

UTest unknown_member_names_both_threads_and_exceptions{"AbiTableExceptions/UnknownMemberNamesBothThreadsAndExceptions", [] {
    constexpr auto src = R"(
[package]
name    = "a"
version = "0.1.0"
[target.x.abi]
frobnicate = true
)";
    auto m = mcpp::manifest::parse_string(src);
    expect(fatal(!m.has_value()));
    if (auto&& failed = expect(ne(m.error().message.find(
        "has no member 'frobnicate'; the members are: threads, exceptions"), std::string::npos)); !failed) failed << m.error().message;
}};

UTest a_non_boolean_exceptions_is_refused{"AbiTableExceptions/ANonBooleanExceptionsIsRefused", [] {
    constexpr auto src = R"(
[package]
name    = "a"
version = "0.1.0"
[target.x.abi]
exceptions = 1
)";
    auto m = mcpp::manifest::parse_string(src);
    expect(fatal(!m.has_value()));
    if (auto&& failed = expect(ne(m.error().message.find(".exceptions must be true, false, \"wasm\" or \"js\""), std::string::npos)); !failed) failed << m.error().message;
}};

// ─── `requires_abi` as a direct key of `[target.<sel>]` ─────────────────────

UTest selector_scoped_requirement_parses_into_the_conditional_config{"RequiresAbiOnTargetAxis/SelectorScopedRequirementParsesIntoTheConditionalConfig", [] {
    constexpr auto src = R"(
[package]
name    = "adapter"
version = "0.1.0"
[target.'cfg(linux)']
requires_abi = { threads = true }
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    auto* cc = manifest_abi::find_cc(*m, "cfg(linux)");
    expect(fatal(ne(cc, nullptr)));
    expect(bool(cc->requiresAbiThreads));
    expect(!cc->requiresAbiExceptions);
}};

UTest feature_form_parses_under_the_selector{"RequiresAbiOnTargetAxis/FeatureFormParsesUnderTheSelector", [] {
    constexpr auto src = R"(
[package]
name    = "adapter"
version = "0.1.0"
[target.'cfg(linux)'.feature-requires-abi]
mt = { threads = true }
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    auto* cc = manifest_abi::find_cc(*m, "cfg(linux)");
    expect(fatal(ne(cc, nullptr)));
    expect(fatal(cc->featureRequiresAbiThreads.contains("mt")));
    expect(bool(cc->featureRequiresAbiThreads.at("mt")));
    // The feature-requires-abi table registers the feature UNCONDITIONALLY
    // (the same rule feature-deps follows, #359), so it does not trip the
    // unknown-feature diagnostic on a build that never selects this target.
    expect(m->featuresMap.contains("mt"));
}};

UTest unknown_member_inside_requires_abi_names_both{"RequiresAbiOnTargetAxis/UnknownMemberInsideRequiresAbiNamesBoth", [] {
    constexpr auto src = R"(
[package]
name    = "adapter"
version = "0.1.0"
[target.'cfg(linux)']
requires_abi = { thread = true }
)";
    auto m = mcpp::manifest::parse_string(src);
    expect(fatal(!m.has_value()));
    if (auto&& failed = expect(ne(m.error().message.find(
        "requires_abi.thread: the members are `threads`, `exceptions`, booleans"), std::string::npos)); !failed) failed << m.error().message;
}};

UTest unknown_member_inside_feature_form_names_both{"RequiresAbiOnTargetAxis/UnknownMemberInsideFeatureFormNamesBoth", [] {
    constexpr auto src = R"(
[package]
name    = "adapter"
version = "0.1.0"
[target.'cfg(linux)'.feature-requires-abi]
mt = { thread = true }
)";
    auto m = mcpp::manifest::parse_string(src);
    expect(fatal(!m.has_value()));
    if (auto&& failed = expect(ne(m.error().message.find(
        "feature-requires-abi.mt].thread: the members are `threads`, "
        "`exceptions`, booleans"), std::string::npos)); !failed) failed << m.error().message;
}};

// ─── The union: a matching selector's requirement joins the package's ──────
//
// No cross toolchain needed -- merge_conditional_config only evaluates the
// predicate against a resolved TRIPLE, which `cfgpred::context_for` builds
// without touching a compiler. Same technique as
// tests/unit/test_target_xlings_axis.cpp's `merged_for`.

namespace manifest_abi {

mcpp::manifest::Manifest merged_for(std::string_view src, std::string_view triple) {
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(m.has_value()); !failed) failed << (m.has_value() ? "" : m.error().format());
    if (!m) return {};
    mcpp::build::merge_conditional_config(*m, cfgpred::context_for(triple));
    return *m;
}

}  // namespace manifest_abi

UTest matching_selector_unions_into_the_packages_requirement{"RequiresAbiOnTargetAxis/MatchingSelectorUnionsIntoThePackagesRequirement", [] {
    constexpr auto src = R"(
[package]
name    = "adapter"
version = "0.1.0"
[target.'cfg(linux)']
requires_abi = { threads = true }
)";
    auto lin = manifest_abi::merged_for(src, "x86_64-unknown-linux-gnu");
    expect(fatal(eq(lin.targetRequiresAbiThreads.size(), 1u)));
    expect(eq(lin.targetRequiresAbiThreads.front(), "cfg(linux)"));
    expect(lin.targetRequiresAbiExceptions.empty());
}};

UTest a_selector_that_does_not_match_the_host_imposes_nothing{"RequiresAbiOnTargetAxis/ASelectorThatDoesNotMatchTheHostImposesNothing", [] {
    constexpr auto src = R"(
[package]
name    = "adapter"
version = "0.1.0"
[target.'cfg(windows)']
requires_abi = { threads = true }
)";
    // A Windows-only selector must not reach a Linux-resolved build -- the
    // negative direction of the union above, and the case the design record
    // calls out explicitly (A6: "the Web build is unaffected ... in both
    // directions").
    auto lin = manifest_abi::merged_for(src, "x86_64-unknown-linux-gnu");
    expect(lin.targetRequiresAbiThreads.empty());
}};

UTest feature_form_unions_only_for_the_matching_selector{"RequiresAbiOnTargetAxis/FeatureFormUnionsOnlyForTheMatchingSelector", [] {
    constexpr auto src = R"(
[package]
name    = "adapter"
version = "0.1.0"
[target.'cfg(linux)'.feature-requires-abi]
mt = { threads = true }
[target.'cfg(windows)'.feature-requires-abi]
mt = { exceptions = true }
)";
    auto lin = manifest_abi::merged_for(src, "x86_64-unknown-linux-gnu");
    expect(fatal(lin.targetFeatureRequiresAbiThreads.contains("mt")));
    expect(eq(lin.targetFeatureRequiresAbiThreads.at("mt").front(), "cfg(linux)"));
    expect(!lin.targetFeatureRequiresAbiExceptions.contains("mt"));
}};

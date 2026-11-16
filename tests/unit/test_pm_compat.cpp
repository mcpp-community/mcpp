
import std;
import mcpplibs.mtest;
import mcpp.pm.compat;
import mcpp.pm.dependency_selector;

using namespace mcpplibs::mtest;

UTest install_dir_candidates_include_nested_namespace_fallback{"PmCompat/InstallDirCandidatesIncludeNestedNamespaceFallback", [] {
    auto candidates = mcpp::pm::compat::install_dir_candidates(
        "mcpplibs", "capi.lua", "mcpplibs");

    expect(ne(std::find(candidates.begin(), candidates.end(),
                  "mcpplibs.capi-x-mcpplibs.capi.lua"), candidates.end()));
}};

UTest normalize_nested_namespace_preserves_qualified_name{"PmCompat/NormalizeNestedNamespacePreservesQualifiedName", [] {
    std::string ns = "mcpplibs";
    std::string shortName = "capi.lua";

    mcpp::pm::compat::normalize_nested_namespace(ns, shortName,
                                                  /*legacyDottedKey=*/true);

    expect(eq(ns, "mcpplibs.capi"));
    expect(eq(shortName, "lua"));
    expect(eq(mcpp::pm::compat::qualified_name(ns, shortName), "mcpplibs.capi.lua"));
}};

UTest split_legacy_dependency_key_marks_dotted_key_as_compat{"PmCompat/SplitLegacyDependencyKeyMarksDottedKeyAsCompat", [] {
    auto key = mcpp::pm::compat::split_legacy_dependency_key(
        "mcpplibs.capi.lua");

    expect(eq(key.namespace_, "mcpplibs"));
    expect(eq(key.shortName, "capi.lua"));
    expect(bool(key.legacyDottedKey));
}};

UTest normalize_nested_namespace_skips_canonical_namespaced_deps{"PmCompat/NormalizeNestedNamespaceSkipsCanonicalNamespacedDeps", [] {
    std::string ns = "mcpplibs.capi";
    std::string shortName = "lua.extra";

    mcpp::pm::compat::normalize_nested_namespace(ns, shortName,
                                                  /*legacyDottedKey=*/false);

    expect(eq(ns, "mcpplibs.capi"));
    expect(eq(shortName, "lua.extra"));
}};

UTest dotted_selector_is_one_exact_namespace{"DependencySelector/DottedSelectorIsOneExactNamespace", [] {
    auto selector = mcpp::pm::resolve_dependency_selector(
        "imgui.backend.glfw_opengl3");

    expect(eq(selector.stableMapKey, "imgui.backend.glfw_opengl3"));
    expect(fatal(eq(selector.candidates.size(), 1u)));
    expect(eq(selector.candidates[0].namespace_, "imgui.backend"));
    expect(eq(selector.candidates[0].shortName, "glfw_opengl3"));
}};

UTest bare_selector_uses_only_default_namespace{"DependencySelector/BareSelectorUsesOnlyDefaultNamespace", [] {
    auto selector = mcpp::pm::resolve_dependency_selector("imgui");

    expect(eq(selector.stableMapKey, "imgui"));
    expect(fatal(eq(selector.candidates.size(), 1u)));
    expect(eq(selector.candidates[0].namespace_, "mcpplibs"));
    expect(eq(selector.candidates[0].shortName, "imgui"));
}};

UTest explicit_mcpplibs_prefix_does_not_add_peer_fallback{"DependencySelector/ExplicitMcpplibsPrefixDoesNotAddPeerFallback", [] {
    auto selector = mcpp::pm::resolve_dependency_selector(
        "mcpplibs.capi.lua");

    expect(eq(selector.stableMapKey, "mcpplibs.capi.lua"));
    expect(fatal(eq(selector.candidates.size(), 1u)));
    expect(eq(selector.candidates[0].namespace_, "mcpplibs.capi"));
    expect(eq(selector.candidates[0].shortName, "lua"));
}};

UTest explicit_root_selector_has_only_that_root{"DependencySelector/ExplicitRootSelectorHasOnlyThatRoot", [] {
    auto selector = mcpp::pm::make_direct_dependency_selector(
        "compat", "gtest", "compat.gtest");

    expect(eq(selector.stableMapKey, "compat.gtest"));
    expect(fatal(eq(selector.candidates.size(), 1u)));
    expect(eq(selector.candidates[0].namespace_, "compat"));
    expect(eq(selector.candidates[0].shortName, "gtest"));
}};

UTest shared_parser_normalizes_bare_and_dotted_selectors{"DependencySelector/SharedParserNormalizesBareAndDottedSelectors", [] {
    auto bare = mcpp::pm::parse_package_selector("lua");
    expect(fatal(bare.has_value()));
    expect(!(bare->namespace_.has_value()));
    expect(eq(bare->name, "lua"));
    expect(eq(bare->spelling, "lua"));

    auto bareCoord = mcpp::pm::normalize_package_selector(*bare);
    expect(eq(bareCoord.namespace_, "mcpplibs"));
    expect(eq(bareCoord.shortName, "lua"));
    expect(eq(mcpp::pm::format_package_selector(bareCoord), "lua"));

    auto nested = mcpp::pm::parse_package_selector("mcpplibs.capi.lua");
    expect(fatal(nested.has_value()));
    expect(fatal(nested->namespace_.has_value()));
    expect(eq(*nested->namespace_, "mcpplibs.capi"));
    expect(eq(nested->name, "lua"));

    auto nestedCoord = mcpp::pm::normalize_package_selector(*nested);
    expect(eq(nestedCoord.namespace_, "mcpplibs.capi"));
    expect(eq(nestedCoord.shortName, "lua"));
    expect(eq(mcpp::pm::format_package_selector(nestedCoord), "mcpplibs.capi.lua"));
}};

UTest shared_parser_rejects_ambiguous_or_unsafe_segments{"DependencySelector/SharedParserRejectsAmbiguousOrUnsafeSegments", [] {
    const std::vector<std::string> invalid{
        "",
        ".imgui",
        "ocornut..imgui",
        "imgui.",
        "acme/widget",
        "acme\\widget",
        "acme widget",
        "acme[widget",
        "acme=widget",
        "acme\"widget",
        "acme#widget",
        "acme@widget",
        "acme:widget",
        std::string("acme\x1fwidget", 11),
    };
    for (auto const& spelling : invalid) {
        auto parsed = mcpp::pm::parse_package_selector(spelling);
        expect(!parsed.has_value()) << spelling;
        if (!parsed)
            expect(!(parsed.error().message.empty())) << spelling;
    }
}};

UTest legacy_dotted_primary_is_explicit_and_never_implicitly_used{"DependencySelector/LegacyDottedPrimaryIsExplicitAndNeverImplicitlyUsed", [] {
    auto capi = mcpp::pm::parse_package_selector("capi.lua");
    expect(fatal(capi.has_value()));
    auto legacy = mcpp::pm::legacy_prefixed_coordinate(
        mcpp::pm::normalize_package_selector(*capi));
    expect(fatal(legacy.has_value()));
    expect(eq(legacy->namespace_, "mcpplibs.capi"));
    expect(eq(legacy->shortName, "lua"));

    auto explicitDefault = mcpp::pm::parse_package_selector(
        "mcpplibs.capi.lua");
    expect(fatal(explicitDefault.has_value()));
    expect(!(mcpp::pm::legacy_prefixed_coordinate(
        mcpp::pm::normalize_package_selector(*explicitDefault)).has_value()));

    auto bare = mcpp::pm::parse_package_selector("lua");
    expect(fatal(bare.has_value()));
    expect(!(mcpp::pm::legacy_prefixed_coordinate(
        mcpp::pm::normalize_package_selector(*bare)).has_value()));
}};

// ─── descriptor_coordinates (package-template fetch) ────────────────

UTest descriptor_coordinates_legacy_embedded_namespace{"PmCompat/DescriptorCoordinatesLegacyEmbeddedNamespace", [] {
    // pkgs/l/llmapi.lua: namespace = "mcpplibs", name = "mcpplibs.llmapi"
    auto r = mcpp::pm::compat::descriptor_coordinates(
        "llmapi", "mcpplibs", "mcpplibs.llmapi");

    expect(eq(r.namespace_, "mcpplibs"));
    expect(eq(r.shortName, "llmapi"));
    expect(bool(r.usedLegacySplit));
}};

UTest descriptor_coordinates_canonical_namespace_field{"PmCompat/DescriptorCoordinatesCanonicalNamespaceField", [] {
    auto r = mcpp::pm::compat::descriptor_coordinates(
        "llmapi", "mcpplibs", "llmapi");

    expect(eq(r.namespace_, "mcpplibs"));
    expect(eq(r.shortName, "llmapi"));
    expect(!r.usedLegacySplit);
}};

UTest descriptor_coordinates_root_package_stays_in_root{"PmCompat/DescriptorCoordinatesRootPackageStaysInRoot", [] {
    // pkgs/i/imgui.lua: namespace = "", name = "imgui" — must NOT be
    // promoted to the default namespace (it installs by its bare name).
    auto r = mcpp::pm::compat::descriptor_coordinates("imgui", "", "imgui");

    expect(eq(r.namespace_, ""));
    expect(eq(r.shortName, "imgui"));
    expect(!r.usedLegacySplit);
}};

UTest descriptor_coordinates_legacy_dotted_name_without_namespace{"PmCompat/DescriptorCoordinatesLegacyDottedNameWithoutNamespace", [] {
    auto r = mcpp::pm::compat::descriptor_coordinates(
        "tinyhttps", "", "mcpplibs.tinyhttps");

    expect(eq(r.namespace_, "mcpplibs"));
    expect(eq(r.shortName, "tinyhttps"));
    expect(bool(r.usedLegacySplit));
}};

UTest descriptor_coordinates_falls_back_to_spec_when_name_missing{"PmCompat/DescriptorCoordinatesFallsBackToSpecWhenNameMissing", [] {
    auto r = mcpp::pm::compat::descriptor_coordinates("imgui", "", "");

    expect(eq(r.namespace_, ""));
    expect(eq(r.shortName, "imgui"));
}};

UTest descriptor_coordinates_compat_namespace{"PmCompat/DescriptorCoordinatesCompatNamespace", [] {
    auto r = mcpp::pm::compat::descriptor_coordinates(
        "mbedtls", "compat", "compat.mbedtls");

    expect(eq(r.namespace_, "compat"));
    expect(eq(r.shortName, "mbedtls"));
}};

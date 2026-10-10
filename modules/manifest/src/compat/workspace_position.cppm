// mcpp.pm.compat.workspace_position - the workspace root's own tables, given
// to its members by POSITION.
//
// COMPAT(workspace-position): removed with the rest of mcpp.pm.compat in
// mcpp 1.0.0 (DEPRECATION SCHEDULE in compat.cppm).
//
// Before 2026.10.10.1 a workspace root's `[toolchain]`, `[indices]`,
// `[profile.<name>]`, the scalar rows of `[target.<triple>]` and its
// `[xlings]` / `[target.<sel>.xlings]` entries reached every member because
// of WHERE they were written, while `[target.<sel>.build]` beside them reached
// none (#785, #786). From 2026.10.10.1 the prefix carries the meaning: a
// table outside `workspace.` speaks about its own package, and
// `[workspace.X]` speaks to every member (SPEC-004 §9.10, W1-W3).
//
// Deprecated:                          Canonical:
//
//     [toolchain]                          [workspace.toolchain]
//     default = "llvm@23.1.3"              default = "llvm@23.1.3"
//
//     [target.x86_64-linux-musl]           [workspace.target.x86_64-linux-musl]
//     linkage = "static"                   linkage = "static"
//
// This module is the only place that knows the old reading. It answers which
// tables of a workspace root the old reading still applies to (those written
// at the root position and not also as `[workspace.X]`) and applies the old
// merge to them unchanged: a member's own `[toolchain]` replaces the root's
// whole, a `[target.<triple>]` or `[profile.<name>]` row of the same name
// replaces the root's row whole, and `[indices]` is taken only by a member
// that declares none. The caller reports each use as a warning
// (`manifest/workspace-position`) with the canonical spelling.
//
// Exported through the `mcpp.manifest` umbrella rather than the
// `mcpp.pm.compat` facade: it reads `Manifest`, and `mcpp.manifest.types`
// imports that facade.

export module mcpp.pm.compat.workspace_position;

import std;
import mcpp.manifest.types;

export namespace mcpp::pm::compat {

// The tables of workspace root `ws` the position reading applies to, in a
// fixed order: `toolchain`, `indices`, `profile`, `target`, `xlings` --
// each written at the root position and not also as `[workspace.X]`.
// `target` covers the scalar rows; its `.xlings` rows travel with `xlings`'s
// merge but are named `target` here.
std::vector<std::string> position_tables(const mcpp::manifest::Manifest& ws) {
    std::vector<std::string> out;
    if (!ws.workspace.present) return out;
    auto inLayer = [&](std::string_view t) {
        return std::ranges::find(ws.workspace.layerTables, t) != ws.workspace.layerTables.end();
    };
    auto add = [&](std::string_view t, bool written) {
        if (written && !inLayer(t)) out.emplace_back(t);
    };
    add("toolchain", !ws.toolchain.byPlatform.empty() || !ws.toolchain.bootstrap.empty());
    add("indices", !ws.indices.empty());
    add("profile", !ws.profiles.empty());
    const bool targetXlings = std::ranges::any_of(ws.conditionalConfigs, [](auto const& cc) {
        return !cc.xlings.deps.empty() || !cc.xlings.overrides.empty();
    });
    // Every `[target.<sel>]` header makes a row; one with no scalar written
    // (only `.build`, say) gives a member nothing by position.
    const bool targetScalars = std::ranges::any_of(ws.targetOverrides, [](auto const& kv) {
        auto const& e = kv.second;
        return !e.toolchain.empty() || !e.linkage.empty() || !e.cxxRuntime.empty()
            || e.sysrootDeclared || e.minApiLevel != 0 || !e.runner.empty()
            || !e.namedRunners.empty();
    });
    add("target", targetScalars || targetXlings);
    add("xlings", !ws.xlings.deps.empty() || !ws.xlings.overrides.empty());
    return out;
}

// The position reading of `toolchain`, `indices`, `profile` and `target`
// (scalar rows) for `member` as the root of a build, for the tables in
// `tables`. Returns the tables that gave the member a value.
//
// `wsRoot` anchors a relative `[indices].path`, which was written against the
// workspace root (#224).
std::vector<std::string>
inherit_root_position_by_position(mcpp::manifest::Manifest& member,
                                  const mcpp::manifest::Manifest& ws,
                                  const std::filesystem::path& wsRoot,
                                  std::span<const std::string> tables) {
    std::vector<std::string> used;
    auto wanted = [&](std::string_view t) { return std::ranges::find(tables, t) != tables.end(); };
    // COMPAT(workspace-position): [toolchain] -- whole, when the member has none.
    if (wanted("toolchain") && member.toolchain.byPlatform.empty()) {
        member.toolchain = ws.toolchain;
        for (auto const& [platform, _] : ws.toolchain.byPlatform)
            member.toolchain.fileByPlatform.try_emplace(platform, ws.sourcePath);
        used.emplace_back("toolchain");
    }
    // COMPAT(workspace-position): [target.<triple>] -- per triple, whole.
    if (wanted("target")) {
        bool any = false;
        for (auto const& [triple, entry] : ws.targetOverrides)
            if (member.targetOverrides.try_emplace(triple, entry).second) any = true;
        if (any) used.emplace_back("target");
    }
    // COMPAT(workspace-position): [profile.<name>] -- per name, whole.
    if (wanted("profile")) {
        bool any = false;
        for (auto const& [name, profile] : ws.profiles)
            if (member.profiles.try_emplace(name, profile).second) any = true;
        if (any) used.emplace_back("profile");
    }
    // COMPAT(workspace-position): [indices] -- all of them, when the member has none.
    if (wanted("indices") && member.indices.empty() && !ws.indices.empty()) {
        member.indices = ws.indices;
        for (auto& [_, idx] : member.indices)
            if (idx.is_local() && idx.path.is_relative())
                idx.path = (wsRoot / idx.path).lexically_normal();
        used.emplace_back("indices");
    }
    return used;
}

// What the warning for one table says.
std::string position_warning(std::string_view table, std::string_view member) {
    std::string_view example =
          table == "toolchain" ? "[workspace.toolchain]\n          default = \"llvm@23.1.3\""
        : table == "indices"   ? "[workspace.indices]\n          acme = { path = \"vendor/index\" }"
        : table == "profile"   ? "[workspace.profile.release]\n          opt = 3"
        : table == "target"    ? "[workspace.target.x86_64-linux-musl]\n          linkage = \"static\""
        :                        "[workspace.xlings.workspace]\n          cmake = \"4.0.2\"";
    const std::string mirror = table == "profile" ? "[workspace.profile.<name>]"
                             : table == "target"  ? "[workspace.target.<selector>]"
                             : std::format("[workspace.{}]", table);
    const std::string original = table == "profile" ? "[profile.<name>]"
                               : table == "target"  ? "[target.<selector>]"
                               : std::format("[{}]", table);
    return std::format(
        "{} on the workspace root reaches {} by its position; this reading is "
        "deprecated and is removed in mcpp 1.0.0. Write it as {} to give it to every "
        "member, for example:\n          {}",
        original, member.empty() ? std::string("every member") : std::format("member '{}'", member),
        mirror, example);
}

}  // namespace mcpp::pm::compat


import std;
import mcpplibs.mtest;
import mcpp.manifest;
import mcpp.build.prepare;

using namespace mcpplibs::mtest;

// SPEC-004 §4 — the tool plane has TWO resolution axes, and this file is the
// measurement that tells them apart.
//
//   HOST   axis: platform keys INSIDE a top-level `[xlings]` value. Resolved
//                against the machine that runs the build, at parse time.
//   TARGET axis: `[target.<selector>.xlings…]`. The selector is evaluated
//                against the RESOLVED target.
//
// They coincide on a native build, which is why the gap went unnoticed for as
// long as it did: every existing project states target facts on the host axis
// and is right by accident. A cross build is where they part, so the two legs
// below feed two different targets to one manifest. That is what makes this a
// measurement rather than a restatement — on one machine, with no cross
// toolchain, and it fails if the target axis is dropped.

namespace cfgpred = mcpp::build::cfgpred;

namespace {

constexpr const char* kSrc = R"(
[package]
name    = "axis"
version = "0.1.0"

[xlings.workspace]
"xim:hosttool" = "1.0.0"

[target.'cfg(os = "linux")'.xlings.workspace]
"xim:linuxonly" = "9.9.9"

[target.'cfg(os = "windows")'.xlings.workspace]
"xim:winonly" = "8.8.8"

[target.'cfg(os = "windows")'.feature-xlings.gate]
"xim:gatedwin" = "7.7.7"
)";

bool has(const std::vector<std::string>& v, std::string_view a) {
    return std::ranges::find(v, a) != v.end();
}

// One manifest, merged against one resolved target. Returned by value so the
// two legs cannot share state: a merge that appended to a manifest the other
// leg had already merged would report the union and pass either way.
mcpp::manifest::Manifest merged_for(std::string_view triple) {
    auto m = mcpp::manifest::parse_string(kSrc);
    expect(m.has_value());
    if (!m) return {};
    mcpp::build::merge_conditional_config(*m, cfgpred::context_for(triple));
    return *m;
}

} // namespace

UTest selector_decides_which_entries_apply{"TargetXlingsAxis/SelectorDecidesWhichEntriesApply", [] {
    auto lin = merged_for("x86_64-unknown-linux-gnu");
    auto win = merged_for("x86_64-pc-windows-gnu");

    // The host axis is the SAME on both legs. It is not conditional on the
    // target and must not become so: `xim:dpcpp` is a compiler that runs here
    // no matter what it is asked to emit.
    expect(bool(has(lin.xlings.deps, "xim:hosttool@1.0.0")));
    expect(bool(has(win.xlings.deps, "xim:hosttool@1.0.0")));

    // The target axis follows the target, and nothing else in this test does.
    expect(bool(has(lin.xlings.deps, "xim:linuxonly@9.9.9")));
    expect(!has(lin.xlings.deps, "xim:winonly@8.8.8"));
    expect(bool(has(win.xlings.deps, "xim:winonly@8.8.8")));
    expect(!has(win.xlings.deps, "xim:linuxonly@9.9.9"));

    // The pin reaches the materialised resolution layer, keyed the way the
    // host axis keys it. An entry installed without its pin resolves to
    // whatever the machine already has, which is a different package on a
    // different day.
    auto pin = win.xlings.workspace.find("winonly");
    expect(fatal(ne(pin, win.xlings.workspace.end())));
    expect(eq(pin->second, "xim:8.8.8"));
    expect(!win.xlings.workspace.contains("linuxonly"));
}};

UTest gate_and_selector_compose{"TargetXlingsAxis/GateAndSelectorCompose", [] {
    // `[target.<sel>.feature-xlings.<f>]` is a gate INSIDE a condition, the
    // same composition `[target.<sel>.feature-deps.<f>]` already had (#359).
    // The feature is registered unconditionally — whether the target matches
    // decides what the feature pulls in, not whether the feature exists —
    // otherwise asking for it on the other target trips the unknown-feature
    // diagnostic, which points at the wrong file.
    auto lin = merged_for("x86_64-unknown-linux-gnu");
    auto win = merged_for("x86_64-pc-windows-gnu");

    expect(lin.featuresMap.contains("gate"));
    expect(win.featuresMap.contains("gate"));

    auto lg = lin.xlings.featureDeps.find("gate");
    expect(lg == lin.xlings.featureDeps.end() || lg->second.empty());

    auto wg = win.xlings.featureDeps.find("gate");
    expect(fatal(ne(wg, win.xlings.featureDeps.end())));
    expect(bool(has(wg->second, "xim:gatedwin@7.7.7")));
    expect(eq(win.xlings.featurePins.at("xim:gatedwin@7.7.7"), "xim:7.7.7"));
}};

UTest condition_written_twice_is_refused{"TargetXlingsAxis/ConditionWrittenTwiceIsRefused", [] {
    // Platform keys inside a value under a selector say a second time what the
    // selector already said. Refused rather than resolved: two statements of
    // one fact can disagree, and whichever one loses does so in silence.
    auto m = mcpp::manifest::parse_string(R"(
[package]
name    = "axis"
version = "0.1.0"

[target.'cfg(os = "linux")'.xlings.workspace]
"xim:tool" = { linux = "1.0.0", macosx = "2.0.0" }
)");
    expect(fatal(!m.has_value()));
    auto msg = m.error().format();
    expect(ne(msg.find("xim:tool"), std::string::npos)) << msg;
    expect(ne(msg.find("platform keys"), std::string::npos)) << msg;
    // The message must name the OUTER selector, because that is the half the
    // author has to delete, and it is not the half they are looking at.
    expect(ne(msg.find("cfg(os = \"linux\")"), std::string::npos)) << msg;
}};

UTest subos_is_not_conditional_on_a_target{"TargetXlingsAxis/SubosIsNotConditionalOnATarget", [] {
    // A project has one environment, not one per target. Refused rather than
    // ignored: a silently dropped environment is the failure #531 was filed
    // for.
    auto m = mcpp::manifest::parse_string(R"(
[package]
name    = "axis"
version = "0.1.0"

[target.'cfg(os = "linux")'.xlings]
subos = "3"
)");
    expect(fatal(!m.has_value()));
    auto msg = m.error().format();
    expect(ne(msg.find("subos"), std::string::npos)) << msg;
}};

UTest host_axis_keeps_its_platform_keys{"TargetXlingsAxis/HostAxisKeepsItsPlatformKeys", [] {
    // V4, at unit scale: the existing spelling is untouched. The value object
    // under a TOP-LEVEL `[xlings]` entry is not legacy — it is how the host
    // axis is written, and it stays resolved against the host.
    auto m = mcpp::manifest::parse_string(R"(
[package]
name    = "axis"
version = "0.1.0"

[xlings.workspace]
"xim:tool" = { linux = "1.0.0", macosx = "2.0.0", windows = "3.0.0", default = "4.0.0" }
)");
    expect(fatal(m.has_value()));
    const auto& pin = m->xlings.workspace.at("tool");
#if defined(__linux__)
    expect(eq(pin, "xim:1.0.0"));
    expect(bool(has(m->xlings.deps, "xim:tool@1.0.0")));
#elif defined(__APPLE__)
    expect(eq(pin, "xim:2.0.0"));
    expect(bool(has(m->xlings.deps, "xim:tool@2.0.0")));
#elif defined(_WIN32)
    expect(eq(pin, "xim:3.0.0"));
    expect(bool(has(m->xlings.deps, "xim:tool@3.0.0")));
#else
    expect(eq(pin, "xim:4.0.0"));
    expect(bool(has(m->xlings.deps, "xim:tool@4.0.0")));
#endif
    // And it is NOT reported as a conditional section: the host axis resolves
    // at parse time and leaves nothing for the merge to do.
    expect(m->conditionalConfigs.empty());
}};

UTest both_axes_naming_one_package_resolve_to_one_install{"TargetXlingsAxis/BothAxesNamingOnePackageResolveToOneInstall", [] {
    // `xim:glibc` and `xim:glibc@2.40` are two addresses for ONE install.
    // Keeping both would ask xlings for the same package at two versions, and
    // that is not a build that fails — it is a build whose environment depends
    // on which entry the provisioner reached first.
    auto m = mcpp::manifest::parse_string(R"(
[package]
name    = "axis"
version = "0.1.0"

[xlings.workspace]
"xim:glibc" = "2.39"

[target.'cfg(os = "linux")'.xlings.workspace]
"xim:glibc" = "2.40"
)");
    expect(fatal(m.has_value()));
    mcpp::build::merge_conditional_config(
        *m, cfgpred::context_for("x86_64-unknown-linux-gnu"));

    auto named = std::ranges::count_if(m->xlings.deps, [](const std::string& a) {
        return mcpp::manifest::parse_address(a).target == "glibc";
    });
    expect(eq(named, 1));
    expect(bool(has(m->xlings.deps, "xim:glibc@2.40")));
    expect(eq(m->xlings.workspace.at("glibc"), "xim:2.40"));
}};

UTest a_bare_triple_selector_is_a_selector_too{"TargetXlingsAxis/ABareTripleSelectorIsASelectorToo", [] {
    // `[target.<triple>.xlings.workspace]` is the other spelling of a selector,
    // and it was reached by the same dispatcher rather than by the
    // `[target.<triple>]` handler that owns `toolchain` and `runner`. Asserted
    // because "it went to the right handler" is not visible from the manifest
    // and would fail silently: the entry would simply never appear.
    auto src = R"(
[package]
name    = "axis"
version = "0.1.0"

[target.x86_64-unknown-linux-gnu.xlings.workspace]
"xim:tool" = "1.0.0"
)";
    auto lin = mcpp::manifest::parse_string(src);
    expect(fatal(lin.has_value()));
    mcpp::build::merge_conditional_config(
        *lin, cfgpred::context_for("x86_64-unknown-linux-gnu"));
    expect(bool(has(lin->xlings.deps, "xim:tool@1.0.0")));

    auto other = mcpp::manifest::parse_string(src);
    expect(fatal(other.has_value()));
    mcpp::build::merge_conditional_config(
        *other, cfgpred::context_for("aarch64-unknown-linux-gnu"));
    expect(!has(other->xlings.deps, "xim:tool@1.0.0"));
}};

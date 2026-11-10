
import std;
import mcpplibs.mtest;
import mcpp.manifest;
import mcpp.targetside;
import mcpp.libs.toml;
import mcpp.pm.dep_spec;
import mcpplibs.os.platform;
import mcpp.toolchain.platform;

using namespace mcpplibs::mtest;
import mcpp.build.prepare;   // merge_conditional_config: a row's statement replaces

UTest pe_auto_export_defaults_on_and_accepts_an_explicit_opt_out{"Manifest/PeAutoExportDefaultsOnAndAcceptsAnExplicitOptOut", [] {
    for (auto const& declaration : {"", "windows_auto_export = true", "windows_auto_export = false"}) {
        auto m = mcpp::manifest::parse_string(std::format(
            "[package]\nname = \"dll\"\nversion = \"0.1.0\"\n"
            "[targets.dll]\nkind = \"shared\"\n{}\n", declaration));
        if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
        expect(fatal(eq(m->targets.size(), 1u)));
        expect(eq(m->targets.front().windows_auto_export(), std::string_view(declaration) != "windows_auto_export = false"));
        // An omitted key stays distinguishable from a stated `true`.
        expect(eq(m->targets.front().windowsAutoExportDeclared, *declaration != '\0'));
        expect(m->schemaWarnings.empty());
    }
    auto bad = mcpp::manifest::parse_string(
        "[package]\nname = \"dll\"\nversion = \"0.1.0\"\n"
        "[targets.dll]\nkind = \"shared\"\nwindows_auto_export = \"false\"\n");
    expect(fatal(!bad.has_value()));
    expect(ne(bad.error().format().find("windows_auto_export must be a boolean"), std::string::npos));
}};

UTest pe_auto_export_has_the_same_default_and_boolean_contract{"SynthesizeFromXpkgLua/PeAutoExportHasTheSameDefaultAndBooleanContract", [] {
    for (auto const& declaration : {"", "windows_auto_export = true,", "windows_auto_export = false,"}) {
        auto m = mcpp::manifest::synthesize_from_xpkg_lua(std::format(R"(
package = {{
    spec = "1", name = "dll",
    xpm = {{ windows = {{ ["0.1.0"] = {{ url = "u", sha256 = "h" }} }} }},
    mcpp = {{ sources = {{ "*/api.cpp" }}, targets = {{ ["dll"] = {{ kind = "shared", {} }} }} }},
}}
)", declaration), "dll", "0.1.0", mcpp::toolchain::platform::HostPlatform::current());
        if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
        expect(fatal(eq(m->targets.size(), 1u)));
        expect(eq(m->targets.front().windows_auto_export(), std::string_view(declaration) != "windows_auto_export = false,"));
        expect(eq(m->targets.front().windowsAutoExportDeclared, *declaration != '\0'));
    }
    auto bad = mcpp::manifest::synthesize_from_xpkg_lua(R"(
package = {
    spec = "1", name = "dll",
    xpm = { windows = { ["0.1.0"] = { url = "u", sha256 = "h" } } },
    mcpp = { sources = { "*/api.cpp" }, targets = { ["dll"] = { kind = "shared", windows_auto_export = "false" } } },
}
)", "dll", "0.1.0", mcpp::toolchain::platform::HostPlatform::current());
    expect(fatal(!bad.has_value()));
    expect(ne(bad.error().format().find("windows_auto_export must be a boolean"), std::string::npos));
}};

UTest cpp_fly_standard{"Manifest/CppFlyStandard", [] {
    // standard = "c++fly": latest level + all experimental gates (design
    // 2026-07-14-std-features-experimental-gate-design.md §5.1).
    auto cfg = mcpp::manifest::normalize_cpp_standard("c++fly");
    expect(fatal(cfg.has_value()));
    expect(eq(cfg->canonical, "c++fly"));
    expect(eq(cfg->level, 1000));          // above c++latest's 999
    expect(bool(cfg->experimental));
    expect(!(mcpp::manifest::normalize_cpp_standard("c++latest")->experimental));
    expect(!(mcpp::manifest::normalize_cpp_standard("c++26")->experimental));
    auto bad = mcpp::manifest::normalize_cpp_standard("c++flyy");
    expect(fatal(!bad.has_value()));
    expect(ne(bad.error().find("c++fly"), std::string::npos));  // listed in the allow-list message
}};

UTest cpp20_standard{"Manifest/Cpp20Standard", [] {
    // C++20 is the floor of the allow-list: named modules are a C++20 feature,
    // so nothing below it exists for this build model
    // (.agents/docs/2026-07-31-cpp20-standard-support-design.md).
    auto cfg = mcpp::manifest::normalize_cpp_standard("c++20");
    expect(fatal(cfg.has_value()));
    expect(eq(cfg->canonical, "c++20"));
    expect(eq(cfg->flag, "-std=c++20"));
    expect(eq(cfg->level, 20));
    expect(!cfg->gnuDialect);
    expect(!cfg->experimental);

    // Aliases, symmetric with the existing c++2b / c++2c handling.
    auto alias = mcpp::manifest::normalize_cpp_standard("C++2a");   // also case-folded
    expect(fatal(alias.has_value()));
    expect(eq(alias->canonical, "c++20"));
    expect(eq(alias->level, 20));

    auto gnu = mcpp::manifest::normalize_cpp_standard("gnu++20");
    expect(fatal(gnu.has_value()));
    expect(eq(gnu->canonical, "gnu++20"));
    expect(eq(gnu->flag, "-std=gnu++20"));
    expect(eq(gnu->level, 20));
    expect(bool(gnu->gnuDialect));
    expect(eq(mcpp::manifest::normalize_cpp_standard("gnu++2a")->canonical, "gnu++20"));

    // Below the floor stays rejected, and the message advertises the floor.
    auto bad = mcpp::manifest::normalize_cpp_standard("c++17");
    expect(fatal(!bad.has_value()));
    if (auto&& failed = expect(ne(bad.error().find("c++20"), std::string::npos)); !failed) failed << bad.error();
}};

UTest manifest_cpp_standard_level_name{"Manifest/CppStandardLevelName", [] {
    expect(eq(mcpp::manifest::cpp_standard_level_name(20), "c++20"));
    expect(eq(mcpp::manifest::cpp_standard_level_name(23), "c++23"));
    expect(eq(mcpp::manifest::cpp_standard_level_name(26), "c++26"));
}};

UTest minimal_valid{"Manifest/MinimalValid", [] {
    constexpr auto src = R"(
[package]
name = "hello"
version = "0.1.0"
[language]
standard = "c++23"
[modules]
sources = ["src/**/*.cppm"]
[targets.hello]
kind = "bin"
main = "src/main.cpp"
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(eq(m->package.name, "hello"));
    expect(eq(m->package.version, "0.1.0"));
    expect(eq(m->language.standard, "c++23"));
    expect(bool(m->language.modules));
    expect(fatal(eq(m->targets.size(), 1u)));
    expect(eq(m->targets[0].name, "hello"));
    expect(eq(m->targets[0].kind, mcpp::manifest::Target::Binary));
}};

UTest xlings_subos_preserves_declaration_presence{"Manifest/XlingsSubosPreservesDeclarationPresence", [] {
    auto absent = mcpp::manifest::parse_string(R"(
[package]
name = "absent"
version = "0.1.0"
)");
    if (auto&& failed = expect(fatal(absent.has_value())); !failed) failed << absent.error().format();
    expect(!(absent->xlings.subosDeclared));

    auto explicitDefault = mcpp::manifest::parse_string(R"(
[package]
name = "explicit"
version = "0.1.0"
[xlings]
subos = "default"
)");
    if (auto&& failed = expect(fatal(explicitDefault.has_value())); !failed) failed << explicitDefault.error().format();
    expect(bool(explicitDefault->xlings.subosDeclared));
    expect(eq(explicitDefault->xlings.subos, "default"));

    auto empty = mcpp::manifest::parse_string(R"(
[package]
name = "bad"
version = "0.1.0"
[xlings]
subos = ""
)");
    if (auto&& failed = expect(fatal(empty.has_value())); !failed) failed << empty.error().format();
    expect(bool(empty->xlings.subosDeclared));
    expect(empty->xlings.subos.empty());
}};

UTest shared_target_soname{"Manifest/SharedTargetSoname", [] {
    constexpr auto src = R"(
[package]
name = "dep"
version = "0.1.0"
[build]
sources = ["src/*.c"]
[targets.dep]
kind = "shared"
soname = "libdep.so.1"
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->targets.size(), 1u)));
    expect(eq(m->targets[0].kind, mcpp::manifest::Target::SharedLibrary));
    expect(eq(m->targets[0].soname, "libdep.so.1"));
}};

UTest rejects_soname_on_a_non_library_target{"Manifest/RejectsSonameOnANonLibraryTarget", [] {
    // NARROWED from "non-shared" to "non-library" (#519). A soname is the
    // name a library is FOUND by, and a package needs to be able to state it
    // while still being consumed as a static library — otherwise two copies
    // of one library can never resolve to a single file. An EXECUTABLE still
    // has no business declaring one.
    constexpr auto src = R"(
[package]
name = "app"
version = "0.1.0"
[targets.app]
kind = "bin"
main = "src/main.cpp"
soname = "libapp.so.1"
)";
    auto m = mcpp::manifest::parse_string(src);
    expect(fatal(!m.has_value()));
    expect(ne(m.error().message.find("soname is only valid for library targets"), std::string::npos));
}};

UTest package_standard_cpp26_accepted_and_mirrored{"Manifest/PackageStandardCpp26AcceptedAndMirrored", [] {
    constexpr auto src = R"(
[package]
name = "hello26"
version = "0.1.0"
standard = "c++26"
[modules]
sources = ["src/**/*.cppm"]
[targets.hello26]
kind = "bin"
main = "src/main.cpp"
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(eq(m->package.standard, "c++26"));
    expect(eq(m->language.standard, "c++26"));
}};

UTest legacy_language_cpp2c_normalizes_to_cpp26{"Manifest/LegacyLanguageCpp2cNormalizesToCpp26", [] {
    constexpr auto src = R"(
[package]
name = "hello26"
version = "0.1.0"
[language]
standard = "c++2c"
[modules]
sources = ["src/**/*.cppm"]
[targets.hello26]
kind = "bin"
main = "src/main.cpp"
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(eq(m->package.standard, "c++26"));
    expect(eq(m->language.standard, "c++26"));
}};

UTest rejects_std_flag_in_cxxflags{"Manifest/RejectsStdFlagInCxxflags", [] {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "x"
version = "0.1.0"
[build]
cxxflags = ["-std=c++26"]
[modules]
sources = ["src/**/*.cppm"]
[targets.x]
kind = "bin"
main = "src/main.cpp"
)");
    expect(fatal(!m.has_value()));
    if (auto&& failed = expect(ne(m.error().message.find("[package].standard"), std::string::npos)); !failed) failed << m.error().message;
}};

UTest reject_missing_version{"Manifest/RejectMissingVersion", [] {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "x"
[modules]
sources = ["src/**/*.cppm"]
[targets.x]
kind = "bin"
main = "src/main.cpp"
)");
    expect(fatal(!m.has_value()));
    expect(ne(m.error().message.find("package.version"), std::string::npos));
}};

UTest accepts_cpp20_with_import_std{"Manifest/AcceptsCpp20WithImportStd", [] {
    // Was RejectImportStdWithoutCpp23: it asserted this manifest is rejected,
    // but the only thing rejecting it was the standard allow-list (there has
    // never been an import_std-specific rule) — so the test's name described
    // coverage it did not have. C++20 is legal now, and `import std;` is
    // available there on every toolchain mcpp ships, so the same manifest must
    // parse, with the legacy [language] section still mirroring to the new home.
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "x"
version = "0.1.0"
[language]
standard = "c++20"
import_std = true
[modules]
sources = ["src/**/*.cppm"]
[targets.x]
kind = "bin"
main = "src/main.cpp"
)");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().message;
    expect(eq(m->package.standard, "c++20"));     // canonical, mirrored from [language]
    expect(eq(m->language.standard, "c++20"));
    expect(eq(m->cppStandard.level, 20));
    expect(eq(m->cppStandard.flag, "-std=c++20"));
}};

UTest rejects_standard_below_cpp20{"Manifest/RejectsStandardBelowCpp20", [] {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "x"
version = "0.1.0"
standard = "c++17"
[targets.x]
kind = "bin"
main = "src/main.cpp"
)");
    expect(fatal(!m.has_value()));
    if (auto&& failed = expect(ne(m.error().message.find("c++17"), std::string::npos)); !failed) failed << m.error().message;
}};

UTest reject_modules_false{"Manifest/RejectModulesFalse", [] {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "x"
version = "0.1.0"
[language]
standard = "c++23"
modules = false
[modules]
sources = ["src/**/*.cppm"]
[targets.x]
kind = "bin"
main = "src/main.cpp"
)");
    expect(fatal(!m.has_value()));
}};

UTest parses_dependencies{"Manifest/ParsesDependencies", [] {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "x"
version = "0.1.0"
[modules]
sources = ["src/**/*.cppm"]
[targets.x]
kind = "bin"
main = "src/main.cpp"
[dependencies]
"mcpplibs.primitives" = "0.0.1"
[dev-dependencies]
"gtest" = "1.15.2"
)");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(eq(m->dependencies.size(), 1u));
    expect(eq(m->dependencies.at("mcpplibs.primitives").version, "0.0.1"));
    expect(eq(m->devDependencies.at("gtest").version, "1.15.2"));
}};

UTest parses_dependency_visibility{"Manifest/ParsesDependencyVisibility", [] {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "x"
version = "0.1.0"
[modules]
sources = ["src/**/*.cppm"]
[targets.x]
kind = "bin"
main = "src/main.cpp"
[dependencies.compat]
imgui = { version = "1.92.8", visibility = "private" }
glfw = { version = "3.4", visibility = "interface" }
opengl = "2026.05.31"
)");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->dependencies.size(), 3u)));
    expect(eq(m->dependencies.at("compat.imgui").visibility, "private"));
    expect(eq(m->dependencies.at("compat.glfw").visibility, "interface"));
    expect(eq(m->dependencies.at("compat.opengl").visibility, "public"));
}};

// #242 — consumer-side `default-features = false`: a consumer opts out of a
// dependency's own [features].default set (Cargo parity). The flag parses into
// DependencySpec.defaultFeatures; explicitly requested `features = [...]` still
// come through. Default (omitted) stays true.
UTest parses_dependency_default_features_opt_out{"Manifest/ParsesDependencyDefaultFeaturesOptOut", [] {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "x"
version = "0.1.0"
[modules]
sources = ["src/**/*.cppm"]
[targets.x]
kind = "bin"
main = "src/main.cpp"
[dependencies.compat]
ffmpeg = { version = "6.1", default-features = false, features = ["avcodec"] }
zlib = "1.3"
)");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->dependencies.size(), 2u)));
    // Opt-out dependency: flag is false, but the explicit feature still parses.
    const auto& ff = m->dependencies.at("compat.ffmpeg");
    expect(!ff.defaultFeatures);
    expect(fatal(eq(ff.features.size(), 1u)));
    expect(eq(ff.features[0], "avcodec"));
    expect(eq(ff.version, "6.1"));
    // Bare-string dependency: defaultFeatures defaults to true.
    expect(bool(m->dependencies.at("compat.zlib").defaultFeatures));
}};

UTest rejects_invalid_dependency_visibility{"Manifest/RejectsInvalidDependencyVisibility", [] {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "x"
version = "0.1.0"
[dependencies.compat]
imgui = { version = "1.92.8", visibility = "implementation" }
)");
    expect(fatal(!m.has_value()));
    expect(ne(m.error().message.find("visibility"), std::string::npos));
}};

UTest default_template_round_trip{"Manifest/DefaultTemplateRoundTrip", [] {
    auto src = mcpp::manifest::default_template("hello");
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(eq(m->package.name, "hello"));
}};

UTest upserts_canonical_dependency_by_exact_package_id{"ManifestEditor/UpsertsCanonicalDependencyByExactPackageId", [] {
    constexpr std::string_view source = R"([package]
name = "app"
version = "0.1.0"

[dependencies.compat]
widget = "1.0.0"
)";
    auto edited = mcpp::manifest::upsert_dependency_text(source, {
        .namespace_ = "acme",
        .shortName = "widget",
        .version = "2.0.0",
        .features = {"gui", "vulkan"},
    });
    if (auto&& failed = expect(fatal(edited.has_value())); !failed) failed << edited.error();
    expect(ne(edited->find("[dependencies.acme]\nwidget = { version = \"2.0.0\", features = [\"gui\", \"vulkan\"] }"), std::string::npos));
    auto parsed = mcpp::manifest::parse_string(*edited);
    if (auto&& failed = expect(fatal(parsed.has_value())); !failed) failed << parsed.error().format();
    expect(fatal(eq(parsed->dependencies.size(), 2u)));
    expect(eq(parsed->dependencies.at("acme.widget").namespace_, "acme"));
    expect(eq(parsed->dependencies.at("acme.widget").shortName, "widget"));
    expect(eq(parsed->dependencies.at("acme.widget").version, "2.0.0"));
    expect(eq(parsed->dependencies.at("acme.widget").features, (std::vector<std::string>{"gui", "vulkan"})));
    expect(eq(parsed->dependencies.at("compat.widget").version, "1.0.0"));
}};

UTest replaces_canonical_entry_and_rejects_invalid_source{"ManifestEditor/ReplacesCanonicalEntryAndRejectsInvalidSource", [] {
    constexpr std::string_view source = R"([package]
name = "app"
version = "0.1.0"

[dependencies.acme]
widget = "1.0.0"
)";
    auto edited = mcpp::manifest::upsert_dependency_text(source, {
        .namespace_ = "acme",
        .shortName = "widget",
        .version = "3.0.0",
    });
    if (auto&& failed = expect(fatal(edited.has_value())); !failed) failed << edited.error();
    auto parsed = mcpp::manifest::parse_string(*edited);
    if (auto&& failed = expect(fatal(parsed.has_value())); !failed) failed << parsed.error().format();
    expect(eq(parsed->dependencies.at("acme.widget").version, "3.0.0"));
    expect(eq(std::ranges::count(*edited, '\n'), std::ranges::count(source, '\n')));

    auto invalid = mcpp::manifest::upsert_dependency_text(
        "[package\nname = \"broken\"", {
            .namespace_ = "acme",
            .shortName = "widget",
            .version = "1.0.0",
        });
    expect(fatal(!invalid.has_value()));
    expect(ne(invalid.error().find("invalid manifest"), std::string::npos));
}};

UTest multiple_entries_across_platforms{"ListXpkgVersions/MultipleEntriesAcrossPlatforms", [] {
    constexpr auto src = R"(
package = {
    name = "foo",
    xpm = {
        linux = {
            ["0.1.0"] = { url = "u1", sha256 = "x" },
            ["0.2.0"] = { url = "u2", sha256 = "y" },
            ["1.0.0"] = { url = "u3", sha256 = "z" },
        },
        macosx = {
            ["0.1.0"] = { url = "u1m", sha256 = "xm" },
        },
    },
}
)";
    auto linux = mcpp::manifest::list_xpkg_versions(src, mcpp::toolchain::platform::TargetPlatform::for_lint_of("linux"));
    expect(fatal(eq(linux.size(), 3u)));
    expect(eq(linux[0], "0.1.0"));
    expect(eq(linux[1], "0.2.0"));
    expect(eq(linux[2], "1.0.0"));

    auto mac = mcpp::manifest::list_xpkg_versions(src, mcpp::toolchain::platform::TargetPlatform::for_lint_of("macosx"));
    expect(fatal(eq(mac.size(), 1u)));
    expect(eq(mac[0], "0.1.0"));

    // No block of its own, and no artifact the same in every block: nothing.
    auto win = mcpp::manifest::list_xpkg_versions(src, mcpp::toolchain::platform::TargetPlatform::for_lint_of("windows"));
    expect(win.empty());
}};

// THE ARTIFACT THAT IS ONE FOR EVERY TARGET (mcpp#911 G6): a target whose block
// the descriptor lacks takes the versions whose artifact is the same in every
// block it has -- a source archive -- and not those built per system.
UTest an_artifact_the_same_in_every_block_serves_every_target{"ListXpkgVersions/AnArtifactTheSameInEveryBlockServesEveryTarget", [] {
    constexpr auto src = R"(
package = {
    name = "foo",
    xpm = {
        linux = {
            ["1.0.0"] = { url = "src.tar.gz", sha256 = "s" },
            ["1.1.0"] = { url = "linux.tar.gz", sha256 = "l" },
        },
        macosx = {
            ["1.0.0"] = { url = "src.tar.gz", sha256 = "s" },
            ["1.1.0"] = { url = "macos.tar.gz", sha256 = "m" },
        },
    },
}
)";
    auto any = mcpp::manifest::list_xpkg_versions(src, mcpp::toolchain::platform::TargetPlatform::with_keys({"toyos"}));
    expect(fatal(eq(any.size(), 1u)));
    expect(eq(any[0], "1.0.0"));
    auto linux = mcpp::manifest::list_xpkg_versions(src, mcpp::toolchain::platform::TargetPlatform::for_lint_of("linux"));
    expect(eq(linux.size(), 2u));
}};

UTest missing_xpm_returns_empty{"ListXpkgVersions/MissingXpmReturnsEmpty", [] {
    constexpr auto src = R"(package = { name = "foo" })";
    expect(mcpp::manifest::list_xpkg_versions(src, mcpp::toolchain::platform::TargetPlatform::for_lint_of("linux")).empty());
}};

// #363: `["25.0.4"] = { ref = "25.0.4.7.1" }` is a POINTER at another entry,
// not a release. Shape copied from the real jdk-corretto / jdk-temurin
// descriptors. Treating it as a version gave range resolution a candidate that
// ties with (or truncates to something other than) its own target.
UTest alias_entries_are_flagged{"ListXpkgVersions/AliasEntriesAreFlagged", [] {
    constexpr auto src = R"(
package = {
    name = "jdk",
    xpm = {
        linux = {
            ["latest"]     = { ref = "25.0.4.7.1" },
            ["25.0.4"]     = { ref = "25.0.4.7.1" },
            ["25.0.4.7.1"] = { url = "u", sha256 = "z" },
        },
    },
}
)";
    auto e = mcpp::manifest::list_xpkg_version_entries(
        src, mcpp::toolchain::platform::TargetPlatform::for_lint_of("linux"));
    expect(fatal(eq(e.size(), 3u)));
    expect(eq(e[0].version, "latest"));      expect(bool(e[0].alias));
    expect(eq(e[1].version, "25.0.4"));      expect(bool(e[1].alias));
    expect(eq(e[2].version, "25.0.4.7.1"));  expect(!(e[2].alias));
    expect(e[0].sha256.empty());
    expect(e[1].sha256.empty());
    expect(eq(e[2].sha256, "z"));

    // The keys-only view is unchanged for every existing caller.
    auto keys = mcpp::manifest::list_xpkg_versions(
        src, mcpp::toolchain::platform::TargetPlatform::for_lint_of("linux"));
    expect(fatal(eq(keys.size(), 3u)));
    expect(eq(keys[2], "25.0.4.7.1"));
}};

// openxlings/xlings#620, mcpp#524 A: a version entry's `revision` is read the
// way the reference implementation reads it -- a non-negative integer, and 0
// for anything else or for its absence -- including the per-arch shape, whose
// arch tables must not be mistaken for the revision.
UTest revision_is_read_per_entry{"ListXpkgVersions/RevisionIsReadPerEntry", [] {
    constexpr auto src = R"(
package = {
    name = "rev",
    xpm = {
        linux = {
            ["latest"] = { ref = "1.2.0" },
            ["1.0.0"]  = { url = "u", sha256 = "a" },
            ["1.1.0"]  = { url = "u", sha256 = "b", revision = 2 },
            ["1.2.0"]  = { x86_64 = { url = "u", sha256 = "c" }, revision = 1 },
            ["1.3.0"]  = { url = "u", sha256 = "d", revision = "3" },
            ["1.4.0"]  = { url = "u", sha256 = "e", revision = -1 },
        },
    },
}
)";
    auto e = mcpp::manifest::list_xpkg_version_entries(
        src, mcpp::toolchain::platform::TargetPlatform::for_lint_of("linux"));
    expect(fatal(eq(e.size(), 6u)));
    expect(eq(e[0].revision, 0));   // an alias carries none
    expect(eq(e[1].revision, 0));
    expect(eq(e[2].revision, 2));
    expect(eq(e[3].revision, 1));
    expect(eq(e[4].revision, 0));   // a string is not a revision
    expect(eq(e[5].revision, 0));   // nor is a negative number
}};

// The scanner used to walk the platform table character by character, so a
// bracket key nested inside a version's own body (mirror tables write
// `["GLOBAL"] = "https://..."`) counted as a published version.
UTest nested_bracket_keys_are_not_versions{"ListXpkgVersions/NestedBracketKeysAreNotVersions", [] {
    constexpr auto src = R"(
package = {
    name = "foo",
    xpm = {
        linux = {
            ["1.0.0"] = {
                url = { ["GLOBAL"] = "u-global", ["CN"] = "u-cn" },
                sha256 = "z",
            },
            ["1.1.0"] = { url = "u", sha256 = "z" },
        },
    },
}
)";
    auto keys = mcpp::manifest::list_xpkg_versions(
        src, mcpp::toolchain::platform::TargetPlatform::for_lint_of("linux"));
    expect(fatal(eq(keys.size(), 2u)));
    expect(eq(keys[0], "1.0.0"));
    expect(eq(keys[1], "1.1.0"));
}};

UTest build_cflags_cxxflags_and_c_standard{"Manifest/BuildCflagsCxxflagsAndCStandard", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[build]
sources    = ["src/**/*.{cppm,c}"]
cflags     = ["-Wall", "-DFOO=1"]
cxxflags   = ["-Wextra"]
ldflags    = ["-lfoo", "-Wl,--as-needed"]
c_standard = "c11"
[targets.x]
kind = "lib"
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->buildConfig.cflags.size(), 2u)));
    expect(eq(m->buildConfig.cflags[0], "-Wall"));
    expect(eq(m->buildConfig.cflags[1], "-DFOO=1"));
    expect(fatal(eq(m->buildConfig.cxxflags.size(), 1u)));
    expect(eq(m->buildConfig.cxxflags[0], "-Wextra"));
    expect(fatal(eq(m->buildConfig.ldflags.size(), 2u)));
    expect(eq(m->buildConfig.ldflags[0], "-lfoo"));
    expect(eq(m->buildConfig.ldflags[1], "-Wl,--as-needed"));
    expect(eq(m->buildConfig.cStandard, "c11"));
}};

// #296: [build].defines is sugar for `-D<x>` on both C and C++ channels; it
// must parse into buildConfig.defines so prepare_build can fold it into flags
// before the P1689 scan and fingerprint. Order is preserved because the fold
// appends in declaration order and "last flag wins" is the override semantics.
UTest build_defines_parses_into_separate_vector{"Manifest/BuildDefinesParsesIntoSeparateVector", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[build]
defines = ["TEST_USE_MODULES", "VALUE=42"]
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->buildConfig.defines.size(), 2u)));
    expect(eq(m->buildConfig.defines[0], "TEST_USE_MODULES"));
    expect(eq(m->buildConfig.defines[1], "VALUE=42"));
    // A well-formed [build] must not warn — the unknown-key guard below is
    // only allowed to fire on keys the parser genuinely does not read.
    expect(m->schemaWarnings.empty())
        << (m->schemaWarnings.empty() ? "" : m->schemaWarnings[0]);
}};

// ── #540: the three vocabularies, and the drift that keeps happening ────────
//
// `kKnownBuildKeys`, `kKnownConditionalBuildKeys` and the xpkg `target_cfg`
// list are hand-written transcriptions of sets that exist elsewhere in
// machine-readable form — the `doc->get_*()` reads immediately above them, and
// `BuildInputs`'s member list. All three had drifted, in different directions,
// and `kKnownBuildKeys` had drifted TWICE: the comment beside it narrates the
// `bmi_schedule` occurrence in detail while three keys eight lines above it
// were in the same state.
//
// The user-visible cost is not a missing warning. It is a warning that is
// FALSE — "[build] has unsupported key 'std-module' (ignored)" for a key read
// a few hundred lines above and taking full effect — which is worse than
// silence, because the only sentence mcpp offers about the key says the
// opposite of what happens.
//
// THE LIST BELOW IS ANOTHER COPY, AND THAT IS THE POINT. A copy that fails
// loudly when it disagrees is the whole difference from the arrangement that
// let this happen twice. Add a key to the parser, add it here; if you forget,
// the negative control at the bottom of each test still passes and this one
// goes red.
UTest every_build_key_the_parser_reads_is_accepted{"Manifest/EveryBuildKeyTheParserReadsIsAccepted", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[build]
allow_host_libs       = false
bmi_schedule          = "auto"
build_program_timeout = 120
c_standard            = "c17"
cache                 = "local"
cflags                = ["-Wall"]
cxxflags              = ["-Wall"]
cxx_runtime           = "self-contained"
default-profile       = "release"
defines               = ["A=1"]
dependency_linkage    = "static"
dialect_cxxflags      = ["-fno-rtti"]
include_dirs          = ["include"]
include_dirs_after    = ["compat"]
private_include_dirs  = ["internal"]
jobs                  = 4
ldflags               = ["-Wl,--as-needed"]
macos_deployment_target = "14.0"
module_extensions     = [".ixx"]
flags                 = []
profile               = "release"
target                = "x86_64-linux-gnu"
sources               = ["src/**/*.cpp"]
static_stdlib         = false
std-module            = "gen/std.cppm"
std-compat-module     = "gen/std.compat.cppm"
std-module-flags      = ["-D_GNU_SOURCE"]
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    std::string all;
    for (auto const& w : m->schemaWarnings) { all += w; all += '\n'; }
    expect(m->schemaWarnings.empty())
        << "a key the parser reads was reported as unsupported:\n" << all;
    // The three that were absent, asserted individually so a regression names
    // itself rather than arriving as "some warning appeared".
    expect(eq(m->stdModule, "gen/std.cppm"));
    expect(eq(m->stdCompatModule, "gen/std.compat.cppm"));
    expect(fatal(eq(m->buildConfig.stdModuleFlags.size(), 1u)));
    expect(eq(m->buildConfig.stdModuleFlags[0], "-D_GNU_SOURCE"));
}};

// The negative control. Without it the test above passes against a parser whose
// unknown-key check has been deleted outright, which is the failure mode a
// "no warnings" assertion invites.
UTest an_unknown_build_key_is_still_reported{"Manifest/AnUnknownBuildKeyIsStillReported", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[build]
std_module = "gen/std.cppm"
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->schemaWarnings.size(), 1u)));
    // The underscore spelling is the plausible typo for the hyphenated key, so
    // it is the one worth holding: accepting it silently is how `schedule` got
    // in while `bmi_schedule` was being reported as unsupported.
    expect(ne(m->schemaWarnings[0].find("std_module"), std::string::npos))
        << m->schemaWarnings[0];
    // The message must carry the SAME list the check uses — it used to be a
    // third hand-written copy and had drifted from both others.
    expect(ne(m->schemaWarnings[0].find("std-module"), std::string::npos))
        << "the message must name the supported keys it actually checks against";
}};

// `kKnownConditionalBuildKeys`' own comment says its vocabulary "is exactly
// that struct's members" and "MUST stay in sync with the reads above and with
// types.cppm's BuildInputs". It was two members short of that claim, and one of
// the two — `private_include_dirs` — is ACCEPTED by the xpkg descriptor's
// `target_cfg` block, the other grammar for this same axis. So one spelling of
// a conditional private include directory worked and the other reported the
// key as unsupported.
UTest every_conditional_build_key_the_axis_carries_is_accepted{"Manifest/EveryConditionalBuildKeyTheAxisCarriesIsAccepted", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[target.'cfg(linux)'.build]
cflags               = ["-Wall"]
cxxflags             = ["-Wall"]
defines              = ["A=1"]
include_dirs         = ["include"]
include_dirs_after   = ["compat"]
ldflags              = ["-Wl,--as-needed"]
private_include_dirs = ["internal"]
sources              = ["src/extra.cpp"]
std-module-flags     = ["-D_GNU_SOURCE"]
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    std::string all;
    for (auto const& w : m->schemaWarnings) { all += w; all += '\n'; }
    expect(m->schemaWarnings.empty())
        << "a BuildInputs member the axis carries was reported unsupported:\n" << all;
    expect(fatal(eq(m->conditionalConfigs.size(), 1u)));
    auto const& in = m->conditionalConfigs[0].inputs;
    // #494 moved `std-module-flags` onto BuildInputs specifically so this axis
    // could carry it — "membership here is what makes the cfg axis carry it",
    // with -D_GNU_SOURCE right for musl and glibc and wrong for picolibc as the
    // stated case. The member and the merge landed; the READ did not.
    expect(fatal(eq(in.stdModuleFlags.size(), 1u)));
    expect(eq(in.stdModuleFlags[0], "-D_GNU_SOURCE"));
    expect(fatal(eq(in.privateIncludeDirs.size(), 1u)));
    expect(eq(in.privateIncludeDirs[0].generic_string(), "internal"));
}};

UTest an_unknown_conditional_build_key_is_still_reported{"Manifest/AnUnknownConditionalBuildKeyIsStillReported", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[target.'cfg(linux)'.build]
static_stdlib = true
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->schemaWarnings.size(), 1u)));
    if (auto&& failed = expect(ne(m->schemaWarnings[0].find("static_stdlib"), std::string::npos)); !failed) failed << m->schemaWarnings[0];
    // Built from the list, not written beside it: the prose form named eight
    // keys and was two behind the check it was describing.
    expect(ne(m->schemaWarnings[0].find("std-module-flags"), std::string::npos))
        << "the message must be generated from the same list the check uses";
}};

// #717: `dialect_cxxflags` under `[target.<selector>.build]` used to be
// reported as "unsupported key 'dialect_cxxflags' (ignored)" and reached no
// command (measured on 2026.9.27.1). It is now accepted, without a warning,
// and parsed into `ConditionalConfig::dialectCxxflags` -- a member of its own,
// NOT of `BuildInputs` (`.inputs`), because the key is graph-wide rather than
// a per-package additive input (design 2026-09-27 §6.2).
UTest conditional_dialect_cxxflags_accepted_and_kept_off_build_inputs{"Manifest/ConditionalDialectCxxflagsAcceptedAndKeptOffBuildInputs", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[target.'cfg(linux)'.build]
cxxflags         = ["-DORDINARY"]
dialect_cxxflags = ["-DDIALECT_A", "-DDIALECT_B"]
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    if (auto&& failed = expect(m->schemaWarnings.empty()); !failed) failed << (m->schemaWarnings.empty() ? "" : m->schemaWarnings[0]);
    expect(fatal(eq(m->conditionalConfigs.size(), 1u)));
    auto const& cc = m->conditionalConfigs[0];
    expect(fatal(eq(cc.dialectCxxflags.size(), 2u)));
    expect(eq(cc.dialectCxxflags[0], "-DDIALECT_A"));
    expect(eq(cc.dialectCxxflags[1], "-DDIALECT_B"));
    // An ordinary per-package build input written in the SAME section still
    // lands on BuildInputs, as always -- only `dialect_cxxflags` is diverted.
    expect(fatal(eq(cc.inputs.cxxflags.size(), 1u)));
    expect(eq(cc.inputs.cxxflags[0], "-DORDINARY"));
}};

// The negative control for the positive test above: a section carrying ONLY
// `dialect_cxxflags` must still be recorded, the same emptiness-gate rule
// `ConditionalSectionWithOnlyDefinesIsRecorded` states for `defines`.
UTest conditional_section_with_only_dialect_cxxflags_is_recorded{"Manifest/ConditionalSectionWithOnlyDialectCxxflagsIsRecorded", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[target.linux.build]
dialect_cxxflags = ["-DONLY_DIALECT"]
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(m->schemaWarnings.empty());
    expect(fatal(eq(m->conditionalConfigs.size(), 1u)));
    expect(fatal(eq(m->conditionalConfigs[0].dialectCxxflags.size(), 1u)));
    expect(eq(m->conditionalConfigs[0].dialectCxxflags[0], "-DONLY_DIALECT"));
}};

// The resolution rules: only a matching row contributes, a non-matching one
// contributes nothing, and the order is the root's own `[build]` first, then
// each matching `[target.<selector>.build]` in manifest order -- the same
// technique `tests/unit/test_abi.cpp`'s `RequiresAbiOnTargetAxis` tests use
// (`merge_conditional_config` against a resolved triple, no toolchain needed).
namespace manifest_dialect {

mcpp::manifest::Manifest merged_for(std::string_view src, std::string_view triple) {
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(m.has_value()); !failed) failed << (m.has_value() ? "" : m.error().format());
    if (!m) return {};
    mcpp::build::merge_conditional_config(*m, mcpp::build::cfgpred::context_for(triple));
    return *m;
}

}  // namespace manifest_dialect

UTest root_build_precedes_the_matching_row{"ConditionalDialectCxxflags/RootBuildPrecedesTheMatchingRow", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[build]
dialect_cxxflags = ["-DBASE"]
[target.linux.build]
dialect_cxxflags = ["-DLINUX_ONE"]
[target.windows.build]
dialect_cxxflags = ["-DWINDOWS_ONLY"]
)";
    auto lin = manifest_dialect::merged_for(src, "x86_64-unknown-linux-gnu");
    expect(fatal(eq(lin.buildConfig.dialectCxxflags.size(), 2u)));
    expect(eq(lin.buildConfig.dialectCxxflags[0], "-DBASE"));
    expect(eq(lin.buildConfig.dialectCxxflags[1], "-DLINUX_ONE"));
}};

// Two DIFFERENT selectors that both match the same resolved triple both
// contribute -- this is not "last matching wins" (that rule belongs to a
// conditional REPLACING a scalar or a dependency identity, §3.1.1); every
// matching row is APPENDED, as `cxxflags` already is. The relative order
// between two conditional rows is not asserted here: `m.conditionalConfigs`
// is populated from the underlying `Table` (`std::map`, ordered by selector
// text), not by source position, which is a property of the parser this
// feature inherits rather than one it introduces or could change.
UTest two_matching_selectors_both_contribute{"ConditionalDialectCxxflags/TwoMatchingSelectorsBothContribute", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[build]
dialect_cxxflags = ["-DBASE"]
[target.linux.build]
dialect_cxxflags = ["-DLINUX_ONE"]
[target.'cfg(arch = "x86_64")'.build]
dialect_cxxflags = ["-DX86"]
)";
    auto lin = manifest_dialect::merged_for(src, "x86_64-unknown-linux-gnu");
    expect(fatal(eq(lin.buildConfig.dialectCxxflags.size(), 3u)));
    if (auto&& failed = expect(eq(lin.buildConfig.dialectCxxflags[0], "-DBASE")); !failed) failed << "the root's own unconditional [build] value always leads: it is "
           "set before the conditional-merge loop runs, regardless of which "
           "matching row the loop visits first";
    auto const& flags = lin.buildConfig.dialectCxxflags;
    expect(ne(std::find(flags.begin(), flags.end(), "-DLINUX_ONE"), flags.end()));
    expect(ne(std::find(flags.begin(), flags.end(), "-DX86"), flags.end()));
}};

UTest a_selector_that_does_not_match_contributes_nothing{"ConditionalDialectCxxflags/ASelectorThatDoesNotMatchContributesNothing", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[build]
dialect_cxxflags = ["-DBASE"]
[target.windows.build]
dialect_cxxflags = ["-DWINDOWS_ONLY"]
)";
    auto lin = manifest_dialect::merged_for(src, "x86_64-unknown-linux-gnu");
    expect(fatal(eq(lin.buildConfig.dialectCxxflags.size(), 1u)));
    expect(eq(lin.buildConfig.dialectCxxflags[0], "-DBASE"));
}};

// #540: `[features]` was the one structured section with no schema check at
// all, so a misplaced `include_dirs` inside a feature built successfully with
// zero diagnostics — while the identical misplacement in `[build]` or
// `[target.<pred>.build]` is reported.
UTest every_feature_key_the_parser_reads_is_accepted{"Manifest/EveryFeatureKeyTheParserReadsIsAccepted", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[features]
full = { implies = ["fast"], forward = ["dep/feat"], defines = ["WITH_FULL"], sources = ["src/full.cpp"], requires = ["cap"], provides = ["cap2"], flags = [] }
fast = []
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    std::string all;
    for (auto const& w : m->schemaWarnings) { all += w; all += '\n'; }
    expect(m->schemaWarnings.empty())
        << "a feature key the parser reads was reported as unsupported:\n" << all;
}};

UTest an_unknown_feature_key_is_reported{"Manifest/AnUnknownFeatureKeyIsReported", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[features]
fast = { implies = [], include_dirs = ["nope"] }
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->schemaWarnings.size(), 1u)));
    if (auto&& failed = expect(ne(m->schemaWarnings[0].find("include_dirs"), std::string::npos)); !failed) failed << m->schemaWarnings[0];
    expect(ne(m->schemaWarnings[0].find("fast"), std::string::npos))
        << "the message must name WHICH feature carries the key";
}};

// `deps` is reserved rather than wrong — the parser's own comment has promised
// it since Feature System v2 — so it is reported as reserved. Saying
// "unsupported" would deny a documented plan; saying nothing is what let it
// look implemented.
UTest a_reserved_feature_key_says_it_is_reserved{"Manifest/AReservedFeatureKeySaysItIsReserved", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[features]
fast = { implies = [], deps = ["zlib"] }
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->schemaWarnings.size(), 1u)));
    if (auto&& failed = expect(ne(m->schemaWarnings[0].find("reserved"), std::string::npos)); !failed) failed << m->schemaWarnings[0];
}};

// #296: `defines` is a BuildInputs member, NOT a BuildConfig-only field — so
// the cfg axis carries it like any other build input. This is the regression
// that matters: a platform-only macro must be expressible, because a key that
// parses unconditionally but vanishes under `[target.'cfg(...)'.build]` is the
// exact silent-drop failure #296 was filed for, just one section over.
UTest conditional_build_defines_are_carried_by_the_cfg_axis{"Manifest/ConditionalBuildDefinesAreCarriedByTheCfgAxis", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[build]
defines = ["BASE"]
[target.'cfg(windows)'.build]
defines = ["USE_WIN32", "WINVER=0x0A00"]
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->conditionalConfigs.size(), 1u)));
    expect(eq(m->conditionalConfigs[0].predicate, "cfg(windows)"));
    expect(fatal(eq(m->conditionalConfigs[0].inputs.defines.size(), 2u)));
    expect(eq(m->conditionalConfigs[0].inputs.defines[0], "USE_WIN32"));
    expect(eq(m->conditionalConfigs[0].inputs.defines[1], "WINVER=0x0A00"));
    if (auto&& failed = expect(m->schemaWarnings.empty()); !failed) failed << (m->schemaWarnings.empty() ? "" : m->schemaWarnings[0]);
}};

// A conditional section carrying ONLY `defines` must still be recorded — the
// emptiness gate that decides whether to push a ConditionalConfig has to know
// about every BuildInputs member, or the section is dropped before it is ever
// evaluated.
UTest conditional_section_with_only_defines_is_recorded{"Manifest/ConditionalSectionWithOnlyDefinesIsRecorded", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[target.'cfg(linux)'.build]
defines = ["ONLY_DEFINE"]
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->conditionalConfigs.size(), 1u)));
    expect(fatal(eq(m->conditionalConfigs[0].inputs.defines.size(), 1u)));
    expect(eq(m->conditionalConfigs[0].inputs.defines[0], "ONLY_DEFINE"));
}};

// The single additive merge must carry `defines` too — merge_conditional_
// build_inputs routes the cfg axis through append(), so a member missing here
// is a member that silently never reaches the build.
UTest append_build_inputs_merges_defines{"Manifest/AppendBuildInputsMergesDefines", [] {
    mcpp::manifest::BuildInputs dst, src;
    dst.defines = {"BASE"};
    src.defines = {"COND"};
    mcpp::manifest::append(dst, src);
    expect(fatal(eq(dst.defines.size(), 2u)));
    expect(eq(dst.defines[0], "BASE"));
    // Conditional entries land AFTER the base ones so GNU last-wins gives the
    // conditional rule precedence (an off-OS `-U` after the base `-D`).
    expect(eq(dst.defines[1], "COND"));
}};

// #296 root cause: an unknown [build] key used to vanish without a word, and
// the build then failed much later with a module-graph divergence naming
// neither the key nor the manifest. Same policy as [targets.<name>]: a schema
// warning (an error under --strict).
UTest unknown_build_key_is_reported_not_silently_dropped{"Manifest/UnknownBuildKeyIsReportedNotSilentlyDropped", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[build]
cxxflags = ["-Wall"]
defnes   = ["TYPO"]
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->schemaWarnings.size(), 1u)));
    if (auto&& failed = expect(ne(m->schemaWarnings[0].find("defnes"), std::string::npos)); !failed) failed << m->schemaWarnings[0];
    if (auto&& failed = expect(ne(m->schemaWarnings[0].find("[build]"), std::string::npos)); !failed) failed << m->schemaWarnings[0];
    // The key is still ignored — this is a warning, not a parse failure.
    expect(eq(m->buildConfig.cxxflags.size(), 1u));
}};

// The conditional axis may only contribute build INPUTS, so a selection knob
// under it is inexpressible by construction. Say so rather than dropping it.
UTest unknown_conditional_build_key_is_reported{"Manifest/UnknownConditionalBuildKeyIsReported", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[target.'cfg(windows)'.build]
cxxflags      = ["-DOK"]
static_stdlib = false
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->schemaWarnings.size(), 1u)));
    if (auto&& failed = expect(ne(m->schemaWarnings[0].find("static_stdlib"), std::string::npos)); !failed) failed << m->schemaWarnings[0];
    if (auto&& failed = expect(ne(m->schemaWarnings[0].find("cfg(windows)"), std::string::npos)); !failed) failed << m->schemaWarnings[0];
}};

// Every key the [build] parser actually reads must be absent from the
// unknown-key warning — a guard that fires on a supported key is worse than
// no guard, because --strict turns it into a build failure.
UTest every_supported_build_key_passes_the_unknown_key_guard{"Manifest/EverySupportedBuildKeyPassesTheUnknownKeyGuard", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[build]
sources                 = ["src/**/*.cpp"]
cflags                  = ["-O2"]
cxxflags                = ["-Wall"]
ldflags                 = ["-lm"]
defines                 = ["A"]
flags                   = [{ glob = "src/*.c", defines = ["B"] }]
include_dirs            = ["include"]
include_dirs_after      = ["vendor"]
dialect_cxxflags        = ["-fcontracts"]
c_standard              = "c11"
target                  = "x86_64-linux-musl"
static_stdlib           = true
allow_host_libs         = false
profile                 = "dev"
macos_deployment_target = "14.0"
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    if (auto&& failed = expect(m->schemaWarnings.empty()); !failed) failed << (m->schemaWarnings.empty() ? "" : m->schemaWarnings[0]);
}};

// `default-profile` is the canonical spelling of the `profile` alias; both
// must clear the guard.
UTest build_default_profile_spelling_passes_the_unknown_key_guard{"Manifest/BuildDefaultProfileSpellingPassesTheUnknownKeyGuard", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[build]
default-profile = "dev"
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(eq(m->buildConfig.defaultProfile, "dev"));
    if (auto&& failed = expect(m->schemaWarnings.empty()); !failed) failed << (m->schemaWarnings.empty() ? "" : m->schemaWarnings[0]);
}};

// #249: `[build] include_dirs_after` parses into buildConfig.includeDirsAfter
// — the -idirafter channel (searched AFTER the toolchain's system dirs), so
// an extracted-tarball root containing a file named like a standard header
// (ffmpeg's VERSION vs libc++'s <version> on case-insensitive macOS) can't
// shadow it while its real headers stay findable.
UTest build_include_dirs_after_toml{"Manifest/BuildIncludeDirsAfterToml", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[build]
sources           = ["src/**/*.cpp"]
include_dirs      = ["include"]
include_dirs_after = ["*", "vendor/include"]
[targets.x]
kind = "lib"
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->buildConfig.includeDirs.size(), 1u)));
    expect(eq(m->buildConfig.includeDirs[0], "include"));
    expect(fatal(eq(m->buildConfig.includeDirsAfter.size(), 2u)));
    expect(eq(m->buildConfig.includeDirsAfter[0], "*"));
    expect(eq(m->buildConfig.includeDirsAfter[1], "vendor/include"));
}};

// Feature System v2 Stage 1: a [features] entry may be a TABLE carrying
// package-owned `defines` (and `implies`), while the array shorthand keeps
// meaning "implied features". See
// .agents/docs/2026-06-29-feature-capability-model-design.md.
UTest feature_table_form_defines_and_implies{"Manifest/FeatureTableFormDefinesAndImplies", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[targets.x]
kind = "lib"
[features]
default  = ["base"]
base     = []
accel    = { defines = ["APP_ACCEL=1", "APP_FAST"], implies = ["base"] }
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();

    // Array shorthand still registers an implied-feature list.
    expect(fatal(m->featuresMap.contains("default")));
    expect(fatal(eq(m->featuresMap["default"].size(), 1u)));
    expect(eq(m->featuresMap["default"][0], "base"));

    // Table form: `implies` flows into featuresMap, `defines` into featureDefines.
    expect(fatal(m->featuresMap.contains("accel")));
    expect(fatal(eq(m->featuresMap["accel"].size(), 1u)));
    expect(eq(m->featuresMap["accel"][0], "base"));

    expect(fatal(m->buildConfig.featureDefines.contains("accel")));
    expect(fatal(eq(m->buildConfig.featureDefines["accel"].size(), 2u)));
    expect(eq(m->buildConfig.featureDefines["accel"][0], "APP_ACCEL=1"));
    expect(eq(m->buildConfig.featureDefines["accel"][1], "APP_FAST"));

    // A feature with no defines contributes no featureDefines entry.
    expect(!(m->buildConfig.featureDefines.contains("base")));
}};

// Feature System v2 Stage 3: capabilities. A feature may `requires`/`provides`
// abstract capabilities; the package may `provides` them at package level; and
// the root [capabilities] table pins providers.
UTest capabilities_provides_requires_and_pins{"Manifest/CapabilitiesProvidesRequiresAndPins", [] {
    constexpr auto src = R"(
[package]
name     = "x"
version  = "0.1.0"
provides = ["blas"]
[targets.x]
kind = "lib"
[features]
default     = []
use_blas    = { requires = ["blas"] }
provide_lap = { provides = ["lapack"] }
[capabilities]
blas = "openblas"
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();

    expect(fatal(eq(m->provides.size(), 1u)));
    expect(eq(m->provides[0], "blas"));

    expect(fatal(m->featureRequires.contains("use_blas")));
    expect(fatal(eq(m->featureRequires["use_blas"].size(), 1u)));
    expect(eq(m->featureRequires["use_blas"][0], "blas"));

    expect(fatal(m->featureProvides.contains("provide_lap")));
    expect(eq(m->featureProvides["provide_lap"][0], "lapack"));

    expect(fatal(m->capabilityPins.contains("blas")));
    expect(eq(m->capabilityPins["blas"], "openblas"));
}};

// The Lua descriptor surface (index packages) parses package-level `provides`
// and feature-scoped `requires`/`provides`/`defines`.
UTest capabilities_and_feature_defines{"SynthesizeFromXpkgLua/CapabilitiesAndFeatureDefines", [] {
    constexpr auto lua = R"(
package = {
    spec = "1",
    name = "compat.eigen",
    xpm  = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        sources  = { "*/anchor.c" },
        provides = { "blas" },
        targets  = { ["eigen"] = { kind = "lib" } },
        features = {
            ["use_blas"]   = { defines = { "EIGEN_USE_BLAS" }, requires = { "blas" } },
            ["eigen_blas"] = { sources = { "*/blas/*.cpp" },   provides = { "blas" } },
        },
    },
}
)";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(lua, "compat.eigen", "1.0.0", mcpp::toolchain::platform::HostPlatform::current());
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();

    expect(fatal(eq(m->provides.size(), 1u)));
    expect(eq(m->provides[0], "blas"));

    expect(fatal(m->buildConfig.featureDefines.contains("use_blas")));
    expect(eq(m->buildConfig.featureDefines["use_blas"][0], "EIGEN_USE_BLAS"));
    expect(fatal(m->featureRequires.contains("use_blas")));
    expect(eq(m->featureRequires["use_blas"][0], "blas"));
    expect(fatal(m->featureProvides.contains("eigen_blas")));
    expect(eq(m->featureProvides["eigen_blas"][0], "blas"));
    // sources still gated as before.
    expect(fatal(m->buildConfig.featureSources.contains("eigen_blas")));
}};

// #253: a [features] table-form entry may carry `flags` — per-feature per-glob
// compile flags, the same entry grammar as [build].flags. Parsed into
// buildConfig.featureFlags keyed by feature; base globFlags stay untouched at
// parse time (activation folds them in later, in prepare_build).
UTest feature_flags_toml_table_form{"Manifest/FeatureFlagsTomlTableForm", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[targets.x]
kind = "lib"
[build]
flags = [{ glob = "src/base/**", defines = ["BASE"] }]
[features]
default = []
simd    = { sources = ["src/simd/**"], flags = [
              { glob = "src/simd/**/*.avx2.cpp", cxxflags = ["-mavx2"], defines = ["HAVE_AVX2"] },
              { glob = "src/simd/**/*.neon.cpp", cxxflags = ["-mfpu=neon"] } ] }
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();

    expect(fatal(m->buildConfig.featureFlags.contains("simd")));
    auto& ff = m->buildConfig.featureFlags["simd"];
    expect(fatal(eq(ff.size(), 2u)));   // declaration order preserved
    expect(eq(ff[0].glob, "src/simd/**/*.avx2.cpp"));
    expect(fatal(eq(ff[0].cxxflags.size(), 1u)));
    expect(eq(ff[0].cxxflags[0], "-mavx2"));
    expect(fatal(eq(ff[0].defines.size(), 1u)));
    expect(eq(ff[0].defines[0], "HAVE_AVX2"));
    expect(eq(ff[1].glob, "src/simd/**/*.neon.cpp"));
    // Parse-time base globFlags carry ONLY the [build].flags entry.
    expect(fatal(eq(m->buildConfig.globFlags.size(), 1u)));
    expect(eq(m->buildConfig.globFlags[0].glob, "src/base/**"));
    // Gated sources still land in featureSources alongside.
    expect(fatal(m->buildConfig.featureSources.contains("simd")));
}};

// #253: feature `flags` entries share [build].flags' closed grammar — a
// missing `glob` and an unknown entry key are hard errors naming the
// feature-scoped anchoring key.
UTest feature_flags_toml_errors{"Manifest/FeatureFlagsTomlErrors", [] {
    constexpr auto missingGlob = R"(
[package]
name = "x"
version = "0.1.0"
[targets.x]
kind = "lib"
[features]
simd = { flags = [{ cxxflags = ["-mavx2"] }] }
)";
    auto m1 = mcpp::manifest::parse_string(missingGlob);
    expect(fatal(!m1.has_value()));
    if (auto&& failed = expect(ne(m1.error().message.find("[features].simd.flags"), std::string::npos)); !failed) failed << m1.error().message;
    expect(ne(m1.error().message.find("glob"), std::string::npos));

    constexpr auto badKey = R"(
[package]
name = "x"
version = "0.1.0"
[targets.x]
kind = "lib"
[features]
simd = { flags = [{ glob = "src/**", ldflags = ["-lfoo"] }] }
)";
    auto m2 = mcpp::manifest::parse_string(badKey);
    expect(fatal(!m2.has_value()));
    if (auto&& failed = expect(ne(m2.error().message.find("ldflags"), std::string::npos)); !failed) failed << m2.error().message;
}};

// #253 Lua descriptor surface: `features.<name>.flags` parses into
// featureFlags (same entry grammar as the build-level `flags` key) and is a
// KNOWN subfield — it must not land in xpkgUnknownKeys.
UTest feature_flags_parse{"SynthesizeFromXpkgLua/FeatureFlagsParse", [] {
    constexpr auto lua = R"(
package = {
    spec = "1",
    name = "compat.opencv",
    xpm  = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        sources = { "*/core/*.cpp" },
        targets = { ["opencv"] = { kind = "lib" } },
        flags   = { { glob = "*/core/*.cpp", defines = { "BASE" } } },
        features = {
            ["dnn"] = {
                sources = { "*/3rdparty/mlas/lib/*.cpp" },
                flags = {
                    { glob = "**/3rdparty/mlas/**",
                      defines = { "BUILD_MLAS_NO_ONNXRUNTIME=1", "MLAS_GEMM_ONLY=1" } },
                    { glob = "**/modules/dnn/**/*.avx2.cpp",
                      cxxflags = { "-mavx2" } },
                },
            },
        },
    },
}
)";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(lua, "compat.opencv", "1.0.0", mcpp::toolchain::platform::HostPlatform::current());
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(m->xpkgUnknownKeys.empty());

    expect(fatal(m->buildConfig.featureFlags.contains("dnn")));
    auto& ff = m->buildConfig.featureFlags["dnn"];
    expect(fatal(eq(ff.size(), 2u)));
    expect(eq(ff[0].glob, "**/3rdparty/mlas/**"));
    expect(fatal(eq(ff[0].defines.size(), 2u)));
    expect(eq(ff[0].defines[0], "BUILD_MLAS_NO_ONNXRUNTIME=1"));
    expect(eq(ff[1].glob, "**/modules/dnn/**/*.avx2.cpp"));
    // Base per-glob flags untouched at parse time.
    expect(fatal(eq(m->buildConfig.globFlags.size(), 1u)));
    expect(eq(m->buildConfig.globFlags[0].glob, "*/core/*.cpp"));

    // Unknown entry subkey is a hard error carrying the feature-scoped label.
    constexpr auto bad = R"(
package = {
    spec = "1",
    name = "compat.opencv",
    xpm  = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        sources  = { "*/core/*.cpp" },
        features = { ["dnn"] = { flags = { { glob = "x", ldflags = { "-lfoo" } } } } },
    },
}
)";
    auto mb = mcpp::manifest::synthesize_from_xpkg_lua(bad, "compat.opencv", "1.0.0", mcpp::toolchain::platform::HostPlatform::current());
    expect(fatal(!mb.has_value()));
    if (auto&& failed = expect(ne(mb.error().message.find("features.dnn.flags"), std::string::npos)); !failed) failed << mb.error().message;
}};

// #253: per-OS descriptor sections participate in `features` via the same
// textual-splice additive overlay every mcpp-segment key gets — same-named
// features merge per-subkey by APPEND (base body first, OS body second), and a
// per-OS section may register an OS-only feature. Locked here because the
// common/delta pattern (neutral common payload + per-OS delta) depends on it.
UTest per_os_features_additive_merge{"SynthesizeFromXpkgLua/PerOsFeaturesAdditiveMerge", [] {
    constexpr auto lua = R"(
package = {
    spec = "1",
    name = "compat.opencv",
    xpm  = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        sources = { "*/core/*.cpp" },
        features = {
            ["dnn"] = {
                defines = { "HAVE_OPENCV_DNN" },
                sources = { "*/modules/dnn/src/*.cpp" },
            },
        },
        linux = {
            features = {
                ["dnn"] = {
                    sources = { "*/3rdparty/mlas/lib/x86_64/*.S" },
                    flags   = { { glob = "**/3rdparty/mlas/**", defines = { "MLAS_X86" } } },
                },
                ["vaapi"] = { defines = { "HAVE_VAAPI" } },
            },
        },
        macosx = {
            features = {
                ["dnn"] = { sources = { "*/modules/dnn/neon/*.cpp" } },
            },
        },
    },
}
)";
    // linux leg: neutral payload + linux delta, macosx section invisible.
    auto ml = mcpp::manifest::synthesize_from_xpkg_lua(
        lua, "compat.opencv", "1.0.0", mcpp::toolchain::platform::TargetPlatform::for_lint_of("linux"));
    if (auto&& failed = expect(fatal(ml.has_value())); !failed) failed << ml.error().format();
    expect(fatal(ml->buildConfig.featureSources.contains("dnn")));
    {
        auto& srcs = ml->buildConfig.featureSources["dnn"];
        expect(fatal(eq(srcs.size(), 2u)));   // append order: neutral first, OS delta after
        expect(eq(srcs[0], "*/modules/dnn/src/*.cpp"));
        expect(eq(srcs[1], "*/3rdparty/mlas/lib/x86_64/*.S"));
    }
    expect(fatal(ml->buildConfig.featureFlags.contains("dnn")));
    expect(eq(ml->buildConfig.featureFlags["dnn"][0].glob, "**/3rdparty/mlas/**"));
    // OS-only feature registered (requestable) on this leg only.
    expect(ml->featuresMap.contains("vaapi"));
    expect(fatal(ml->buildConfig.featureDefines.contains("vaapi")));
    expect(eq(ml->buildConfig.featureDefines["vaapi"][0], "HAVE_VAAPI"));

    // macosx leg: NEON delta instead, no mlas flags, no vaapi.
    auto mm = mcpp::manifest::synthesize_from_xpkg_lua(
        lua, "compat.opencv", "1.0.0", mcpp::toolchain::platform::TargetPlatform::for_lint_of("macosx"));
    if (auto&& failed = expect(fatal(mm.has_value())); !failed) failed << mm.error().format();
    {
        auto& srcs = mm->buildConfig.featureSources["dnn"];
        expect(fatal(eq(srcs.size(), 2u)));
        expect(eq(srcs[0], "*/modules/dnn/src/*.cpp"));
        expect(eq(srcs[1], "*/modules/dnn/neon/*.cpp"));
    }
    expect(!(mm->buildConfig.featureFlags.contains("dnn")));
    expect(!(mm->featuresMap.contains("vaapi")));
}};

// Feature System v2 Stage 2a: optional deps activated by a feature.
// TOML surface uses a dedicated [feature-deps.<name>] section.
// The fixture used to say `zlib = "1.3.x"`, which cannot resolve: a trailing
// `.x` is not a selector this resolver has. It went unnoticed because the same
// form was in the documented `[feature-deps]` example, and because a fixture
// only has to PARSE — nothing here ever asked the index for that package. It
// reached a published package (`std-freestanding` 0.3.0) before it was measured.
UTest feature_deps_toml_section{"Manifest/FeatureDepsTomlSection", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[targets.x]
kind = "lib"
[features]
default = []
backend = []
[feature-deps.backend]
zlib = "1.3"
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(m->featureDeps.contains("backend")));
    expect(eq(m->featureDeps["backend"].size(), 1u));
    // The optional dep is NOT in the always-on dependency set.
    expect(!(m->dependencies.contains("zlib")));
}};

// Lua descriptor surface: a feature carries `deps` (nested table) + `implies`.
UTest feature_deps_and_implies{"SynthesizeFromXpkgLua/FeatureDepsAndImplies", [] {
    constexpr auto lua = R"(
package = {
    spec = "1",
    name = "compat.eigen",
    xpm  = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        sources = { "*/anchor.c" },
        targets = { ["eigen"] = { kind = "lib" } },
        features = {
            ["use_blas"]         = { defines = { "EIGEN_USE_BLAS" }, requires = { "blas" } },
            ["backend-openblas"] = { implies = { "use_blas" }, deps = { ["compat.openblas"] = "0.3.x" } },
        },
    },
}
)";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(lua, "compat.eigen", "1.0.0", mcpp::toolchain::platform::HostPlatform::current());
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    // implies recorded in featuresMap
    expect(fatal(m->featuresMap.contains("backend-openblas")));
    expect(fatal(eq(m->featuresMap["backend-openblas"].size(), 1u)));
    expect(eq(m->featuresMap["backend-openblas"][0], "use_blas"));
    // deps recorded in featureDeps, NOT in the always-on dependency set
    expect(fatal(m->featureDeps.contains("backend-openblas")));
    expect(eq(m->featureDeps["backend-openblas"].size(), 1u));
    expect(m->dependencies.empty());
}};

// Feature System v2 #243: dep/feat forwarding (Cargo parity). A `[features]`
// implied-feature token containing '/' means "when this feature is active,
// request <depFeature> from dependency <depKey>". It is split out of the
// implies list into featureForwards; plain names stay implies.
UTest feature_forward_toml_array_and_implies{"Manifest/FeatureForwardTomlArrayAndImplies", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[targets.x]
kind = "lib"
[features]
default = []
dnn = ["dnn-local", "compat.opencv/dnn"]
[dependencies]
compat.opencv = { version = "0.0.3", default-features = false }
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();

    // The plain name stays an implied feature.
    expect(fatal(m->featuresMap.contains("dnn")));
    expect(fatal(eq(m->featuresMap["dnn"].size(), 1u)));
    expect(eq(m->featuresMap["dnn"][0], "dnn-local"));

    // The `dep/feat` token becomes a forward (depKey verbatim = stableMapKey).
    expect(fatal(m->featureForwards.contains("dnn")));
    expect(fatal(eq(m->featureForwards["dnn"].size(), 1u)));
    expect(eq(m->featureForwards["dnn"][0].first, "compat.opencv"));
    expect(eq(m->featureForwards["dnn"][0].second, "dnn"));
}};

// The table form accepts forwards both mixed into `implies` and via the
// dedicated self-documenting `forward` key; both feed featureForwards.
UTest feature_forward_table_forms{"Manifest/FeatureForwardTableForms", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[targets.x]
kind = "lib"
[features]
default = []
a = { implies = ["base", "dep.one/x"] }
b = { forward = ["dep.two/y", "dep.two/z"] }
base = []
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();

    // `implies` keeps the plain name, forwards the slashed token.
    expect(fatal(eq(m->featuresMap["a"].size(), 1u)));
    expect(eq(m->featuresMap["a"][0], "base"));
    expect(fatal(m->featureForwards.contains("a")));
    expect(fatal(eq(m->featureForwards["a"].size(), 1u)));
    expect(eq(m->featureForwards["a"][0].first, "dep.one"));
    expect(eq(m->featureForwards["a"][0].second, "x"));

    // Dedicated `forward` key: all tokens are forwards; feature has no implies.
    expect(m->featuresMap["b"].empty());
    expect(fatal(m->featureForwards.contains("b")));
    expect(fatal(eq(m->featureForwards["b"].size(), 2u)));
    expect(eq(m->featureForwards["b"][0].first, "dep.two"));
    expect(eq(m->featureForwards["b"][0].second, "y"));
    expect(eq(m->featureForwards["b"][1].second, "z"));
}};

// The Lua descriptor surface splits `dep/feat` tokens the same way (one shared
// split point, two grammars).
UTest feature_forward_parse{"SynthesizeFromXpkgLua/FeatureForwardParse", [] {
    constexpr auto lua = R"(
package = {
    spec = "1",
    name = "opencv",
    xpm  = { linux = { ["0.0.3"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        sources = { "*/anchor.c" },
        targets = { ["opencv"] = { kind = "lib" } },
        features = {
            ["dnn"] = { sources = { "*/dnn.cppm" }, implies = { "compat.opencv/dnn" } },
        },
    },
}
)";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(lua, "opencv", "0.0.3", mcpp::toolchain::platform::HostPlatform::current());
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    // The forward token left featuresMap for featureForwards.
    expect(m->featuresMap["dnn"].empty());
    expect(fatal(m->featureForwards.contains("dnn")));
    expect(fatal(eq(m->featureForwards["dnn"].size(), 1u)));
    expect(eq(m->featureForwards["dnn"][0].first, "compat.opencv"));
    expect(eq(m->featureForwards["dnn"][0].second, "dnn"));
    // sources still gated normally (untouched by the split).
    expect(fatal(m->buildConfig.featureSources.contains("dnn")));
}};

// mcpp#237: an unknown mcpp-segment key is collected (so the build path can
// surface it) and the did-you-mean helper maps well-known confusables +
// edit-distance typos back to the closed key vocabulary.
UTest collected_and_did_you_mean{"XpkgUnknownKeys/CollectedAndDidYouMean", [] {
    // `dependencies` is the canonical wrong spelling of `deps` (issue #237).
    expect(eq(mcpp::manifest::closest_known_xpkg_key("dependencies"), "deps"));
    expect(eq(mcpp::manifest::closest_known_xpkg_key("dependency"), "deps"));
    // single-typo of a real key resolves via edit distance.
    expect(eq(mcpp::manifest::closest_known_xpkg_key("cxxflag"), "cxxflags"));
    expect(eq(mcpp::manifest::closest_known_xpkg_key("feature"), "features"));
    // a genuinely unrelated key has no suggestion.
    expect(eq(mcpp::manifest::closest_known_xpkg_key("wibble"), ""));

    // The build-path synthesizer records the unknown key rather than dropping
    // it silently.
    constexpr auto lua = R"(
package = {
    name = "x",
    xpm  = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        sources = { "*/a.c" },
        targets = { ["x"] = { kind = "lib" } },
        dependencies = { ["zlib"] = "1.3.x" },
    },
}
)";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(lua, "x", "1.0.0", mcpp::toolchain::platform::HostPlatform::current());
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->xpkgUnknownKeys.size(), 1u)));
    expect(eq(m->xpkgUnknownKeys[0], "dependencies"));
}};

// A feature's UNKNOWN sub-key (e.g. an unsupported `include_dirs`) is recorded
// as `features.<name>.<sub>` instead of being silently swallowed — the
// adoption-site warning (warn_unknown_xpkg_keys) then names it. Known sub-keys
// keep parsing around it.
UTest feature_sub_key_recorded{"XpkgUnknownKeys/FeatureSubKeyRecorded", [] {
    constexpr auto lua = R"(
package = {
    name = "x",
    xpm  = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        sources = { "*/a.c" },
        targets = { ["x"] = { kind = "lib" } },
        features = {
            ["opt"] = {
                include_dirs = { "inc" },
                defines = { "OPT_ON" },
            },
        },
    },
}
)";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(lua, "x", "1.0.0", mcpp::toolchain::platform::HostPlatform::current());
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->xpkgUnknownKeys.size(), 1u)));
    expect(eq(m->xpkgUnknownKeys[0], "features.opt.include_dirs"));
    // the known sub-key after the unknown one still parsed.
    expect(fatal(m->buildConfig.featureDefines.contains("opt")));
    expect(eq(m->buildConfig.featureDefines.at("opt").size(), 1u));
}};

UTest build_macos_deployment_target{"Manifest/BuildMacosDeploymentTarget", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[build]
macos_deployment_target = "11.0"
[targets.x]
kind = "lib"
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(eq(m->buildConfig.macosDeploymentTarget, "11.0"));
}};

UTest build_macos_deployment_target_defaults_empty{"Manifest/BuildMacosDeploymentTargetDefaultsEmpty", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[targets.x]
kind = "lib"
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(m->buildConfig.macosDeploymentTarget.empty());
}};

UTest runtime_config{"Manifest/RuntimeConfig", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[runtime]
library_dirs = ["runtime/lib", "plugins"]
dlopen_libs = ["libGLX.so.0", "libGL.so.1"]
capabilities = ["x11.display", "opengl.glx.driver"]
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->runtimeConfig.libraryDirs.size(), 2u)));
    expect(eq(m->runtimeConfig.libraryDirs[0], "runtime/lib"));
    expect(eq(m->runtimeConfig.libraryDirs[1], "plugins"));
    expect(fatal(eq(m->runtimeConfig.dlopenLibs.size(), 2u)));
    expect(eq(m->runtimeConfig.dlopenLibs[0], "libGLX.so.0"));
    expect(eq(m->runtimeConfig.dlopenLibs[1], "libGL.so.1"));
    expect(fatal(eq(m->runtimeConfig.capabilities.size(), 2u)));
    expect(eq(m->runtimeConfig.capabilities[0], "x11.display"));
    expect(eq(m->runtimeConfig.capabilities[1], "opengl.glx.driver"));
}};

UTest cflags_cxxflags_ldflags_and_c_standard{"SynthesizeFromXpkgLua/CflagsCxxflagsLdflagsAndCStandard", [] {
    constexpr auto src = R"(
package = {
    spec = "1",
    name = "tinyc",
    xpm  = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        sources    = { "*/src/*.c" },
        cflags     = { "-Wall", "-Dunused" },
        cxxflags   = { "-Wextra" },
        ldflags    = { "-ltinyc" },
        c_standard = "c11",
        targets    = { ["tinyc"] = { kind = "lib" } },
    },
}
)";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(src, "tinyc", "1.0.0", mcpp::toolchain::platform::HostPlatform::current());
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->buildConfig.cflags.size(), 2u)));
    expect(eq(m->buildConfig.cflags[0], "-Wall"));
    expect(eq(m->buildConfig.cflags[1], "-Dunused"));
    expect(fatal(eq(m->buildConfig.cxxflags.size(), 1u)));
    expect(eq(m->buildConfig.cxxflags[0], "-Wextra"));
    expect(fatal(eq(m->buildConfig.ldflags.size(), 1u)));
    expect(eq(m->buildConfig.ldflags[0], "-ltinyc"));
    expect(eq(m->buildConfig.cStandard, "c11"));
    expect(fatal(eq(m->modules.sources.size(), 1u)));
    expect(eq(m->modules.sources[0], "*/src/*.c"));
}};

UTest synthesize_from_xpkg_lua_shared_target_soname{"SynthesizeFromXpkgLua/SharedTargetSoname", [] {
    constexpr auto src = R"(
package = {
    spec = "1",
    name = "tinyshared",
    xpm  = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        sources = { "*/src/*.c" },
        targets = { ["tinyshared"] = { kind = "shared", soname = "libtinyshared.so.1" } },
    },
}
)";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(src, "tinyshared", "1.0.0", mcpp::toolchain::platform::HostPlatform::current());
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->targets.size(), 1u)));
    expect(eq(m->targets[0].kind, mcpp::manifest::Target::SharedLibrary));
    expect(eq(m->targets[0].soname, "libtinyshared.so.1"));
}};

UTest feature_gated_sources{"SynthesizeFromXpkgLua/FeatureGatedSources", [] {
    // gtest-style: gtest_main.cc listed in base `sources` (old-mcpp compat) AND
    // under the `main` feature → featureSources records it; the feature is
    // registered in featuresMap. prepare_build later gates it (off by default).
    constexpr auto src = R"(
package = {
    spec = "1",
    name = "gtestlike",
    xpm  = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        sources  = { "*/src/all.cc", "*/src/main.cc" },
        targets  = { ["gtestlike"] = { kind = "lib" } },
        features = {
            ["main"] = { sources = { "*/src/main.cc" } },
        },
    },
}
)";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(src, "gtestlike", "1.0.0", mcpp::toolchain::platform::HostPlatform::current());
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    // base sources keep both (old mcpp ignores `features` → no regression)
    expect(fatal(eq(m->buildConfig.sources.size(), 2u)));
    // the `main` feature is registered + carries its gated source
    expect(fatal(m->featuresMap.contains("main")));
    expect(fatal(m->buildConfig.featureSources.contains("main")));
    expect(fatal(eq(m->buildConfig.featureSources.at("main").size(), 1u)));
    expect(eq(m->buildConfig.featureSources.at("main")[0], "*/src/main.cc"));
}};

UTest synthesize_from_xpkg_lua_runtime_config{"SynthesizeFromXpkgLua/RuntimeConfig", [] {
    constexpr auto src = R"(
package = {
    spec = "1",
    name = "glfw",
    xpm  = { linux = { ["3.4"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        sources = { "*/src/context.c" },
        runtime = {
            library_dirs = { "mcpp_generated/runtime/lib" },
            dlopen_libs = { "libGLX.so.0", "libGL.so.1" },
            capabilities = { "x11.display", "opengl.glx.driver" },
        },
        targets = { ["glfw"] = { kind = "lib" } },
    },
}
)";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(src, "glfw", "3.4", mcpp::toolchain::platform::HostPlatform::current());
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->runtimeConfig.libraryDirs.size(), 1u)));
    expect(eq(m->runtimeConfig.libraryDirs[0], "mcpp_generated/runtime/lib"));
    expect(fatal(eq(m->runtimeConfig.dlopenLibs.size(), 2u)));
    expect(eq(m->runtimeConfig.dlopenLibs[0], "libGLX.so.0"));
    expect(eq(m->runtimeConfig.dlopenLibs[1], "libGL.so.1"));
    expect(fatal(eq(m->runtimeConfig.capabilities.size(), 2u)));
    expect(eq(m->runtimeConfig.capabilities[0], "x11.display"));
    expect(eq(m->runtimeConfig.capabilities[1], "opengl.glx.driver"));
}};

UTest applies_current_platform_mcpp_overlay{"SynthesizeFromXpkgLua/AppliesCurrentPlatformMcppOverlay", [] {
    constexpr auto src = R"(
package = {
    spec = "1",
    name = "tinyc",
    xpm  = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        sources      = { "*/src/common.c" },
        include_dirs = { "*/include" },
        cflags       = { "-DCOMMON=1" },
        deps         = { ["compat.base"] = "1.0.0" },
        targets      = { ["tinyc"] = { kind = "lib" } },
        linux = {
            sources      = { "*/src/linux.c" },
            include_dirs = { "*/src/linux" },
            cflags       = { "-DLINUX=1" },
            deps         = { ["compat.x11"] = "1.8.13" },
        },
        macosx = {
            sources      = { "*/src/cocoa.m" },
            include_dirs = { "*/src/macos" },
            cflags       = { "-DMACOS=1" },
            deps         = { ["compat.cocoa"] = "1.0.0" },
        },
        windows = {
            sources      = { "*/src/win32.c" },
            include_dirs = { "*/src/win32" },
            cflags       = { "-DWINDOWS=1" },
            deps         = { ["compat.win32"] = "1.0.0" },
        },
    },
}
)";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(src, "tinyc", "1.0.0", mcpp::toolchain::platform::HostPlatform::current());
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();

    std::string expectedSource = "*/src/linux.c";
    std::string expectedInclude = "*/src/linux";
    std::string expectedCflag = "-DLINUX=1";
    std::string expectedDep = "compat.x11";
    if constexpr (mcpplibs::os::is_macos) {
        expectedSource = "*/src/cocoa.m";
        expectedInclude = "*/src/macos";
        expectedCflag = "-DMACOS=1";
        expectedDep = "compat.cocoa";
    } else if constexpr (mcpplibs::os::is_windows) {
        expectedSource = "*/src/win32.c";
        expectedInclude = "*/src/win32";
        expectedCflag = "-DWINDOWS=1";
        expectedDep = "compat.win32";
    }

    expect(fatal(eq(m->modules.sources.size(), 2u)));
    expect(eq(m->modules.sources[0], "*/src/common.c"));
    expect(eq(m->modules.sources[1], expectedSource));
    expect(fatal(eq(m->buildConfig.includeDirs.size(), 2u)));
    expect(eq(m->buildConfig.includeDirs[0], "*/include"));
    expect(eq(m->buildConfig.includeDirs[1], expectedInclude));
    expect(fatal(eq(m->buildConfig.cflags.size(), 2u)));
    expect(eq(m->buildConfig.cflags[0], "-DCOMMON=1"));
    expect(eq(m->buildConfig.cflags[1], expectedCflag));
    expect(eq(m->dependencies.count("compat.base"), 1u));
    expect(eq(m->dependencies.count(expectedDep), 1u));

    // osOverride seam (`mcpp xpkg parse --all-os`): a foreign OS section is
    // spliced on request, independent of the running host — the build path
    // (empty override) above stays host-selected. Reuses this fixture.
    auto win = mcpp::manifest::synthesize_from_xpkg_lua(src, "tinyc", "1.0.0", mcpp::toolchain::platform::TargetPlatform::for_lint_of("windows"));
    if (auto&& failed = expect(fatal(win.has_value())); !failed) failed << win.error().format();
    expect(fatal(eq(win->modules.sources.size(), 2u)));
    expect(eq(win->modules.sources[1], "*/src/win32.c"));
    expect(fatal(eq(win->buildConfig.cflags.size(), 2u)));
    expect(eq(win->buildConfig.cflags[1], "-DWINDOWS=1"));
    expect(eq(win->dependencies.count("compat.win32"), 1u));
}};

UTest synthesize_from_xpkg_lua_generated_files{"SynthesizeFromXpkgLua/GeneratedFiles", [] {
    constexpr auto src = R"(
package = {
    spec = "1",
    name = "tinyc",
    xpm  = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        sources = { "*/src/*.c" },
        generated_files = {
            ["mcpp_generated/include/config.h"] = "#pragma once\n#define TINYC_OK 1\n",
        },
        include_dirs = { "mcpp_generated/include" },
        targets = { ["tinyc"] = { kind = "lib" } },
    },
}
)";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(src, "tinyc", "1.0.0", mcpp::toolchain::platform::HostPlatform::current());
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->buildConfig.generatedFiles.size(), 1u)));
    auto it = m->buildConfig.generatedFiles.find("mcpp_generated/include/config.h");
    expect(fatal(ne(it, m->buildConfig.generatedFiles.end())));
    expect(eq(it->second, "#pragma once\n#define TINYC_OK 1\n"));
}};

// #249: the xpkg descriptor body accepts `include_dirs_after` alongside
// `include_dirs` and keeps the two channels separate (after-dirs are never
// upgraded to -I).
UTest synthesize_from_xpkg_lua_include_dirs_after{"SynthesizeFromXpkgLua/IncludeDirsAfter", [] {
    constexpr auto src = R"(
package = {
    spec = "1",
    name = "tinyc",
    xpm  = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        sources            = { "*/src/*.c" },
        include_dirs       = { "*/include" },
        include_dirs_after = { "*" },
        targets = { ["tinyc"] = { kind = "lib" } },
    },
}
)";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(src, "tinyc", "1.0.0", mcpp::toolchain::platform::HostPlatform::current());
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->buildConfig.includeDirs.size(), 1u)));
    expect(eq(m->buildConfig.includeDirs[0], "*/include"));
    expect(fatal(eq(m->buildConfig.includeDirsAfter.size(), 1u)));
    expect(eq(m->buildConfig.includeDirsAfter[0], "*"));
    expect(m->xpkgUnknownKeys.empty());
}};

UTest dependencies_flat_default_namespace{"Manifest/DependenciesFlatDefaultNamespace", [] {
    constexpr auto src = R"(
[package]
name    = "x"
version = "0.1.0"
[dependencies]
gtest = "1.15.2"
foo   = { path = "../foo" }
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->dependencies.size(), 2u)));
    auto& g = m->dependencies.at("gtest");
    expect(eq(g.namespace_, "mcpplibs"));
    expect(eq(g.shortName, "gtest"));
    expect(eq(g.version, "1.15.2"));
    auto& f = m->dependencies.at("foo");
    expect(eq(f.namespace_, "mcpplibs"));
    expect(eq(f.shortName, "foo"));
    expect(eq(f.path, "../foo"));
}};

UTest dependencies_namespaced_subtable{"Manifest/DependenciesNamespacedSubtable", [] {
    constexpr auto src = R"(
[package]
name    = "x"
version = "0.1.0"

[dependencies.mcpplibs]
cmdline   = "0.0.2"
templates = { version = "0.0.1" }

[dependencies]
gtest = "1.15.2"
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->dependencies.size(), 3u)));

    auto& cmdline = m->dependencies.at("mcpplibs.cmdline");
    expect(eq(cmdline.namespace_, "mcpplibs"));
    expect(eq(cmdline.shortName, "cmdline"));
    expect(eq(cmdline.version, "0.0.2"));
    expect(!cmdline.legacyDottedKey);

    auto& tmpl = m->dependencies.at("mcpplibs.templates");
    expect(eq(tmpl.namespace_, "mcpplibs"));
    expect(eq(tmpl.shortName, "templates"));
    expect(eq(tmpl.version, "0.0.1"));

    auto& gtest = m->dependencies.at("gtest");
    expect(eq(gtest.namespace_, "mcpplibs"));
    expect(eq(gtest.shortName, "gtest"));
    expect(eq(gtest.version, "1.15.2"));
}};

UTest dependencies_legacy_dotted_key_still_parsed{"Manifest/DependenciesLegacyDottedKeyStillParsed", [] {
    // Pre-namespace-aware mcpp.toml: quoted dotted key.
    constexpr auto src = R"(
[package]
name    = "x"
version = "0.1.0"

[dependencies]
"mcpplibs.cmdline" = "0.0.2"
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->dependencies.size(), 1u)));
    auto& s = m->dependencies.at("mcpplibs.cmdline");
    expect(eq(s.namespace_, "mcpplibs"));
    expect(eq(s.shortName, "cmdline"));
    expect(eq(s.version, "0.0.2"));
    expect(bool(s.legacyDottedKey));
}};

UTest dependencies_dotted_selector_is_one_exact_identity{"Manifest/DependenciesDottedSelectorIsOneExactIdentity", [] {
    constexpr auto src = R"(
[package]
name    = "x"
version = "0.1.0"

[dependencies]
capi.lua = "0.0.3"
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->dependencies.size(), 1u)));

    auto& s = m->dependencies.at("capi.lua");
    expect(eq(s.namespace_, "capi"));
    expect(eq(s.shortName, "lua"));
    expect(eq(s.version, "0.0.3"));
    expect(!s.legacyDottedKey);
    expect(bool(s.legacyCandidateSearch));
    expect(fatal(eq(s.candidates.size(), 1u)));
    expect(eq(s.candidates[0].namespace_, "capi"));
    expect(eq(s.candidates[0].shortName, "lua"));
}};

UTest dependency_selectors_reject_unsafe_or_ambiguous_segments{"Manifest/DependencySelectorsRejectUnsafeOrAmbiguousSegments", [] {
    const std::vector<std::string> invalid{
        "acme/widget", "acme widget", "acme[widget",
        "acme=widget", "acme#widget", "acme..widget",
    };
    for (auto const& selector : invalid) {
        auto src = std::format(R"(
[package]
name = "x"
version = "0.1.0"
[dependencies]
"{}" = "1.0.0"
)", selector);
        auto m = mcpp::manifest::parse_string(src);
        expect(!m.has_value()) << selector;
        if (!m) expect(ne(m.error().message.find("invalid package selector"), std::string::npos)) << m.error().format();
    }
}};

UTest dependencies_namespaced_subtable_nested_dotted_key_is_canonical{"Manifest/DependenciesNamespacedSubtableNestedDottedKeyIsCanonical", [] {
    constexpr auto src = R"(
[package]
name    = "x"
version = "0.1.0"

[dependencies.mcpplibs]
capi.lua = "0.0.3"
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->dependencies.size(), 1u)));

    auto& s = m->dependencies.at("mcpplibs.capi.lua");
    expect(eq(s.namespace_, "mcpplibs.capi"));
    expect(eq(s.shortName, "lua"));
    expect(eq(s.version, "0.0.3"));
    expect(!s.legacyDottedKey);
    expect(!s.legacyCandidateSearch);
    expect(fatal(eq(s.candidates.size(), 1u)));
    expect(eq(s.candidates[0].namespace_, "mcpplibs.capi"));
    expect(eq(s.candidates[0].shortName, "lua"));
}};

UTest dependencies_inline_spec_coexists_with_subtable{"Manifest/DependenciesInlineSpecCoexistsWithSubtable", [] {
    // `bar = { git = "...", tag = "..." }` looks like a subtable but has
    // only dep-spec keys → treated as inline spec under default ns.
    // `[dependencies.acme]` is a real namespace subtable.
    constexpr auto src = R"(
[package]
name    = "x"
version = "0.1.0"

[dependencies]
bar = { git = "https://example.com/bar.git", tag = "v1" }

[dependencies.acme]
util = "2.0.0"
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->dependencies.size(), 2u)));
    auto& bar = m->dependencies.at("bar");
    expect(eq(bar.namespace_, "mcpplibs"));
    expect(eq(bar.shortName, "bar"));
    expect(eq(bar.git, "https://example.com/bar.git"));
    expect(eq(bar.gitRev, "v1"));
    expect(eq(bar.gitRefKind, "tag"));

    auto& util = m->dependencies.at("acme.util");
    expect(eq(util.namespace_, "acme"));
    expect(eq(util.shortName, "util"));
    expect(eq(util.version, "2.0.0"));
}};

UTest deps_key_split_namespace{"SynthesizeFromXpkgLua/DepsKeySplitNamespace", [] {
    constexpr auto src = R"(
package = {
    spec = "1",
    name = "consumer",
    xpm  = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        sources = { "*/src/*.cppm" },
        deps    = {
            ["mbedtls"]          = "3.6.1",
            ["mcpplibs.cmdline"] = "0.0.2",
        },
        targets = { ["consumer"] = { kind = "lib" } },
    },
}
)";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(src, "consumer", "1.0.0", mcpp::toolchain::platform::HostPlatform::current());
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->dependencies.size(), 2u)));

    auto& a = m->dependencies.at("mbedtls");
    expect(eq(a.namespace_, "mcpplibs"));
    expect(eq(a.shortName, "mbedtls"));
    expect(eq(a.version, "3.6.1"));

    auto& b = m->dependencies.at("mcpplibs.cmdline");
    expect(eq(b.namespace_, "mcpplibs"));
    expect(eq(b.shortName, "cmdline"));
    expect(eq(b.version, "0.0.2"));
}};

UTest deps_dotted_selectors_are_exact{"SynthesizeFromXpkgLua/DepsDottedSelectorsAreExact", [] {
    constexpr auto src = R"(
package = {
    spec = "1",
    name = "consumer",
    xpm  = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        sources = { "*/src/*.cppm" },
        deps    = {
            ["capi.lua"]     = "0.0.3",
            ["imgui.core"]   = "0.0.1",
            ["compat.gtest"] = "1.15.2",
        },
        targets = { ["consumer"] = { kind = "lib" } },
    },
}
)";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(src, "consumer", "1.0.0", mcpp::toolchain::platform::HostPlatform::current());
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->dependencies.size(), 3u)));

    auto& lua = m->dependencies.at("capi.lua");
    expect(eq(lua.namespace_, "capi"));
    expect(eq(lua.shortName, "lua"));
    expect(eq(lua.version, "0.0.3"));
    expect(bool(lua.legacyCandidateSearch));
    expect(fatal(eq(lua.candidates.size(), 1u)));
    expect(eq(lua.candidates[0].namespace_, "capi"));
    expect(eq(lua.candidates[0].shortName, "lua"));

    auto& imgui = m->dependencies.at("imgui.core");
    expect(eq(imgui.namespace_, "imgui"));
    expect(eq(imgui.shortName, "core"));
    expect(eq(imgui.version, "0.0.1"));
    expect(bool(imgui.legacyCandidateSearch));
    expect(fatal(eq(imgui.candidates.size(), 1u)));
    expect(eq(imgui.candidates[0].namespace_, "imgui"));
    expect(eq(imgui.candidates[0].shortName, "core"));

    auto& gtest = m->dependencies.at("compat.gtest");
    expect(eq(gtest.namespace_, "compat"));
    expect(eq(gtest.shortName, "gtest"));
    expect(eq(gtest.version, "1.15.2"));
    expect(bool(gtest.legacyCandidateSearch));
    expect(fatal(eq(gtest.candidates.size(), 1u)));
    expect(eq(gtest.candidates[0].namespace_, "compat"));
    expect(eq(gtest.candidates[0].shortName, "gtest"));
}};

UTest invalid_dependency_selector_is_an_error{"SynthesizeFromXpkgLua/InvalidDependencySelectorIsAnError", [] {
    constexpr auto src = R"(
package = {
    spec = "1",
    name = "consumer",
    xpm  = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        sources = { "*/src/*.cppm" },
        deps = { ["acme/widget"] = "1.0.0" },
    },
}
)";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(
        src, "consumer", "1.0.0", mcpp::toolchain::platform::HostPlatform::current());
    expect(fatal(!m.has_value()));
    if (auto&& failed = expect(ne(m.error().message.find("invalid package selector"), std::string::npos)); !failed) failed << m.error().format();
}};

UTest workspace_section_parsed{"Manifest/WorkspaceSectionParsed", [] {
    constexpr auto src = R"(
[workspace]
members = ["libs/core", "libs/http", "apps/server"]
exclude = ["libs/experimental"]

[workspace.dependencies]
cmdline = "0.0.2"

[workspace.dependencies.compat]
gtest = "1.15.2"
mbedtls = "3.6.1"
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(bool(m->workspace.present));
    expect(fatal(eq(m->workspace.members.size(), 3u)));
    expect(eq(m->workspace.members[0], "libs/core"));
    expect(eq(m->workspace.members[1], "libs/http"));
    expect(eq(m->workspace.members[2], "apps/server"));
    expect(fatal(eq(m->workspace.exclude.size(), 1u)));
    expect(eq(m->workspace.exclude[0], "libs/experimental"));
    expect(fatal(eq(m->workspace.dependencies.size(), 3u)));
    auto& gt = m->workspace.dependencies.at("compat.gtest");
    expect(eq(gt.version, "1.15.2"));
    expect(eq(gt.namespace_, "compat"));
}};

UTest workspace_dependencies_use_exact_dotted_selectors{"Manifest/WorkspaceDependenciesUseExactDottedSelectors", [] {
    constexpr auto src = R"(
[workspace]
members = ["libs/core"]

[workspace.dependencies]
capi.lua = "0.0.3"
imgui.core = "0.0.1"
compat.gtest = "1.15.2"
mcpplibs.templates = "0.0.1"
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->workspace.dependencies.size(), 4u)));

    auto& lua = m->workspace.dependencies.at("capi.lua");
    expect(eq(lua.namespace_, "capi"));
    expect(eq(lua.shortName, "lua"));
    expect(eq(lua.version, "0.0.3"));
    expect(bool(lua.legacyCandidateSearch));
    expect(fatal(eq(lua.candidates.size(), 1u)));
    expect(eq(lua.candidates[0].namespace_, "capi"));
    expect(eq(lua.candidates[0].shortName, "lua"));

    auto& imgui = m->workspace.dependencies.at("imgui.core");
    expect(eq(imgui.namespace_, "imgui"));
    expect(eq(imgui.shortName, "core"));
    expect(eq(imgui.version, "0.0.1"));
    expect(bool(imgui.legacyCandidateSearch));
    expect(fatal(eq(imgui.candidates.size(), 1u)));
    expect(eq(imgui.candidates[0].namespace_, "imgui"));
    expect(eq(imgui.candidates[0].shortName, "core"));

    auto& gt = m->workspace.dependencies.at("compat.gtest");
    expect(eq(gt.namespace_, "compat"));
    expect(eq(gt.shortName, "gtest"));
    expect(eq(gt.version, "1.15.2"));
    expect(bool(gt.legacyCandidateSearch));
    expect(fatal(eq(gt.candidates.size(), 1u)));
    expect(eq(gt.candidates[0].namespace_, "compat"));
    expect(eq(gt.candidates[0].shortName, "gtest"));

    auto& tmpl = m->workspace.dependencies.at("mcpplibs.templates");
    expect(eq(tmpl.namespace_, "mcpplibs"));
    expect(eq(tmpl.shortName, "templates"));
    expect(eq(tmpl.version, "0.0.1"));
    expect(!tmpl.legacyCandidateSearch);
    expect(fatal(eq(tmpl.candidates.size(), 1u)));
    expect(eq(tmpl.candidates[0].namespace_, "mcpplibs"));
    expect(eq(tmpl.candidates[0].shortName, "templates"));
}};

UTest workspace_true_in_dependency{"Manifest/WorkspaceTrueInDependency", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[dependencies.compat]
mbedtls = { workspace = true }
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    auto& s = m->dependencies.at("compat.mbedtls");
    expect(bool(s.inheritWorkspace));
    expect(eq(s.namespace_, "compat"));
    expect(eq(s.shortName, "mbedtls"));
}};

UTest indices_default_key_normalizes_to_default_namespace{"Manifest/IndicesDefaultKeyNormalizesToDefaultNamespace", [] {
    // R6: `[indices] default = {...}` (the canonical spelling) is a
    // redirect for the DEFAULT namespace (bare `gtest = "1.15.2"` deps),
    // not a literal index named "default". It must be stored under
    // kDefaultNamespace so the two short-circuits in prepare.cppm
    // (usesBuiltinIndex / findIndexForNs) can find it.
    constexpr auto src = R"(
[package]
name = "hello"
version = "0.1.0"
[language]
standard = "c++23"
[modules]
sources = ["src/**/*.cppm"]
[indices]
default = { path = "../local-pkgs" }
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    auto it = m->indices.find(std::string(mcpp::manifest::kDefaultNamespace));
    if (auto&& failed = expect(fatal(ne(it, m->indices.end()))); !failed) failed << "[indices] default = {...} must normalize to kDefaultNamespace";
    expect(eq(it->second.path.string(), "../local-pkgs"));
    expect(!(it->second.is_builtin()));
    expect(eq(m->indices.find("default"), m->indices.end()))
        << "the literal key \"default\" must not survive normalization";
}};

UTest indices_empty_string_key_normalizes_to_default_namespace{"Manifest/IndicesEmptyStringKeyNormalizesToDefaultNamespace", [] {
    // The empty-quoted key `""` is also accepted as a spelling of the
    // default-namespace redirect.
    constexpr auto src = R"(
[package]
name = "hello"
version = "0.1.0"
[language]
standard = "c++23"
[modules]
sources = ["src/**/*.cppm"]
[indices]
"" = { path = "../local-pkgs" }
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    auto it = m->indices.find(std::string(mcpp::manifest::kDefaultNamespace));
    if (auto&& failed = expect(fatal(ne(it, m->indices.end()))); !failed) failed << "[indices] \"\" = {...} must normalize to kDefaultNamespace";
    expect(eq(it->second.path.string(), "../local-pkgs"));
}};

UTest indices_default_key_with_url_is_rejected{"Manifest/IndicesDefaultKeyWithUrlIsRejected", [] {
    // R6's design goal is redirecting the default namespace to a LOCAL
    // index checkout (`path = ...`), so the index repo can test module
    // packages. The `url` form is not a design requirement, and silently
    // accepting it is worse than rejecting it: is_builtin() (index_spec
    // .cppm) treats any "mcpplibs"-named entry with an empty `path` as
    // builtin, so a `default = { url = ... }` redirect would silently
    // no-op — every consumer falls through to the real builtin registry
    // and the configured url is just ignored, with no error surfaced.
    // Reject it loudly at parse time instead.
    constexpr auto src = R"(
[package]
name = "hello"
version = "0.1.0"
[language]
standard = "c++23"
[modules]
sources = ["src/**/*.cppm"]
[indices]
default = { url = "https://example.com/mcpp-index.git" }
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(!m.has_value())); !failed) failed << "[indices] default = { url = ... } must be rejected, not silently "
           "accepted as a no-op redirect";
    if (auto&& failed = expect(ne(m.error().format().find("path"), std::string::npos)); !failed) failed << "error should mention that only 'path' is supported for the "
           "default-namespace redirect; got: " << m.error().format();
}};

UTest indices_mcpplibs_url_pin_still_accepted{"Manifest/IndicesMcpplibsUrlPinStillAccepted", [] {
    // A literal `[indices] mcpplibs = { url = ..., rev = ... }` is a
    // DIFFERENT key spelling from the `default`/`""` alias — it pins the
    // real builtin registry to a commit, not a default-namespace redirect
    // — and must keep working even though it also normalizes to the
    // kDefaultNamespace map slot and also carries a `url`.
    constexpr auto src = R"(
[package]
name = "hello"
version = "0.1.0"
[language]
standard = "c++23"
[modules]
sources = ["src/**/*.cppm"]
[indices]
mcpplibs = { url = "https://example.com/mcpp-index.git", rev = "abc123" }
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    auto it = m->indices.find(std::string(mcpp::manifest::kDefaultNamespace));
    expect(fatal(ne(it, m->indices.end())));
    expect(eq(it->second.url, "https://example.com/mcpp-index.git"));
    expect(eq(it->second.rev, "abc123"));
    expect(it->second.is_builtin());
}};

UTest indices_default_and_mcpplibs_collision_rejected{"Manifest/IndicesDefaultAndMcpplibsCollisionRejected", [] {
    // `default`, `""`, and a literal `mcpplibs` key all normalize to the
    // same map slot (kDefaultNamespace). Declaring more than one must be a
    // parse error, not a silent last-writer-wins clobber.
    constexpr auto src = R"(
[package]
name = "hello"
version = "0.1.0"
[language]
standard = "c++23"
[modules]
sources = ["src/**/*.cppm"]
[indices]
mcpplibs = { path = "../local-pkgs-a" }
default = { path = "../local-pkgs-b" }
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(!m.has_value())); !failed) failed << "[indices] mcpplibs + default together must be rejected as a "
           "collision on the same default-namespace slot";
    if (auto&& failed = expect(ne(m.error().format().find("collide"), std::string::npos)); !failed) failed << "error should mention the collision; got: " << m.error().format();
}};

UTest workspace_dependencies_path_form_flat_key{"Manifest/WorkspaceDependenciesPathFormFlatKey", [] {
    // #224: `[workspace.dependencies] ylib = { path = "ylib" }` — a plain
    // (non-namespaced, non-dotted) top-level key with an inline path table
    // — must parse to a path-form DependencySpec, not get misrouted into
    // the namespace/selector-table parsers (which would treat "path" as a
    // nested selector key).
    constexpr auto src = R"(
[workspace]
members = ["a"]

[workspace.dependencies]
ylib = { path = "ylib" }
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(m->workspace.dependencies.contains("ylib")))
        << "workspace.dependencies must contain a flat 'ylib' key";
    auto& s = m->workspace.dependencies.at("ylib");
    expect(eq(s.path, "ylib"));
    expect(s.version.empty());
}};

UTest a_package_named_like_a_spec_field{"Manifest/APackageNamedLikeASpecField", [] {
    // #918: `gpp.version = { path = ... }` is the package `version` in the
    // namespace `gpp`. A spec's `version` is a string; a table under the same
    // key is a package, in [dependencies] and in [workspace.dependencies].
    constexpr auto src = R"(
[package]
name = "app"
version = "0.1.0"

[dependencies]
gpp.core = { path = "../core" }
gpp.version = { path = "../version" }
solo = { version = "2.0.0" }

[workspace.dependencies]
gpp.git = { path = "../git" }
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(m->dependencies.contains("gpp.version")));
    expect(eq(m->dependencies.at("gpp.version").path, "../version"));
    expect(eq(m->dependencies.at("gpp.core").path, "../core"));
    expect(eq(m->dependencies.at("solo").version, "2.0.0"));
    expect(!m->dependencies.contains("gpp"));
    expect(fatal(m->workspace.dependencies.contains("gpp.git")));
    expect(eq(m->workspace.dependencies.at("gpp.git").path, "../git"));
}};

UTest no_workspace_section_means_not_present{"Manifest/NoWorkspaceSectionMeansNotPresent", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
)";
    auto m = mcpp::manifest::parse_string(src);
    expect(fatal(m.has_value()));
    expect(!(m->workspace.present));
}};

UTest lib_root_inferred_from_package_name{"Manifest/LibRootInferredFromPackageName", [] {
    constexpr auto src = R"(
[package]
name    = "mcpplibs.tinyhttps"
version = "0.2.0"
[targets.tinyhttps]
kind = "lib"
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(m->lib.path.empty());
    expect(mcpp::manifest::has_lib_target(*m));
    auto root = mcpp::manifest::resolve_lib_root_path(*m);
    expect(eq(root, std::filesystem::path("src/tinyhttps.cppm")));
}};

UTest lib_root_bare_name_no_namespace{"Manifest/LibRootBareNameNoNamespace", [] {
    constexpr auto src = R"(
[package]
name    = "gtest"
version = "1.0.0"
[targets.gtest]
kind = "lib"
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    auto root = mcpp::manifest::resolve_lib_root_path(*m);
    expect(eq(root, std::filesystem::path("src/gtest.cppm")));
}};

UTest lib_root_explicit_override{"Manifest/LibRootExplicitOverride", [] {
    constexpr auto src = R"(
[package]
name    = "mcpplibs.tinyhttps"
version = "0.2.0"
[lib]
path = "src/api.cppm"
[targets.tinyhttps]
kind = "lib"
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(eq(m->lib.path.string(), "src/api.cppm"));
    auto root = mcpp::manifest::resolve_lib_root_path(*m);
    expect(eq(root.string(), "src/api.cppm"));
}};

UTest has_lib_target_false_for_bare_binary_manifest{"Manifest/HasLibTargetFalseForBareBinaryManifest", [] {
    // No [targets.*] declared → parse_string leaves targets empty.
    // load() would later infer a bin/lib from sources, but parse_string
    // alone leaves it bare; either way no lib target.
    constexpr auto src = R"(
[package]
name    = "mcpp"
version = "0.0.2"
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(!mcpp::manifest::has_lib_target(*m));
}};

UTest parses_per_target_flags_and_required_features{"Manifest/ParsesPerTargetFlagsAndRequiredFeatures", [] {
    constexpr auto src = R"(
[package]
name    = "app"
version = "0.1.0"
[targets.server]
kind     = "bin"
main     = "src/server.cpp"
defines  = ["BUILD_SERVER=1", "PORT=8080"]
cxxflags = ["-fno-exceptions"]
cflags   = ["-DPURE_C"]
required_features = ["server"]
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->targets.size(), 1u)));
    auto& t = m->targets[0];
    expect(fatal(eq(t.defines.size(), 2u)));
    expect(eq(t.defines[0], "BUILD_SERVER=1"));
    expect(eq(t.defines[1], "PORT=8080"));
    expect(fatal(eq(t.cxxflags.size(), 1u)));
    expect(eq(t.cxxflags[0], "-fno-exceptions"));
    expect(fatal(eq(t.cflags.size(), 1u)));
    expect(eq(t.cflags[0], "-DPURE_C"));
    expect(fatal(eq(t.requiredFeatures.size(), 1u)));
    expect(eq(t.requiredFeatures[0], "server"));
    expect(m->schemaWarnings.empty());
}};

UTest warns_on_unsupported_target_key{"Manifest/WarnsOnUnsupportedTargetKey", [] {
    constexpr auto src = R"(
[package]
name    = "app"
version = "0.1.0"
[targets.app]
kind   = "bin"
main   = "src/main.cpp"
cxxfalgs = ["-DTYPO"]
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->schemaWarnings.size(), 1u)));
    expect(ne(m->schemaWarnings[0].find("cxxfalgs"), std::string::npos));
    expect(ne(m->schemaWarnings[0].find("unsupported key"), std::string::npos));
}};

// #618: `windows_subsystem` / `windows_entry` on an executable target.
UTest parses_windows_subsystem_and_entry_on_a_binary_target{"Manifest/ParsesWindowsSubsystemAndEntryOnABinaryTarget", [] {
    constexpr auto src = R"(
[package]
name    = "app"
version = "0.1.0"
[targets.app]
kind              = "bin"
main              = "src/main.cpp"
windows_subsystem = "windows"
windows_entry     = "wWinMain"
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->targets.size(), 1u)));
    expect(eq(m->targets[0].windowsSubsystem, "windows"));
    expect(eq(m->targets[0].windowsEntry, "wWinMain"));
    expect(m->schemaWarnings.empty());
}};

// #693: `windows_code_page` states the process code page of an executable, in
// the vocabulary of the manifest's `activeCodePage` element.
UTest parses_windows_code_page_on_a_binary_target{"Manifest/ParsesWindowsCodePageOnABinaryTarget", [] {
    for (std::string_view value : {"utf-8", "legacy"}) {
        const auto src = std::format(R"(
[package]
name    = "app"
version = "0.1.0"
[targets.app]
kind              = "bin"
main              = "src/main.cpp"
windows_code_page = "{}"
)", value);
        auto m = mcpp::manifest::parse_string(src);
        if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
        expect(fatal(eq(m->targets.size(), 1u)));
        expect(eq(m->targets[0].windowsCodePage, value));
        expect(m->schemaWarnings.empty());
    }
}};

// `windows_long_paths` (design 2026-10-16 §11): an executable's statement
// that it handles paths past MAX_PATH; unset when the author says nothing.
UTest parses_windows_long_paths_on_a_binary_target_and_for_tests{"Manifest/ParsesWindowsLongPathsOnABinaryTargetAndForTests", [] {
    for (bool value : {true, false}) {
        const auto src = std::format(R"(
[package]
name    = "app"
version = "0.1.0"
[targets.app]
kind               = "bin"
main               = "src/main.cpp"
windows_long_paths = {0}
[test]
windows_long_paths = {0}
)", value);
        auto m = mcpp::manifest::parse_string(src);
        if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
        expect(fatal(eq(m->targets.size(), 1u)));
        expect(fatal(m->targets[0].windowsLongPaths.has_value()));
        expect(eq(*m->targets[0].windowsLongPaths, value));
        expect(fatal(m->testWindowsLongPaths.has_value()));
        expect(eq(*m->testWindowsLongPaths, value));
        expect(m->schemaWarnings.empty());
    }
    auto unset = mcpp::manifest::parse_string(
        "[package]\nname = \"app\"\nversion = \"0.1.0\"\n[targets.app]\nkind = \"bin\"\nmain = \"m.cpp\"\n");
    expect(fatal(unset.has_value()));
    expect(!(unset->targets[0].windowsLongPaths.has_value()));
    auto wrong = mcpp::manifest::parse_string(
        "[package]\nname = \"app\"\nversion = \"0.1.0\"\n[targets.app]\nkind = \"bin\"\nmain = \"m.cpp\"\n"
        "windows_long_paths = \"yes\"\n");
    expect(fatal(!wrong.has_value()));
    expect(ne(wrong.error().message.find("windows_long_paths must be true or false"), std::string::npos));
    auto library = mcpp::manifest::parse_string(
        "[package]\nname = \"app\"\nversion = \"0.1.0\"\n[targets.core]\nkind = \"lib\"\n"
        "windows_long_paths = true\n");
    expect(fatal(!library.has_value()));
    if (auto&& failed = expect(ne(library.error().message.find("targets.core.windows_long_paths"), std::string::npos)); !failed) failed << library.error().message;
}};

UTest refuses_an_unknown_windows_code_page_naming_the_accepted_ones{"Manifest/RefusesAnUnknownWindowsCodePageNamingTheAcceptedOnes", [] {
    constexpr auto src = R"(
[package]
name    = "app"
version = "0.1.0"
[targets.app]
kind              = "bin"
main              = "src/main.cpp"
windows_code_page = "65001"
)";
    auto m = mcpp::manifest::parse_string(src);
    expect(fatal(!m.has_value()));
    if (auto&& failed = expect(ne(m.error().message.find("\"65001\" is not one of \"utf-8\", \"legacy\""), std::string::npos)); !failed) failed << m.error().message;
}};

UTest refuses_windows_keys_on_a_library_naming_the_target_and_the_key{"Manifest/RefusesWindowsKeysOnALibraryNamingTheTargetAndTheKey", [] {
    const std::pair<std::string_view, std::string_view> keys[] = {
        {"windows_subsystem", "windows"}, {"windows_entry", "wmain"},
        {"windows_code_page", "utf-8"}};
    for (std::string_view kind : {"lib", "shared"}) {
        for (auto [key, value] : keys) {
            const auto src = std::format(R"(
[package]
name    = "app"
version = "0.1.0"
[targets.core]
kind = "{}"
{} = "{}"
)", kind, key, value);
            auto m = mcpp::manifest::parse_string(src);
            expect(fatal(!m.has_value())) << kind << " " << key;
            if (auto&& failed = expect(ne(m.error().message.find(std::format("targets.core.{}", key)), std::string::npos)); !failed) failed << m.error().message;
            if (auto&& failed = expect(ne(m.error().message.find("executable"), std::string::npos)); !failed) failed << m.error().message;
        }
    }
}};

// #622 A3: `kind = "app"` -- "the thing a user launches", on every row. Its
// link form is a property of the row (`toolchain::triple::application_form`),
// not of the manifest; see test_artifact_naming.cpp for that half.
UTest kind_app_parses_to_application{"Manifest/KindAppParsesToApplication", [] {
    constexpr auto src = R"(
[package]
name    = "myapp"
version = "0.1.0"
[targets.myapp]
kind = "app"
main = "src/main.cpp"
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->targets.size(), 1u)));
    expect(eq(m->targets[0].kind, mcpp::manifest::Target::Application));
    expect(eq(m->targets[0].main, "src/main.cpp"));
    expect(m->targets[0].is_program());
}};

// `is_program()` is exactly {Binary, Application} -- the negative direction,
// asserted on every OTHER kind so the predicate cannot silently widen.
UTest is_program_is_exactly_binary_and_application{"Manifest/IsProgramIsExactlyBinaryAndApplication", [] {
    auto kind_of = [](std::string_view kind) {
        auto src = std::format(R"(
[package]
name    = "p"
version = "0.1.0"
[targets.t]
kind = "{}"
{}
)", kind, (kind == "bin" || kind == "app") ? "main = \"src/main.cpp\"" : "");
        auto m = mcpp::manifest::parse_string(src);
        return m;
    };
    for (auto [kind, expected] : {std::pair{"bin", true}, {"app", true},
                                   {"lib", false}, {"shared", false}}) {
        auto m = kind_of(kind);
        if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << kind << ": " << m.error().format();
        expect(fatal(eq(m->targets.size(), 1u))) << kind;
        expect(eq(m->targets[0].is_program(), expected)) << kind;
    }
}};

// The refusal for an unrecognized `kind` lists all five accepted spellings --
// including the newest -- so a typo's message never falls behind the parser.
UTest kind_refusal_lists_all_five_kinds{"Manifest/KindRefusalListsAllFiveKinds", [] {
    constexpr auto src = R"(
[package]
name    = "p"
version = "0.1.0"
[targets.t]
kind = "framework"
)";
    auto m = mcpp::manifest::parse_string(src);
    expect(fatal(!m.has_value()));
    if (auto&& failed = expect(ne(m.error().message.find("'bin', 'app', 'lib', 'shared' or 'entry'"), std::string::npos)); !failed) failed << m.error().message;
}};

// `main` is required for `app` exactly as it is for `bin` -- on Android it
// becomes a translation unit of the shared library rather than an
// executable's entry, but it is still named the same way.
UTest application_requires_main_field{"Manifest/ApplicationRequiresMainField", [] {
    constexpr auto src = R"(
[package]
name    = "myapp"
version = "0.1.0"
[targets.myapp]
kind = "app"
)";
    auto m = mcpp::manifest::parse_string(src);
    expect(fatal(!m.has_value()));
    if (auto&& failed = expect(ne(m.error().message.find("requires 'main'"), std::string::npos)); !failed) failed << m.error().message;
}};

// `windows_subsystem` / `windows_entry` accept `app` exactly as they accept
// `bin` (#622 A3) -- the negative direction is
// RefusesWindowsKeysOnALibraryNamingTheTargetAndTheKey above, which still
// covers `lib` and `shared`.
UTest windows_keys_are_accepted_on_an_application_target{"Manifest/WindowsKeysAreAcceptedOnAnApplicationTarget", [] {
    constexpr auto src = R"(
[package]
name    = "app"
version = "0.1.0"
[targets.app]
kind              = "app"
main              = "src/main.cpp"
windows_subsystem = "windows"
windows_entry     = "wWinMain"
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->targets.size(), 1u)));
    expect(eq(m->targets[0].windowsSubsystem, "windows"));
    expect(eq(m->targets[0].windowsEntry, "wWinMain"));
}};

UTest refuses_an_unknown_windows_value_naming_the_accepted_ones{"Manifest/RefusesAnUnknownWindowsValueNamingTheAcceptedOnes", [] {
    constexpr auto subsystem = R"(
[package]
name    = "app"
version = "0.1.0"
[targets.app]
kind              = "bin"
main              = "src/main.cpp"
windows_subsystem = "gui"
)";
    auto m = mcpp::manifest::parse_string(subsystem);
    expect(fatal(!m.has_value()));
    if (auto&& failed = expect(ne(m.error().message.find("\"gui\" is not one of \"console\", \"windows\""), std::string::npos)); !failed) failed << m.error().message;

    constexpr auto entry = R"(
[package]
name    = "app"
version = "0.1.0"
[targets.app]
kind          = "bin"
main          = "src/main.cpp"
windows_entry = "mainCRTStartup"
)";
    auto e = mcpp::manifest::parse_string(entry);
    expect(fatal(!e.has_value()));
    if (auto&& failed = expect(ne(e.error().message.find(
                  "is not one of \"main\", \"wmain\", \"WinMain\", \"wWinMain\""), std::string::npos)); !failed) failed << e.error().message;
}};

// The key list in the warning is generated from the list the parser accepts,
// so a key the parser reads cannot be missing from the message.
UTest unsupported_target_key_warning_lists_every_accepted_key{"Manifest/UnsupportedTargetKeyWarningListsEveryAcceptedKey", [] {
    constexpr auto src = R"(
[package]
name    = "app"
version = "0.1.0"
[targets.app]
kind  = "bin"
main  = "src/main.cpp"
bogus = "x"
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->schemaWarnings.size(), 1u)));
    for (auto key : {"exports", "windows_subsystem", "windows_entry", "required_features"})
        expect(ne(m->schemaWarnings[0].find(key), std::string::npos))
            << key << " missing from: " << m->schemaWarnings[0];
}};

UTest rejects_std_flag_in_target_cxxflags{"Manifest/RejectsStdFlagInTargetCxxflags", [] {
    constexpr auto src = R"(
[package]
name    = "app"
version = "0.1.0"
[targets.app]
kind     = "bin"
main     = "src/main.cpp"
cxxflags = ["-std=c++20"]
)";
    auto m = mcpp::manifest::parse_string(src);
    expect(fatal(!m.has_value()));
    expect(ne(m.error().message.find("[package].standard"), std::string::npos));
}};

UTest ignores_commented_entries{"ListXpkgVersions/IgnoresCommentedEntries", [] {
    constexpr auto src = R"(
package = {
    xpm = {
        linux = {
            -- ["1.2.3"] = { ... },   -- this is a comment
            ["0.1.0"] = { url = "u" },
        },
    },
}
)";
    auto v = mcpp::manifest::list_xpkg_versions(src, mcpp::toolchain::platform::TargetPlatform::for_lint_of("linux"));
    expect(fatal(eq(v.size(), 1u)));
    expect(eq(v[0], "0.1.0"));
}};

// ─── xpkg_lua_identity_matches — descriptor identity gate ───────────
//
// Regression cover for the compat.zlib vs upstream bare zlib.lua collision:
// a file found by a candidate filename must DECLARE the requested package.

namespace {
constexpr std::string_view kCompatZlibLua = R"(
package = {
    namespace   = "compat",
    name        = "compat.zlib",
    version     = "1.3.2",
    mcpp = {
        sources = { "*.c" },
    },
}
)";

// Upstream xim zlib: declares a bare name and NO namespace, no mcpp block.
constexpr std::string_view kUpstreamZlibLua = R"(
package = {
    name = "zlib",
    description = "A massively spiffy yet delicately unobtrusive compression library",
}
)";
}  // namespace

UTest compat_descriptor_matches_compat_request{"XpkgIdentity/CompatDescriptorMatchesCompatRequest", [] {
    expect(mcpp::manifest::xpkg_lua_identity_matches(
        kCompatZlibLua, "compat", "zlib"));
}};

UTest upstream_bare_zlib_does_not_match_compat_request{"XpkgIdentity/UpstreamBareZlibDoesNotMatchCompatRequest", [] {
    // The crux of the incident: a foreign bare `zlib.lua` must be rejected for
    // a `compat.zlib` request, no matter that the candidate filename matched.
    expect(!mcpp::manifest::xpkg_lua_identity_matches(
        kUpstreamZlibLua, "compat", "zlib"));
}};

UTest descriptor_declaring_namespace_matches_only_that_namespace{"XpkgIdentity/DescriptorDeclaringNamespaceMatchesOnlyThatNamespace", [] {
    // When a descriptor DECLARES its namespace, it matches that namespace and no
    // other. (Index-owned namespace for *no-namespace* descriptors — attributing
    // a bare `zlib.lua` to its owning index as `(xim, zlib)` — is deferred to the
    // §4.1 follow-up, since the content-only gate cannot know a file's index.)
    constexpr std::string_view ximZlib =
        R"(package = { namespace = "xim", name = "zlib" })";
    expect(mcpp::manifest::xpkg_lua_identity_matches(
        ximZlib, "xim", "zlib", /*allowLegacyBareDefault=*/false));
    expect(!mcpp::manifest::xpkg_lua_identity_matches(
        ximZlib, "compat", "zlib"));
}};

UTest no_declared_name_is_accepted_leniently{"XpkgIdentity/NoDeclaredNameIsAcceptedLeniently", [] {
    // A descriptor without a package.name cannot be verified → accepted.
    constexpr std::string_view noName = R"(package = { version = "1.0.0" })";
    expect(mcpp::manifest::xpkg_lua_identity_matches(noName, "compat", "zlib"));
}};

UTest default_namespace_bare_name_gated_by_flag{"XpkgIdentity/DefaultNamespaceBareNameGatedByFlag", [] {
    // Default-namespace (mcpplibs) bare-named legacy descriptor: accepted only
    // when the legacy-bare flag is on.
    constexpr std::string_view bareDefault = R"(package = { name = "cmdline" })";
    expect(mcpp::manifest::xpkg_lua_identity_matches(
        bareDefault, "mcpplibs", "cmdline", /*allowLegacyBareDefault=*/true));
    expect(!mcpp::manifest::xpkg_lua_identity_matches(
        bareDefault, "mcpplibs", "cmdline", /*allowLegacyBareDefault=*/false));
}};

UTest empty_namespace_discovery_matches_namespaced_descriptor{"XpkgIdentity/EmptyNamespaceDiscoveryMatchesNamespacedDescriptor", [] {
    // `mcpp new --template llmapi` reads with an empty (unknown) namespace and
    // derives the real namespace from the file. A namespaced descriptor must
    // still be discoverable by its short name.
    constexpr std::string_view llmapi = R"(
package = { namespace = "mcpplibs", name = "mcpplibs.llmapi", version = "0.2.8" }
)";
    expect(mcpp::manifest::xpkg_lua_identity_matches(llmapi, "", "llmapi"));
    // ...but it is NOT a match for a different short name.
    expect(!mcpp::manifest::xpkg_lua_identity_matches(llmapi, "", "zlib"));
}};

UTest default_namespace_request_does_not_match_compat_package{"XpkgIdentity/DefaultNamespaceRequestDoesNotMatchCompatPackage", [] {
    // Namespace omission now means exactly mcpplibs. A compat package must be
    // selected as `compat.gtest`, never smuggled through an identity alias.
    constexpr std::string_view compatGtest =
        R"(package = { namespace = "compat", name = "compat.gtest", version = "1.15.2" })";
    expect(!mcpp::manifest::xpkg_lua_identity_matches(
        compatGtest, "mcpplibs", "gtest"));
    expect(!mcpp::manifest::xpkg_lua_identity_matches(
        compatGtest, "mcpplibs", "gtest", /*allowLegacyBareDefault=*/false));
    // But a different default-ns name does not match it.
    expect(!mcpp::manifest::xpkg_lua_identity_matches(
        compatGtest, "mcpplibs", "zlib"));
}};

// ─── xpkg_wire_address — the address xlings is asked to install ─────
//
// REGRESSION LOCK for the 2026-07-25 CI break (6 of 7 workflows red on main).
// Once a descriptor is selected by an explicit `compat.gtest` request, the
// wire address has to come from THAT descriptor — both halves. Taking the name from the
// descriptor and the namespace from the request emitted `mcpplibs:gtest`, which
// no index is keyed by; it only ever worked because the literal name used to
// read `compat.gtest`, so a hardcoded `compat.<short>` retry caught it. The
// index's SPEC-001 short-name migration removed that coincidence.

UTest compat_descriptor_addresses_compat{"XpkgWireAddress/CompatDescriptorAddressesCompat", [] {
    constexpr std::string_view compatGtest =
        R"(package = { namespace = "compat", name = "gtest", version = "1.15.2" })";
    auto addr = mcpp::manifest::xpkg_wire_address(compatGtest, "compat", "gtest");
    expect(eq(addr.ns, "compat"));
    expect(eq(addr.name, "gtest"));
    expect(eq(addr.target, "compat:gtest"));   // NOT "mcpplibs:gtest"
}};

UTest default_namespace_descriptor_is_unchanged{"XpkgWireAddress/DefaultNamespaceDescriptorIsUnchanged", [] {
    // The 8 mcpplibs descriptors in the live index must address exactly as
    // before — this fix moves only the packages that were already broken.
    constexpr std::string_view cmdline =
        R"(package = { namespace = "mcpplibs", name = "cmdline", version = "0.0.1" })";
    auto addr = mcpp::manifest::xpkg_wire_address(cmdline, "mcpplibs", "cmdline");
    expect(eq(addr.target, "mcpplibs:cmdline"));
}};

UTest legacy_fqn_descriptor_keeps_its_literal_name{"XpkgWireAddress/LegacyFqnDescriptorKeepsItsLiteralName", [] {
    // Pre-SPEC-001 spelling. The literal `name` IS the index's key, so it goes
    // out verbatim — only the namespace half is corrected.
    constexpr std::string_view legacy =
        R"(package = { namespace = "compat", name = "compat.zlib", version = "1.3.2" })";
    auto addr = mcpp::manifest::xpkg_wire_address(legacy, "mcpplibs", "zlib");
    expect(eq(addr.ns, "compat"));
    expect(eq(addr.name, "compat.zlib"));
    expect(eq(addr.target, "compat:compat.zlib"));
}};

UTest qualified_request_is_unchanged{"XpkgWireAddress/QualifiedRequestIsUnchanged", [] {
    // `[dependencies.compat] gtest = ...` already worked (this is the spelling
    // every mcpp-index example uses, which is why the index CI stayed green).
    constexpr std::string_view compatGtest =
        R"(package = { namespace = "compat", name = "gtest", version = "1.15.2" })";
    auto addr = mcpp::manifest::xpkg_wire_address(compatGtest, "compat", "gtest");
    expect(eq(addr.target, "compat:gtest"));
}};

UTest nested_namespace_is_unchanged{"XpkgWireAddress/NestedNamespaceIsUnchanged", [] {
    constexpr std::string_view capiLua =
        R"(package = { namespace = "mcpplibs.capi", name = "lua", version = "5.4.7" })";
    auto addr = mcpp::manifest::xpkg_wire_address(capiLua, "mcpplibs.capi", "lua");
    expect(eq(addr.target, "mcpplibs.capi:lua"));
}};

UTest descriptor_without_namespace_inherits_the_request{"XpkgWireAddress/DescriptorWithoutNamespaceInheritsTheRequest", [] {
    // A no-namespace descriptor inherits the namespace owned by the index it
    // was found in — at this seam, the request coordinate. Same rule the
    // identity gate used to accept it.
    constexpr std::string_view bare = R"(package = { name = "cmdline" })";
    auto addr = mcpp::manifest::xpkg_wire_address(bare, "mcpplibs", "cmdline");
    expect(eq(addr.ns, "mcpplibs"));
    expect(eq(addr.target, "mcpplibs:cmdline"));
}};

UTest namespaceless_request_addresses_the_bare_literal{"XpkgWireAddress/NamespacelessRequestAddressesTheBareLiteral", [] {
    // xim upstream packages (`opencv`) have no namespace on either side.
    constexpr std::string_view opencv = R"(package = { name = "opencv" })";
    auto addr = mcpp::manifest::xpkg_wire_address(opencv, "", "opencv");
    expect(eq(addr.ns, ""));
    expect(eq(addr.target, "opencv"));
}};

UTest no_descriptor_keeps_the_historical_derivation{"XpkgWireAddress/NoDescriptorKeepsTheHistoricalDerivation", [] {
    // Index not synced yet: mcpp has nothing to read, so it must NOT invent a
    // new address — the failure has to stay exactly where it was.
    auto addr = mcpp::manifest::xpkg_wire_address("", "mcpplibs", "gtest");
    expect(eq(addr.name, "mcpplibs.gtest"));
    expect(eq(addr.target, "mcpplibs:mcpplibs.gtest"));

    auto bare = mcpp::manifest::xpkg_wire_address("", "", "opencv");
    expect(eq(bare.target, "opencv"));
}};

UTest descriptor_without_name_falls_back_to_derivation{"XpkgWireAddress/DescriptorWithoutNameFallsBackToDerivation", [] {
    // A descriptor that declares no `name` cannot supply a wire key (the
    // identity gate accepts it leniently, so it can reach here).
    constexpr std::string_view noName = R"(package = { version = "1.0.0" })";
    auto addr = mcpp::manifest::xpkg_wire_address(noName, "mcpplibs", "gtest");
    expect(eq(addr.target, "mcpplibs:mcpplibs.gtest"));
}};

// ─── INV-NAME: xpkg_name_form_violation (#278) ──────────────────────
//
// `package.name` is a SINGLE ATOMIC SEGMENT; all hierarchy lives in
// `package.namespace` (SPEC-001 §3.2). The legacy fully-qualified spelling
// (`namespace="compat", name="compat.zlib"`) stays accepted for compatibility.

UTest short_name_is_canonical{"XpkgNameForm/ShortNameIsCanonical", [] {
    // SPEC-001 form: namespace carries the path, name is one segment.
    expect(!(mcpp::manifest::xpkg_name_form_violation("chriskohlhoff", "asio")
                     .has_value()));
    expect(!(mcpp::manifest::xpkg_name_form_violation("mcpplibs.capi", "lua")
                     .has_value()));
}};

UTest fqn_form_is_clean{"XpkgNameForm/FqnFormIsClean", [] {
    expect(!(mcpp::manifest::xpkg_name_form_violation(
        "chriskohlhoff", "chriskohlhoff.asio").has_value()));
}};

UTest no_namespace_is_clean{"XpkgNameForm/NoNamespaceIsClean", [] {
    // xim upstream packages declare no namespace and are keyed by the bare
    // name — legal, and outside INV-NAME's scope.
    expect(!(mcpp::manifest::xpkg_name_form_violation("", "opencv")
                     .has_value()));
}};

UTest no_name_is_clean{"XpkgNameForm/NoNameIsClean", [] {
    // Cannot verify → lenient, matching the identity gate's stance.
    expect(!(mcpp::manifest::xpkg_name_form_violation("compat", "")
                     .has_value()));
}};

UTest compat_alias_is_clean{"XpkgNameForm/CompatAliasIsClean", [] {
    // Legacy descriptors repeat the declared namespace in package.name. That
    // source shape remains valid even though consumers now select compat
    // explicitly.
    expect(!(mcpp::manifest::xpkg_name_form_violation("compat", "compat.gtest")
                     .has_value()));
}};

UTest nested_namespace_is_clean{"XpkgNameForm/NestedNamespaceIsClean", [] {
    expect(!(mcpp::manifest::xpkg_name_form_violation(
        "mcpplibs.capi", "mcpplibs.capi.lua").has_value()));
}};

UTest prefix_without_short_segment_is_a_violation{"XpkgNameForm/PrefixWithoutShortSegmentIsAViolation", [] {
    // `name` equal to the namespace itself carries no short segment.
    expect(mcpp::manifest::xpkg_name_form_violation("compat", "compat.")
                    .has_value());
}};

UTest from_lua_accepts_both_canonical_and_legacy{"XpkgNameForm/FromLuaAcceptsBothCanonicalAndLegacy", [] {
    constexpr std::string_view shortLua =
        R"(package = { namespace = "aimol", name = "tensorvia-cpu" })";
    constexpr std::string_view legacyFqnLua =
        R"(package = { namespace = "aimol", name = "aimol.tensorvia-cpu" })";
    // Canonical short-name form and the legacy FQN spelling both resolve to
    // the same identity, so both are accepted.
    expect(!(mcpp::manifest::xpkg_name_form_violation_from_lua(shortLua).has_value()));
    expect(!(mcpp::manifest::xpkg_name_form_violation_from_lua(legacyFqnLua).has_value()));
    // But a short name that still carries a dot is rejected.
    constexpr std::string_view nonAtomic =
        R"(package = { namespace = "mcpplibs", name = "capi.lua" })";
    expect(mcpp::manifest::xpkg_name_form_violation_from_lua(nonAtomic).has_value());
}};

// ─── canonical_xpkg_identity — the unified (ns, name) model (§4.2) ───
//
// Identity is a 2-tuple: ns is a hierarchical namespace path, name is a single
// atomic segment. Every surface spelling normalizes to this tuple.

namespace {
mcpp::manifest::XpkgIdentity id(std::string_view ns, std::string_view name,
                                std::string_view indexNs = {}) {
    return mcpp::manifest::canonical_xpkg_identity(ns, name, indexNs);
}
mcpp::manifest::XpkgIdentity want(std::string n, std::string s) {
    return mcpp::manifest::XpkgIdentity{std::move(n), std::move(s)};
}
}  // namespace

UTest prefix_embedded_name_collapses{"CanonicalIdentity/PrefixEmbeddedNameCollapses", [] {
    // ns=compat, name=compat.zlib  →  (compat, zlib)
    expect(eq(id("compat", "compat.zlib"), want("compat", "zlib")));
}};

UTest bare_name_combines_with_namespace{"CanonicalIdentity/BareNameCombinesWithNamespace", [] {
    // ns=mcpplibs, name=cmdline  →  (mcpplibs, cmdline)
    expect(eq(id("mcpplibs", "cmdline"), want("mcpplibs", "cmdline")));
}};

UTest already_qualified_name_is_idempotent{"CanonicalIdentity/AlreadyQualifiedNameIsIdempotent", [] {
    expect(eq(id("mcpplibs", "mcpplibs.cmdline"), want("mcpplibs", "cmdline")));
}};

UTest no_namespace_inherits_owning_index{"CanonicalIdentity/NoNamespaceInheritsOwningIndex", [] {
    // ns=∅, name=zlib, index=xim  →  (xim, zlib)
    expect(eq(id("", "zlib", "xim"), want("xim", "zlib")));
    expect(eq(id("", "tinycfg", "local-dev"), want("local-dev", "tinycfg")));
}};

UTest declared_namespace_wins_over_index_default{"CanonicalIdentity/DeclaredNamespaceWinsOverIndexDefault", [] {
    expect(eq(id("compat", "compat.zlib", "xim"), want("compat", "zlib")));
}};

UTest dotted_name_with_no_namespace_is_not_reinterpreted{"CanonicalIdentity/DottedNameWithNoNamespaceIsNotReinterpreted", [] {
    // SPEC-001: identity is exactly what the descriptor declares. A dotted
    // `name` with no namespace is NOT silently split into one — that inferred
    // a namespace nobody wrote. It stays literal here and is rejected by
    // xpkg_name_form_violation instead.
    expect(eq(id("", "a.b"), want("", "a.b")));
    expect(eq(id("", "x.y.z"), want("", "x.y.z")));
    expect(mcpp::manifest::xpkg_name_form_violation("", "a.b").has_value());
}};

UTest hierarchical_namespace_is_supported{"CanonicalIdentity/HierarchicalNamespaceIsSupported", [] {
    // ns=a.b, name=c  →  (a.b, c) ; name is the single trailing segment.
    expect(eq(id("a.b", "c"), want("a.b", "c")));
    expect(eq(id("mcpplibs.capi", "lua"), want("mcpplibs.capi", "lua")));
    // Legacy FQN spelling under the SAME namespace strips the prefix.
    expect(eq(id("mcpplibs.capi", "mcpplibs.capi.lua"), want("mcpplibs.capi", "lua")));
    // But a shorter declared namespace no longer "absorbs" the extra segment:
    // the descriptor said mcpplibs, so the identity is mcpplibs — the leftover
    // dot makes it a form violation rather than a silent (mcpplibs.capi, lua).
    expect(eq(id("mcpplibs", "mcpplibs.capi.lua"), want("mcpplibs", "capi.lua")));
    expect(mcpp::manifest::xpkg_name_form_violation(
        "mcpplibs", "mcpplibs.capi.lua").has_value());
}};

UTest bare_name_no_namespace_no_index_stays_rootless{"CanonicalIdentity/BareNameNoNamespaceNoIndexStaysRootless", [] {
    // The incident villain: a foreign bare descriptor with nothing to anchor it.
    expect(eq(id("", "zlib"), want("", "zlib")));
}};

UTest from_lua_reads_declared_fields{"CanonicalIdentity/FromLuaReadsDeclaredFields", [] {
    constexpr std::string_view lua =
        R"(package = { namespace = "compat", name = "compat.zlib" })";
    expect(eq(mcpp::manifest::canonical_xpkg_identity_from_lua(lua), want("compat", "zlib")));
    // No-namespace descriptor + owning index.
    constexpr std::string_view bare = R"(package = { name = "tinycfg" })";
    expect(eq(mcpp::manifest::canonical_xpkg_identity_from_lua(bare, "local-dev"), want("local-dev", "tinycfg")));
    // No declared name → empty identity.
    constexpr std::string_view noName = R"(package = { version = "1.0" })";
    expect(eq(mcpp::manifest::canonical_xpkg_identity_from_lua(noName), want("", "")));
}};

// ── Descriptor grammar v2: Lua long-bracket strings + block comments ──
// Design: .agents/docs/2026-07-08-index-version-semantics-and-descriptor-grammar-design.md (D1)

UTest generated_files_multiline{"XpkgLongBracket/GeneratedFilesMultiline", [] {
    // Content carries braces, quotes, an embedded `]]` (level-2 brackets),
    // and a leading newline that Lua semantics strip.
    constexpr auto lua = R"(
package = {
    spec = "1",
    name = "lbtest",
    xpm  = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        sources = { "mcpp_generated/gen.cc" },
        generated_files = {
            ["mcpp_generated/gen.cc"] = [==[
export module gen;
int f() { return "x]]y"[0]; }
]==],
        },
        targets = { ["gen"] = { kind = "lib" } },
    },
}
)";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(lua, "lbtest", "1.0.0", mcpp::toolchain::platform::HostPlatform::current());
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(m->buildConfig.generatedFiles.contains("mcpp_generated/gen.cc")));
    expect(eq(m->buildConfig.generatedFiles["mcpp_generated/gen.cc"], "export module gen;\nint f() { return \"x]]y\"[0]; }\n"));
}};

UTest block_comment_with_braces_and_fake_mcpp{"XpkgLongBracket/BlockCommentWithBracesAndFakeMcpp", [] {
    // A block comment containing braces and the literal text `mcpp = {`
    // must neither corrupt the structural depth count nor false-match
    // the segment locator.
    constexpr auto lua = R"(
--[==[
decoy: mcpp = { sources = { "WRONG" } } }}}}
]==]
package = {
    spec = "1",
    name = "cmt",
    xpm  = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        --[[ inline block comment { with braces } ]]
        sources = { [[src/*.cpp]] },
        targets = { ["cmt"] = { kind = "lib" } },
    },
}
)";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(lua, "cmt", "1.0.0", mcpp::toolchain::platform::HostPlatform::current());
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->modules.sources.size(), 1u)));
    expect(eq(m->modules.sources[0], "src/*.cpp"));
}};

UTest no_leading_newline_kept_verbatim{"XpkgLongBracket/NoLeadingNewlineKeptVerbatim", [] {
    // Without a newline after the opening bracket, content starts
    // immediately; CRLF right after the bracket is stripped as one break.
    constexpr auto lua =
        "package = {\n"
        "    spec = \"1\",\n"
        "    name = \"nl\",\n"
        "    xpm  = { linux = { [\"1.0.0\"] = { url = \"u\", sha256 = \"h\" } } },\n"
        "    mcpp = {\n"
        "        sources = { \"g.cc\" },\n"
        "        generated_files = { [\"g.cc\"] = [[inline]], [\"h.cc\"] = [[\r\ncrlf]] },\n"
        "        targets = { [\"nl\"] = { kind = \"lib\" } },\n"
        "    },\n"
        "}\n";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(lua, "nl", "1.0.0", mcpp::toolchain::platform::HostPlatform::current());
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(eq(m->buildConfig.generatedFiles["g.cc"], "inline"));
    expect(eq(m->buildConfig.generatedFiles["h.cc"], "crlf"));
}};

UTest unknown_keys_recorded_schema_accepted{"XpkgLongBracket/UnknownKeysRecordedSchemaAccepted", [] {
    constexpr auto lua = R"(
package = {
    spec = "1",
    name = "uk",
    xpm  = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        schema  = "0.1",
        sources = { "a.c" },
        bogus_key = "x",
        future_table = { nested = { 1, 2 } },
        targets = { ["uk"] = { kind = "lib" } },
    },
}
)";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(lua, "uk", "1.0.0", mcpp::toolchain::platform::HostPlatform::current());
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->xpkgUnknownKeys.size(), 2u)));
    expect(eq(m->xpkgUnknownKeys[0], "bogus_key"));
    expect(eq(m->xpkgUnknownKeys[1], "future_table"));
}};

// ── scan_overrides: author-asserted scan results ──
// Design: .agents/docs/2026-07-08-scanner-backend-abstraction-design.md §3-pre

UTest xpkg_segment_parses{"ScanOverrides/XpkgSegmentParses", [] {
    constexpr auto lua = R"(
package = {
    spec = "1",
    namespace = "fmtlib",
    name = "fmtlib.fmt",
    xpm  = { linux = { ["12.2.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        sources  = { "*/src/fmt.cc" },
        cxxflags = { "-DFMT_IMPORT_STD" },
        scan_overrides = {
            ["*/src/fmt.cc"] = { provides = { "fmt" }, imports = { "std" } },
        },
        targets  = { ["fmt"] = { kind = "lib" } },
    },
}
)";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(lua, "fmtlib.fmt", "12.2.0", mcpp::toolchain::platform::HostPlatform::current());
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->modules.scanOverrides.size(), 1u)));
    auto& ov = m->modules.scanOverrides.at("*/src/fmt.cc");
    expect(fatal(eq(ov.provides.size(), 1u)));
    expect(eq(ov.provides[0], "fmt"));
    expect(fatal(eq(ov.imports.size(), 1u)));
    expect(eq(ov.imports[0], "std"));
    expect(m->xpkgUnknownKeys.empty());
}};

UTest xpkg_rejects_empty_declaration{"ScanOverrides/XpkgRejectsEmptyDeclaration", [] {
    constexpr auto lua = R"(
package = {
    spec = "1",
    name = "so",
    xpm  = { linux = { ["1.0.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        sources = { "a.cc" },
        scan_overrides = { ["a.cc"] = { } },
        targets = { ["so"] = { kind = "lib" } },
    },
}
)";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(lua, "so", "1.0.0", mcpp::toolchain::platform::HostPlatform::current());
    expect(fatal(!m.has_value()));
    if (auto&& failed = expect(ne(m.error().message.find("neither provides nor imports"), std::string::npos)); !failed) failed << m.error().format();
}};

UTest toml_parses{"ScanOverrides/TomlParses", [] {
    constexpr auto src = R"(
[package]
name = "app"
version = "0.1.0"
[language]
standard = "c++23"
[modules]
sources = ["src/**/*.cppm", "vendor/fmt.cc"]
["scan_overrides"."vendor/fmt.cc"]
provides = ["fmt"]
imports  = ["std"]
[targets.app]
kind = "bin"
main = "src/main.cpp"
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->modules.scanOverrides.size(), 1u)));
    auto& ov = m->modules.scanOverrides.at("vendor/fmt.cc");
    expect(eq(ov.provides, std::vector<std::string>{"fmt"}));
    expect(eq(ov.imports, std::vector<std::string>{"std"}));
}};

// ── declarative source-set parity: mcpp.toml ↔ xpkg descriptor ───────────────
// One data model, two grammars (manifest.cppm): what a descriptor can express
// through `features.<f>.sources` / `generated_files`, mcpp.toml must express
// identically. This test locks the symmetry as a contract.

UTest feature_sources_parse_from_toml{"Manifest/FeatureSourcesParseFromToml", [] {
    constexpr auto src = R"(
[package]
name = "sym"
version = "1.0.0"
[features]
default = []
compiled = { sources = ["src/impl/**"], defines = ["SYM_COMPILED"] }
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(m->buildConfig.featureSources.contains("compiled")));
    expect(eq(m->buildConfig.featureSources.at("compiled"), (std::vector<std::string>{"src/impl/**"})));
    expect(fatal(m->buildConfig.featureDefines.contains("compiled")));
}};

UTest generated_files_parse_from_toml{"Manifest/GeneratedFilesParseFromToml", [] {
    constexpr auto src = R"(
[package]
name = "genf"
version = "1.0.0"

[generated_files]
"src/gen/wrap.cppm" = """
module;
export module wrap;
"""
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->buildConfig.generatedFiles.size(), 1u)));
    auto it = m->buildConfig.generatedFiles.find(
        std::filesystem::path("src/gen/wrap.cppm"));
    expect(fatal(ne(it, m->buildConfig.generatedFiles.end())));
    expect(eq(it->second, "module;\nexport module wrap;\n"));
}};

UTest generated_files_reject_escaping_paths{"Manifest/GeneratedFilesRejectEscapingPaths", [] {
    for (auto path : { "\"../evil.cpp\"", "\"/abs/evil.cpp\"", "\"a/../../evil.cpp\"" }) {
        auto src = std::format(R"(
[package]
name = "genf"
version = "1.0.0"
[generated_files]
{} = "x"
)", path);
        auto m = mcpp::manifest::parse_string(src);
        expect(!m.has_value()) << "path accepted: " << path;
    }
}};

UTest toml_and_xpkg_feature_sources_are_symmetric{"Manifest/TomlAndXpkgFeatureSourcesAreSymmetric", [] {
    constexpr auto toml = R"(
[package]
name = "sym"
version = "1.0.0"
[features]
compiled = { sources = ["src/impl/**"], defines = ["SYM_COMPILED"] }
[generated_files]
"src/gen/wrap.cppm" = "module;\nexport module wrap;\n"
)";
    constexpr auto lua = R"(
package = { name = "sym" }
mcpp = {
    sources = { "src/**/*.cpp" },
    features = {
        compiled = { sources = { "src/impl/**" }, defines = { "SYM_COMPILED" } },
    },
    generated_files = {
        ["src/gen/wrap.cppm"] = "module;\nexport module wrap;\n",
    },
}
)";
    auto mt = mcpp::manifest::parse_string(toml);
    if (auto&& failed = expect(fatal(mt.has_value())); !failed) failed << mt.error().format();
    auto mx = mcpp::manifest::synthesize_from_xpkg_lua(lua, "sym", "1.0.0", mcpp::toolchain::platform::HostPlatform::current());
    if (auto&& failed = expect(fatal(mx.has_value())); !failed) failed << mx.error().format();

    expect(eq(mt->buildConfig.featureSources, mx->buildConfig.featureSources));
    expect(eq(mt->buildConfig.featureDefines, mx->buildConfig.featureDefines));
    expect(eq(mt->buildConfig.generatedFiles, mx->buildConfig.generatedFiles));
}};

UTest cfg_conditional_sources_parse_from_toml{"Manifest/CfgConditionalSourcesParseFromToml", [] {
    constexpr auto src = R"(
[package]
name = "condsrc"
version = "1.0.0"

[target.'cfg(arch = "x86_64")'.build]
sources  = ["src/x86/**/*.asm", "!src/x86/legacy/**"]
cxxflags = ["-DHAVE_X86=1"]
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->conditionalConfigs.size(), 1u)));
    auto& cc = m->conditionalConfigs[0];
    expect(eq(cc.predicate, "cfg(arch = \"x86_64\")"));
    expect(eq(cc.inputs.sources, (std::vector<std::string>{
        "src/x86/**/*.asm", "!src/x86/legacy/**"})));
    expect(eq(cc.inputs.cxxflags, (std::vector<std::string>{"-DHAVE_X86=1"})));
}};

UTest target_cfg_parses_from_xpkg_symmetrically{"Manifest/TargetCfgParsesFromXpkgSymmetrically", [] {
    constexpr auto lua = R"(
package = { name = "condsrc" }
mcpp = {
    sources = { "src/**/*.c" },
    target_cfg = {
        ['cfg(arch = "x86_64")'] = {
            sources  = { "src/x86/**/*.asm" },
            cxxflags = { "-DHAVE_X86=1" },
        },
        ['cfg(windows)'] = {
            ldflags = { "-lws2_32" },
        },
    },
}
)";
    auto mx = mcpp::manifest::synthesize_from_xpkg_lua(lua, "condsrc", "1.0.0", mcpp::toolchain::platform::HostPlatform::current());
    if (auto&& failed = expect(fatal(mx.has_value())); !failed) failed << mx.error().format();
    expect(fatal(eq(mx->conditionalConfigs.size(), 2u)));
    expect(eq(mx->conditionalConfigs[0].predicate, "cfg(arch = \"x86_64\")"));
    expect(eq(mx->conditionalConfigs[0].inputs.sources, (std::vector<std::string>{"src/x86/**/*.asm"})));
    expect(eq(mx->conditionalConfigs[1].predicate, "cfg(windows)"));
    expect(eq(mx->conditionalConfigs[1].inputs.ldflags, (std::vector<std::string>{"-lws2_32"})));
}};

UTest per_glob_flags_parse_ordered_from_toml{"Manifest/PerGlobFlagsParseOrderedFromToml", [] {
    constexpr auto src = R"(
[package]
name = "globf"
version = "1.0.0"

[build]
flags = [
  { glob = "third_party/**", cflags = ["-w"], cxxflags = ["-w"] },
  { glob = "src/simd/**/*.avx2.cpp", cxxflags = ["-mavx2"], defines = ["HAVE_AVX2"] },
  { glob = "src/x86/**/*.asm", asmflags = ["-DPIC"] },
]
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    auto& gfs = m->buildConfig.globFlags;
    expect(fatal(eq(gfs.size(), 3u)));
    // Declaration order IS the data (override semantics ride on it).
    expect(eq(gfs[0].glob, "third_party/**"));
    expect(eq(gfs[0].cxxflags, (std::vector<std::string>{"-w"})));
    expect(eq(gfs[1].glob, "src/simd/**/*.avx2.cpp"));
    expect(eq(gfs[1].defines, (std::vector<std::string>{"HAVE_AVX2"})));
    expect(eq(gfs[2].asmflags, (std::vector<std::string>{"-DPIC"})));
}};

UTest per_glob_flags_reject_unknown_keys_and_missing_glob{"Manifest/PerGlobFlagsRejectUnknownKeysAndMissingGlob", [] {
    auto bad1 = mcpp::manifest::parse_string(R"(
[package]
name = "globf"
version = "1.0.0"
[build]
flags = [{ glob = "src/**", ldflags = ["-lfoo"] }]
)");
    expect(!bad1.has_value());   // ldflags is not per-TU — closed grammar

    auto bad2 = mcpp::manifest::parse_string(R"(
[package]
name = "globf"
version = "1.0.0"
[build]
flags = [{ cxxflags = ["-w"] }]
)");
    expect(!bad2.has_value());   // missing glob
}};

UTest per_glob_flags_parse_from_xpkg_symmetrically{"Manifest/PerGlobFlagsParseFromXpkgSymmetrically", [] {
    constexpr auto lua = R"(
package = { name = "globf" }
mcpp = {
    sources = { "src/**/*.cpp" },
    flags = {
        { glob = "third_party/**", cxxflags = { "-w" } },
        { glob = "src/simd/**", cxxflags = { "-mavx2" }, defines = { "HAVE_AVX2" } },
    },
}
)";
    auto mx = mcpp::manifest::synthesize_from_xpkg_lua(lua, "globf", "1.0.0", mcpp::toolchain::platform::HostPlatform::current());
    if (auto&& failed = expect(fatal(mx.has_value())); !failed) failed << mx.error().format();
    expect(fatal(eq(mx->buildConfig.globFlags.size(), 2u)));
    expect(eq(mx->buildConfig.globFlags[0].glob, "third_party/**"));
    expect(eq(mx->buildConfig.globFlags[1].cxxflags, (std::vector<std::string>{"-mavx2"})));
    expect(eq(mx->buildConfig.globFlags[1].defines, (std::vector<std::string>{"HAVE_AVX2"})));
}};

UTest allows_build_flags_array_of_tables_syntax{"Manifest/AllowsBuildFlagsArrayOfTablesSyntax", [] {
    // `[[build.flags]]` header form must still parse at the manifest layer
    // (not just the lower libs/toml layer) — the closed-grammar guard must
    // allowlist exactly this path.
    constexpr auto src = R"(
[package]
name = "globf"
version = "1.0.0"

[[build.flags]]
glob = "third_party/**"
cxxflags = ["-w"]

[[build.flags]]
glob = "src/x86/**/*.asm"
asmflags = ["-DPIC"]
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    auto& gfs = m->buildConfig.globFlags;
    expect(fatal(eq(gfs.size(), 2u)));
    expect(eq(gfs[0].glob, "third_party/**"));
    expect(eq(gfs[0].cxxflags, (std::vector<std::string>{"-w"})));
    expect(eq(gfs[1].glob, "src/x86/**/*.asm"));
    expect(eq(gfs[1].asmflags, (std::vector<std::string>{"-DPIC"})));
}};

// Closed-grammar guard (review fix on #227/#228): `[[dependencies]]` is a
// doubled-bracket typo for `[dependencies]`. Before this guard it parsed
// silently as an array-of-tables at that path; every consumer reads
// [dependencies] via get_table(), which returns nullptr for a non-table
// Value, so the whole section (all dependencies) silently vanished with no
// error and no warning. It must now be a loud parse error.
UTest rejects_array_of_tables_typo_on_dependencies{"Manifest/RejectsArrayOfTablesTypoOnDependencies", [] {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "x"
version = "0.1.0"

[[dependencies]]
gtest = "1.15.2"
)");
    expect(fatal(!m.has_value()));
    if (auto&& failed = expect(ne(m.error().message.find("array-of-tables"), std::string::npos)); !failed) failed << m.error().message;
    if (auto&& failed = expect(ne(m.error().message.find("dependencies"), std::string::npos)); !failed) failed << m.error().message;
}};

// Same guard, a different non-allowlisted path — confirms the check isn't
// hardcoded to just "dependencies".
UTest rejects_array_of_tables_typo_on_toolchain{"Manifest/RejectsArrayOfTablesTypoOnToolchain", [] {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "x"
version = "0.1.0"

[[toolchain]]
linux = "gcc@15.1.0"
)");
    expect(fatal(!m.has_value()));
    if (auto&& failed = expect(ne(m.error().message.find("array-of-tables"), std::string::npos)); !failed) failed << m.error().message;
}};

// #258: `[target.'cfg(<os>)'.build]` accepts the same per-glob `flags` array
// `[build]` does. Before this, the conditional reader knew exactly four keys
// and dropped everything else silently, so a per-OS flag table was
// inexpressible in mcpp.toml while the xpkg descriptor's mcpp.<os> sections
// supported it — manifest-built packages were second-class.
UTest conditional_section_carries_per_glob_flags{"Manifest/ConditionalSectionCarriesPerGlobFlags", [] {
    auto tmp = std::filesystem::temp_directory_path() / "mcpp_cond_flags";
    std::filesystem::create_directories(tmp);
    auto path = tmp / "mcpp.toml";
    {
        std::ofstream os(path);
        os << R"(
[package]
name = "condflags"
version = "0.1.0"

[build]
flags = [{ glob = "src/zlib/**", defines = ["HAVE_UNISTD_H=1"] }]

[target.'cfg(windows)'.build]
cflags = ["-DWIN32"]
include_dirs = ["vendor/win/include"]
flags = [
  { glob = "src/zlib/**", defines = ["NO_FSEEKO"], cflags = ["-UHAVE_UNISTD_H"] },
]
)";
    }
    auto m = mcpp::manifest::load(path);
    if (auto&& failed = expect(fatal(bool(m))); !failed) failed << (m ? "" : m.error().message);

    // Base table is unconditional.
    expect(fatal(eq(m->buildConfig.globFlags.size(), 1u)));
    expect(eq(m->buildConfig.globFlags[0].glob, "src/zlib/**"));

    expect(fatal(eq(m->conditionalConfigs.size(), 1u)));
    auto& cc = m->conditionalConfigs[0];
    expect(eq(cc.predicate, "cfg(windows)"));
    expect(fatal(eq(cc.inputs.globFlags.size(), 1u)));
    expect(eq(cc.inputs.globFlags[0].glob, "src/zlib/**"));
    expect(eq(cc.inputs.globFlags[0].defines, (std::vector<std::string>{"NO_FSEEKO"})));
    // The removal case: a windows-only -U that must land AFTER the base -D.
    expect(eq(cc.inputs.globFlags[0].cflags, (std::vector<std::string>{"-UHAVE_UNISTD_H"})));
    expect(fatal(eq(cc.inputs.includeDirs.size(), 1u)));
    expect(eq(cc.inputs.includeDirs[0], std::filesystem::path("vendor/win/include")));
    std::filesystem::remove_all(tmp);
}};

// The array-of-tables spelling must reach the same place, and must be
// allowlisted by the #227 closed-grammar guard.
UTest conditional_per_glob_flags_accept_array_of_tables_spelling{"Manifest/ConditionalPerGlobFlagsAcceptArrayOfTablesSpelling", [] {
    auto tmp = std::filesystem::temp_directory_path() / "mcpp_cond_flags_aot";
    std::filesystem::create_directories(tmp);
    auto path = tmp / "mcpp.toml";
    {
        std::ofstream os(path);
        os << R"(
[package]
name = "condflagsaot"
version = "0.1.0"

[[target.'cfg(linux)'.build.flags]]
glob = "src/**"
defines = ["ON_LINUX=1"]
)";
    }
    auto m = mcpp::manifest::load(path);
    if (auto&& failed = expect(fatal(bool(m))); !failed) failed << (m ? "" : m.error().message);
    expect(fatal(eq(m->conditionalConfigs.size(), 1u)));
    expect(fatal(eq(m->conditionalConfigs[0].inputs.globFlags.size(), 1u)));
    expect(eq(m->conditionalConfigs[0].inputs.globFlags[0].defines, (std::vector<std::string>{"ON_LINUX=1"})));
    std::filesystem::remove_all(tmp);
}};

// Parity: the xpkg descriptor's target_cfg gets the same key, and its
// unknown-key policy stays a HARD ERROR. Sharing a parser must not be read
// as licence to relax a grammar that was already closed (#263 tracks making
// the policies consistent across sections; this test pins that #258 did not
// quietly change one).
UTest xpkg_target_cfg_carries_per_glob_flags_and_still_rejects_unknown_keys{"Manifest/XpkgTargetCfgCarriesPerGlobFlagsAndStillRejectsUnknownKeys", [] {
    // Custom delimiter: the descriptor contains `)"` inside ["cfg(windows)"],
    // which would terminate a plain R"( ... )".
    const char* lua = R"LUA(
mcpp = {
    sources = { "src/**" },
    target_cfg = {
        ["cfg(windows)"] = {
            cflags = { "-DWIN32" },
            flags = { { glob = "src/z/**", defines = { "NO_FSEEKO" } } },
        },
    },
}
)LUA";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(lua, "p", "1.0.0", mcpp::toolchain::platform::HostPlatform::current());
    if (auto&& failed = expect(fatal(bool(m))); !failed) failed << (m ? "" : m.error().message);
    expect(fatal(eq(m->conditionalConfigs.size(), 1u)));
    expect(fatal(eq(m->conditionalConfigs[0].inputs.globFlags.size(), 1u)));
    expect(eq(m->conditionalConfigs[0].inputs.globFlags[0].glob, "src/z/**"));

    const char* bad = R"LUA(
mcpp = {
    sources = { "src/**" },
    target_cfg = { ["cfg(windows)"] = { linkage = { "static" } } },
}
)LUA";
    auto bm = mcpp::manifest::synthesize_from_xpkg_lua(bad, "p", "1.0.0", mcpp::toolchain::platform::HostPlatform::current());
    expect(!bm) << "unknown target_cfg key must stay a hard error";
}};

// #254: the host and target platform axes are distinct types, so an API
// states which one it wants and a call site must name the one it supplies.
// Before this, both were std::string_view and mixing them cost nothing —
// which is exactly how every xpkg per-OS splice ended up keyed on a
// compile-time HOST constant while [target.'cfg(...)'] was evaluated against
// the resolved TARGET.
UTest host_and_target_are_not_interchangeable{"PlatformAxis/HostAndTargetAreNotInterchangeable", [] {
    using mcpp::toolchain::platform::HostPlatform;
    using mcpp::toolchain::platform::TargetPlatform;

    static_assert(!std::is_convertible_v<HostPlatform, TargetPlatform>,
                  "the host axis must not silently become the target axis");
    static_assert(!std::is_convertible_v<TargetPlatform, HostPlatform>,
                  "the target axis must not silently become the host axis");
    // And neither is constructible from a bare string: passing "linux"
    // where an axis is expected is the bug class this type exists to stop.
    static_assert(!std::is_constructible_v<TargetPlatform, std::string_view>);
    static_assert(!std::is_constructible_v<HostPlatform, std::string_view>);

    // The triple vocabulary says "macos"; xpkg descriptors say "macosx".
    // That translation belongs to the axis, not to every call site.
    expect(eq(TargetPlatform::for_os("macos").key(), "macosx"));
    expect(eq(TargetPlatform::for_os("macosx").key(), "macosx"));
    expect(eq(TargetPlatform::for_os("windows").key(), "windows"));
    expect(eq(TargetPlatform::for_os("linux").key(), "linux"));
    // A token naming none of the three reads its own block, never the
    // host's (design 2026-10-29 §5.1, #851).
    expect(eq(TargetPlatform::for_os("emscripten").key(), "emscripten"));
    expect(eq(TargetPlatform::for_os("none").key(), "none"));
}};

// The behaviour half: a descriptor's per-OS section must be spliced for the
// TARGET. Native builds cannot observe this (host == target), which is why
// three-platform CI never caught it.
UTest xpkg_per_os_section_splices_for_the_target_not_the_host{"Manifest/XpkgPerOsSectionSplicesForTheTargetNotTheHost", [] {
    const char* lua = R"LUA(
mcpp = {
    sources = { "base.cpp" },
    linux   = { sources = { "linux_only.cpp" } },
    macosx  = { sources = { "macos_only.cpp" } },
    windows = { sources = { "windows_only.cpp" } },
}
)LUA";
    auto has = [](const mcpp::manifest::Manifest& m, std::string_view glob) {
        return std::ranges::find(m.modules.sources, glob) != m.modules.sources.end();
    };

    for (auto [tripleOs, expected] : std::initializer_list<
             std::pair<std::string_view, std::string_view>>{
             {"linux", "linux_only.cpp"},
             {"macos", "macos_only.cpp"},
             {"windows", "windows_only.cpp"}}) {
        auto m = mcpp::manifest::synthesize_from_xpkg_lua(
            lua, "p", "1.0.0", mcpp::toolchain::platform::TargetPlatform::for_os(tripleOs));
        if (auto&& failed = expect(fatal(bool(m))); !failed) failed << (m ? "" : m.error().message);
        expect(bool(has(*m, expected)))
            << "target os " << tripleOs << " must splice its own section";
        expect(bool(has(*m, "base.cpp")));
        // and must NOT carry the other legs
        for (std::string_view other : {"linux_only.cpp", "macos_only.cpp",
                                       "windows_only.cpp"}) {
            if (other == expected) continue;
            expect(!has(*m, other))
                << "target os " << tripleOs << " must not carry " << other;
        }
    }
}};

// Zero behaviour change for native builds is the safety net for #254: when
// host == target the splice must be byte-identical to what it always was.
UTest native_build_splice_is_unchanged{"Manifest/NativeBuildSpliceIsUnchanged", [] {
    const char* lua = R"LUA(
mcpp = {
    sources = { "base.cpp" },
    linux   = { sources = { "linux_only.cpp" }, cxxflags = { "-DL" } },
    macosx  = { sources = { "macos_only.cpp" }, cxxflags = { "-DM" } },
    windows = { sources = { "windows_only.cpp" }, cxxflags = { "-DW" } },
}
)LUA";
    auto host = mcpp::manifest::synthesize_from_xpkg_lua(
        lua, "p", "1.0.0", mcpp::toolchain::platform::HostPlatform::current());
    auto nativeTarget = mcpp::manifest::synthesize_from_xpkg_lua(
        lua, "p", "1.0.0",
        mcpp::toolchain::platform::TargetPlatform::for_lint_of(
            mcpp::toolchain::platform::HostPlatform::current().key()));
    expect(fatal(bool(host)));
    expect(fatal(bool(nativeTarget)));
    expect(eq(host->modules.sources, nativeTarget->modules.sources));
    expect(eq(host->buildConfig.cxxflags, nativeTarget->buildConfig.cxxflags));
}};

UTest indices_artifact_and_source_parsed_and_pin_suppresses{"Manifest/IndicesArtifactAndSourceParsedAndPinSuppresses", [] {
    // #269: [indices] entries may declare an xlings >= 0.4.68 artifact
    // source. rev/tag/branch pins force git (the artifact channel only
    // tracks the latest pointer), which artifact_applicable() encodes.
    constexpr auto src = R"(
[package]
name = "hello"
version = "0.1.0"
[language]
standard = "c++23"
[modules]
sources = ["src/**/*.cppm"]
[indices]
fast   = { url = "https://example.com/i.git", artifact = "https://example.com/res/i", source = "auto" }
pinned = { url = "https://example.com/i.git", artifact = "https://example.com/res/i", rev = "abc123" }
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    auto& fast = m->indices.at("fast");
    expect(eq(fast.artifact, "https://example.com/res/i"));
    expect(eq(fast.source, "auto"));
    expect(bool(fast.artifact_applicable()));
    auto& pinned = m->indices.at("pinned");
    expect(eq(pinned.artifact, "https://example.com/res/i"));
    expect(!pinned.artifact_applicable());
}};

// #359: `reexport = true` on a dependency edge — the knob that lets a library
// hand its build-time provisions (tools, host module, dependency dir) to its
// own consumers. Off by default, deliberately: the edge's `visibility` already
// defaults to "public", so riding that would have made every dependency at
// every depth able to push entries into a consumer's tool namespace silently.
UTest dependency_reexport_is_opt_in{"Manifest/DependencyReexportIsOptIn", [] {
    auto tmp = std::filesystem::temp_directory_path() / "mcpp_dep_reexport";
    std::filesystem::create_directories(tmp);
    auto path = tmp / "mcpp.toml";
    {
        std::ofstream os(path);
        os << R"(
[package]
name = "reexporter"
version = "0.1.0"

[dependencies.compat]
protobuf = { version = "35.1", tools = ["protoc"], reexport = true }
zlib     = { version = "1.3.1", tools = ["minigzip"] }
)";
    }
    auto m = mcpp::manifest::load(path);
    if (auto&& failed = expect(fatal(bool(m))); !failed) failed << (m ? "" : m.error().message);

    const mcpp::pm::DependencySpec* pb = nullptr;
    const mcpp::pm::DependencySpec* zl = nullptr;
    for (auto const& [k, spec] : m->dependencies) {
        if (spec.shortName == "protobuf") pb = &spec;
        if (spec.shortName == "zlib")     zl = &spec;
    }
    expect(fatal(ne(pb, nullptr)));
    expect(fatal(ne(zl, nullptr)));
    expect(bool(pb->reexport));
    expect(!zl->reexport);
}};

// #359 D3a: the conditional channel carried three of the four dependency maps
// and silently lacked `feature-deps`. A library that puts a host tool behind a
// feature has no other way to say "not on this platform", and an unconditional
// declaration turns an unsupported platform into a hard error raised from
// inside the LIBRARY's manifest, which its user cannot work around.
//
// The FEATURE is registered regardless of the predicate — only what it pulls
// in is conditional — so requesting it on a non-matching platform is not an
// unknown-feature error.
UTest conditional_feature_deps_are_parsed_and_the_feature_is_registered{"Manifest/ConditionalFeatureDepsAreParsedAndTheFeatureIsRegistered", [] {
    auto tmp = std::filesystem::temp_directory_path() / "mcpp_cond_feature_deps";
    std::filesystem::create_directories(tmp);
    auto path = tmp / "mcpp.toml";
    {
        std::ofstream os(path);
        os << R"(
[package]
name = "condfeatdeps"
version = "0.1.0"

[target.'cfg(not(windows))'.feature-deps.codegen]
"compat.protobuf" = { version = "35.1", tools = ["protoc"], reexport = true }
)";
    }
    auto m = mcpp::manifest::load(path);
    if (auto&& failed = expect(fatal(bool(m))); !failed) failed << (m ? "" : m.error().message);

    expect(m->featuresMap.contains("codegen"));
    // Not folded into the unconditional map: that happens in prepare, after
    // the predicate is evaluated against the RESOLVED target.
    expect(m->featureDeps.empty());

    expect(fatal(eq(m->conditionalConfigs.size(), 1u)));
    auto const& cc = m->conditionalConfigs[0];
    expect(eq(cc.predicate, "cfg(not(windows))"));
    expect(fatal(cc.featureDeps.contains("codegen")));
    expect(fatal(eq(cc.featureDeps.at("codegen").size(), 1u)));
    auto const& spec = cc.featureDeps.at("codegen").begin()->second;
    expect(eq(spec.shortName, "protobuf"));
    expect(fatal(eq(spec.tools.size(), 1u)));
    expect(eq(spec.tools[0], "protoc"));
    expect(bool(spec.reexport));
}};

// A dependency-spec key must be listed in TWO places in the TOML reader: the
// predicate that tells an inline spec from a nested namespace table, and the
// filler that reads the value. Miss the first and the diagnostic is actively
// misleading — the table is taken for a namespace and the user is told their
// bool "must be a string, inline dep table, or nested table". #359 hit exactly
// that while adding `reexport`.
//
// One manifest using every key at once is the cheapest way to keep the two in
// sync: a key added to the filler but not the predicate fails this test.
UTest every_dependency_spec_key_is_accepted{"Manifest/EveryDependencySpecKeyIsAccepted", [] {
    auto tmp = std::filesystem::temp_directory_path() / "mcpp_dep_spec_keys";
    std::filesystem::create_directories(tmp);
    auto path = tmp / "mcpp.toml";
    {
        std::ofstream os(path);
        os << R"(
[package]
name = "depspeckeys"
version = "0.1.0"

[dependencies.compat]
everything = { version = "1.0.0", features = ["x"], default-features = false, visibility = "private", backend = "openblas", tools = ["t"], artifacts = ["a"], host-module = true, reexport = true, linkage = "shared", patches = ["p/one.patch"], embed = true, exclude = ["src/prim.c"], override = true }
bygit      = { git = "https://example.invalid/x.git", tag = "v1", visibility = "interface" }
bypath     = { path = "../sibling" }
)";
    }
    auto m = mcpp::manifest::load(path);
    if (auto&& failed = expect(fatal(bool(m))); !failed) failed << (m ? "" : m.error().message);

    const mcpp::pm::DependencySpec* all = nullptr;
    for (auto const& [k, spec] : m->dependencies)
        if (spec.shortName == "everything") all = &spec;
    expect(fatal(ne(all, nullptr)));
    expect(eq(all->version, "1.0.0"));
    expect(eq(all->visibility, "private"));
    expect(!all->defaultFeatures);
    expect(bool(all->hostModule));
    expect(bool(all->reexport));
    expect(fatal(eq(all->tools.size(), 1u)));
    expect(eq(all->tools[0], "t"));
    expect(fatal(eq(all->artifacts.size(), 1u)));
    expect(eq(all->artifacts[0], "a"));
    // `backend = "openblas"` is sugar for requesting the backend-<impl> feature.
    expect(ne(std::find(all->features.begin(), all->features.end(), "backend-openblas"), all->features.end()));
    expect(eq(all->linkage, "shared"));
    expect(bool(all->override_));   // design 2026-11-10 supplement, T2
    // design 2026-11-02 §14.1: a patch is named relative to the manifest that
    // carries it, and read as an absolute path.
    expect(fatal(eq(all->patches.size(), 1u)));
    expect(eq(std::filesystem::path(all->patches[0]), (tmp / "p" / "one.patch").lexically_normal()));
    expect(bool(all->embed));
    expect(fatal(eq(all->exclude.size(), 1u)));
    expect(eq(all->exclude[0], "src/prim.c"));
}};

// design 2026-11-02 §14.1: `exclude` without `embed` would change what a
// library built on its own compiles for every consumer, which is a patch's to
// state; the manifest is refused, naming the edge.
UTest exclude_without_embed_is_refused{"Manifest/ExcludeWithoutEmbedIsRefused", [] {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "x"
version = "0.1.0"

[dependencies]
"compat.zlib" = { version = "1.3.2", exclude = ["gzlib.c"] }
)");
    expect(fatal(!m));
    if (auto&& failed = expect(ne(m.error().message.find("exclude is allowed only with embed"), std::string::npos)); !failed) failed << m.error().message;
}};

// ── #519: the dependency-form axis ────────────────────────────────────────

namespace {

std::expected<mcpp::manifest::Manifest, mcpp::manifest::ManifestError>
load_inline(std::string_view name, std::string_view body) {
    auto tmp = std::filesystem::temp_directory_path()
             / std::format("mcpp_{}_{}", name, std::random_device{}());
    std::filesystem::create_directories(tmp);
    auto path = tmp / "mcpp.toml";
    { std::ofstream os(path); os << body; }
    return mcpp::manifest::load(path);
}

} // namespace

UTest dependency_linkage_defaults_to_empty_meaning_static{"Manifest/DependencyLinkageDefaultsToEmptyMeaningStatic", [] {
    auto m = load_inline("deplinkage_default", R"(
[package]
name = "x"
version = "0.1.0"
)");
    expect(fatal(bool(m)));
    expect(m->buildConfig.dependencyLinkage.empty());
}};

UTest dependency_linkage_is_a_closed_vocabulary{"Manifest/DependencyLinkageIsAClosedVocabulary", [] {
    auto ok = load_inline("deplinkage_ok", R"(
[package]
name = "x"
version = "0.1.0"
[build]
dependency_linkage = "shared"
)");
    expect(fatal(bool(ok)));
    expect(eq(ok->buildConfig.dependencyLinkage, "shared"));

    // "dynamic" is the LIBC axis' word. Accepting it here would silently
    // mean "static", because that is what an unparsed value resolves to.
    auto bad = load_inline("deplinkage_bad", R"(
[package]
name = "x"
version = "0.1.0"
[build]
dependency_linkage = "dynamic"
)");
    expect(!bad);
}};

UTest profile_dependency_linkage_is_optional_so_it_does_not_reset_build{"Manifest/ProfileDependencyLinkageIsOptionalSoItDoesNotResetBuild", [] {
    // Resolving a profile REPLACES the whole Profile struct, so a plain field
    // would make any declared profile silently undo `[build]`.
    auto m = load_inline("deplinkage_profile", R"(
[package]
name = "x"
version = "0.1.0"
[build]
dependency_linkage = "shared"
[profile.dev]
opt = "0"
[profile.fast]
dependency_linkage = "static"
)");
    expect(fatal(bool(m)));
    expect(fatal(m->profiles.contains("dev")));
    expect(!(m->profiles.at("dev").dependencyLinkageDeclared));
    expect(fatal(m->profiles.contains("fast")));
    expect(fatal(bool(m->profiles.at("fast").dependencyLinkageDeclared)));
    expect(eq(m->profiles.at("fast").dependencyLinkage, "static"));
}};

UTest a_dependency_edge_linkage_is_a_closed_vocabulary_too{"Manifest/ADependencyEdgeLinkageIsAClosedVocabularyToo", [] {
    auto bad = load_inline("edge_linkage_bad", R"(
[package]
name = "x"
version = "0.1.0"
[dependencies]
zlib = { version = "1.0.0", linkage = "dynamic" }
)");
    expect(!bad);
}};

UTest a_library_target_may_declare_a_soname{"Manifest/ALibraryTargetMayDeclareASoname", [] {
    // This used to make the WHOLE MANIFEST FAIL TO LOAD, in both parsers.
    // A soname is the name a library is FOUND by, and it is the only way two
    // copies of one library can resolve to a single file — so a package has
    // to be able to state it while still being consumed as a static library,
    // which is the normal case.
    auto m = load_inline("soname_on_lib", R"(
[package]
name = "z"
version = "0.1.0"
[targets.z]
kind = "lib"
soname = "libz.so.1"
)");
    if (auto&& failed = expect(fatal(bool(m))); !failed) failed << (m ? "" : m.error().message);
    expect(fatal(eq(m->targets.size(), 1u)));
    expect(eq(m->targets[0].soname, "libz.so.1"));
}};

UTest a_soname_on_a_binary_target_is_still_refused{"Manifest/ASonameOnABinaryTargetIsStillRefused", [] {
    auto m = load_inline("soname_on_bin", R"(
[package]
name = "z"
version = "0.1.0"
[targets.z]
kind = "bin"
main = "src/main.cpp"
soname = "libz.so.1"
)");
    expect(!m);
}};

UTest a_soname_must_still_be_a_basename{"Manifest/ASonameMustStillBeABasename", [] {
    auto m = load_inline("soname_path", R"(
[package]
name = "z"
version = "0.1.0"
[targets.z]
kind = "lib"
soname = "lib/libz.so.1"
)");
    expect(!m);
}};

// #359: a manifest written for a NEWER mcpp must still load.
//
// What made this impossible was that one predicate served two purposes: it
// decided "inline dep spec vs nested namespace table" AND enumerated the
// vocabulary. A key the reader did not know therefore did not read as an
// unknown option — the table failed the discriminator, was taken for a
// namespace, and the user was told their `reexport = true` "must be a string,
// inline dep table, or nested table". A published package could consequently
// never adopt a new key: every older client failed to load it outright.
//
// The discriminator is now "does it name a source", which a namespace table
// cannot accidentally satisfy, and an unrecognized key is a recorded
// degradation (--strict still refuses it).
UTest unknown_dependency_key_degrades_instead_of_failing_the_load{"Manifest/UnknownDependencyKeyDegradesInsteadOfFailingTheLoad", [] {
    auto tmp = std::filesystem::temp_directory_path() / "mcpp_dep_future_key";
    std::filesystem::create_directories(tmp);
    auto path = tmp / "mcpp.toml";
    {
        std::ofstream os(path);
        os << R"(
[package]
name = "futurekey"
version = "0.1.0"

[dependencies]
zlib = { version = "1.3.1", some-future-field = true }
)";
    }
    auto m = mcpp::manifest::load(path);
    if (auto&& failed = expect(fatal(bool(m))); !failed) failed << (m ? "" : m.error().message);

    const mcpp::pm::DependencySpec* z = nullptr;
    for (auto const& [k, spec] : m->dependencies)
        if (spec.shortName == "zlib") z = &spec;
    expect(fatal(ne(z, nullptr)));
    expect(eq(z->version, "1.3.1"));   // the part this reader understands still applies

    bool warned = false;
    for (auto const& w : m->schemaWarnings)
        if (w.find("some-future-field") != std::string::npos) warned = true;
    expect(bool(warned)) << "the ignored key must be reported, not swallowed";
}};

// The discriminator must not mistake a namespace subtable for an inline spec.
// `[dependencies.compat] zlib = "1.3.1"` has one key, `zlib`, which names no
// source — so it is a namespace, and the entry below it is the dependency.
UTest namespace_subtable_is_not_mistaken_for_an_inline_spec{"Manifest/NamespaceSubtableIsNotMistakenForAnInlineSpec", [] {
    auto tmp = std::filesystem::temp_directory_path() / "mcpp_ns_vs_spec";
    std::filesystem::create_directories(tmp);
    auto path = tmp / "mcpp.toml";
    {
        std::ofstream os(path);
        os << R"(
[package]
name = "nsvsspec"
version = "0.1.0"

[dependencies.compat]
zlib = "1.3.1"
gtest = { version = "1.15.2", features = ["main"] }
)";
    }
    auto m = mcpp::manifest::load(path);
    if (auto&& failed = expect(fatal(bool(m))); !failed) failed << (m ? "" : m.error().message);
    std::set<std::string> got;
    for (auto const& [k, spec] : m->dependencies) {
        got.insert(spec.shortName);
        expect(eq(spec.namespace_, "compat")) << spec.shortName;
    }
    expect(got.contains("zlib"));
    expect(got.contains("gtest"));
}};

// `cxx_runtime` gained a third role key. The table is validated by an explicit
// whitelist, so a new spelling that nobody added there is rejected with a
// message naming the key — which is the behaviour, not an accident.
UTest cxx_runtime_accepts_the_per_role_table_including_shared{"Manifest/CxxRuntimeAcceptsThePerRoleTableIncludingShared", [] {
    constexpr auto src = R"(
[package]
name    = "app"
version = "0.1.0"
[build]
cxx_runtime = { default = "self-contained", tests = "host-coupled", shared = "toolchain-coupled" }
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(eq(m->buildConfig.cxxRuntime, "self-contained"));
    expect(eq(m->buildConfig.cxxRuntimeTests, "host-coupled"));
    expect(eq(m->buildConfig.cxxRuntimeShared, "toolchain-coupled"));
    // And it must not be reported as an unsupported [build] key. The whole
    // feature is reachable only through this spelling, so a warning saying it
    // is "ignored" would be both noisy and false.
    expect(m->schemaWarnings.empty())
        << (m->schemaWarnings.empty() ? "" : m->schemaWarnings[0]);
}};

UTest cxx_runtime_rejects_an_unknown_role_key{"Manifest/CxxRuntimeRejectsAnUnknownRoleKey", [] {
    constexpr auto src = R"(
[package]
name    = "app"
version = "0.1.0"
[build]
cxx_runtime = { sharedd = "self-contained" }
)";
    auto m = mcpp::manifest::parse_string(src);
    expect(fatal(!m.has_value()));
    if (auto&& failed = expect(ne(m.error().format().find("sharedd"), std::string::npos)); !failed) failed << m.error().format();
}};

// The scalar spelling still means "every role", including shared libraries.
UTest cxx_runtime_scalar_still_applies_to_every_role{"Manifest/CxxRuntimeScalarStillAppliesToEveryRole", [] {
    constexpr auto src = R"(
[package]
name    = "app"
version = "0.1.0"
[build]
cxx_runtime = "host-coupled"
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(eq(m->buildConfig.cxxRuntime, "host-coupled"));
    expect(m->buildConfig.cxxRuntimeShared.empty());
    expect(m->schemaWarnings.empty());
}};

UTest bmi_schedule_key{"Manifest/BmiScheduleKey", [] {
    // The KEY SPELLING, not just the field. `[build] bmi_schedule` reaches the
    // build only through this one string in mcpp.manifest.toml, and nothing
    // downstream can tell a mistyped key from an absent one: the value silently
    // stays empty, `requested_switch` returns "auto", and the build quietly uses
    // the default schedule. Every other test would still pass.
    //
    // It was called `schedule` until it was renamed to agree with its own
    // environment override, `MCPP_BMI_SCHEDULE`. This is what makes the next
    // such rename fail loudly instead of silently.
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[build]
bmi_schedule = "on"
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(eq(m->buildConfig.bmiSchedule, "on"));

    // And the old spelling must NOT still work: leaving it accepted would mean
    // two keys for one switch, which is how a rename ends up half-done.
    constexpr auto old_key = R"(
[package]
name = "x"
version = "0.1.0"
[build]
schedule = "on"
)";
    auto o = mcpp::manifest::parse_string(old_key);
    if (auto&& failed = expect(fatal(o.has_value())); !failed) failed << o.error().format();
    expect(o->buildConfig.bmiSchedule.empty())
        << "the pre-rename key `schedule` is still being read";

    // AND THE PARSER MUST NOT WARN ABOUT ITS OWN KEY.
    //
    // Checking only the VALUE is what let this ship broken. The rename reached
    // the read (`build.bmi_schedule`) but not the accepted-key list, which kept
    // the old `schedule` — so the assertions above passed while a user writing
    // the one documented way to enable the feature got
    //
    //     [build] has unsupported key 'bmi_schedule' (ignored)
    //
    // and that message is FALSE: the value is read. The only way to turn the
    // feature on announced that it had been ignored, and the dead `schedule`
    // was accepted in silence. Both halves of the rename are pinned now.
    for (const auto& w : m->schemaWarnings)
        expect(eq(w.find("bmi_schedule"), std::string::npos))
            << "the parser reads `bmi_schedule` but also warns about it: " << w;

    expect(std::ranges::any_of(o->schemaWarnings,
                                    [](const std::string& w) {
                                        return w.find("schedule") != std::string::npos;
                                    }))
        << "the dead key `schedule` is accepted silently — an unread key that "
           "produces no diagnostic is a typo that costs a debugging session";
}};

// #418 — an unsupported SCALAR under `[target.<triple>]` is reported; the
// conditional sub-TABLES are not.
//
// `[target.<triple>]` had no unknown-key check at all, so `cxx_runtime_tests`
// — a key that existed on the struct, was parsed nowhere and applied nowhere —
// was accepted in silence. A configuration key that does nothing is worse than
// one that does not exist.
//
// AND THE CHECK MUST NOT SEE THE CONDITIONAL CHANNEL. TOML presents
// `[target.<pred>.dependencies]` as a KEY of `[target.<pred>]`, so a
// hand-written "known keys" list has to enumerate `build`, `dependencies`,
// `dev-dependencies`, `build-dependencies`, `feature-deps` too — and that list
// is precisely what this codebase has watched drift twice (ConditionalConfig's
// own comments on #258 and #359). The first version of this check did exactly
// that and warned about a documented feature. Scalars only; tables are skipped
// on principle, not by enumeration.
UTest per_target_unknown_scalar_warns_but_sub_tables_do_not{"Manifest/PerTargetUnknownScalarWarnsButSubTablesDoNot", [] {
    constexpr auto src = R"(
[package]
name = "t"
version = "0.1.0"

[target.'cfg(unix)'.dependencies]
dep = { path = "dep" }

[target.x86_64-linux-gnu]
linkage = "dynamic"
cxx_runtime_tests = "host-coupled"

[target.x86_64-linux-gnu.build]
defines = ["FOO=1"]
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();

    auto mentions = [&](std::string_view needle) {
        return std::ranges::any_of(m->schemaWarnings, [&](const std::string& w) {
            return w.find(needle) != std::string::npos;
        });
    };

    expect(bool(mentions("cxx_runtime_tests")))
        << "a per-target scalar that nothing reads must be reported, not dropped";

    // The conditional channel is not a typo, and warning about it would be a
    // false positive on a documented feature (e2e 195 uses it).
    expect(!mentions("'dependencies'"))
        << "[target.<pred>.dependencies] is the conditional channel, not an unknown key";
    if (auto&& failed = expect(!mentions("'build'")); !failed) failed << "[target.<pred>.build] is the conditional channel, not an unknown key";

    // ...and the field itself is gone, so nothing downstream can read it.
    expect(eq(m->targetOverrides.at("x86_64-linux-gnu").cxxRuntime, ""));
}};

// ── [target.<triple>].runner ────────────────────────────────────────────────

UTest target_runner_parses_and_does_not_warn{"Manifest/TargetRunnerParsesAndDoesNotWarn", [] {
    constexpr auto src = R"(
[package]
name = "fw"
version = "0.1.0"

[target.riscv64-none-elf]
runner = ["qemu-system-riscv64", "-machine", "virt", "-kernel"]
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    auto it = m->targetOverrides.find("riscv64-none-elf");
    expect(fatal(ne(it, m->targetOverrides.end())));
    expect(fatal(eq(it->second.runner.size(), 4u)));
    expect(eq(it->second.runner.front(), "qemu-system-riscv64"));
    expect(eq(it->second.runner.back(), "-kernel"));
    // The unknown-key sweep is about SCALARS ("a scalar that does
    // nothing"). It skipped tables but not arrays, so this key was reported as
    // "unsupported key 'runner' (ignored)" while in fact being honoured —
    // worse than either statement being true. Seen in CI.
    expect(m->schemaWarnings.empty())
        << (m->schemaWarnings.empty() ? "" : m->schemaWarnings[0]);
}};

UTest target_runner_rejects_shapes_that_would_run_nothing{"Manifest/TargetRunnerRejectsShapesThatWouldRunNothing", [] {
    // An empty template would exec nothing and report success.
    constexpr auto empty = R"(
[package]
name = "fw"
version = "0.1.0"
[target.riscv64-none-elf]
runner = []
)";
    expect(!(mcpp::manifest::parse_string(empty).has_value()));

    // A bare string is the shape a user reaches for first; taking it would
    // mean guessing where the word boundaries are.
    constexpr auto scalar = R"(
[package]
name = "fw"
version = "0.1.0"
[target.riscv64-none-elf]
runner = "qemu-system-riscv64 -kernel"
)";
    expect(!(mcpp::manifest::parse_string(scalar).has_value()));
}};

// ── [target.<triple>].sysroot — the target's C library, per project ──────────
//
// The axis exists because the target table binds one C library per triple, and
// three independent needs want to disagree with it: a kernel wants none, a
// project on a vendor SDK wants that SDK's newlib, and a `std-freestanding`
// stack over openkal wants openkal's C library.

UTest target_sysroot_override_is_parsed{"Manifest/TargetSysrootOverrideIsParsed", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[target.riscv64-none-elf]
sysroot = "xim:newlib-riscv@4.4"
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    auto it = m->targetOverrides.find("riscv64-none-elf");
    expect(fatal(ne(it, m->targetOverrides.end())));
    expect(fatal(bool(it->second.sysrootDeclared)));
    expect(eq(it->second.sysroot, "xim:newlib-riscv@4.4"));
    if (auto&& failed = expect(m->schemaWarnings.empty()); !failed) failed << (m->schemaWarnings.empty() ? "" : m->schemaWarnings[0]);
}};

// The distinction this test pins is the reason the field is an optional.
// An empty string is a REQUEST (no C library), not an absence.
UTest target_sysroot_empty_string_is_recorded_as_present{"Manifest/TargetSysrootEmptyStringIsRecordedAsPresent", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[target.riscv64-none-elf]
sysroot = ""
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    auto it = m->targetOverrides.find("riscv64-none-elf");
    expect(fatal(ne(it, m->targetOverrides.end())));
    expect(fatal(bool(it->second.sysrootDeclared)));
    expect(it->second.sysroot.empty());
}};

UTest target_sysroot_absent_is_not_declared{"Manifest/TargetSysrootAbsentIsNotDeclared", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[target.riscv64-none-elf]
linkage = "static"
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    auto it = m->targetOverrides.find("riscv64-none-elf");
    expect(fatal(ne(it, m->targetOverrides.end())));
    expect(!(it->second.sysrootDeclared));
}};

// A bare name is the plausible typo, and accepting it would install nothing
// and then fail much later naming a missing libc.
UTest target_sysroot_rejects_a_non_xpkg_reference{"Manifest/TargetSysrootRejectsANonXpkgReference", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[target.riscv64-none-elf]
sysroot = "newlib"
)";
    auto m = mcpp::manifest::parse_string(src);
    expect(fatal(!m.has_value()));
    expect(ne(m.error().format().find("is not a sysroot"), std::string::npos));
}};

// The table form names a directory, absolute against the manifest's own
// directory, and carries what it supplies (design 2026-10-23 §3.1).
UTest target_sysroot_table_names_a_directory{"Manifest/TargetSysrootTableNamesADirectory", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[target.x86_64-linux-gnu]
sysroot = { path = "third_party/sysroot", provides = ["mcpp:c-abi=glibc"] }
run_under = ["remote-run", "team-mac", "--"]
)";
    // An absolute origin on this host (a drive letter on Windows).
    const auto proj = (std::filesystem::temp_directory_path() / "proj").lexically_normal();
    auto m = mcpp::manifest::parse_string(src, proj / "mcpp.toml");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    auto it = m->targetOverrides.find("x86_64-linux-gnu");
    expect(fatal(ne(it, m->targetOverrides.end())));
    expect(bool(it->second.sysrootDeclared));
    expect(eq(it->second.sysroot, "path:" + (proj / "third_party" / "sysroot").lexically_normal().generic_string()));
    expect(fatal(eq(it->second.sysrootProvides.size(), 1u)));
    expect(eq(it->second.runUnder, (std::vector<std::string>{"remote-run", "team-mac", "--"})));
    for (auto const& w : m->schemaWarnings)
        expect(eq(w.find("sysroot"), std::string::npos)) << w;
}};

UTest target_sysroot_table_refuses_other_keys{"Manifest/TargetSysrootTableRefusesOtherKeys", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[target.x86_64-linux-gnu]
sysroot = { path = "/opt/s", libc = "glibc" }
)";
    auto m = mcpp::manifest::parse_string(src);
    expect(fatal(!m.has_value()));
    if (auto&& failed = expect(ne(m.error().format().find("`path`"), std::string::npos)); !failed) failed << m.error().format();
}};

// The unknown-scalar sweep must not report a key it now honours — the exact
// shape of #418, where a key was reported as ignored while being applied.
UTest target_sysroot_is_not_reported_as_unsupported{"Manifest/TargetSysrootIsNotReportedAsUnsupported", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[target.riscv64-none-elf]
sysroot = ""
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    for (auto const& w : m->schemaWarnings)
        expect(eq(w.find("sysroot"), std::string::npos)) << w;
}};

// ── On an MSVC-ABI row the sysroot is an MSVC toolset ─────────────────────────
//
// The compiler is the toolchain (cl.exe or clang) and the toolset -- its STL,
// its CRT and the SDK that follows it -- is what the compiler builds against.
// The row takes the msvc family's own spellings and nothing else.

namespace {
std::expected<mcpp::manifest::Manifest, mcpp::manifest::ManifestError>
msvc_row(std::string_view triple, std::string_view sysroot) {
    return mcpp::manifest::parse_string(std::format(R"(
[package]
name = "x"
version = "0.1.0"
[target.{}]
sysroot = "{}"
)", triple, sysroot));
}
} // namespace

UTest msvc_row_sysroot_takes_the_msvc_spellings{"Manifest/MsvcRowSysrootTakesTheMsvcSpellings", [] {
    for (auto triple : {"x86_64-windows-msvc", "x86_64-pc-windows-msvc"}) {
        for (auto value : {"msvc@system", "msvc@14.44.35207", "msvc@14.44",
                           "xim:msvc@14.44.35207"}) {
            auto m = msvc_row(triple, value);
            if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << triple << " " << value << ": "
                                       << m.error().format();
            auto it = m->targetOverrides.find(triple);
            expect(fatal(ne(it, m->targetOverrides.end())));
            expect(fatal(bool(it->second.sysrootDeclared)));
            expect(eq(it->second.sysroot, value));
        }
    }
}};

UTest msvc_row_sysroot_refuses_what_is_not_an_msvc_toolset{"Manifest/MsvcRowSysrootRefusesWhatIsNotAnMsvcToolset", [] {
    // A C library package, the zero-libc tier, a package asked for the
    // machine's toolset, and a bare family name are all refused by name.
    for (auto value : {"xim:glibc@2.39", "", "xim:msvc@system", "msvc", "gcc@16.1.0"}) {
        auto m = msvc_row("x86_64-windows-msvc", value);
        expect(fatal(!m.has_value())) << "'" << value << "' was accepted";
        if (auto&& failed = expect(ne(m.error().format().find("MSVC toolset"), std::string::npos)); !failed) failed << m.error().format();
    }
}};

UTest the_msvc_spelling_is_accepted_by_the_grammar_elsewhere{"Manifest/TheMsvcSpellingIsAcceptedByTheGrammarElsewhere", [] {
    // On every other row the spelling is a locator the grammar knows; which
    // row takes it is decided where the target is known
    // (`mcpp.toolchain.base::refusal`, tests/unit/test_target_base.cpp).
    auto m = msvc_row("x86_64-windows-gnu", "msvc@14.44.35207");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
}};

// ── Dependency version requirements are checked where they are written ───────
//
// The parser that decides which published version satisfies a requirement
// existed all along; the dependency reader did not use it, and handed its
// string to the installer instead. A requirement the matcher could never
// satisfy therefore reached the network and came back as
// `E_NOT_FOUND: package '…@0.1.x' not found` — naming the PACKAGE, which
// exists. This repository's own documentation recommended that form.

// PARSE-acceptance, which is a weaker property than installability and was
// measured to be weaker: `"0.0"` parses here and then fails at fetch with
// `install path missing`. The check added below is a manifest check and has no
// business ruling on the installer's path derivation, so this test pins what
// the PARSER must keep accepting and says nothing about what fetches.
UTest dependency_version_parser_accepts_every_established_form{"Manifest/DependencyVersionParserAcceptsEveryEstablishedForm", [] {
    // This test exists to prevent the check from NARROWING anything.
    for (const char* v : { "0.0.1", "0.0", "^0.0.1", ">=0.0.1, <0.1.0", "*" }) {
        auto src = std::format(R"(
[package]
name = "x"
version = "0.1.0"
[dependencies]
cmdline = "{}"
)", v);
        auto m = mcpp::manifest::parse_string(src);
        if (auto&& failed = expect(m.has_value()); !failed) failed << v << ": " << (m ? "" : m.error().format());
    }
}};

// REPORTED, NOT REJECTED, and the distinction is load-bearing. The first
// version of this check returned an error, and a project pinned to a PUBLISHED
// package carrying such a string stopped loading entirely — including when the
// offending entry belonged to a feature nobody activates. Published data must
// not be invalidated by a new program any more than the reverse.
UTest dependency_version_reports_a_trailing_x_without_failing_the_load{"Manifest/DependencyVersionReportsATrailingXWithoutFailingTheLoad", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[dependencies]
cmdline = "0.0.x"
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << (m ? "" : m.error().format());
    // THE ASSERTION IS ON THE PROPERTIES THE MESSAGE MUST CARRY, not on its
    // prose. Keying on a phrase makes a reworded warning either a silent
    // no-op or a failure about wording; what a reader needs is the offending
    // STRING (so they can find it), the word PACKAGE (because the failure
    // they will otherwise see names exactly that, and the package is not the
    // problem), and an example of an accepted form.
    bool found = false;
    for (auto const& w : m->schemaWarnings) {
        if (w.find("0.0.x") == std::string::npos) continue;
        found = true;
        expect(ne(w.find("PACKAGE"), std::string::npos)) << w;
        expect(ne(w.find("^1.2.3"), std::string::npos)) << w;
    }
    expect(bool(found)) << "no warning quoting the offending version string";
}};

UTest dependency_version_reports_trailing_garbage{"Manifest/DependencyVersionReportsTrailingGarbage", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[dependencies]
cmdline = "1.2.3abc"
)";
    auto m = mcpp::manifest::parse_string(src);
    expect(fatal(m.has_value()));
    // "some warning was produced" is satisfied by a warning about anything at
    // all, including a typo elsewhere in the fixture. The warning has to quote
    // the string it is about, or the reader cannot find it.
    bool found = false;
    for (auto const& w : m->schemaWarnings)
        if (w.find("1.2.3abc") != std::string::npos) found = true;
    expect(bool(found)) << "no warning quoting the offending version string";
}};

// A path or git dependency has no version, and requiring one would break every
// workspace member.
UTest dependency_without_a_version_is_unaffected{"Manifest/DependencyWithoutAVersionIsUnaffected", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[dependencies]
local = { path = "../local" }
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(m.has_value()); !failed) failed << (m ? "" : m.error().format());
}};

// The long form reaches a different code path than the bare string, and only
// one of the two used to set `spec.version`. Both are checked.
UTest long_form_dependency_version_is_checked_too{"Manifest/LongFormDependencyVersionIsCheckedToo", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[dependencies]
cmdline = { version = "0.0.x", features = ["a"] }
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << (m ? "" : m.error().format());
    bool found = false;
    for (auto const& w : m->schemaWarnings)
        if (w.find("0.0.x") != std::string::npos) found = true;
    expect(bool(found)) << "the long form's version was not checked";
}};

// ─── [hooks] — project build lifecycle commands (#496) ───────────────────
//
// The section is parsed here rather than by the module that runs the commands,
// so this is where its grammar is pinned. The invocation policy (what a
// failure costs, when each event fires) is e2e 314's subject.

namespace {
constexpr std::string_view kHookPreamble = R"(
[package]
name = "x"
version = "0.1.0"
)";

std::expected<mcpp::manifest::Manifest, mcpp::manifest::ManifestError>
parse_with_hooks(std::string_view hooksSection) {
    return mcpp::manifest::parse_string(
        std::string(kHookPreamble) + std::string(hooksSection));
}
}  // namespace

UTest absent_section_is_inert_but_not_disabled{"ManifestHooks/AbsentSectionIsInertButNotDisabled", [] {
    auto m = mcpp::manifest::parse_string(kHookPreamble);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << (m ? "" : m.error().format());
    // `enabled` defaults to true and says nothing about whether there is work
    // to do; `active()` is the predicate the fast path is allowed to consult.
    expect(bool(m->hooks.enabled));
    expect(!(m->hooks.active()));
    expect(eq(m->hooks.timeoutSeconds, 10));
    // Experimental: a hook may not decide whether a build succeeded, so this
    // defaults to false and is the only value the parser accepts.
    expect(!(m->hooks.sideEffect));
}};

// The experimental gate. Refused rather than downgraded: honouring it would
// give an experimental feature a veto over every build, and ignoring it would
// leave the project believing its build is gated on a notifier when nothing
// is. Both silent options are worse than the error.
UTest side_effect_true_is_refused_while_the_feature_is_experimental{"ManifestHooks/SideEffectTrueIsRefusedWhileTheFeatureIsExperimental", [] {
    auto m = parse_with_hooks(R"(
[hooks]
build_finished = "echo done"
side_effect = true
)");
    expect(fatal(!m.has_value()));
    if (auto&& failed = expect(ne(m.error().message.find("experimental"), std::string::npos)); !failed) failed << m.error().message;
    if (auto&& failed = expect(ne(m.error().message.find("side_effect"), std::string::npos)); !failed) failed << m.error().message;
}};

// The key stays in the vocabulary: writing the value that IS the current
// behaviour must keep working, so a manifest does not have to change when the
// feature is promoted.
UTest side_effect_false_is_accepted_so_the_key_stays_reserved{"ManifestHooks/SideEffectFalseIsAcceptedSoTheKeyStaysReserved", [] {
    auto m = parse_with_hooks(R"(
[hooks]
build_finished = "echo done"
side_effect = false
)");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << (m ? "" : m.error().format());
    expect(!(m->hooks.sideEffect));
}};

UTest commands_and_policy_are_read{"ManifestHooks/CommandsAndPolicyAreRead", [] {
    auto m = parse_with_hooks(R"(
[hooks]
build_start = "echo start"
build_failed = "echo failed"
build_finished = "echo finished"
timeout_seconds = 30
side_effect = false
)");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << (m ? "" : m.error().format());
    expect(eq(m->hooks.buildStart.cmd, "echo start"));
    expect(eq(m->hooks.buildFailed.cmd, "echo failed"));
    expect(eq(m->hooks.buildFinished.cmd, "echo finished"));
    expect(eq(m->hooks.timeoutSeconds, 30));
    expect(!(m->hooks.sideEffect));
    expect(bool(m->hooks.active()));
}};

// `enabled = false` has to switch off a table that still names commands —
// otherwise the only way to silence hooks is to delete them.
UTest disabled_table_is_not_active{"ManifestHooks/DisabledTableIsNotActive", [] {
    auto m = parse_with_hooks(R"(
[hooks]
build_start = "echo start"
enabled = false
)");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << (m ? "" : m.error().format());
    expect(eq(m->hooks.buildStart.cmd, "echo start"));
    expect(!(m->hooks.active()));
}};

// Policy without a command is not work: a table that only raises the timeout
// must leave the fast path alone.
UTest policy_only_table_is_not_active{"ManifestHooks/PolicyOnlyTableIsNotActive", [] {
    auto m = parse_with_hooks(R"(
[hooks]
timeout_seconds = 60
)");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << (m ? "" : m.error().format());
    expect(!(m->hooks.active()));
    expect(eq(m->hooks.timeoutSeconds, 60));
}};

// Both ends of the documented range are the range: the largest accepted value
// is asserted next to the smallest rejected one, so a change to either is a
// change to a test.
UTest the_documented_timeout_ceiling_is_accepted{"ManifestHooks/TheDocumentedTimeoutCeilingIsAccepted", [] {
    auto m = parse_with_hooks(R"(
[hooks]
build_start = "echo start"
timeout_seconds = 86400
)");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << (m ? "" : m.error().format());
    expect(eq(m->hooks.timeoutSeconds, 86400));
}};

UTest value_errors_are_rejected_with_the_key_named{"ManifestHooks/ValueErrorsAreRejectedWithTheKeyNamed", [] {
    struct Case { std::string_view section; std::string_view needle; };
    for (auto const& c : {
        Case{R"([hooks]
timeout_seconds = 0)", "timeout_seconds"},
        Case{R"([hooks]
timeout_seconds = "10")", "timeout_seconds"},
        Case{R"([hooks]
build_start = "")", "build_start"},
        Case{R"([hooks]
build_finished = 7)", "build_finished"},
        Case{R"([hooks]
enabled = "yes")", "enabled"},
        Case{R"([hooks]
side_effect = 1)", "side_effect"},
        // The upper bound is documented (1–86400), so it is asserted. An
        // undocumented ceiling and no ceiling look the same from outside.
        Case{R"([hooks]
timeout_seconds = 86401)", "timeout_seconds"},
    }) {
        auto m = parse_with_hooks(c.section);
        expect(fatal(!m.has_value())) << c.section;
        if (auto&& failed = expect(ne(m.error().message.find(c.needle), std::string::npos)); !failed) failed << c.section << " -> " << m.error().message;
    }
}};

// `hooks = "…"` is the shape a user reaches for when they want one command
// and no table. Saying so beats parsing nothing and running nothing.
UTest a_scalar_hooks_key_is_rejected{"ManifestHooks/AScalarHooksKeyIsRejected", [] {
    auto m = mcpp::manifest::parse_string(R"(
hooks = "echo hi"

[package]
name = "x"
version = "0.1.0"
)");
    expect(fatal(!m.has_value()));
    if (auto&& failed = expect(ne(m.error().message.find("[hooks]"), std::string::npos)); !failed) failed << m.error().message;
}};

// An unknown KEY is a warning, not an error — the same split [build] uses.
// A manifest written for a later mcpp (say, a `build_cancelled` event) still
// loads on this one, while a typo is still reported.
UTest unknown_key_warns_instead_of_failing{"ManifestHooks/UnknownKeyWarnsInsteadOfFailing", [] {
    auto m = parse_with_hooks(R"(
[hooks]
build_start = "echo start"
build_stared = "echo typo"
)");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << (m ? "" : m.error().format());
    bool found = false;
    for (auto const& w : m->schemaWarnings)
        if (w.find("build_stared") != std::string::npos) found = true;
    expect(bool(found));
    expect(bool(m->hooks.active()));
}};

// ─── The table form, and the interval each event names ───────────────────

UTest a_bare_string_is_sugar_for_a_table{"ManifestHooks/ABareStringIsSugarForATable", [] {
    auto m = parse_with_hooks(R"(
[hooks]
build_start = "echo start"
)");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << (m ? "" : m.error().format());
    expect(eq(m->hooks.buildStart.cmd, "echo start"));
    expect(eq(m->hooks.buildStart.timeoutSeconds, 0));   // 0 = inherit
    expect(!(m->hooks.buildStart.loop));
}};

// The per-event value wins over the table's, and 0 is what makes "not given"
// expressible — without it there is no way to tell an inherited 10 from a
// declared one.
UTest a_per_event_timeout_overrides_the_table_default{"ManifestHooks/APerEventTimeoutOverridesTheTableDefault", [] {
    auto m = parse_with_hooks(R"(
[hooks]
timeout_seconds = 10
build_finished = { cmd = "slow-notifier", timeout_seconds = 45 }
build_start = "echo start"
)");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << (m ? "" : m.error().format());
    expect(eq(m->hooks.timeout_for(m->hooks.buildFinished), 45));
    expect(eq(m->hooks.timeout_for(m->hooks.buildStart), 10));
}};

UTest during_build_is_read_and_makes_the_table_active{"ManifestHooks/DuringBuildIsReadAndMakesTheTableActive", [] {
    auto m = parse_with_hooks(R"(
[hooks]
during_build = { cmd = "play bgm.mp3", loop = true }
)");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << (m ? "" : m.error().format());
    expect(eq(m->hooks.duringBuild.cmd, "play bgm.mp3"));
    expect(bool(m->hooks.duringBuild.loop));
    expect(bool(m->hooks.active()));
}};

// The two keys that only exist for one interval. Accepted-and-ignored is
// the failure mode this rejects: the user writes `loop` on `build_start`,
// nothing repeats, and the feature looks broken rather than misused. Each
// message names the event that does support the key.
UTest a_key_offered_to_the_wrong_interval_is_rejected{"ManifestHooks/AKeyOfferedToTheWrongIntervalIsRejected", [] {
    auto loopOnSelfClosing = parse_with_hooks(R"(
[hooks]
build_start = { cmd = "play bgm.mp3", loop = true }
)");
    expect(fatal(!loopOnSelfClosing.has_value()));
    if (auto&& failed = expect(ne(loopOnSelfClosing.error().message.find("during_build"), std::string::npos)); !failed) failed << loopOnSelfClosing.error().message;

    auto timeoutOnSpanning = parse_with_hooks(R"(
[hooks]
during_build = { cmd = "play bgm.mp3", timeout_seconds = 30 }
)");
    expect(fatal(!timeoutOnSpanning.has_value()));
    if (auto&& failed = expect(ne(timeoutOnSpanning.error().message.find("timeout_seconds"), std::string::npos)); !failed) failed << timeoutOnSpanning.error().message;
}};

UTest a_table_without_cmd_is_rejected{"ManifestHooks/ATableWithoutCmdIsRejected", [] {
    for (std::string_view section : {
        R"([hooks]
during_build = { loop = true })",
        R"([hooks]
build_start = { cmd = "" })",
        R"([hooks]
build_start = 42)",
    }) {
        auto m = parse_with_hooks(section);
        expect(fatal(!m.has_value())) << section;
        if (auto&& failed = expect(ne(m.error().message.find("cmd"), std::string::npos)); !failed) failed << section << " -> " << m.error().message;
    }
}};

UTest an_unknown_key_inside_an_event_table_warns{"ManifestHooks/AnUnknownKeyInsideAnEventTableWarns", [] {
    auto m = parse_with_hooks(R"(
[hooks]
during_build = { cmd = "play bgm.mp3", lopo = true }
)");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << (m ? "" : m.error().format());
    bool found = false;
    for (auto const& w : m->schemaWarnings)
        if (w.find("lopo") != std::string::npos) found = true;
    expect(bool(found));
    expect(bool(m->hooks.active()));
}};

// ─── #544: array keys in the [target.<triple>] sweep; [xlings] per host ────

// The sweep skipped arrays by design (so that `runner`, an array it reads, was
// not reported as unsupported), which also let an array-valued typo pass in
// silence and left `runner` out of the "supported keys" list. Both halves of
// the issue's third ask are pinned here; TargetRunnerParsesAndDoesNotWarn
// above keeps the other direction (the real key stays unreported).
UTest target_sweep_reports_array_typos_and_lists_runner{"Manifest/TargetSweepReportsArrayTyposAndListsRunner", [] {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "app"
version = "0.1.0"
[target.aarch64-linux-musl]
runnerX = ["qemu-aarch64-static"]
)");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->schemaWarnings.size(), 1u)));
    if (auto&& failed = expect(ne(m->schemaWarnings[0].find("'runnerX'"), std::string::npos)); !failed) failed << m->schemaWarnings[0];
    // #610 / #630 item 5 — `min_api_level` joined this list; see
    // tests/unit/test_target_scalar_keys.cpp for the test that keeps this
    // string (and the array beside it) from drifting from the parser again.
    expect(ne(m->schemaWarnings[0].find(
                  "Supported keys: cxx_runtime, linkage, min_api_level, platform, run_under, "
                  "runner, sysroot, toolchain"), std::string::npos)) << m->schemaWarnings[0];
}};

// `[xlings]` mirrors `.xlings.json`, whose values may be an object keyed by
// platform. The parser resolves the form against this host at load, so the
// assertions below are written in terms of host_platform_key() rather than a
// fixed platform: the same test file runs on all three CI hosts.
UTest xlings_deps_accept_per_platform_entries{"Manifest/XlingsDepsAcceptPerPlatformEntries", [] {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "app"
version = "0.1.0"
[xlings]
deps = ["xim:ninja", { linux = "qemu-user-aarch64" }, { windows = "nasm", default = "yasm" }]
)");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    const auto host = mcpp::manifest::host_platform_key();
    std::vector<std::string> want{"xim:ninja"};
    if (host == "linux") want.push_back("qemu-user-aarch64");
    want.push_back(host == "windows" ? "nasm" : "yasm");
    expect(eq(m->xlings.deps, want));
}};

UTest xlings_workspace_accepts_per_platform_values{"Manifest/XlingsWorkspaceAcceptsPerPlatformValues", [] {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "app"
version = "0.1.0"
[xlings.workspace]
gcc = { linux = "15.1.0" }
llvm = { macos = "20", default = "22" }
xmake = "3.0.7"
)");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    const auto host = mcpp::manifest::host_platform_key();
    if (host == "linux") expect(eq(m->xlings.workspace.at("gcc"), "15.1.0"));
    else                 expect(eq(m->xlings.workspace.count("gcc"), 0u));
    // `host_platform_key()` is xlings' spelling, `macosx`, since 2026.9.3.1.
    // Comparing against "macos" here made this the one assertion in the file
    // that could only fail on a macOS runner — and it did.
    expect(eq(m->xlings.workspace.at("llvm"), host == "macosx" ? "20" : "22"));
    expect(eq(m->xlings.workspace.at("xmake"), "3.0.7"));
}};

UTest xlings_unknown_platform_key_is_an_error{"Manifest/XlingsUnknownPlatformKeyIsAnError", [] {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "app"
version = "0.1.0"
[xlings]
deps = [{ linxu = "qemu-user-aarch64" }]
)");
    expect(fatal(!m.has_value()));
    if (auto&& failed = expect(ne(m.error().message.find("linxu"), std::string::npos)); !failed) failed << m.error().message;
    if (auto&& failed = expect(ne(m.error().message.find("linux, macosx (or macos), windows, default"), std::string::npos)); !failed) failed << m.error().message;
    if (auto&& failed = expect(ne(m.error().message.find("deps[0]"), std::string::npos)); !failed) failed << m.error().message;
}};

UTest xlings_workspace_non_string_leaf_is_an_error{"Manifest/XlingsWorkspaceNonStringLeafIsAnError", [] {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "app"
version = "0.1.0"
[xlings.workspace]
gcc = { linux = 15 }
)");
    expect(fatal(!m.has_value()));
    if (auto&& failed = expect(ne(m.error().message.find("[xlings.workspace] gcc"), std::string::npos)); !failed) failed << m.error().message;
}};

// The resolution rule itself, with the platform passed explicitly so all
// three hosts' answers are asserted on every host.
UTest resolve_host_value_table{"Manifest/ResolveHostValueTable", [] {
    using mcpp::manifest::resolve_host_value;
    auto d = mcpp::libs::toml::parse(R"(v = { linux = "a", macosx = "b", default = "c" })");
    expect(fatal(d.has_value()));
    const auto& v = d->root().at("v");
    expect(eq(resolve_host_value(v, "linux").value().value(), "a"));
    expect(eq(resolve_host_value(v, "macos").value().value(), "b"));   // macosx alias
    expect(eq(resolve_host_value(v, "windows").value().value(), "c"));
    auto e = mcpp::libs::toml::parse(R"(v = { linux = "a" })");
    expect(fatal(e.has_value()));
    expect(!(resolve_host_value(e->root().at("v"), "windows").value().has_value()));
    auto s = mcpp::libs::toml::parse(R"(v = "plain")");
    expect(fatal(s.has_value()));
    expect(eq(resolve_host_value(s->root().at("v"), "windows").value().value(), "plain"));
}};

// ── `[xlings.workspace]` as the one table (2026-09-03) ─────────────────────
//
// The design is `.agents/docs/2026-09-03-xlings-workspace-as-the-one-table.md`.
// Each entry produces two projections — an install address and a resolution
// pin — and the assertions are on both, because a test that checked only one
// could not tell a dropped namespace from a dropped version.

UTest xlings_workspace_produces_an_address_and_a_pin{"Manifest/XlingsWorkspaceProducesAnAddressAndAPin", [] {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "app"
version = "0.1.0"
[xlings.workspace]
cmake = "3.28"
)");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(eq(m->xlings.workspace.at("cmake"), "3.28"));
    expect(fatal(eq(m->xlings.deps.size(), 1u)));
    expect(eq(m->xlings.deps[0], "cmake@3.28"));
}};

UTest xlings_workspace_namespace_on_either_half_is_the_same_entry{"Manifest/XlingsWorkspaceNamespaceOnEitherHalfIsTheSameEntry", [] {
    auto onValue = mcpp::manifest::parse_string(R"(
[package]
name = "app"
version = "0.1.0"
[xlings.workspace]
picolibc-riscv = "xim:1.8.12"
)");
    if (auto&& failed = expect(fatal(onValue.has_value())); !failed) failed << onValue.error().format();
    auto onKey = mcpp::manifest::parse_string(R"(
[package]
name = "app"
version = "0.1.0"
[xlings.workspace]
"xim:picolibc-riscv" = "1.8.12"
)");
    if (auto&& failed = expect(fatal(onKey.has_value())); !failed) failed << onKey.error().format();
    expect(eq(onValue->xlings.deps, onKey->xlings.deps));
    expect(eq(onValue->xlings.workspace, onKey->xlings.workspace));
    expect(eq(onValue->xlings.deps.at(0), "xim:picolibc-riscv@1.8.12"));
    expect(eq(onValue->xlings.workspace.at("picolibc-riscv"), "xim:1.8.12"));
}};

UTest xlings_workspace_disagreeing_namespaces_are_refused{"Manifest/XlingsWorkspaceDisagreeingNamespacesAreRefused", [] {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "app"
version = "0.1.0"
[xlings.workspace]
"xim:foo" = "other:1.0"
)");
    expect(fatal(!m.has_value()));
    if (auto&& failed = expect(ne(m.error().message.find("xim"), std::string::npos)); !failed) failed << m.error().message;
    if (auto&& failed = expect(ne(m.error().message.find("other"), std::string::npos)); !failed) failed << m.error().message;
}};

UTest xlings_workspace_naming_one_package_twice_is_refused{"Manifest/XlingsWorkspaceNamingOnePackageTwiceIsRefused", [] {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "app"
version = "0.1.0"
[xlings.workspace]
foo = "1.0"
"xim:foo" = "2.0"
)");
    expect(fatal(!m.has_value()));
    if (auto&& failed = expect(ne(m.error().message.find("twice"), std::string::npos)); !failed) failed << m.error().message;
    if (auto&& failed = expect(ne(m.error().message.find("xim:foo"), std::string::npos)); !failed) failed << m.error().message;
}};

// `""` is the authored spelling for "present, version unconstrained"; it is
// what `d2mcpp/.xlings.json` already writes. Both halves are asserted: an
// address with no `@`, and no pin at all.
UTest xlings_workspace_empty_version_asks_only_for_presence{"Manifest/XlingsWorkspaceEmptyVersionAsksOnlyForPresence", [] {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "app"
version = "0.1.0"
[xlings.workspace]
code = ""
)");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(eq(m->xlings.deps.at(0), "code"));
    expect(m->xlings.workspace.at("code").empty());
}};

// The descriptor emitter needs every platform, and the host resolution has
// already discarded two of them — so the unresolved declaration is kept.
UTest xlings_workspace_keeps_every_platform_for_the_emitter{"Manifest/XlingsWorkspaceKeepsEveryPlatformForTheEmitter", [] {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "app"
version = "0.1.0"
[xlings.workspace]
qemu = { linux = "9.2.4-1", windows = "9.2.4-2" }
ninja = "1.12.1"
)");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    const auto& byPlat = m->xlings.workspaceByPlatform;
    expect(fatal(byPlat.contains("linux")));
    expect(fatal(byPlat.contains("windows")));
    expect(ne(std::ranges::find(byPlat.at("linux"), "qemu@9.2.4-1"), byPlat.at("linux").end()));
    expect(ne(std::ranges::find(byPlat.at("windows"), "qemu@9.2.4-2"), byPlat.at("windows").end()));
    // A plain string speaks for every platform, macosx included, even though
    // `qemu` named none there.
    expect(fatal(byPlat.contains("macosx")));
    expect(ne(std::ranges::find(byPlat.at("macosx"), "ninja@1.12.1"), byPlat.at("macosx").end()));
    expect(eq(std::ranges::find(byPlat.at("macosx"), "qemu@9.2.4-1"), byPlat.at("macosx").end()));
}};

// `deps` is honoured and reported. Refusing it would reach a DEPENDENCY's
// manifest, which a consumer that pinned that package cannot edit.
UTest xlings_deps_is_honoured_and_reported{"Manifest/XlingsDepsIsHonouredAndReported", [] {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "app"
version = "0.1.0"
[xlings]
deps = ["xim:qemu-riscv@9.2.4-1"]
)");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(eq(m->xlings.deps.at(0), "xim:qemu-riscv@9.2.4-1"));
    expect(fatal(eq(m->schemaWarnings.size(), 1u)));
    if (auto&& failed = expect(ne(m->schemaWarnings[0].find("[xlings.workspace]"), std::string::npos)); !failed) failed << m->schemaWarnings[0];
    // The message shows the line to write, in the RECOMMENDED spelling: the
    // namespace on the key, which is what an author writes and what every
    // official package uses.
    expect(ne(m->schemaWarnings[0].find("\"xim:qemu-riscv\" = \"9.2.4-1\""), std::string::npos)) << m->schemaWarnings[0];
}};

// `envs` was materialised and read by nothing, while the documentation said it
// reached the tool environment. A key that does nothing is worse when
// something claims it does, so this is an error rather than a warning.
UTest xlings_envs_is_refused{"Manifest/XlingsEnvsIsRefused", [] {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "app"
version = "0.1.0"
[xlings.envs]
OPENBLAS_NUM_THREADS = "1"
)");
    expect(fatal(!m.has_value()));
    if (auto&& failed = expect(ne(m.error().message.find("[xlings.envs]"), std::string::npos)); !failed) failed << m.error().message;
}};

// `macos` and `macosx` are one platform under two vocabularies — mcpp's triple
// says the first, a descriptor and xlings' project file say the second. The
// rule lives in mcpp.toolchain.platform; this asserts the manifest reaches it,
// on every host, by resolving explicitly rather than against this one.
UTest xlings_workspace_accepts_both_macos_spellings{"Manifest/XlingsWorkspaceAcceptsBothMacosSpellings", [] {
    auto with_macos = mcpp::manifest::parse_string(R"(
[package]
name = "app"
version = "0.1.0"
[xlings.workspace]
llvm = { macos = "20", default = "22" }
)");
    if (auto&& failed = expect(fatal(with_macos.has_value())); !failed) failed << with_macos.error().format();
    auto with_macosx = mcpp::manifest::parse_string(R"(
[package]
name = "app"
version = "0.1.0"
[xlings.workspace]
llvm = { macosx = "20", default = "22" }
)");
    if (auto&& failed = expect(fatal(with_macosx.has_value())); !failed) failed << with_macosx.error().format();
    // Same platform, so the same per-platform declaration and the same
    // host-resolved answer, whichever way it was written.
    expect(eq(with_macos->xlings.workspaceByPlatform, with_macosx->xlings.workspaceByPlatform));
    expect(eq(with_macos->xlings.workspace, with_macosx->xlings.workspace));
    expect(ne(std::ranges::find(with_macos->xlings.workspaceByPlatform.at("macosx"),
                                "llvm@20"), with_macos->xlings.workspaceByPlatform.at("macosx").end()));
    expect(ne(std::ranges::find(with_macos->xlings.workspaceByPlatform.at("linux"),
                                "llvm@22"), with_macos->xlings.workspaceByPlatform.at("linux").end()));
}};

// ─── Tool tiers: `when` on an entry, and `[feature-xlings.<f>]` ────────────
//
// THE TWO SPELLINGS OF A TABLE MUST NOT BE CONFUSABLE. `[xlings.workspace]`
// already accepted an object keyed by platform; the tier form is an object
// keyed by `version` and `when`. Neither key set contains a member of the
// other, so the presence of `version`/`when` decides — and this test is what
// says so, because nothing in the parser's shape forces it.
UTest xlings_workspace_entry_tiers{"Manifest/XlingsWorkspaceEntryTiers", [] {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "app"
version = "0.1.0"
[xlings.workspace]
plain  = "1.0"
built  = { version = "2.0", when = "build" }
ran    = { version = "3.0", when = "run" }
devd   = { version = "4.0", when = "dev" }
anyver = { version = "",    when = "run" }
)");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    using W = mcpp::manifest::ToolWhen;
    // An entry with no `when` is on the tier every verb installs. That is the
    // pre-2026.9.4.2 behaviour, and it is the reason no manifest has to change.
    expect(eq(m->xlings.when_of("plain@1.0"), W::Always));
    expect(eq(m->xlings.when_of("built@2.0"), W::Build));
    expect(eq(m->xlings.when_of("ran@3.0"), W::Run));
    expect(eq(m->xlings.when_of("devd@4.0"), W::Dev));
    // A tier does not change the address, only when it is asked for. An entry
    // that names no version asks for presence, exactly as the plain spelling
    // does — the address carries no `@`.
    expect(eq(m->xlings.when_of("anyver"), W::Run));
    expect(eq(m->xlings.workspace.at("plain"), "1.0"));
    expect(eq(m->xlings.workspace.at("ran"), "3.0"));
    expect(m->xlings.workspace.at("anyver").empty());
}};

UTest xlings_workspace_platform_table_still_parses{"Manifest/XlingsWorkspacePlatformTableStillParses", [] {
    // The regression this pair guards: adding the tier form must not make a
    // platform table read as a malformed tier table.
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "app"
version = "0.1.0"
[xlings.workspace]
llvm = { macos = "20", default = "22" }
)");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(eq(m->xlings.when_of(m->xlings.deps.at(0)), mcpp::manifest::ToolWhen::Always));
}};

UTest xlings_workspace_bad_tier_is_an_error{"Manifest/XlingsWorkspaceBadTierIsAnError", [] {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "app"
version = "0.1.0"
[xlings.workspace]
tool = { version = "1.0", when = "sometimes" }
)");
    expect(fatal(!m.has_value()));
    if (auto&& failed = expect(ne(m.error().message.find("sometimes"), std::string::npos)); !failed) failed << m.error().message;
    if (auto&& failed = expect(ne(m.error().message.find("'build', 'run' or 'dev'"), std::string::npos)); !failed) failed << m.error().message;
}};

UTest xlings_workspace_scoped_entry_needs_version{"Manifest/XlingsWorkspaceScopedEntryNeedsVersion", [] {
    // `{ when = "run" }` and a typo of `version` are indistinguishable, so the
    // key is required and `""` is how "any version" is said.
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "app"
version = "0.1.0"
[xlings.workspace]
tool = { versoin = "1.0", when = "run" }
)");
    expect(fatal(!m.has_value()));
    if (auto&& failed = expect(ne(m.error().message.find("versoin"), std::string::npos)); !failed) failed << m.error().message;
}};

UTest feature_xlings_is_gated_and_reported{"Manifest/FeatureXlingsIsGatedAndReported", [] {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "app"
version = "0.1.0"
[features]
default  = ["emulator"]
emulator = {}
hardware = {}
[feature-xlings.hardware]
"xim:probe-rs" = "0.24.0"
[feature-xlings.hardwear]
"xim:typo" = "1.0"
)");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->xlings.featureDeps.count("hardware"), 1u)));
    expect(eq(m->xlings.featureDeps.at("hardware").at(0), "xim:probe-rs@0.24.0"));
    // A feature-gated tool is NOT in the unconditional list; a consumer that
    // never activates the feature never asks for it.
    expect(eq(std::ranges::find(m->xlings.deps, "xim:probe-rs@0.24.0"), m->xlings.deps.end()));
    // A misspelt feature installs nothing and says nothing, which is the
    // shape #531 was filed for. Reported as a schema warning rather than an
    // error, so a package may adopt a feature before its consumers upgrade.
    bool sawTypo = false;
    for (auto const& w : m->schemaWarnings)
        if (w.find("hardwear") != std::string::npos) sawTypo = true;
    if (auto&& failed = expect(bool(sawTypo)); !failed) failed << "no warning for [feature-xlings.hardwear]";
}};

// ── [package] exclusive ────────────────────────────────────────────────────
//
// The key is a claim about THIS package's own symbols, so it can only apply to
// a capability the package supplies. The parse therefore accepts the list and
// reports an entry that is not supplied — the value is still carried, because
// the binding-time check is the one that acts on it and a warning must not
// silently change what was declared.

// ── [build] sources: constrained entries ───────────────────────────────────
//
// A table entry carries a glob and the accel it is for. The glob joins the
// plain list (every existing reader keeps working); the constraint is what
// prepare narrows on.

UTest a_table_entry_joins_the_list_and_carries_its_constraint{"ManifestSourceConstraints/ATableEntryJoinsTheListAndCarriesItsConstraint", [] {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name    = "infer"
version = "0.1.0"
[build]
sources = ["src/**/*.cppm", { glob = "src/kernels/**/*.cu", accel = "cuda12.9+{sm_89}" }]
)");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->buildConfig.sources.size(), 2u)));
    expect(eq(m->buildConfig.sources[0], "src/**/*.cppm"));
    expect(eq(m->buildConfig.sources[1], "src/kernels/**/*.cu"));
    expect(bool(m->buildConfig.sourcesDeclared));
    expect(fatal(eq(m->buildConfig.sourceConstraints.size(), 1u)));
    expect(eq(m->buildConfig.sourceConstraints[0].glob, "src/kernels/**/*.cu"));
    expect(eq(m->buildConfig.sourceConstraints[0].accel, "cuda12.9+{sm_89}"));
}};

UTest a_table_entry_without_accel_is_just_a_glob{"ManifestSourceConstraints/ATableEntryWithoutAccelIsJustAGlob", [] {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name    = "infer"
version = "0.1.0"
[build]
sources = [{ glob = "src/**/*.cpp" }]
)");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->buildConfig.sources.size(), 1u)));
    expect(m->buildConfig.sourceConstraints.empty());
}};

UTest an_entry_without_glob_is_refused{"ManifestSourceConstraints/AnEntryWithoutGlobIsRefused", [] {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name    = "infer"
version = "0.1.0"
[build]
sources = [{ accel = "cuda12.9+{sm_89}" }]
)");
    expect(fatal(!m.has_value()));
    if (auto&& failed = expect(ne(m.error().format().find("glob"), std::string::npos)); !failed) failed << m.error().format();
}};

UTest an_unknown_key_is_refused_rather_than_skipped{"ManifestSourceConstraints/AnUnknownKeyIsRefusedRatherThanSkipped", [] {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name    = "infer"
version = "0.1.0"
[build]
sources = [{ glob = "src/k/*.cu", acel = "cuda12.9+{sm_89}" }]
)");
    expect(fatal(!m.has_value()));
    if (auto&& failed = expect(ne(m.error().format().find("acel"), std::string::npos)); !failed) failed << m.error().format();
}};

UTest an_exclusion_cannot_be_constrained{"ManifestSourceConstraints/AnExclusionCannotBeConstrained", [] {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name    = "infer"
version = "0.1.0"
[build]
sources = [{ glob = "!src/k/*.cu", accel = "cuda12.9+{sm_89}" }]
)");
    expect(fatal(!m.has_value()));
}};

UTest a_constrained_entry_under_the_conditional_axis_is_refused{"ManifestSourceConstraints/AConstrainedEntryUnderTheConditionalAxisIsRefused", [] {
    // read_list keeps strings only; a table that vanished there would be a
    // device glob nobody ever narrowed, so it is refused with a pointer to
    // where it belongs.
    auto m = mcpp::manifest::parse_string(R"(
[package]
name    = "infer"
version = "0.1.0"
[target.'cfg(linux)'.build]
sources = [{ glob = "src/k/*.cu", accel = "cuda12.9+{sm_89}" }]
)");
    expect(fatal(!m.has_value()));
    if (auto&& failed = expect(ne(m.error().format().find("[build].sources"), std::string::npos)); !failed) failed << m.error().format();
}};

UTest carries_the_list_and_warns_about_nothing{"ManifestExclusive/CarriesTheListAndWarnsAboutNothing", [] {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name      = "compat.cublas"
version   = "0.1.0"
provides  = ["gpu-blas", "gpu-fft"]
exclusive = ["gpu-blas"]
)");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->exclusive.size(), 1u)));
    expect(eq(m->exclusive[0], "gpu-blas"));
    std::string all;
    for (auto const& w : m->schemaWarnings) { all += w; all += '\n'; }
    expect(m->schemaWarnings.empty()) << all;
}};

UTest claiming_something_not_provided_is_reported{"ManifestExclusive/ClaimingSomethingNotProvidedIsReported", [] {
    // Not an error: the declaration is carried so the binding-time check sees
    // exactly what was written. What it cannot do is act on a capability this
    // package does not supply, and a silent no-op is how a typo survives.
    auto m = mcpp::manifest::parse_string(R"(
[package]
name      = "compat.cublas"
version   = "0.1.0"
provides  = ["gpu-blas"]
exclusive = ["gpu-bals"]
)");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->exclusive.size(), 1u)));
    std::string all;
    for (auto const& w : m->schemaWarnings) { all += w; all += '\n'; }
    expect(ne(all.find("gpu-bals"), std::string::npos))
        << "the warning must name the entry that cannot be enforced:\n" << all;
}};

UTest a_feature_provided_capability_counts{"ManifestExclusive/AFeatureProvidedCapabilityCounts", [] {
    // `provides` is not the only way a package supplies a capability, and a
    // check that only looked there would warn about a correct manifest.
    auto m = mcpp::manifest::parse_string(R"(
[package]
name      = "compat.cublas"
version   = "0.1.0"
exclusive = ["gpu-blas"]

[features]
cuda = { provides = ["gpu-blas"] }
)");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    std::string all;
    for (auto const& w : m->schemaWarnings) { all += w; all += '\n'; }
    expect(eq(all.find("gpu-blas"), std::string::npos))
        << "a feature-provided capability is supplied; no warning is due:\n" << all;
}};

// A predicate that carries ONLY a `[target.<pred>.runtime]` table is recorded.
//
// It was not. The gate deciding whether to keep a ConditionalConfig was a
// hand-written disjunction over the fields the writer knew about, and
// `libraries` / `link_library_dirs` were added to the struct without being
// added to it — so the block was parsed, populated and dropped. Adding any
// unrelated key under the same predicate made it work, which is the control
// below: the two manifests differ in one dimension that has nothing to do with
// libraries, and before the fix that dimension decided the outcome.
// `frameworks` is the third per-target runtime key (#622 A2): a table that
// carries only it is recorded, it is not reported as unsupported, and a key
// outside the three is still reported naming all three.
UTest a_conditional_frameworks_table_is_recorded_and_not_warned{"Manifest/AConditionalFrameworksTableIsRecordedAndNotWarned", [] {
    constexpr auto only_frameworks = R"(
[package]
name = "x"
version = "0.1.0"
[target.'cfg(os = "ios")'.runtime]
frameworks = ["UIKit", "MobileCoreServices"]
)";
    auto m = mcpp::manifest::parse_string(only_frameworks);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->conditionalConfigs.size(), 1u)))
        << "a frameworks-only predicate was parsed and then discarded";
    expect(fatal(eq(m->conditionalConfigs[0].frameworks.size(), 2u)));
    expect(eq(m->conditionalConfigs[0].frameworks[0], "UIKit"));
    for (auto const& w : m->schemaWarnings)
        expect(eq(w.find("frameworks"), std::string::npos)) << w;

    constexpr auto a_fourth_key = R"(
[package]
name = "x"
version = "0.1.0"
[target.macos.runtime]
deploy_files = ["x"]
)";
    auto m2 = mcpp::manifest::parse_string(a_fourth_key);
    if (auto&& failed = expect(fatal(m2.has_value())); !failed) failed << m2.error().format();
    bool named = false;
    for (auto const& w : m2->schemaWarnings)
        if (w.find("unsupported key 'deploy_files'") != std::string::npos
            && w.find("frameworks, libraries, link_library_dirs") != std::string::npos)
            named = true;
    expect(bool(named)) << "the unsupported-key report must list the three keys";
}};

UTest a_conditional_runtime_table_alone_is_recorded{"Manifest/AConditionalRuntimeTableAloneIsRecorded", [] {
    constexpr auto only_runtime = R"(
[package]
name = "x"
version = "0.1.0"
[target.linux.runtime]
libraries = ["dl"]
link_library_dirs = ["lib"]
)";
    auto m = mcpp::manifest::parse_string(only_runtime);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->conditionalConfigs.size(), 1u)))
        << "a runtime-only predicate was parsed and then discarded";
    expect(fatal(eq(m->conditionalConfigs[0].libraries.size(), 1u)));
    expect(eq(m->conditionalConfigs[0].libraries[0], "dl"));
    expect(eq(m->conditionalConfigs[0].linkLibraryDirs.size(), 1u));
}};

UTest a_conditional_runtime_table_with_an_unrelated_sibling_is_also_recorded{"Manifest/AConditionalRuntimeTableWithAnUnrelatedSiblingIsAlsoRecorded", [] {
    // The control for the test above. This shape ALWAYS worked, which is why
    // the defect survived: every fixture that exercised the neutral link intent
    // was a generated distribution package, and those carry `ldflags` too.
    constexpr auto with_sibling = R"(
[package]
name = "x"
version = "0.1.0"
[target.linux.runtime]
libraries = ["dl"]
[target.linux.build]
defines = ["UNRELATED_TO_LIBRARIES"]
)";
    auto m = mcpp::manifest::parse_string(with_sibling);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->conditionalConfigs.size(), 1u)));
    expect(fatal(eq(m->conditionalConfigs[0].libraries.size(), 1u)));
    expect(eq(m->conditionalConfigs[0].libraries[0], "dl"));
}};

// An unsupported key in `[runtime]` is REPORTED, not dropped — the rule
// `[build]`, `[target.<triple>]` and `[target.<pred>.build]` have each followed
// since #418/#249/#544, and which these two tables did not.
UTest an_unknown_runtime_key_is_reported{"Manifest/AnUnknownRuntimeKeyIsReported", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[runtime]
libraries = ["m"]
dlopen_lib = ["one"]
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->schemaWarnings.size(), 1u)));
    if (auto&& failed = expect(ne(m->schemaWarnings[0].find("dlopen_lib"), std::string::npos)); !failed) failed << m->schemaWarnings[0];
    // The message names the list the check uses, so a reader can find the key
    // they meant without leaving the message.
    expect(ne(m->schemaWarnings[0].find("dlopen_libs"), std::string::npos))
        << m->schemaWarnings[0];
    // The correctly spelled sibling still took effect.
    expect(fatal(eq(m->runtimeConfig.linkIntent.libraries.size(), 1u)));
}};

UTest an_unknown_conditional_runtime_key_is_reported{"Manifest/AnUnknownConditionalRuntimeKeyIsReported", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[target.linux.runtime]
libraries = ["dl"]
deploy_files = ["x"]
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->schemaWarnings.size(), 1u)));
    // `deploy_files` is a real `[runtime]` key and not a per-target one, so
    // the plausible-looking case is the one held here. (`frameworks` was the
    // example until #622 made it the third per-target key.)
    expect(ne(m->schemaWarnings[0].find("deploy_files"), std::string::npos))
        << m->schemaWarnings[0];
    if (auto&& failed = expect(ne(m->schemaWarnings[0].find("[target.linux.runtime]"), std::string::npos)); !failed) failed << m->schemaWarnings[0];
}};

// The negative control for both sweeps. Without it they pass against a parser
// that reports every key, which reads identically in a green suite.
UTest correctly_spelled_runtime_keys_are_silent{"Manifest/CorrectlySpelledRuntimeKeysAreSilent", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[runtime]
libraries = ["m"]
library_dirs = ["lib"]
dlopen_libs = ["one"]
capabilities = ["cap"]
provides = ["thing"]
frameworks = ["Cocoa"]
link_library_dirs = ["lib"]
transitive_needed_dirs = ["lib"]
runtime_search_dirs = ["lib"]
deploy_files = ["a.txt"]
[runtime.somecapability]
provider = "pkg"
[[runtime.artifacts]]
role       = "interface"
path       = "include"
provenance = "source"
[[runtime.requirements]]
kind  = "capability"
value = "display.present"
phase = "run"
[target.linux.runtime]
libraries = ["dl"]
link_library_dirs = ["lib"]
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    std::string all;
    for (auto const& w : m->schemaWarnings) { all += w; all += '\n'; }
    expect(m->schemaWarnings.empty()) << all;
    // A `[runtime.<capability>]` sub-table is a provider override, not a typo.
    expect(eq(m->runtimeConfig.providerOverrides.at("somecapability"), "pkg"));
    // `[[runtime.artifacts]]` and `[[runtime.requirements]]` are arrays, not
    // tables, so they reach the sweep rather than being skipped with the
    // provider channel. They are exactly what `mcpp pack` emits into every
    // packed library, so a false positive here would warn on all of them.
    expect(eq(m->runtimeConfig.artifacts.size(), 1u));
    expect(eq(m->runtimeConfig.requirements.size(), 1u));
}};

// A manifest authored on Windows commonly begins with a UTF-8 byte-order mark.
// It used to reach the lexer as a bare key and produce `1:1: expected key`, a
// true statement about the token that says nothing about the file.
UTest a_byte_order_mark_on_the_manifest_is_not_an_error{"Manifest/AByteOrderMarkOnTheManifestIsNotAnError", [] {
    const std::string src =
        "\xEF\xBB\xBF"
        "[package]\n"
        "name = \"x\"\n"
        "version = \"0.1.0\"\n";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(eq(m->package.name, "x"));
}};

// ── #615: `runtime.deploy` ──────────────────────────────────────────────────

UTest runtime_deploy_parses_from_and_to{"Manifest/RuntimeDeployParsesFromAndTo", [] {
    constexpr auto src = R"(
[package]
name    = "icd"
version = "0.1.0"
[runtime]
deploy = [
    { from = "share/vulkan/icd.d/lvp_icd.json", to = "vulkan/icd.d" },
    { from = "share/readme.txt", to = "." },
]
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    auto const& d = m->runtimeConfig.linkIntent.deploy;
    expect(fatal(eq(d.size(), 2u)));
    expect(eq(d[0].from, std::filesystem::path("share/vulkan/icd.d/lvp_icd.json")));
    expect(eq(d[0].to, "vulkan/icd.d"));
    expect(eq(d[1].from, std::filesystem::path("share/readme.txt")));
    expect(eq(d[1].to, "."));
    // A key of its own: `deploy_files` is untouched.
    expect(m->runtimeConfig.linkIntent.deployFiles.empty());
    expect(m->schemaWarnings.empty());
}};

UTest runtime_deploy_refuses_each_malformed_entry_naming_it{"Manifest/RuntimeDeployRefusesEachMalformedEntryNamingIt", [] {
    const std::pair<std::string_view, std::string_view> cases[] = {
        {R"({ from = "a.json", to = "../outside" })", "`to` has a `.` or `..` component"},
        {R"({ from = "/etc/a.json", to = "x" })", "`from` is absolute"},
        {R"({ from = "C:/a.json", to = "x" })", "`from` names a drive or a scheme"},
        {R"({ from = "a\\b.json", to = "x" })", "`from` contains a backslash"},
        {R"({ from = "a.json", to = "" })", "`to` is empty"},
        {R"({ from = ".", to = "x" })", "`from` has a `.` or `..` component"},
        {R"({ from = "a//b.json", to = "x" })", "`from` has an empty path component"},
        {R"({ from = "a.json", dest = "x" })", "has unsupported key 'dest'"},
        {R"({ from = "a.json", to = 3 })", "`from` and `to` must be strings"},
        {R"("a.json")", "must be a table with `from` and `to`"},
    };
    for (auto [entry, expected] : cases) {
        const auto src = std::format(R"(
[package]
name    = "icd"
version = "0.1.0"
[runtime]
deploy = [ {} ]
)", entry);
        auto m = mcpp::manifest::parse_string(src);
        expect(fatal(!m.has_value())) << entry;
        if (auto&& failed = expect(ne(m.error().message.find(expected), std::string::npos)); !failed) failed << entry << " -> " << m.error().message;
        if (auto&& failed = expect(ne(m.error().message.find("runtime.deploy[1]"), std::string::npos)); !failed) failed << m.error().message;
    }
}};

// ── `requires_abi` (design 2026-09-12, section 5.2) ────────────────────────

UTest requires_abi_threads_parses_on_the_package_and_on_a_feature{"Manifest/RequiresAbiThreadsParsesOnThePackageAndOnAFeature", [] {
    constexpr auto src = R"(
[package]
name         = "wasmrt"
version      = "0.1.0"
requires_abi = { threads = true }
[features]
mt = { requires_abi = { threads = true } }
st = { requires_abi = { threads = false } }
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(bool(m->requiresAbiThreads));
    expect(fatal(m->featureRequiresAbiThreads.contains("mt")));
    expect(bool(m->featureRequiresAbiThreads.at("mt")));
    expect(fatal(m->featureRequiresAbiThreads.contains("st")));
    expect(!(m->featureRequiresAbiThreads.at("st")));
    expect(m->schemaWarnings.empty());
}};

// `exceptions` joined `threads` as the second `abi` member 2026-09-12 (the UI
// framework record, section 2.1, A1); the accepted set and every refusal
// below name both. See tests/unit/test_abi.cpp for the member's own coverage
// (parsing, rendering, the target-axis `requires_abi` forms).
UTest abi_tables_refuse_anything_but_a_boolean_member{"Manifest/AbiTablesRefuseAnythingButABooleanMember", [] {
    const std::pair<std::string_view, std::string_view> cases[] = {
        {"[package]\nname = \"a\"\nversion = \"0.1.0\"\nrequires_abi = true\n",
         "[package] requires_abi must be a table such as `{ threads = true }`"},
        {"[package]\nname = \"a\"\nversion = \"0.1.0\"\nrequires_abi = { threads = 1 }\n",
         "[package] requires_abi.threads: the members are `threads`, `exceptions`, booleans"},
        {"[package]\nname = \"a\"\nversion = \"0.1.0\"\n[features]\nmt = { requires_abi = { thread = true } }\n",
         "features.mt.requires_abi.thread: the members are `threads`, `exceptions`, booleans"},
        {"[package]\nname = \"a\"\nversion = \"0.1.0\"\n[target.'cfg(os = \"linux\")'.abi]\nthread = true\n",
         "has no member 'thread'; the members are: threads, exceptions"},
        {"[package]\nname = \"a\"\nversion = \"0.1.0\"\n[target.'cfg(os = \"linux\")'.abi]\nthreads = \"yes\"\n",
         ".threads must be true or false"},
    };
    for (auto [src, expected] : cases) {
        auto m = mcpp::manifest::parse_string(src);
        expect(fatal(!m.has_value())) << src;
        if (auto&& failed = expect(ne(m.error().message.find(expected), std::string::npos)); !failed) failed << src << " -> " << m.error().message;
    }
}};

// ── The per-target tool declaration (design 2026-09-12, section 5.1) ───────
//
// A package written directly under `[target.<selector>.xlings]` is refused, and
// the refusal names the table that does accept it, which is the statement the
// SDK batch's record was missing when it concluded the declaration did not exist.
UTest target_xlings_refusal_names_the_workspace_table{"Manifest/TargetXlingsRefusalNamesTheWorkspaceTable", [] {
    constexpr auto src = R"(
[package]
name    = "app"
version = "0.1.0"
[target.aarch64-ios-sim.xlings]
"xim:apple-simulator-tools" = ""
)";
    auto m = mcpp::manifest::parse_string(src);
    expect(fatal(!m.has_value()));
    if (auto&& failed = expect(ne(m.error().message.find(
                  "[target.aarch64-ios-sim.xlings] does not accept 'xim:apple-simulator-tools'"), std::string::npos)); !failed) failed << m.error().message;
    if (auto&& failed = expect(ne(m.error().message.find("[target.aarch64-ios-sim.xlings.workspace]"), std::string::npos)); !failed) failed << m.error().message;
}};

// ─── `linkage` beside `kind`: a library's default form (#642 E1) ────────────
//
// `kind = "shared"` is a constraint; `linkage` is the answer a consumer gets
// when it asks for nothing. The parse accepts the default on a library, refuses
// it beside the constraint and on a program, and a row states one of the two.
UTest target_linkage_is_parsed_as_the_librarys_default_form{"Manifest/TargetLinkageIsParsedAsTheLibrarysDefaultForm", [] {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name    = "fw"
version = "0.1.0"
[targets.fw]
kind    = "lib"
linkage = "shared"
)");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->targets.size(), 1u)));
    expect(eq(m->targets[0].kind, mcpp::manifest::Target::Library));
    expect(eq(m->targets[0].linkageDefault, "shared"));
    expect(eq(m->targets[0].linkageDeclaredBy, "[targets.fw] linkage = \"shared\""));
    // A known key: no schema warning.
    for (auto const& w : m->schemaWarnings)
        expect(eq(w.find("'linkage'"), std::string::npos)) << w;
}};

UTest target_linkage_is_refused_beside_kind_shared_and_on_a_program{"Manifest/TargetLinkageIsRefusedBesideKindSharedAndOnAProgram", [] {
    auto both = mcpp::manifest::parse_string(R"(
[package]
name    = "fw"
version = "0.1.0"
[targets.fw]
kind    = "shared"
linkage = "static"
)");
    expect(fatal(!both.has_value()));
    if (auto&& failed = expect(ne(both.error().message.find(
                  "states both `kind = \"shared\"` and `linkage = \"static\"`"), std::string::npos)); !failed) failed << both.error().message;

    auto program = mcpp::manifest::parse_string(R"(
[package]
name    = "app"
version = "0.1.0"
[targets.app]
kind    = "bin"
main    = "src/main.cpp"
linkage = "shared"
)");
    expect(fatal(!program.has_value()));
    if (auto&& failed = expect(ne(program.error().message.find("a program target has no link form"), std::string::npos)); !failed) failed << program.error().message;

    auto value = mcpp::manifest::parse_string(R"(
[package]
name    = "fw"
version = "0.1.0"
[targets.fw]
kind    = "lib"
linkage = "dynamic"
)");
    expect(fatal(!value.has_value()));
    if (auto&& failed = expect(ne(value.error().message.find("`static` or `shared`"), std::string::npos)); !failed) failed << value.error().message;
}};

UTest a_row_states_either_kind_or_linkage{"Manifest/ARowStatesEitherKindOrLinkage", [] {
    auto ok = mcpp::manifest::parse_string(R"(
[package]
name    = "fw"
version = "0.1.0"
[targets.fw]
kind = "lib"
[target.'cfg(os = "linux")'.targets.fw]
linkage = "shared"
)");
    if (auto&& failed = expect(fatal(ok.has_value())); !failed) failed << ok.error().format();
    expect(fatal(eq(ok->conditionalConfigs.size(), 1u)));
    auto const& rows = ok->conditionalConfigs[0].targetKinds;
    expect(fatal(eq(rows.count("fw"), 1u)));
    expect(eq(rows.at("fw").linkage, "shared"));
    expect(eq(rows.at("fw").statement, "[target.'cfg(os = \"linux\")'.targets.fw] linkage = \"shared\""));
    for (auto const& w : ok->schemaWarnings)
        expect(eq(w.find("unsupported key"), std::string::npos)) << w;

    auto both = mcpp::manifest::parse_string(R"(
[package]
name    = "fw"
version = "0.1.0"
[targets.fw]
kind = "lib"
[target.'cfg(os = "linux")'.targets.fw]
kind    = "shared"
linkage = "shared"
)");
    expect(fatal(!both.has_value()));
    if (auto&& failed = expect(ne(both.error().message.find("states both `kind` and `linkage`"), std::string::npos)); !failed) failed << both.error().message;
}};

namespace {

mcpp::manifest::Manifest merged_for_linux(std::string_view src) {
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(m.has_value()); !failed) failed << (m.has_value() ? "" : m.error().format());
    if (!m) return {};
    mcpp::build::merge_conditional_config(
        *m, mcpp::build::cfgpred::context_for("x86_64-unknown-linux-gnu"));
    return *m;
}

} // namespace

UTest a_row_default_replaces_an_unconditional_kind_shared{"Manifest/ARowDefaultReplacesAnUnconditionalKindShared", [] {
    auto m = merged_for_linux(R"(
[package]
name    = "fw"
version = "0.1.0"
[targets.fw]
kind = "shared"
[target.'cfg(os = "linux")'.targets.fw]
linkage = "static"
)");
    expect(fatal(eq(m.targets.size(), 1u)));
    expect(eq(m.targets[0].kind, mcpp::manifest::Target::Library));
    expect(eq(m.targets[0].linkageDefault, "static"));
}};

UTest a_row_kind_clears_an_unconditional_default{"Manifest/ARowKindClearsAnUnconditionalDefault", [] {
    auto m = merged_for_linux(R"(
[package]
name    = "fw"
version = "0.1.0"
[targets.fw]
kind    = "lib"
linkage = "shared"
[target.'cfg(os = "linux")'.targets.fw]
kind = "lib"
)");
    expect(fatal(eq(m.targets.size(), 1u)));
    expect(eq(m.targets[0].kind, mcpp::manifest::Target::Library));
    expect(m.targets[0].linkageDefault.empty());
}};

UTest a_row_that_does_not_match_leaves_the_unconditional_statement{"Manifest/ARowThatDoesNotMatchLeavesTheUnconditionalStatement", [] {
    auto m = merged_for_linux(R"(
[package]
name    = "fw"
version = "0.1.0"
[targets.fw]
kind    = "lib"
linkage = "shared"
[target.'cfg(os = "windows")'.targets.fw]
kind = "shared"
)");
    expect(fatal(eq(m.targets.size(), 1u)));
    expect(eq(m.targets[0].kind, mcpp::manifest::Target::Library));
    expect(eq(m.targets[0].linkageDefault, "shared"));
}};

// #709: `[features.<f>] tools` names the package's own bin targets, and is
// checked once inference has produced the target list.
UTest a_feature_tool_must_name_a_bin_target_of_the_package{"Manifest/AFeatureToolMustNameABinTargetOfThePackage", [] {
    auto dir = std::filesystem::temp_directory_path()
        / std::format("mcpp_feature_tools_{}", std::random_device{}());
    std::filesystem::create_directories(dir / "src");
    std::ofstream(dir / "src" / "gen.cpp") << "int main() {}\n";
    auto write = [&](std::string_view tool) {
        std::ofstream(dir / "mcpp.toml")
            << "[package]\nname = \"gen\"\nversion = \"0.1.0\"\n\n"
            << "[features.codegen]\ntools = [\"" << tool << "\"]\n\n"
            << "[targets.gen-tool]\nkind = \"bin\"\nmain = \"src/gen.cpp\"\n";
    };
    write("gen-tool");
    auto ok = mcpp::manifest::load(dir / "mcpp.toml");
    if (auto&& failed = expect(fatal(bool(ok))); !failed) failed << (ok ? "" : ok.error().message);
    expect(fatal(eq(ok->featureTools.at("codegen").size(), 1u)));
    expect(eq(ok->featureTools.at("codegen")[0], "gen-tool"));

    write("nosuch");
    auto bad = mcpp::manifest::load(dir / "mcpp.toml");
    expect(fatal(!bad));
    if (auto&& failed = expect(ne(bad.error().message.find("nosuch"), std::string::npos)); !failed) failed << bad.error().message;
    if (auto&& failed = expect(ne(bad.error().message.find("gen-tool"), std::string::npos)); !failed) failed << bad.error().message;

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}};

// #714: `sources = []` states that the default build compiles nothing, so a
// module interface under `src/` is not a library of that build. A build-logic
// package keeps its module behind a feature that host-module consumers
// request; inferring a library for it made every build of the package link an
// archive with no inputs. A glob that happens to match nothing is a different
// statement and keeps the inferred library (and #533's empty-link refusal).
UTest an_explicitly_empty_source_list_infers_no_library{"Manifest/AnExplicitlyEmptySourceListInfersNoLibrary", [] {
    auto dir = std::filesystem::temp_directory_path()
        / std::format("mcpp_empty_sources_{}", std::random_device{}());
    std::filesystem::create_directories(dir / "src");
    std::ofstream(dir / "src" / "buildlib.cppm") << "export module buildlib;\n";
    {
        std::ofstream(dir / "mcpp.toml")
            << "[package]\nname = \"buildlib\"\nversion = \"0.1.0\"\n\n"
               "[build]\nsources = []\n\n"
               "[features.host]\nsources = [\"src/buildlib.cppm\"]\n";
    }
    auto empty = mcpp::manifest::load(dir / "mcpp.toml");
    if (auto&& failed = expect(fatal(bool(empty))); !failed) failed << (empty ? "" : empty.error().message);
    expect(empty->targets.empty());

    {
        std::ofstream(dir / "mcpp.toml")
            << "[package]\nname = \"buildlib\"\nversion = \"0.1.0\"\n\n"
               "[build]\nsources = [\"srcs/**/*.cppm\"]\n";
    }
    auto typo = mcpp::manifest::load(dir / "mcpp.toml");
    if (auto&& failed = expect(fatal(bool(typo))); !failed) failed << (typo ? "" : typo.error().message);
    expect(fatal(eq(typo->targets.size(), 1u)));
    expect(eq(typo->targets[0].kind, mcpp::manifest::Target::Library));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}};

// #649 E6: a consumer reads "every declared target is a program" as "a tool
// provider that contributes nothing to my graph". A target list the loader
// INFERRED from the tree is not that statement, so the loader says which of
// the two it produced.
UTest targets_inferred_is_distinguished_from_declared{"Manifest/TargetsInferredIsDistinguishedFromDeclared", [] {
    auto dir = std::filesystem::temp_directory_path()
        / std::format("mcpp_targets_inferred_{}", std::random_device{}());
    std::filesystem::create_directories(dir / "src");
    {
        std::ofstream(dir / "src" / "main.cpp") << "int main() {}\n";
        std::ofstream(dir / "mcpp.toml") << "[package]\nname = \"inferred\"\nversion = \"0.1.0\"\n";
    }
    auto inferred = mcpp::manifest::load(dir / "mcpp.toml");
    if (auto&& failed = expect(fatal(bool(inferred))); !failed) failed << (inferred ? "" : inferred.error().message);
    expect(fatal(eq(inferred->targets.size(), 1u)));
    expect(eq(inferred->targets[0].kind, mcpp::manifest::Target::Binary));
    expect(bool(inferred->targetsInferred));

    {
        std::ofstream(dir / "mcpp.toml")
            << "[package]\nname = \"declared\"\nversion = \"0.1.0\"\n\n"
               "[targets.declared]\nkind = \"bin\"\nmain = \"src/main.cpp\"\n";
    }
    auto declared = mcpp::manifest::load(dir / "mcpp.toml");
    if (auto&& failed = expect(fatal(bool(declared))); !failed) failed << (declared ? "" : declared.error().message);
    expect(fatal(eq(declared->targets.size(), 1u)));
    expect(!declared->targetsInferred);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}};

// ── #647 E1 / #649 X4: `[package.metadata]` and the `[package]` key check ────
//
// The metadata table is kept verbatim for the graph document, as JSON text, and
// `metadata` is a known key. Any other key the parser does not read is reported
// the way `[build]` reports one, so `licence` is no longer accepted in silence.
UTest package_metadata_is_kept_verbatim_as_json{"Manifest/PackageMetadataIsKeptVerbatimAsJson", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[package.metadata.demo]
resources = "res"
langs = ["en", "zh"]
count = 3
enabled = true
note = "a \"quoted\" value"
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(eq(m->packageMetadataJson, R"({"demo":{"count":3,"enabled":true,"langs":["en","zh"],"note":"a \"quoted\" value","resources":"res"}})"));
    if (auto&& failed = expect(m->schemaWarnings.empty()); !failed) failed << (m->schemaWarnings.empty() ? "" : m->schemaWarnings[0]);
}};

UTest package_without_metadata_has_none{"Manifest/PackageWithoutMetadataHasNone", [] {
    auto m = mcpp::manifest::parse_string("[package]\nname = \"x\"\nversion = \"0.1.0\"\n");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(m->packageMetadataJson.empty());
}};

UTest unknown_package_key_is_reported{"Manifest/UnknownPackageKeyIsReported", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
licence = "MIT"
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->schemaWarnings.size(), 1u)));
    if (auto&& failed = expect(ne(m->schemaWarnings[0].find("[package] has unsupported key 'licence'"), std::string::npos)); !failed) failed << m->schemaWarnings[0];
}};

UTest every_read_package_key_is_known{"Manifest/EveryReadPackageKeyIsKnown", [] {
    constexpr auto src = R"(
[package]
name = "x"
namespace = "ns"
version = "0.1.0"
standard = "c++23"
description = "d"
license = "MIT"
repo = "https://example.org"
authors = ["a"]
platforms = ["linux"]
accelerators = ["cuda"]
provides = ["cap"]
requires = ["other"]
exclusive = ["cap"]
std-module = "m"
std-compat-module = "c"
std-module-flags = ["-x"]
c-environment = "platform"
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    for (auto const& w : m->schemaWarnings)
        expect(eq(w.find("[package] has unsupported key"), std::string::npos)) << w;
    expect(eq(m->cEnvironment, "platform"));
}};

// ── `[c-abi]` — the C environment a `mcpp:c-abi=<impl>` provider declares
// (design 2026-09-18 §3.2) ──────────────────────────────────────────────────

UTest c_abi_block_parses_on_a_provider_of_the_layer{"Manifest/CAbiBlockParsesOnAProviderOfTheLayer", [] {
    constexpr auto src = R"(
[package]
name     = "openkal-musl"
version  = "0.15.0"
provides = ["mcpp:c-abi=musl"]

[c-abi]
presents   = "posix"
data-model = "arch-default"
wchar      = 32
builtins   = "iso"
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(m->cAbiDecl.has_value()));
    expect(bool(m->cAbiDecl->declared));
    expect(eq(m->cAbiDecl->presents, mcpp::targetside::CAbiPresents::Posix));
    expect(eq(m->cAbiDecl->dataModel, mcpp::targetside::CAbiDataModel::ArchDefault));
    expect(eq(m->cAbiDecl->wcharBits, 32));
    expect(eq(m->cAbiDecl->builtins, mcpp::targetside::CAbiBuiltins::Iso));
}};

// `builtins` alone defaults — the other three carry no default (§3.2).
UTest c_abi_builtins_defaults_to_platform{"Manifest/CAbiBuiltinsDefaultsToPlatform", [] {
    constexpr auto src = R"(
[package]
name     = "openkal-musl"
version  = "0.15.0"
provides = ["mcpp:c-abi=musl"]

[c-abi]
presents   = "posix"
data-model = "arch-default"
wchar      = 32
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(m->cAbiDecl.has_value()));
    expect(eq(m->cAbiDecl->builtins, mcpp::targetside::CAbiBuiltins::Platform));
}};

// A package that does not provide the layer has no standing to state what
// environment it presents — an internal contradiction, refused rather than
// silently ignored (§3.2, gap #3 of the design's own self-review).
UTest c_abi_block_without_providing_the_layer_is_refused{"Manifest/CAbiBlockWithoutProvidingTheLayerIsRefused", [] {
    constexpr auto src = R"(
[package]
name    = "not-a-c-library"
version = "0.1.0"

[c-abi]
presents   = "posix"
data-model = "arch-default"
wchar      = 32
)";
    auto m = mcpp::manifest::parse_string(src);
    expect(fatal(!m.has_value()));
    if (auto&& failed = expect(ne(m.error().message.find(
        "[package] provides does not list `mcpp:c-abi=<impl>`"), std::string::npos)); !failed) failed << m.error().message;
}};

UTest c_abi_requires_all_three_keys_with_no_default{"Manifest/CAbiRequiresAllThreeKeysWithNoDefault", [] {
    const std::pair<std::string_view, std::string_view> cases[] = {
        {R"([package]
name = "a"
version = "0.1.0"
provides = ["mcpp:c-abi=x"]
[c-abi]
data-model = "arch-default"
wchar = 32
)", "[c-abi] is missing `presents`"},
        {R"([package]
name = "a"
version = "0.1.0"
provides = ["mcpp:c-abi=x"]
[c-abi]
presents = "posix"
wchar = 32
)", "[c-abi] is missing `data-model`"},
        {R"([package]
name = "a"
version = "0.1.0"
provides = ["mcpp:c-abi=x"]
[c-abi]
presents = "posix"
data-model = "arch-default"
)", "[c-abi] is missing `wchar`"},
    };
    for (auto [src, expected] : cases) {
        auto m = mcpp::manifest::parse_string(src);
        expect(fatal(!m.has_value())) << src;
        if (auto&& failed = expect(ne(m.error().message.find(expected), std::string::npos)); !failed) failed << src << " -> " << m.error().message;
    }
}};

UTest c_abi_closed_value_sets_reject_a_misspelling{"Manifest/CAbiClosedValueSetsRejectAMisspelling", [] {
    const std::pair<std::string_view, std::string_view> cases[] = {
        {R"([package]
name = "a"
version = "0.1.0"
provides = ["mcpp:c-abi=x"]
[c-abi]
presents = "posixx"
data-model = "arch-default"
wchar = 32
)", "presents = \"posixx\" names no known environment identity"},
        {R"([package]
name = "a"
version = "0.1.0"
provides = ["mcpp:c-abi=x"]
[c-abi]
presents = "posix"
data-model = "lp65"
wchar = 32
)", "data-model = \"lp65\" names no known data model"},
        {R"([package]
name = "a"
version = "0.1.0"
provides = ["mcpp:c-abi=x"]
[c-abi]
presents = "posix"
data-model = "arch-default"
wchar = 17
)", "[c-abi].wchar must be the integer 16 or 32"},
        {R"([package]
name = "a"
version = "0.1.0"
provides = ["mcpp:c-abi=x"]
[c-abi]
presents = "posix"
data-model = "arch-default"
wchar = 32
builtins = "gnuish"
)", "builtins = \"gnuish\" names no known policy"},
        {R"([package]
name = "a"
version = "0.1.0"
provides = ["mcpp:c-abi=x"]
[c-abi]
presents = "posix"
data-model = "arch-default"
wchar = 32
extra = 1
)", "[c-abi] has no member 'extra'"},
    };
    for (auto [src, expected] : cases) {
        auto m = mcpp::manifest::parse_string(src);
        expect(fatal(!m.has_value())) << src;
        if (auto&& failed = expect(ne(m.error().message.find(expected), std::string::npos)); !failed) failed << src << " -> " << m.error().message;
    }
}};

// A package with no [c-abi] block resolves `cAbiDecl` to nullopt — the
// absent-block case §3.2 requires to be byte-identical to today.
UTest no_c_abi_block_leaves_c_abi_decl_empty{"Manifest/NoCAbiBlockLeavesCAbiDeclEmpty", [] {
    constexpr auto src = R"(
[package]
name     = "openkal-musl"
version  = "0.15.0"
provides = ["mcpp:c-abi=musl"]
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(!(m->cAbiDecl.has_value()));
}};

// `[package] c-environment` — design §3.4. The only value is "platform".
UTest c_environment_accepts_only_platform{"Manifest/CEnvironmentAcceptsOnlyPlatform", [] {
    constexpr auto ok = R"(
[package]
name = "openkal-windows"
version = "0.1.0"
provides = ["mcpp:kernel-abi=openkal"]
c-environment = "platform"
)";
    auto m = mcpp::manifest::parse_string(ok);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(eq(m->cEnvironment, "platform"));

    constexpr auto bad = R"(
[package]
name = "x"
version = "0.1.0"
c-environment = "native"
)";
    auto m2 = mcpp::manifest::parse_string(bad);
    expect(fatal(!m2.has_value()));
    if (auto&& failed = expect(ne(m2.error().message.find(
        "c-environment = \"native\" names no known override"), std::string::npos)); !failed) failed << m2.error().message;
}};

// `c-environment = "platform"` is INFERRED for a `mcpp:kernel-abi=<impl>`
// provider (coordinator revision, openkal-musl spike: openkal-windows called
// Win32 with a POSIX-substituted `wchar_t` width and misread its own UTF-16
// results). A kernel-abi provider IS the platform boundary by definition, so
// it must never receive the presented [c-abi] environment even when its own
// manifest says nothing about it — this pins the default, not merely the
// explicit key `CEnvironmentAcceptsOnlyPlatform` above already covers. An
// ordinary package, providing nothing kernel-abi-shaped, gets no such
// inference: `cEnvironment` stays empty and the realisation reaches it.
// ── [c-abi-absent] — enumerate the exception, not the rule ─────────────────
//
// The set of POSIX names a C library supplies is not enumerable in a manifest;
// the set it does not supply is. openkal-musl's README lists six in prose, and
// that prose has no executor — it was contradicted once already (0.16.0: a
// disposition accepted for every signal and installed for none).

UTest ac_library_may_enumerate_what_it_does_not_supply{"Manifest/ACLibraryMayEnumerateWhatItDoesNotSupply", [] {
    constexpr auto src = R"(
[package]
name     = "openkal-musl"
version  = "0.17.0"
provides = ["mcpp:c-abi=musl"]

[c-abi]
presents   = "posix"
data-model = "arch-default"
wchar      = 32
builtins   = "iso"

[c-abi-absent]
fork     = { form = "link" }
mprotect = { form = "enosys", note = "openkal has no operation upon a mapping's protection" }
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(m->cAbiDecl.has_value()));
    expect(fatal(eq(m->cAbiDecl->absent.size(), 2u)));
    // Sorted by name, so a diagnostic built from this list reads the same way
    // on every run and a test may index it.
    expect(eq(m->cAbiDecl->absent[0].name, "fork"));
    expect(eq(m->cAbiDecl->absent[0].form, mcpp::targetside::CAbiAbsentForm::Link));
    expect(eq(m->cAbiDecl->absent[1].name, "mprotect"));
    expect(eq(m->cAbiDecl->absent[1].form, mcpp::targetside::CAbiAbsentForm::Enosys));
    expect(!(m->cAbiDecl->absent[1].note.empty()));
}};

UTest an_absence_with_no_named_shape_is_refused{"Manifest/AnAbsenceWithNoNamedShapeIsRefused", [] {
    // A facility absent in an unnamed shape is one nothing can assert
    // against, which is the whole reason for writing it down.
    constexpr auto src = R"(
[package]
name     = "openkal-musl"
version  = "0.17.0"
provides = ["mcpp:c-abi=musl"]

[c-abi]
presents   = "posix"
data-model = "arch-default"
wchar      = 32

[c-abi-absent]
fork = { note = "no process image duplication" }
)";
    auto m = mcpp::manifest::parse_string(src);
    expect(fatal(!m.has_value()));
    auto msg = m.error().format();
    expect(ne(msg.find("form"), std::string::npos)) << msg;
}};

UTest an_unknown_absent_shape_names_the_three_that_exist{"Manifest/AnUnknownAbsentShapeNamesTheThreeThatExist", [] {
    constexpr auto src = R"(
[package]
name     = "openkal-musl"
version  = "0.17.0"
provides = ["mcpp:c-abi=musl"]

[c-abi]
presents   = "posix"
data-model = "arch-default"
wchar      = 32

[c-abi-absent]
fork = { form = "sometimes" }
)";
    auto m = mcpp::manifest::parse_string(src);
    expect(fatal(!m.has_value()));
    auto msg = m.error().format();
    expect(ne(msg.find("accepted-no-effect"), std::string::npos)) << msg;
}};

UTest the_accepted_no_effect_shape_has_a_name{"Manifest/TheAcceptedNoEffectShapeHasAName", [] {
    // It is the shape of the defect openkal-musl 0.16.0 repaired: a call that
    // succeeds and does not do part of what it was asked. Naming it is what
    // makes "how many of these are there" a question with an answer.
    constexpr auto src = R"(
[package]
name     = "openkal-musl"
version  = "0.17.0"
provides = ["mcpp:c-abi=musl"]

[c-abi]
presents   = "posix"
data-model = "arch-default"
wchar      = 32

[c-abi-absent]
tcsetattr = { form = "accepted-no-effect", note = "the fields openkal does not name are not applied" }
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->cAbiDecl->absent.size(), 1u)));
    expect(eq(m->cAbiDecl->absent[0].form, mcpp::targetside::CAbiAbsentForm::AcceptedNoEffect));
}};

UTest the_absence_table_is_top_level_so_older_engines_ignore_it{"Manifest/TheAbsenceTableIsTopLevelSoOlderEnginesIgnoreIt", [] {
    // WHY THIS TABLE IS NOT `[c-abi].absent`, WHICH READS BETTER. Measured
    // against the genuine published 2026.9.18.3 archive -- the index floor
    // when this shipped -- on the exact manifest openkal-musl 0.17.0
    // publishes: nested, every older engine refuses THE WHOLE MANIFEST on
    // every target ("[c-abi] has no member 'absent'"), because the [c-abi]
    // parser enumerates its members. An unknown top-level table is ignored
    // and the build completes.
    //
    // That difference decides whether the table can ship without taking the
    // index away from every client below this release, and nothing else in
    // the engine records it: the two spellings are indistinguishable to every
    // test that only reads the parsed result. So this test asserts the shape
    // directly -- `absent` is NOT a member of [c-abi] -- and the hint that
    // catches the natural mistake.
    constexpr auto src = R"(
[package]
name     = "openkal-musl"
version  = "0.17.0"
provides = ["mcpp:c-abi=musl"]

[c-abi]
presents   = "posix"
data-model = "arch-default"
wchar      = 32
absent     = { fork = { form = "link" } }
)";
    auto m = mcpp::manifest::parse_string(src);
    expect(fatal(!m.has_value()));
    auto msg = m.error().format();
    expect(ne(msg.find("has no member 'absent'"), std::string::npos)) << msg;
    expect(ne(msg.find("[c-abi-absent]"), std::string::npos)) << msg;
}};

UTest absences_may_be_stated_without_an_environment_declaration{"Manifest/AbsencesMayBeStatedWithoutAnEnvironmentDeclaration", [] {
    // The two tables are independent. A C library whose environment identity
    // is already its target's default has nothing to put in [c-abi], and is
    // still entitled to say what it does not supply. `declared` stays false,
    // which is what keeps `cenv::realise` away from fields nobody wrote.
    constexpr auto src = R"(
[package]
name     = "openkal-musl"
version  = "0.17.0"
provides = ["mcpp:c-abi=musl"]

[c-abi-absent]
fork = { form = "link" }
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(m->cAbiDecl.has_value()));
    expect(!m->cAbiDecl->declared);
    expect(fatal(eq(m->cAbiDecl->absent.size(), 1u)));
    expect(eq(m->cAbiDecl->absent[0].name, "fork"));
}};

UTest only_the_c_library_may_state_what_is_absent{"Manifest/OnlyTheCLibraryMayStateWhatIsAbsent", [] {
    // The gate [c-abi] applies, for the reason it applies it. Moving the
    // table to the top level moved it out from behind that gate, so the gate
    // is restated here rather than inherited -- and this is the test that
    // would notice if a later edit dropped it.
    constexpr auto src = R"(
[package]
name    = "an-ordinary-program"
version = "0.1.0"

[c-abi-absent]
fork = { form = "link" }
)";
    auto m = mcpp::manifest::parse_string(src);
    expect(fatal(!m.has_value()));
    auto msg = m.error().format();
    expect(ne(msg.find("mcpp:c-abi="), std::string::npos)) << msg;
}};

UTest ac_library_that_enumerates_nothing_has_claimed_nothing{"Manifest/ACLibraryThatEnumeratesNothingHasClaimedNothing", [] {
    constexpr auto src = R"(
[package]
name     = "openkal-musl"
version  = "0.17.0"
provides = ["mcpp:c-abi=musl"]

[c-abi]
presents   = "posix"
data-model = "arch-default"
wchar      = 32
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(m->cAbiDecl.has_value()));
    expect(m->cAbiDecl->absent.empty());
}};

// ── [kernel-abi] provides-interfaces / requires-interfaces ─────────────────
//
// The resolution-time half of the capability model (design 2026-09-20 §5.5).
// The engine knows no interface name; these tests therefore use openkal's
// real names in one direction and an invented one in the other, and neither
// changes what the parser does.

UTest a_kernel_abi_provider_may_state_which_interfaces_it_provides{"Manifest/AKernelAbiProviderMayStateWhichInterfacesItProvides", [] {
    constexpr auto src = R"(
[package]
name = "openkal-windows"
version = "0.9.0"
provides = ["mcpp:kernel-abi=openkal"]

[kernel-abi]
provides-interfaces = ["openkal.abort", "openkal.stream", "openkal.memory"]
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->kernelAbiProvidesInterfaces.size(), 3u)));
    expect(eq(m->kernelAbiProvidesInterfaces[0], "openkal.abort"));
    expect(m->kernelAbiRequiresInterfaces.empty());
}};

UTest a_package_that_does_not_supply_the_layer_may_not_state_what_it_provides{"Manifest/APackageThatDoesNotSupplyTheLayerMayNotStateWhatItProvides", [] {
    // The same rule `[c-abi]` follows, and for the same reason: a package
    // without the layer is stating a fact about something it does not have.
    constexpr auto src = R"(
[package]
name = "some-library"
version = "1.0.0"

[kernel-abi]
provides-interfaces = ["openkal.fs"]
)";
    auto m = mcpp::manifest::parse_string(src);
    expect(fatal(!m.has_value()))
        << "a package with no `mcpp:kernel-abi=<impl>` in provides must be "
           "refused when it states provides-interfaces";
    auto msg = m.error().format();
    expect(ne(msg.find("mcpp:kernel-abi"), std::string::npos)) << msg;
}};

UTest any_consumer_may_state_which_interfaces_it_requires{"Manifest/AnyConsumerMayStateWhichInterfacesItRequires", [] {
    // `requires-interfaces` is a statement about the package making it, so it
    // carries no such restriction. openkal SPEC 0.14 §3.3: "a consumer states
    // its required set per target, in its own package, which is where a
    // convention among consumers belongs".
    constexpr auto src = R"(
[package]
name = "some-library"
version = "1.0.0"

[kernel-abi]
requires-interfaces = ["openkal.fs", "openkal.net"]
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->kernelAbiRequiresInterfaces.size(), 2u)));
    expect(m->kernelAbiProvidesInterfaces.empty());
}};

UTest an_unknown_kernel_abi_member_is_a_parse_error_naming_the_key{"Manifest/AnUnknownKernelAbiMemberIsAParseErrorNamingTheKey", [] {
    constexpr auto src = R"(
[package]
name = "openkal-linux"
version = "0.14.0"
provides = ["mcpp:kernel-abi=openkal"]

[kernel-abi]
provides-interface = ["openkal.fs"]
)";
    auto m = mcpp::manifest::parse_string(src);
    expect(fatal(!m.has_value())) << "a misspelled member must not be ignored";
    auto msg = m.error().format();
    expect(ne(msg.find("provides-interface"), std::string::npos)) << msg;
}};

UTest a_package_with_no_kernel_abi_table_states_nothing{"Manifest/APackageWithNoKernelAbiTableStatesNothing", [] {
    // The guarantee every addition to this parser owes the graph that predates
    // it: a manifest that writes nothing here is byte-identical in effect.
    constexpr auto src = R"(
[package]
name = "some-library"
version = "1.0.0"
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(m->kernelAbiProvidesInterfaces.empty());
    expect(m->kernelAbiRequiresInterfaces.empty());
}};

UTest c_environment_is_inferred_for_a_kernel_abi_provider{"Manifest/CEnvironmentIsInferredForAKernelAbiProvider", [] {
    constexpr auto kernelAbiPkg = R"(
[package]
name = "openkal-windows"
version = "0.8.0"
provides = ["mcpp:kernel-abi=openkal"]
)";
    auto m = mcpp::manifest::parse_string(kernelAbiPkg);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(eq(m->cEnvironment, "platform"))
        << "a kernel-abi provider must default to the platform boundary "
           "without having to declare c-environment itself";

    constexpr auto ordinaryPkg = R"(
[package]
name = "some-library"
version = "1.0.0"
)";
    auto m2 = mcpp::manifest::parse_string(ordinaryPkg);
    if (auto&& failed = expect(fatal(m2.has_value())); !failed) failed << m2.error().format();
    expect(m2->cEnvironment.empty())
        << "an ordinary package must NOT be inferred into the platform "
           "boundary -- only a kernel-abi provider is";

    // A package that provides c-abi (not kernel-abi) is not the boundary
    // either, and must not be inferred.
    constexpr auto cAbiPkg = R"(
[package]
name = "openkal-musl"
version = "0.15.0"
provides = ["mcpp:c-abi=musl"]
)";
    auto m3 = mcpp::manifest::parse_string(cAbiPkg);
    if (auto&& failed = expect(fatal(m3.has_value())); !failed) failed << m3.error().format();
    expect(m3->cEnvironment.empty());
}};

// The same inference, through the OTHER manifest parser: an xpkg descriptor
// (index packages -- exactly how a released kernel-abi implementation like
// openkal-windows is actually consumed) has no explicit `c-environment` key
// at all (docs/22's own documented gap), so the inference here is
// unconditional rather than "if empty" -- and this is the test that pins
// that it still runs.
UTest synthesize_from_xpkg_lua_c_environment_is_inferred_for_a_kernel_abi_provider{"SynthesizeFromXpkgLua/CEnvironmentIsInferredForAKernelAbiProvider", [] {
    constexpr auto lua = R"(
package = {
    spec = "1",
    name = "openkal-windows",
    xpm  = { windows = { ["0.8.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        sources  = { "*/anchor.c" },
        provides = { "mcpp:kernel-abi=openkal" },
        targets  = { ["openkal-windows"] = { kind = "lib" } },
    },
}
)";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(
        lua, "openkal-windows", "0.8.0", mcpp::toolchain::platform::HostPlatform::current());
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(eq(m->cEnvironment, "platform"));
}};

// ── #728, D7: matching conditional tables apply in order of specificity ──────
//
// A more specific selector applies later, so it replaces a broader one's scalar
// and its list values come after; lexical order of the selector text only
// breaks a tie. The triple below sorts BEFORE `linux` and `cfg(unix)`, so under
// the lexical order these tables used to apply in, the family table was the
// last word on an aarch64 Linux target.
UTest specificity_ranks_a_triple_over_an_os_over_a_family{"ConditionalOrder/SpecificityRanksATripleOverAnOsOverAFamily", [] {
    using mcpp::manifest::cfg::specificity;
    expect(eq(specificity("x86_64-unknown-linux-gnu"), mcpp::manifest::cfg::kTripleRank));
    expect(eq(specificity("linux"), 2));
    expect(eq(specificity("cfg(os = \"linux\")"), 2));
    expect(eq(specificity("unix"), 1));
    expect(eq(specificity("cfg(family = \"unix\")"), 1));
    expect(eq(specificity("cfg(arch = \"x86_64\")"), 1));
    expect(eq(specificity("cfg(all(os = \"linux\", arch = \"aarch64\"))"), 3));
    expect(eq(specificity("cfg(any(linux, windows))"), 0));
    expect(eq(specificity("cfg(not(windows))"), 0));
    expect(eq(specificity("cfg(c-abi = \"musl\")"), 0));
    expect(gt(specificity("aarch64-unknown-linux-gnu"), specificity("cfg(all(os = \"linux\", arch = \"aarch64\"))")));
}};

UTest a_more_specific_table_applies_later{"ConditionalOrder/AMoreSpecificTableAppliesLater", [] {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
[build]
cxxflags = ["-DBASE"]
[target.aarch64-unknown-linux-gnu.build]
cxxflags = ["-DTRIPLE"]
[target.aarch64-unknown-linux-gnu.abi]
threads = true
[target.linux.build]
cxxflags = ["-DOS"]
[target.'cfg(unix)'.build]
cxxflags = ["-DFAMILY"]
[target.'cfg(unix)'.abi]
threads = false
)";
    auto parsed = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(parsed.has_value())); !failed) failed << parsed.error().format();
    std::vector<std::string> order;
    for (auto const& cc : parsed->conditionalConfigs) order.push_back(cc.predicate);
    // The two `cfg(unix)` rows (build, abi) are one table; ties keep text order.
    expect(fatal(ge(order.size(), 3u)));
    expect(eq(order.front(), "cfg(unix)"));
    expect(eq(order.back(), "aarch64-unknown-linux-gnu"));

    auto m = manifest_dialect::merged_for(src, "aarch64-unknown-linux-gnu");
    const auto& f = m.buildConfig.cxxflags;
    expect(fatal(eq(f.size(), 4u)));
    expect(eq(f[0], "-DBASE"));
    expect(eq(f[1], "-DFAMILY"));
    expect(eq(f[2], "-DOS"));
    expect(eq(f[3], "-DTRIPLE"));
    // The scalar: the triple states it last, so the triple's value stands.
    expect(bool(m.buildConfig.abiThreads));
}};

// A tie is broken by the selector text, so the order is the same on every
// reader and every run.
UTest equal_specificity_keeps_the_selector_text_order{"ConditionalOrder/EqualSpecificityKeepsTheSelectorTextOrder", [] {
    std::vector<mcpp::manifest::ConditionalConfig> tables(3);
    tables[0].predicate = "cfg(unix)";
    tables[1].predicate = "cfg(arch = \"x86_64\")";
    tables[2].predicate = "cfg(env = \"gnu\")";
    mcpp::manifest::cfg::order_by_specificity(tables);
    expect(eq(tables[0].predicate, "cfg(arch = \"x86_64\")"));
    expect(eq(tables[1].predicate, "cfg(env = \"gnu\")"));
    expect(eq(tables[2].predicate, "cfg(unix)"));
}};

// `[test] windows_code_page` (2026.10.5.2): the code page of every discovered
// test program, in the closed vocabulary of the target key.
UTest test_windows_code_page_is_a_closed_set_of_two{"Manifest/TestWindowsCodePageIsAClosedSetOfTwo", [] {
    auto ok = mcpp::manifest::parse_string(
        "[package]\nname = \"p\"\nversion = \"0.1.0\"\n[test]\nwindows_code_page = \"utf-8\"\n");
    if (auto&& failed = expect(fatal(ok.has_value())); !failed) failed << ok.error().format();
    expect(eq(ok->testWindowsCodePage, "utf-8"));
    expect(ok->schemaWarnings.empty());
    auto bad = mcpp::manifest::parse_string(
        "[package]\nname = \"p\"\nversion = \"0.1.0\"\n[test]\nwindows_code_page = \"936\"\n");
    expect(fatal(!bad.has_value()));
    expect(ne(bad.error().format().find("windows_code_page"), std::string::npos));
}};

// A Form B descriptor states its interface root with `lib`, below the
// archive's wrapping directory as its `sources` are (SPEC-008 v0.2, mcpp#797).
UTest lib_states_the_interface_root_below_the_wrapping_directory{"SynthesizeFromXpkgLua/LibStatesTheInterfaceRootBelowTheWrappingDirectory", [] {
    constexpr auto src = R"(
package = {
    spec = "1",
    name = "tomlpp",
    xpm  = { linux = { ["3.4.0"] = { url = "u", sha256 = "h" } } },
    mcpp = {
        lib          = "*/modules/tomlplusplus.cppm",
        sources      = { "*/modules/tomlplusplus.cppm" },
        include_dirs = { "*/include" },
        targets      = { ["tomlpp"] = { kind = "lib" } },
    },
}
)";
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(src, "tomlpp", "3.4.0",
                                                      mcpp::toolchain::platform::HostPlatform::current());
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(eq(m->lib.path.generic_string(), "*/modules/tomlplusplus.cppm"));
    expect(m->xpkgUnknownKeys.empty());

    const auto root = std::filesystem::temp_directory_path()
        / std::format("mcpp-lib-root-{}", std::random_device{}());
    std::filesystem::create_directories(root / "tomlplusplus-3.4.0" / "modules");
    std::ofstream(root / "tomlplusplus-3.4.0" / "modules" / "tomlplusplus.cppm") << "export module tomlpp;\n";
    expect(eq(mcpp::manifest::resolve_lib_root_path(*m, root).generic_string(), "tomlplusplus-3.4.0/modules/tomlplusplus.cppm"));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}};

// Design 2026-10-26 §24.3: an entry target, a program that begins in a
// dependency's entry, and the symbol an image is entered at.
UTest parses_entry_targets_and_a_program_that_begins_in_one{"Manifest/ParsesEntryTargetsAndAProgramThatBeginsInOne", [] {
    constexpr auto src = R"(
[package]
name    = "fw"
version = "0.1.0"
[targets.player]
kind              = "entry"
main              = "src/entry/player.cpp"
start             = "_reset"
windows_entry     = "WinMain"
defines           = ["PLAYER"]
required_features = ["player"]
[targets.notes]
kind  = "app"
main  = { from = "openmui/player" }
start = "_start"
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(eq(m->targets.size(), 2u)));
    auto const& entry = m->targets[0].name == "player" ? m->targets[0] : m->targets[1];
    auto const& notes = m->targets[0].name == "notes" ? m->targets[0] : m->targets[1];
    expect(eq(entry.kind, mcpp::manifest::Target::Entry));
    expect(!entry.is_program());
    expect(eq(entry.main, "src/entry/player.cpp"));
    expect(eq(entry.start, "_reset"));
    expect(eq(entry.windowsEntry, "WinMain"));
    expect(eq(notes.main, ""));
    expect(eq(notes.mainFrom, "openmui/player"));
    expect(eq(notes.start, "_start"));
    expect(eq(notes.startDeclaredBy, "[targets.notes] start = \"_start\""));
    expect(m->schemaWarnings.empty());
}};

UTest refuses_what_an_entry_or_a_beginning_cannot_state{"Manifest/RefusesWhatAnEntryOrABeginningCannotState", [] {
    auto refused = [](std::string_view body, std::string_view needle) {
        auto m = mcpp::manifest::parse_string(
            std::format("[package]\nname = \"p\"\nversion = \"0.1.0\"\n{}", body));
        expect(fatal(!m.has_value())) << body;
        if (auto&& failed = expect(ne(m.error().format().find(needle), std::string::npos)); !failed) failed << body << "\n" << m.error().format();
    };
    refused("[targets.e]\nkind = \"entry\"\nmain = \"e.cpp\"\nsoname = \"libe.so\"\n",
            "produces no artifact of its own");
    refused("[targets.e]\nkind = \"entry\"\nmain = { from = \"fw/x\" }\n",
            "requires 'main', the file that is the translation unit");
    refused("[targets.a]\nkind = \"bin\"\nmain = { from = \"fw/x\", path = \"y\" }\n",
            "and nothing else");
    refused("[targets.a]\nkind = \"bin\"\nmain = { from = \"fwx\" }\n",
            "\"<dependency>/<entry>\"");
    refused("[targets.a]\nkind = \"bin\"\nmain = \"a.cpp\"\nstart = \"1x\"\n",
            "is not a symbol name");
    refused("[targets.l]\nkind = \"lib\"\nstart = \"_start\"\n",
            "a library target is not entered");
    refused("[targets.a]\nkind = \"bin\"\nmain = { from = \"fw/x\" }\ndefines = [\"X\"]\n",
            "applies to the target's own entry translation unit");
    refused("[test]\nmain = \"t.cpp\"\n", "[test] main is `{ from");
    refused("[test]\nstart = \"9\"\n", "[test] start");
}};

UTest parses_test_main_from_and_start{"Manifest/ParsesTestMainFromAndStart", [] {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name    = "p"
version = "0.1.0"
[test]
main  = { from = "compat.gtest/main" }
start = "_reset"
)");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(eq(m->testMainFrom, "compat.gtest/main"));
    expect(eq(m->testStart, "_reset"));
}};

// A row states a program's or an entry's beginning, and only of such a target;
// every file a target names as `main` on any row is recorded on it.
UTest a_row_states_a_beginning_of_a_program_or_an_entry{"Manifest/ARowStatesABeginningOfAProgramOrAnEntry", [] {
    auto tmp = std::filesystem::temp_directory_path() / "mcpp_row_beginning";
    std::filesystem::create_directories(tmp);
    auto path = tmp / "mcpp.toml";
    auto write = [&](std::string_view body) {
        std::ofstream os(path);
        os << "[package]\nname = \"p\"\nversion = \"0.1.0\"\n" << body;
    };
    write(R"(
[targets.p]
kind = "lib"
[targets.player]
kind = "entry"
main = "src/entry/player.cpp"
[target.'cfg(os = "android")'.targets.player]
main  = "src/entry/player_android.cpp"
start = "_reset"
)");
    auto m = mcpp::manifest::load(path);
    if (auto&& failed = expect(fatal(bool(m))); !failed) failed << (m ? "" : m.error().message);
    auto it = std::ranges::find_if(m->targets, [](auto const& t) { return t.name == "player"; });
    expect(fatal(ne(it, m->targets.end())));
    expect(eq(it->otherMains, (std::vector<std::string>{"src/entry/player_android.cpp"})));
    expect(fatal(eq(m->conditionalConfigs.size(), 1u)));
    auto const& row = m->conditionalConfigs[0].targetKinds.at("player");
    expect(!row.statesForm);
    expect(eq(row.main, std::optional<std::string>("src/entry/player_android.cpp")));
    expect(eq(row.start, std::optional<std::string>("_reset")));

    write(R"(
[targets.p]
kind = "lib"
[target.'cfg(os = "android")'.targets.p]
main = "x.cpp"
)");
    auto lib = mcpp::manifest::load(path);
    expect(fatal(!lib));
    if (auto&& failed = expect(ne(lib.error().message.find("is a library target"), std::string::npos)); !failed) failed << lib.error().message;

    write(R"(
[targets.p]
kind = "lib"
[target.'cfg(os = "android")'.targets.p]
kind = "shared"
main = "x.cpp"
)");
    auto both = mcpp::manifest::load(path);
    expect(fatal(!both));
    if (auto&& failed = expect(ne(both.error().message.find("a target is one or the other"), std::string::npos)); !failed) failed << both.error().message;
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
}};

// A package that declares only entries still has the library its sources
// infer: an entry is no artifact of its own package.
UTest only_entry_targets_still_infer_the_library{"Manifest/OnlyEntryTargetsStillInferTheLibrary", [] {
    auto tmp = std::filesystem::temp_directory_path() / "mcpp_entry_infer";
    std::filesystem::create_directories(tmp / "src" / "entry");
    { std::ofstream(tmp / "src" / "fw.cppm") << "export module fw;\n"; }
    {
        std::ofstream os(tmp / "mcpp.toml");
        os << "[package]\nname = \"fw\"\nversion = \"0.1.0\"\n"
              "[targets.player]\nkind = \"entry\"\nmain = \"src/entry/player.cpp\"\n";
    }
    auto m = mcpp::manifest::load(tmp / "mcpp.toml");
    if (auto&& failed = expect(fatal(bool(m))); !failed) failed << (m ? "" : m.error().message);
    expect(std::ranges::any_of(m->targets, [](auto const& t) {
        return t.name == "fw" && t.kind == mcpp::manifest::Target::Library;
    }));
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
}};

// ── [pack] targets and [build] target (SPEC-015 §3.3) ────────────────────────

UTest pack_targets_take_targets_and_sets{"Manifest/PackTargetsTakeTargetsAndSets", [] {
    auto list = mcpp::manifest::parse_string(R"(
[package]
name = "p"
version = "0.1.0"
[pack]
targets = ["openkal", "x86_64-linux-musl"]
)");
    if (auto&& failed = expect(fatal(list.has_value())); !failed) failed << list.error().format();
    expect(eq(list->packConfig.targets, (std::vector<std::string>{"openkal", "x86_64-linux-musl"})));
    auto one = mcpp::manifest::parse_string(R"(
[package]
name = "p"
version = "0.1.0"
[pack]
targets = "linux-musl"
)");
    if (auto&& failed = expect(fatal(one.has_value())); !failed) failed << one.error().format();
    expect(eq(one->packConfig.targets, (std::vector<std::string>{"linux-musl"})));
}};

// `[pack] format` and `[pack] systems` are a string or a list, and
// `default_mode` takes the canonical names (design 2026-10-29 §4.1, §4.6).
UTest pack_format_and_systems_are_a_string_or_a_list{"Manifest/PackFormatAndSystemsAreAStringOrAList", [] {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "p"
version = "0.1.0"
[pack]
default_mode = "self-contained"
format = ["tar", "okb"]
systems = "linux-aarch64"
)");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(eq(m->packConfig.defaultMode, "self-contained"));
    expect(eq(m->packConfig.format, (std::vector<std::string>{"tar", "okb"})));
    expect(eq(m->packConfig.systems, (std::vector<std::string>{"linux-aarch64"})));
    auto one = mcpp::manifest::parse_string(R"(
[package]
name = "p"
version = "0.1.0"
[pack]
format = "zip"
)");
    if (auto&& failed = expect(fatal(one.has_value())); !failed) failed << one.error().format();
    expect(eq(one->packConfig.format, (std::vector<std::string>{"zip"})));
}};

UTest pack_targets_refuse_what_is_neither_a_target_nor_a_set{"Manifest/PackTargetsRefuseWhatIsNeitherATargetNorASet", [] {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "p"
version = "0.1.0"
[pack]
targets = ["not a target"]
)");
    expect(fatal(!m.has_value()));
    expect(ne(m.error().format().find("neither a target"), std::string::npos));
}};

UTest build_target_is_one_target{"Manifest/BuildTargetIsOneTarget", [] {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name = "p"
version = "0.1.0"
[build]
target = "openkal"
)");
    expect(fatal(!m.has_value()));
    expect(ne(m.error().format().find("names a target set"), std::string::npos));
    auto ok = mcpp::manifest::parse_string(R"(
[package]
name = "p"
version = "0.1.0"
[build]
target = "x86_64-openkal"
)");
    if (auto&& failed = expect(fatal(ok.has_value())); !failed) failed << ok.error().format();
}};

// A GIT DEPENDENCY NAMES AT MOST ONE OF rev, tag, branch (SPEC-001 §5); none is
// the remote's default branch.
UTest a_git_dependency_names_at_most_one_ref{"Manifest/AGitDependencyNamesAtMostOneRef", [] {
    auto m = mcpp::manifest::parse_string(R"(
[package]
name    = "x"
version = "0.1.0"

[dependencies]
bar = { git = "https://example.com/bar.git" }
)");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    auto const& bar = m->dependencies.at("bar");
    expect(eq(bar.gitRefKind, "branch"));
    expect(bar.gitRev.empty());
    auto two = mcpp::manifest::parse_string(R"(
[package]
name    = "x"
version = "0.1.0"

[dependencies]
bar = { git = "https://example.com/bar.git", tag = "v1", rev = "abc" }
)");
    expect(fatal(!two.has_value()));
    expect(ne(two.error().format().find("a git dependency names at most one"), std::string::npos));
}};

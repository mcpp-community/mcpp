// Sources (mcpp#755): the manifest keys that state where a payload or a
// toolchain comes from, the protocol-15 directives a build program states
// sources with, and the override checks of the payload unification.


import std;
import mcpplibs.mtest;
import mcpp.manifest;
import mcpp.config;
import mcpp.build.build_program;
import mcpp.libs.toml;
import mcpp.build.directives;
import mcpp.toolchain.dialect;
import mcpp.toolchain.registry;
import mcpp.provider.address_set;

using namespace mcpplibs::mtest;

namespace dirs = mcpp::build::directives;
namespace addrset = mcpp::provider::addrset;

namespace {

dirs::Directives parse_directives(std::string_view text) {
    dirs::Directives d;
    const auto root = (std::filesystem::current_path() / "pkg").lexically_normal();
    dirs::accept_output(d, mcpp::toolchain::gnu_dialect(), root, text);
    return d;
}

std::expected<mcpp::manifest::Manifest, mcpp::manifest::ManifestError>
manifest(std::string_view body) {
    return mcpp::manifest::parse_string(std::format(R"(
[package]
name = "app"
version = "0.1.0"
{})", body));
}

} // namespace

// ── [xlings.overrides] ──────────────────────────────────────────────────────

UTest override_path_and_table_forms_parse{"Sources/OverridePathAndTableFormsParse", [] {
    auto m = manifest(R"(
[xlings.overrides]
"xim:cmake" = "/usr/bin/cmake"
vcpkg       = { root = "/opt/vcpkg" }
"xim:slang" = { program = "slangc", version = "2026.14.1" }
)");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    using K = mcpp::manifest::XlingsOverride::Kind;
    const auto& o = m->xlings.overrides;
    expect(fatal(eq(o.size(), 3u)));
    expect(eq(o.at("xim:cmake").kind, K::Path));
    expect(eq(o.at("xim:cmake").value, "/usr/bin/cmake"));
    // A key without a namespace names the `xim` package, as everywhere else.
    expect(eq(o.at("xim:vcpkg").kind, K::Root));
    expect(eq(o.at("xim:slang").kind, K::Program));
    expect(eq(o.at("xim:slang").version, "2026.14.1"));
    expect(gt(o.at("xim:cmake").line, 0));
}};

UTest override_key_with_a_version_is_refused{"Sources/OverrideKeyWithAVersionIsRefused", [] {
    auto m = manifest(R"(
[xlings.overrides]
"xim:cmake@3.31" = "/usr/bin/cmake"
)");
    expect(fatal(!m.has_value()));
    if (auto&& failed = expect(ne(m.error().message.find("names a version"), std::string::npos)); !failed) failed << m.error().message;
}};

UTest override_naming_both_program_and_root_is_refused{"Sources/OverrideNamingBothProgramAndRootIsRefused", [] {
    auto m = manifest(R"(
[xlings.overrides]
cmake = { program = "/usr/bin/cmake", root = "/usr" }
)");
    expect(fatal(!m.has_value()));
    if (auto&& failed = expect(ne(m.error().message.find("exactly one of"), std::string::npos)); !failed) failed << m.error().message;
}};

UTest override_unknown_key_is_refused{"Sources/OverrideUnknownKeyIsRefused", [] {
    auto m = manifest(R"(
[xlings.overrides]
cmake = { programme = "/usr/bin/cmake" }
)");
    expect(fatal(!m.has_value()));
    if (auto&& failed = expect(ne(m.error().message.find("programme"), std::string::npos)); !failed) failed << m.error().message;
}};

UTest override_under_a_target_selector_parses{"Sources/OverrideUnderATargetSelectorParses", [] {
    auto m = manifest(R"(
[target.'cfg(os = "linux")'.xlings.overrides]
"xim:cmake" = "/usr/bin/cmake"
)");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(fatal(!(m->conditionalConfigs.empty())));
    bool found = false;
    for (auto const& cc : m->conditionalConfigs)
        if (cc.xlings.overrides.contains("xim:cmake")) found = true;
    expect(bool(found));
}};

// ── provision = "on-request" ────────────────────────────────────────────────

UTest provision_on_request_is_recorded_on_both_tables{"Sources/ProvisionOnRequestIsRecordedOnBothTables", [] {
    auto m = manifest(R"(
[features]
tools = {}
[xlings.workspace]
"xim:cmake" = { version = ">=3.31", provision = "on-request" }
"xim:ninja" = { version = "1.12", provision = "eager" }
[feature-xlings.tools]
"xim:slang" = { version = "", provision = "on-request", when = "build" }
)");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(bool(m->xlings.on_request("xim:cmake@>=3.31")));
    expect(!(m->xlings.on_request("xim:ninja@1.12")));
    expect(bool(m->xlings.on_request("xim:slang")));
    expect(eq(m->xlings.when_of("xim:slang"), mcpp::manifest::ToolWhen::Build));
}};

UTest provision_unknown_mode_is_refused{"Sources/ProvisionUnknownModeIsRefused", [] {
    auto m = manifest(R"(
[xlings.workspace]
"xim:cmake" = { version = "", provision = "lazy" }
)");
    expect(fatal(!m.has_value()));
    if (auto&& failed = expect(ne(m.error().message.find("on-request"), std::string::npos)); !failed) failed << m.error().message;
}};

// ── [toolchain] tables ──────────────────────────────────────────────────────

UTest toolchain_table_names_a_path{"Sources/ToolchainTableNamesAPath", [] {
    auto m = manifest(R"(
[toolchain]
default   = { path = "/opt/llvm", launcher = "ccache", tools = { ld = "/opt/lld/bin/ld.lld" } }
bootstrap = "llvm@22.1.8"
)");
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << m.error().format();
    expect(eq(m->toolchain.for_platform("linux").value_or(""), "path:/opt/llvm"));
    auto* lt = m->toolchain.local_for("linux");
    expect(fatal(ne(lt, nullptr)));
    expect(eq(lt->launcher, "ccache"));
    expect(fatal(eq(lt->tools.size(), 1u)));
    expect(eq(lt->tools[0].first, "ld"));
    expect(eq(m->toolchain.bootstrap, "llvm@22.1.8"));
    // `bootstrap` is not a platform.
    expect(!(m->toolchain.byPlatform.contains("bootstrap")));
}};

UTest toolchain_configure_is_alone_in_its_table{"Sources/ToolchainConfigureIsAloneInItsTable", [] {
    auto ok = manifest(R"(
[toolchain]
default = { configure = "build.mcpp" }
)");
    if (auto&& failed = expect(fatal(ok.has_value())); !failed) failed << ok.error().format();
    expect(eq(ok->toolchain.for_platform("macos").value_or(""), "configure:build.mcpp"));
    auto both = manifest(R"(
[toolchain]
default = { configure = "build.mcpp", path = "/opt/llvm" }
)");
    expect(fatal(!both.has_value()));
    auto other = manifest(R"(
[toolchain]
default = { configure = "cmake" }
)");
    expect(fatal(!other.has_value()));
    if (auto&& failed = expect(ne(other.error().message.find("build.mcpp"), std::string::npos)); !failed) failed << other.error().message;
}};

UTest toolchain_table_without_path_is_refused{"Sources/ToolchainTableWithoutPathIsRefused", [] {
    auto m = manifest(R"(
[toolchain]
linux = { prefix = "aarch64-none-linux-gnu-" }
)");
    expect(fatal(!m.has_value()));
    if (auto&& failed = expect(ne(m.error().message.find("path"), std::string::npos)); !failed) failed << m.error().message;
}};

UTest toolchain_unknown_tool_role_is_refused{"Sources/ToolchainUnknownToolRoleIsRefused", [] {
    auto m = manifest(R"(
[toolchain]
default = { path = "/opt/llvm", tools = { linker = "/x" } }
)");
    expect(fatal(!m.has_value()));
    if (auto&& failed = expect(ne(m.error().message.find("linker"), std::string::npos)); !failed) failed << m.error().message;
}};

// ── Protocol-15 directives ──────────────────────────────────────────────────

UTest source_directives_fold_into_the_build_config{"Sources/SourceDirectivesFoldIntoTheBuildConfig", [] {
    auto d = parse_directives(
        "mcpp:protocol=15\n"
        "mcpp:decision=tool:mcpp.deps.cmake:cmake\tchoice\t/usr/bin/cmake\t/p/build.mcpp\t9\txim:cmake\n"
        "mcpp:xpkg-request=xim:cmake\n"
        "mcpp:toolchain=path=/opt/llvm\n"
        "mcpp:toolchain=origin=/p/build.mcpp:4\n");
    expect(!(dirs::protocol_error(d).has_value()));
    mcpp::manifest::Manifest m;
    dirs::apply(m, d);
    expect(fatal(eq(m.buildConfig.toolDecisions.size(), 1u)));
    expect(m.buildConfig.toolDecisions[0].starts_with("tool:mcpp.deps.cmake:cmake\tchoice"));
    expect(fatal(eq(m.buildConfig.xpkgRequests.size(), 1u)));
    expect(eq(m.buildConfig.xpkgRequests[0], "xim:cmake"));
    expect(fatal(eq(m.buildConfig.toolchainStatement.size(), 2u)));
    expect(eq(m.buildConfig.toolchainStatement[0], "path=/opt/llvm"));
}};

UTest source_directives_are_protocol_fifteen{"Sources/SourceDirectivesAreProtocolFifteen", [] {
    for (auto wire : {"decision", "xpkg-request", "toolchain"}) {
        auto def = dirs::find_by_wire(wire);
        expect(fatal(ne(def, nullptr))) << wire;
        expect(eq(def->sinceProtocol, 15)) << wire;
        expect(!(def->tag.empty())) << wire;
    }
}};

// ── Override checks in the unification ─────────────────────────────────────

UTest override_version_is_checked_against_requirements{"Sources/OverrideVersionIsCheckedAgainstRequirements", [] {
    std::vector<addrset::Claim> claims{
        {"xim:cmake@>=3.31", "mcpp:plugins", 1},
        {"xim:cmake", "this project", 0},
    };
    expect(!addrset::override_violation(claims, "xim:cmake", "3.31.6", "[xlings.overrides]"));
    auto bad = addrset::override_violation(claims, "xim:cmake", "3.28.3", "[xlings.overrides]");
    expect(fatal(bad.has_value()));
    if (auto&& failed = expect(ne(bad->find(">=3.31"), std::string::npos)); !failed) failed << *bad;
    if (auto&& failed = expect(ne(bad->find("mcpp:plugins"), std::string::npos)); !failed) failed << *bad;
    // No stated version: nothing to compare, and the requirement is listed.
    expect(!addrset::override_violation(claims, "xim:cmake", "", "[xlings.overrides]"));
    auto reqs = addrset::requirements_for(claims, "xim:cmake");
    expect(fatal(eq(reqs.size(), 1u)));
    expect(eq(reqs[0], ">=3.31 by mcpp:plugins"));
}};

// ── The response file a long compile command goes through ───────────────────

UTest response_file_quotes_for_the_windows_tokenizer{"Sources/ResponseFileQuotesForTheWindowsTokenizer", [] {
    const std::vector<std::string> args{
        "-std=c++23",
        "/Tp C:/Program Files/x/build.mcpp",
        R"(-DNAME="v")",
        "C:\\with space\\dir\\",
    };
    const auto body = mcpp::build::response_file_body(args, /*gnuQuoting=*/false);
    expect(eq(std::ranges::count(body, '\n'), 4));
    expect(ne(body.find("-std=c++23\n"), std::string::npos)) << body;
    expect(ne(body.find("\"/Tp C:/Program Files/x/build.mcpp\"\n"), std::string::npos)) << body;
    expect(ne(body.find("\"-DNAME=\\\"v\\\"\"\n"), std::string::npos)) << body;
    // The run of backslashes that ends the argument is doubled, so it does not
    // escape the closing quote.
    expect(ne(body.find("\"C:\\with space\\dir\\\\\"\n"), std::string::npos)) << body;
}};

UTest response_file_keeps_backslashes_literal_for_the_gnu_tokenizer{"Sources/ResponseFileKeepsBackslashesLiteralForTheGnuTokenizer", [] {
    // clang and GCC read a backslash as an escape, inside quotes as well as
    // outside, so a Windows path survives only when every backslash is doubled.
    // Measured with clang 22.1.8: `'-DX=a\b'` in a response file yields `X=ab`,
    // and `-DX=a\\b` yields `X=a\b`.
    const std::vector<std::string> args{
        "-fmodule-file=mcpp=D:\\a\\p\\mcpp.pcm",
        "D:\\a\\obj\\x.o",
        "/Tp D:\\a\\b c\\build.mcpp",
        R"(-DNAME="v")",
    };
    const auto body = mcpp::build::response_file_body(args, /*gnuQuoting=*/true);
    expect(eq(std::ranges::count(body, '\n'), 4));
    expect(ne(body.find("-fmodule-file=mcpp=D:\\\\a\\\\p\\\\mcpp.pcm\n"), std::string::npos)) << body;
    expect(ne(body.find("D:\\\\a\\\\obj\\\\x.o\n"), std::string::npos)) << body;
    // Whitespace is handled by quoting the whole argument; the doubling holds
    // inside the quotes too.
    expect(ne(body.find("\"/Tp D:\\\\a\\\\b c\\\\build.mcpp\"\n"), std::string::npos)) << body;
    // A quote of the argument's own is escaped.
    expect(ne(body.find("-DNAME=\\\"v\\\"\n"), std::string::npos)) << body;
}};

// ── [xlings.overrides] in config.toml ───────────────────────────────────────

UTest config_overrides_parse_with_the_manifests_two_shapes{"Sources/ConfigOverridesParseWithTheManifestsTwoShapes", [] {
    auto doc = mcpp::libs::toml::parse(R"(
[xlings.overrides]
"xim:cmake" = "/usr/bin/cmake"
vcpkg       = { root = "/opt/vcpkg" }
"xim:slang" = { program = "slangc", version = "2026.14.1" }
)");
    expect(fatal(doc.has_value()));
    auto o = mcpp::config::parse_payload_overrides(*doc);
    if (auto&& failed = expect(fatal(o.has_value())); !failed) failed << o.error();
    expect(fatal(eq(o->size(), 3u)));
    expect(eq(o->at("xim:cmake").kind, "path"));
    expect(eq(o->at("xim:cmake").value, "/usr/bin/cmake"));
    // A key without a namespace names the `xim` package, as everywhere else.
    expect(eq(o->at("xim:vcpkg").kind, "root"));
    expect(eq(o->at("xim:slang").kind, "program"));
    expect(eq(o->at("xim:slang").version, "2026.14.1"));
    expect(gt(o->at("xim:cmake").line, 0));
}};

UTest config_override_with_an_unknown_key_is_refused{"Sources/ConfigOverrideWithAnUnknownKeyIsRefused", [] {
    auto doc = mcpp::libs::toml::parse(R"(
[xlings.overrides]
cmake = { programme = "/usr/bin/cmake" }
)");
    expect(fatal(doc.has_value()));
    auto o = mcpp::config::parse_payload_overrides(*doc);
    expect(fatal(!o.has_value()));
    if (auto&& failed = expect(ne(o.error().find("programme"), std::string::npos)); !failed) failed << o.error();
}};

UTest config_without_the_table_has_no_overrides{"Sources/ConfigWithoutTheTableHasNoOverrides", [] {
    auto doc = mcpp::libs::toml::parse("[toolchain]\ndefault = \"gcc@16.1.0\"\n");
    expect(fatal(doc.has_value()));
    auto o = mcpp::config::parse_payload_overrides(*doc);
    if (auto&& failed = expect(fatal(o.has_value())); !failed) failed << o.error();
    expect(o->empty());
}};

// ── A toolchain named by path ───────────────────────────────────────────────

UTest path_spec_takes_the_family_from_the_drivers{"Sources/PathSpecTakesTheFamilyFromTheDrivers", [] {
    const auto base = std::filesystem::temp_directory_path() / "mcpp-test-sources-tc";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base / "llvm" / "bin");
    std::filesystem::create_directories(base / "gcc" / "bin");
    std::ofstream(base / "llvm" / "bin" / "clang++") << "";
    std::ofstream(base / "gcc" / "bin" / "aarch64-none-linux-gnu-g++") << "";
    auto llvm = mcpp::toolchain::parse_toolchain_spec("path:" + (base / "llvm").string());
    if (auto&& failed = expect(fatal(llvm.has_value())); !failed) failed << llvm.error();
    expect(eq(llvm->family, mcpp::toolchain::Family::Llvm));
    expect(eq(llvm->spec_str(), "path:" + (base / "llvm").lexically_normal().generic_string()));
    auto gcc = mcpp::toolchain::parse_toolchain_spec("path:" + (base / "gcc").string());
    if (auto&& failed = expect(fatal(gcc.has_value())); !failed) failed << gcc.error();
    expect(eq(gcc->family, mcpp::toolchain::Family::Gcc));
    auto none = mcpp::toolchain::parse_toolchain_spec("path:" + (base / "missing").string());
    expect(!none.has_value());
    std::filesystem::remove_all(base);
}};

// ── The engine floor read from a manifest that does not parse ───────────────

UTest stated_mcpp_floor_is_read_from_package_only{"Sources/StatedMcppFloorIsReadFromPackageOnly", [] {
    using mcpp::manifest::stated_mcpp_floor;
    expect(eq(stated_mcpp_floor("[package]\nname = \"p\"\nmcpp = \">=2026.10.1.3\"\n"), "2026.10.1.3"));
    // A bare version is the same statement.
    expect(eq(stated_mcpp_floor("[package]\nmcpp = \"2026.9.28.3\"\n"), "2026.9.28.3"));
    // Only `[package]`: a dependency named mcpp states a dependency, not a floor.
    expect(eq(stated_mcpp_floor("[package]\nname = \"p\"\n\n[dependencies]\nmcpp = \"1.0\"\n"), ""));
    // `[package.metadata]` is another table.
    expect(eq(stated_mcpp_floor("[package.metadata]\nmcpp = \"9.9.9.9\"\n"), ""));
    // A key that merely starts with the name is not the key.
    expect(eq(stated_mcpp_floor("[package]\nmcpp_home = \"/x\"\n"), ""));
    expect(eq(stated_mcpp_floor("[package]\n# mcpp = \"9.9.9.9\"\n"), ""));
    expect(eq(stated_mcpp_floor(""), ""));
}};

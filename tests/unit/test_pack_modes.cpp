
import std;
import mcpplibs.mtest;
import mcpp.pack;

using namespace mcpplibs::mtest;

using mcpp::pack::Mode;
using mcpp::pack::parse_mode;
using mcpp::pack::mode_cli_name;
using mcpp::pack::Format;
using mcpp::pack::ClosureUnavailableOutcome;
using mcpp::pack::closure_unavailable_outcome;

UTest canonical_names_parse{"PackModes/CanonicalNamesParse", [] {
    expect(eq(parse_mode("system"), Mode::None));
    expect(eq(parse_mode("vendored"), Mode::BundleProject));
    expect(eq(parse_mode("self-contained"), Mode::BundleAll));
    expect(eq(parse_mode("static"), Mode::Static));
}};

UTest old_names_stay_as_aliases{"PackModes/OldNamesStayAsAliases", [] {
    expect(eq(parse_mode("bundle-project"), Mode::BundleProject));
    expect(eq(parse_mode("bundle-all"), Mode::BundleAll));
}};

UTest unknown_is_nullopt{"PackModes/UnknownIsNullopt", [] {
    expect(!(parse_mode("nonsense").has_value()));
}};

UTest cli_names_are_canonical{"PackModes/CliNamesAreCanonical", [] {
    expect(eq(mode_cli_name(Mode::None), "system"));
    expect(eq(mode_cli_name(Mode::BundleProject), "vendored"));
    expect(eq(mode_cli_name(Mode::BundleAll), "self-contained"));
    expect(eq(mode_cli_name(Mode::Static), "static"));
}};

// ── #630 §3: what happens when a format's dependency closure is unavailable,
// as a function of the format alone ──────────────────────────────────────
//
// The archive formats (`tar`, `dir`) ARE the closure -- an unavailable one is
// the command failing, exactly as before `pack::run` staged declared files
// ahead of the closure walk. A dispatched format receives the staged tree
// regardless: a provider may not need a closure at all.

UTest archive_formats_fail_the_command{"PackClosureUnavailableOutcome/ArchiveFormatsFailTheCommand", [] {
    expect(eq(closure_unavailable_outcome(Format::Tar), ClosureUnavailableOutcome::CommandFails));
    expect(eq(closure_unavailable_outcome(Format::Dir), ClosureUnavailableOutcome::CommandFails));
}};

UTest dispatched_format_stages_without_it{"PackClosureUnavailableOutcome/DispatchedFormatStagesWithoutIt", [] {
    expect(eq(closure_unavailable_outcome(Format::Dispatched), ClosureUnavailableOutcome::StageWithoutClosure));
}};

// `--systems` (design 2026-10-29 §4.1): `<os>` for every architecture of the
// legs, `<os>-<arch>` for one, `all` for the three systems; unstated, this
// machine's.
UTest the_words_name_systems_for_an_architecture{"PackSystems/TheWordsNameSystemsForAnArchitecture", [] {
    using V = std::vector<std::string>;
    expect(eq(mcpp::pack::systems_for({ "linux", "macos-aarch64" }, "x86_64"), V{ "linux" }));
    expect(eq(mcpp::pack::systems_for({ "linux", "macos-aarch64" }, "aarch64"), (V{ "linux", "macos" })));
    expect(eq(mcpp::pack::systems_for({ "all", "linux" }, "x86_64"), (V{ "linux", "macos", "windows" })));
    expect(eq(mcpp::pack::systems_for({}, "x86_64").size(), 1u));
}};

// `-o` names a directory when it ends in a separator or is one (design
// 2026-10-29 §4.2).
UTest an_output_ending_in_a_separator_is_a_directory{"PackSystems/AnOutputEndingInASeparatorIsADirectory", [] {
    expect(bool(mcpp::pack::output_names_directory("dist/")));
    expect(!mcpp::pack::output_names_directory("dist/app.tar.gz"));
    expect(!mcpp::pack::output_names_directory(""));
    expect(bool(mcpp::pack::output_names_directory(std::filesystem::temp_directory_path())));
}};

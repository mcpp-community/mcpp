// scan_short_name_matches — the did-you-mean index walk (#278), now carrying
// each match's published versions (#487) so a failed `mcpp add` can show what
// the suggested packages actually offer. Still DIAGNOSTIC ONLY: nothing here
// feeds resolution, the lockfile, or the install layer.


import std;
import mcpplibs.mtest;
import mcpp.pm.package_fetcher;

using namespace mcpplibs::mtest;

namespace {

struct TempDir {
    std::filesystem::path path = std::filesystem::temp_directory_path()
        / std::format("mcpp-short-name-scan-{}",
                      std::chrono::steady_clock::now()
                          .time_since_epoch().count());

    TempDir() { std::filesystem::create_directories(path); }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

void write_descriptor(const std::filesystem::path& root,
                      const std::string& letterAndFile,
                      std::string_view lua) {
    auto file = root / "pkgs" / letterAndFile.substr(0, 1)
        / (letterAndFile.substr(2) + ".lua");
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file);
    out << lua;
}

std::string descriptor_with_versions(std::string_view ns,
                                     std::string_view name,
                                     std::string_view xpmTable) {
    return std::format(
        "package = {{\n"
        "    spec = \"1\", namespace = \"{}\", name = \"{}\",\n"
        "    {}\n"
        "}}\n",
        ns, name, xpmTable);
}

UTest carries_merged_descending_versions_per_match{"ShortNameScan/CarriesMergedDescendingVersionsPerMatch", [] {
    TempDir temp;
    write_descriptor(temp.path, "a.acme.widget.lua", descriptor_with_versions(
        "acme", "acme.widget",
        "xpm = { linux = { [\"1.0.0\"] = { url = \"u\", sha256 = \"h\" },"
        "               [\"0.9.0\"] = { url = \"u\", sha256 = \"h\" } },"
        "        macosx = { [\"1.0.0\"] = { url = \"u\", sha256 = \"h\" } } }"));
    write_descriptor(temp.path, "c.compat.widget.lua", descriptor_with_versions(
        "compat", "gadget",
        "xpm = { linux = { [\"2.0.0\"] = { url = \"u\", sha256 = \"h\" } } }"));

    auto matches = mcpp::pm::Fetcher::scan_short_name_matches(
        {temp.path}, "widget");

    // Sorted by fqn; each record keeps its fqn AND its version union.
    expect(fatal(eq(matches.size(), 1u)));
    expect(eq(matches[0].fqn, "acme.widget"));
    expect(fatal(eq(matches[0].versions.size(), 2u)));
    expect(eq(matches[0].versions[0], "1.0.0"));
    expect(eq(matches[0].versions[1], "0.9.0"));

    // The short name is matched on the SHORT component only: compat/gadget
    // must not appear for "widget".
}};

UTest descriptor_without_xpm_yields_empty_versions{"ShortNameScan/DescriptorWithoutXpmYieldsEmptyVersions", [] {
    TempDir temp;
    write_descriptor(temp.path, "a.acme.bare.lua", descriptor_with_versions(
        "acme", "acme.bare", "mcpp = { schema = \"0.1\" }"));

    auto matches = mcpp::pm::Fetcher::scan_short_name_matches(
        {temp.path}, "bare");

    expect(fatal(eq(matches.size(), 1u)));
    expect(eq(matches[0].fqn, "acme.bare"));
    expect(matches[0].versions.empty());
}};

UTest empty_result_on_no_match_is_not_an_error{"ShortNameScan/EmptyResultOnNoMatchIsNotAnError", [] {
    TempDir temp;
    write_descriptor(temp.path, "a.acme.widget.lua", descriptor_with_versions(
        "acme", "acme.widget",
        "xpm = { linux = { [\"1.0.0\"] = {} } }"));

    auto matches = mcpp::pm::Fetcher::scan_short_name_matches(
        {temp.path}, "nosuchthing");

    expect(matches.empty());
}};

} // namespace

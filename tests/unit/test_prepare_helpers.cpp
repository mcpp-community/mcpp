// Unit tests for the pure helpers the phases of prepare_build share
// (src/build/prepare/config.cpp and fetch.cpp): the --features request
// grammar, the macro name a define entry names, the previous release's
// reading of a flag element, and whether a git remote is local. Before the
// decomposition of prepare.cppm these were internal to one 16,000-line unit
// and reached only through whole builds; they are exported for this file.


import std;
import mcpplibs.mtest;
import mcpp.build.prepare;

using namespace mcpplibs::mtest;

using Words = std::vector<std::string>;

UTest tokens_split_on_commas_and_spaces{"FeatureRequest/TokensSplitOnCommasAndSpaces", [] {
    expect(eq(mcpp::build::feature_request_tokens("a,b c,,  d"), (Words{"a", "b", "c", "d"})));
    expect(mcpp::build::feature_request_tokens("").empty());
    expect(mcpp::build::feature_request_tokens(" , ").empty());
}};

// #649 E8: a token with `/` opens a dependency's feature, so it is a forward
// of the root and never one of the root's own features. A token with an
// empty half stays whole, for the caller to name.
UTest a_slash_token_is_a_forward_and_not_a_root_feature{"FeatureRequest/ASlashTokenIsAForwardAndNotARootFeature", [] {
    constexpr std::string_view request = "simd, dep/fast,  gpu,/x,y/";
    expect(eq(mcpp::build::parse_feature_request(request), (Words{"simd", "gpu"})));
    expect(eq(mcpp::build::feature_forward_request_tokens(request), (Words{"dep/fast", "/x", "y/"})));
}};

UTest is_the_text_before_the_first_equals{"DefineName/IsTheTextBeforeTheFirstEquals", [] {
    expect(eq(mcpp::build::define_name("NDEBUG"), "NDEBUG"));
    expect(eq(mcpp::build::define_name("VERSION=1"), "VERSION"));
    expect(eq(mcpp::build::define_name("EXPR=a=b"), "EXPR"));
    expect(eq(mcpp::build::define_name("=x"), ""));
}};

// The words an element reached the compiler as before flag_words: `$$` was a
// literal dollar, and `${name}` or `$name` a ninja variable, empty on a
// compile edge. A `defines` entry was read as `-D<entry>`.
UTest read_ninja_escapes_and_variables_as_the_old_edge_did{"PreviousReleaseWords/ReadNinjaEscapesAndVariablesAsTheOldEdgeDid", [] {
    expect(eq(mcpp::build::previous_release_words("-DA=x$$y", false), (Words{"-DA=x$y"})));
    expect(eq(mcpp::build::previous_release_words("-DA=${v}z", false), (Words{"-DA=z"})));
    expect(eq(mcpp::build::previous_release_words("-DA=$v", false), (Words{"-DA="})));
    expect(eq(mcpp::build::previous_release_words("B=1", true), (Words{"-DB=1"})));
}};

// A remote served by plain filesystem reads is local, so `--offline` keeps
// building it; a scheme or scp-like syntax is remote. A Windows drive letter
// has a colon but no `@`, so an existing `C:\repo` stays local.
UTest local_means_a_file_url_or_an_existing_path{"GitRemote/LocalMeansAFileUrlOrAnExistingPath", [] {
    expect(mcpp::build::is_local_git_remote("file:///srv/repo.git"));
    expect(!mcpp::build::is_local_git_remote("https://github.com/x/y.git"));
    expect(!mcpp::build::is_local_git_remote("ssh://git@host/x.git"));
    expect(!mcpp::build::is_local_git_remote("git@github.com:x/y.git"));

    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path()
        / std::format("mcpp-prepare-helpers-{:x}", std::random_device{}());
    fs::create_directories(dir);
    expect(mcpp::build::is_local_git_remote(dir.string()));
    expect(!(mcpp::build::is_local_git_remote((dir / "missing").string())));
    std::error_code ec;
    fs::remove_all(dir, ec);
}};

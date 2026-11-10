import std;
import mcpplibs.mtest;
import mcpp.build.prepare_inputs;
import mcpp.manifest;

using namespace mcpplibs::mtest;

// A cfg bareword is data, not a list (docs/22 §target sections): `unix` is
// the family word, and any other dash-free word is `os = "<word>"`. The
// systems mcpp names are the rows' systems, and a row's `family` states its
// family, so a system a row adds is selectable without a new release.

namespace cfgpred = mcpp::build::cfgpred;

namespace {
bool m(std::string_view predicate, std::string_view triple) {
    return cfgpred::matches(std::string(predicate), cfgpred::context_for(std::string(triple)));
}
}  // namespace

UTest a_bareword_is_its_os{"CfgBarewords/ABarewordIsItsOs", [] {
    expect(m("freebsd", "x86_64-freebsd"));
    expect(m("cfg(freebsd)", "x86_64-freebsd-musl"));
    expect(m(R"(cfg(all(freebsd, arch = "x86_64")))", "x86_64-freebsd"));
    expect(!m("freebsd", "x86_64-linux-musl"));
    expect(m("linux", "x86_64-linux-musl"));
    expect(!m("linux", "x86_64-freebsd"));
}};

UTest the_family_comes_from_the_row{"CfgBarewords/TheFamilyComesFromTheRow", [] {
    expect(m("unix", "x86_64-freebsd"));
    expect(m("cfg(unix)", "aarch64-freebsd-musl"));
    expect(m("unix", "x86_64-linux-gnu"));
    expect(!m("unix", "x86_64-windows-msvc"));
    expect(eq(cfgpred::context_for("x86_64-freebsd-musl").family, "unix"));
}};

UTest a_triple_is_not_a_bareword{"CfgBarewords/ATripleIsNotABareword", [] {
    expect(mcpp::manifest::cfg::is_bareword("freebsd"));
    expect(!mcpp::manifest::cfg::is_bareword("x86_64-freebsd"));
    expect(!mcpp::manifest::cfg::is_bareword("cfg(unix)"));
    expect(!mcpp::manifest::cfg::is_bareword(""));
}};

UTest an_unknown_word_is_reported{"CfgBarewords/AnUnknownWordIsReported", [] {
    expect(eq(cfgpred::unknown_tokens("cfg(freebds)"), std::vector<std::string>({"freebds"})));
    expect(cfgpred::unknown_tokens("cfg(any(freebsd, unix, linux, macos, windows))").empty());
    const auto words = cfgpred::system_barewords();
    expect(std::ranges::find(words, "unix") != words.end());
    expect(std::ranges::find(words, "freebsd") != words.end());
    expect(std::ranges::find(words, "linux") != words.end());
}};

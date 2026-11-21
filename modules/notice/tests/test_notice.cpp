import std;
import mcpplibs.mtest;
import mcpp.notice;

using namespace mcpplibs::mtest;

// SUBSYSTEM-LEVEL: the policies, the record and the phrase of several subjects. That a notice is
// said once per project and again after `mcpp clean` is stated by the e2e suite.

namespace nt = mcpp::notice;

namespace {

std::filesystem::path scratch(std::string_view name) {
    auto dir = std::filesystem::temp_directory_path()
             / std::format("mcpp-notice-{}-{:x}", name, std::random_device{}());
    std::filesystem::create_directories(dir);
    return dir;
}

}  // namespace

UTest a_domain_without_a_row_is_said_always{"Notice/ADomainWithoutARowIsSaidAlways", [] {
    expect(nt::policy_of("build/depfile") == nt::Policy::Always);
    expect(nt::policy_of("build-program/includes-header") == nt::Policy::Once);
    for (auto const& k : nt::kKinds)
        expect(!k.docs.empty()) << k.domain << " points nowhere";
}};

UTest a_record_reads_back_what_it_wrote{"Notice/ARecordReadsBackWhatItWrote", [] {
    const auto dir = scratch("roundtrip");
    auto r = nt::Record::load(dir / nt::kRecordFile);
    expect(!r.shown(nt::key("host-input", "ninja")));
    r.mark(nt::key("host-input", "ninja"));
    r.set_fact("toolchain.default", "gcc@16.2.0");
    expect(fatal(r.save()));
    auto back = nt::Record::load(dir / nt::kRecordFile);
    expect(back.shown(nt::key("host-input", "ninja")));
    expect(eq(back.fact("toolchain.default").value_or(""), std::string("gcc@16.2.0")));
    expect(!back.fact("absent").has_value());
    std::filesystem::remove_all(dir);
}};

UTest two_writers_keep_both_keys{"Notice/TwoWritersKeepBothKeys", [] {
    // Each loads before the other saves, as two processes of one project would.
    const auto dir = scratch("writers");
    auto a = nt::Record::load(dir / nt::kRecordFile);
    auto b = nt::Record::load(dir / nt::kRecordFile);
    a.mark("a");
    b.mark("b");
    b.set_fact("f", "from b");
    expect(fatal(a.save()));
    expect(fatal(b.save()));
    auto back = nt::Record::load(dir / nt::kRecordFile);
    expect(back.shown("a") && back.shown("b")) << back.text();
    expect(eq(back.fact("f").value_or(""), std::string("from b")));
    std::filesystem::remove_all(dir);
}};

UTest a_partial_line_is_skipped{"Notice/APartialLineIsSkipped", [] {
    const auto r = nt::Record::parse("shown\tk1\nfact\tname\tvalue\nshown\nfact\tnovalue\nshow");
    expect(r.shown("k1"));
    expect(eq(r.fact("name").value_or(""), std::string("value")));
    expect(!r.fact("novalue").has_value());
}};

UTest subjects_read_as_one_phrase{"Notice/SubjectsReadAsOnePhrase", [] {
    const std::vector<std::string> one{"a"}, three{"a", "b", "c"},
        seven{"a", "b", "c", "d", "e", "f", "g"};
    expect(eq(nt::subjects(one), std::string("a")));
    expect(eq(nt::subjects(three), std::string("a, b and c")));
    expect(eq(nt::subjects(seven), std::string("a, b, c, d, e and 2 more")));
    expect(eq(nt::subjects(std::vector<std::string>{}), std::string{}));
}};

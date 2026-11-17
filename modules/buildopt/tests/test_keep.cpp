// mcpp.buildopt.keep: what two BMIs differ in, and the cutoff of a build directory
// (mcpp#946 O2, O6). The GCC files are built here as the ELF32 file GCC
// writes, with the sections the reader names; the offsets of the two
// checksums in `.gnu.c++.cfg` are those measured with gcc 16.1.0.
import std;
import mcpplibs.mtest;
import mcpp.buildopt.keep;
import mcpp.buildopt.keep.gcm;

using namespace mcpplibs::mtest;
namespace fs = std::filesystem;
using mcpp::buildopt::keep::Difference;

namespace {

fs::path tmpdir() {
    static int n = 0;
    auto d = fs::temp_directory_path() / std::format("mcpp-bmi-cutoff-{}-{}", static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count()), n++);
    fs::create_directories(d);
    return d;
}

void put32(std::string& s, std::uint32_t v) { for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>(v >> (8 * i))); }
void put16(std::string& s, std::uint16_t v) { s.push_back(static_cast<char>(v)); s.push_back(static_cast<char>(v >> 8)); }

struct Gcm {
    std::string entity = std::string("\x11\x22\x33\x44", 4) + "c_val-body";
    std::string olm    = std::string("\xaa\xbb\xcc\xdd" "c.cppm\0\1\0\0\7\7\0\3\0\5", 20);
    std::string cfg    = std::string("\x09\x74\x2a\x4d\x01\x71\x02\x00\x61\x03\x65\x04", 12) + "C++23-rest";
    std::string clock  = "2026/11/17 03:56:30";
};

// An ELF32 little-endian file: the header, the sections' bytes, the name
// table, the section headers.
std::string gcm(const Gcm& g) {
    const std::string readme = std::string("\0", 1) + "buildtime: " + g.clock + " UTC" + std::string("\0", 1)
                             + "localtime: " + g.clock + " CST" + std::string("\0", 1);
    const std::vector<std::pair<std::string, std::string>> sections = {
        {"c_val", g.entity}, {".gnu.c++.olm", g.olm}, {".gnu.c++.cfg", g.cfg}, {".gnu.c++.README", readme}};
    std::string names(1, '\0'), body, headers(40, '\0');
    std::size_t offset = 52;
    for (auto const& [name, bytes] : sections) {
        std::string h;
        put32(h, static_cast<std::uint32_t>(names.size()));
        names += name + std::string(1, '\0');
        put32(h, 1); put32(h, 0); put32(h, 0);
        put32(h, static_cast<std::uint32_t>(offset)); put32(h, static_cast<std::uint32_t>(bytes.size()));
        h.resize(40, '\0');
        headers += h;
        body += bytes;
        offset += bytes.size();
    }
    std::string h;
    put32(h, static_cast<std::uint32_t>(names.size()));
    names += ".strtab" + std::string(1, '\0');
    put32(h, 3); put32(h, 0); put32(h, 0);
    put32(h, static_cast<std::uint32_t>(offset)); put32(h, static_cast<std::uint32_t>(names.size()));
    h.resize(40, '\0');
    headers += h;
    std::string out = std::string("\x7f" "ELF\x01\x01\x01", 7);
    out.resize(0x20, '\0');
    put32(out, static_cast<std::uint32_t>(offset + names.size()));   // e_shoff
    out.resize(0x2e, '\0');
    put16(out, 40); put16(out, static_cast<std::uint16_t>(sections.size() + 2)); put16(out, static_cast<std::uint16_t>(sections.size() + 1));
    return out + body + names + headers;
}

fs::path write(const fs::path& p, const std::string& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return p;
}

// The same module after a line was inserted above its declarations: the line
// maps, the two checksums of the configuration and the clock.
Gcm shifted(Gcm g) {
    g.olm[17] = '\4';
    g.olm[0] = '\x01';
    g.cfg[0] = '\x5a'; g.cfg[9] = '\x5b';
    g.clock = "2026/11/17 03:59:02";
    return g;
}

UTest lines_alone{"BmiCutoff/AGccBmiWhoseLineMapsAloneMovedDiffersInLocationsOnly", [] {
    const auto d = tmpdir();
    const auto a = write(d / "a.gcm", gcm({}));
    expect(mcpp::buildopt::keep::gcm::is_format(gcm({})));
    expect(mcpp::buildopt::keep::difference(a, write(d / "same.gcm", gcm({.clock = "2026/11/17 04:00:00"}))) == Difference::Same);
    expect(mcpp::buildopt::keep::difference(a, write(d / "b.gcm", gcm(shifted({})))) == Difference::LocationsOnly);
}};

UTest anything_else{"BmiCutoff/AnEntityOrTheRestOfTheConfigurationIsAChange", [] {
    const auto d = tmpdir();
    const auto a = write(d / "a.gcm", gcm({}));
    auto entity = shifted({});
    entity.entity[6] = 'X';
    expect(mcpp::buildopt::keep::difference(a, write(d / "e.gcm", gcm(entity))) == Difference::Changed);
    auto rest = shifted({});
    rest.cfg[13] = 'X';
    expect(mcpp::buildopt::keep::difference(a, write(d / "c.gcm", gcm(rest))) == Difference::Changed);
    auto longer = shifted({});
    longer.olm += "x";
    expect(mcpp::buildopt::keep::difference(a, write(d / "l.gcm", gcm(longer))) == Difference::Changed);
}};

// A template body that reads its line is instantiated from the line maps: the
// BMI names the builtin, and is compared exactly.
UTest a_body_that_reads_its_line{"BmiCutoff/ABmiThatNamesALocationBuiltinIsComparedExactly", [] {
    const auto d = tmpdir();
    for (std::string_view builtin : {"__builtin_LINE", "__builtin_source_location"}) {
        Gcm g;
        g.entity += std::string(builtin);
        const auto a = write(d / "a.gcm", gcm(g));
        expect(mcpp::buildopt::keep::difference(a, write(d / "b.gcm", gcm(shifted(g)))) == Difference::Changed) << builtin;
    }
}};

UTest another_format{"BmiCutoff/AFileOfNoKnownFormatIsComparedByItsBytes", [] {
    const auto d = tmpdir();
    const auto a = write(d / "a.pcm", "CPCH-one");
    expect(mcpp::buildopt::keep::difference(a, write(d / "b.pcm", "CPCH-one")) == Difference::Same);
    expect(mcpp::buildopt::keep::difference(a, write(d / "c.pcm", "CPCH-two")) == Difference::Changed);
    expect(mcpp::buildopt::keep::difference(a, d / "absent.pcm") == Difference::Changed);
}};

// The directory states the cutoff; an edge asks in it; the run's record names
// what was kept, and an exact run removes what an earlier run kept.
UTest the_directory{"BmiCutoff/TheDirectoryStatesTheCutoffAndRecordsTheRun", [] {
    const auto d = tmpdir();
    const auto back = fs::current_path();
    fs::current_path(d);
    write("gcm.cache/c.gcm", gcm(shifted({})));
    write("gcm.cache/c.gcm.bak", gcm({}));
    Gcm other; other.entity[6] = 'X';
    write("gcm.cache/d.gcm", gcm(other));
    write("gcm.cache/d.gcm.bak", gcm({}));

    mcpp::buildopt::keep::run_begin(d, false);
    expect(!mcpp::buildopt::keep::keeps_previous("gcm.cache/c.gcm", "gcm.cache/c.gcm.bak"));
    auto run = mcpp::buildopt::keep::run_end(d);
    expect(run.changed == std::vector<std::string>{"c.gcm"} && run.kept.empty());

    mcpp::buildopt::keep::run_begin(d, true);
    expect(mcpp::buildopt::keep::keeps_previous("gcm.cache/c.gcm", "gcm.cache/c.gcm.bak"));
    expect(!mcpp::buildopt::keep::keeps_previous("gcm.cache/d.gcm", "gcm.cache/d.gcm.bak"));
    run = mcpp::buildopt::keep::run_end(d);
    expect(run.kept == std::vector<std::string>{"c.gcm"} && run.changed == std::vector<std::string>{"d.gcm"});
    const auto lines = mcpp::buildopt::keep::summary(run);
    expect(lines.size() == 2u);
    expect(lines.back().find("bmi_cutoff = \"exact\"") != std::string::npos) << lines.back();

    // The cutoff the directory states holds for a run that states none.
    mcpp::buildopt::keep::run_begin(d, std::nullopt);
    expect(mcpp::buildopt::keep::keeps_previous("gcm.cache/c.gcm", "gcm.cache/c.gcm.bak"));
    (void)mcpp::buildopt::keep::run_end(d);

    // An exact run removes the BMI kept with earlier lines, so its edge runs.
    mcpp::buildopt::keep::run_begin(d, false);
    expect(!fs::exists("gcm.cache/c.gcm"));
    expect(fs::exists("gcm.cache/d.gcm"));
    expect(mcpp::buildopt::keep::summary(mcpp::buildopt::keep::run_end(d)).empty());
    fs::current_path(back);
}};

}  // namespace

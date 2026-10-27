// The vendored xlings is replaced when it is BEHIND the pin, and only then.
//
// acquire_xlings_binary used to return on mere existence, so a home kept
// whatever xlings it first acquired forever -- measured at 2026.8.2.1 against
// a pin of 2026.8.6.3, with `mcpp self env` printing both numbers next to each
// other and saying nothing. That is not cosmetic: the subos_info block arrived
// in xlings 2026.8.5.1, so on that machine the graphics packages' declarations
// were discarded by a client too old to have the API, and the fix for
// mcpp#352 could never take effect no matter how current mcpp itself was.
//
// The comparison is STRICTLY older, not "not equal". A user who deliberately
// put a newer xlings there must not be downgraded by an mcpp pinning an older
// one, and an unparseable version is not evidence of being behind.

#include <gtest/gtest.h>
#include <cstdlib>
#include <fstream>

import std;
import mcpp.invocation;
import mcpp.fallback.xlings_binary;

namespace fb = mcpp::fallback;

namespace {

TEST(XlingsVersionPin, OlderIsBehind) {
    EXPECT_TRUE(fb::version_is_older("2026.8.2.1", "2026.8.6.3"));
    EXPECT_TRUE(fb::version_is_older("2026.7.31.3", "2026.8.1.1"));
    EXPECT_TRUE(fb::version_is_older("2026.8.6", "2026.8.6.1"));
}

TEST(XlingsVersionPin, EqualIsNotBehind) {
    EXPECT_FALSE(fb::version_is_older("2026.8.6.3", "2026.8.6.3"));
    // Trailing zeros compare equal, not older.
    EXPECT_FALSE(fb::version_is_older("2026.8.6.0", "2026.8.6"));
}

// The case that decides whether this is safe to run automatically: a user who
// put a NEWER xlings in place keeps it.
TEST(XlingsVersionPin, NewerIsNotDowngraded) {
    EXPECT_FALSE(fb::version_is_older("2026.9.1.1", "2026.8.6.3"));
    EXPECT_FALSE(fb::version_is_older("2027.1.1.1", "2026.12.31.9"));
}

// A version this code cannot parse says nothing, and "says nothing" must not
// be read as "is behind" -- that would delete a working binary on a guess.
TEST(XlingsVersionPin, UnparseableIsNotBehind) {
    EXPECT_FALSE(fb::version_is_older("dev", "2026.8.6.3"));
    EXPECT_FALSE(fb::version_is_older("2026.8.6.3", ""));
    EXPECT_FALSE(fb::version_is_older("", "2026.8.6.3"));
    EXPECT_FALSE(fb::version_is_older("2026.8.x", "2026.8.6.3"));
}

// The version scheme changed epochs: xlings went 0.4.x -> YYYY.M.D.N. Both
// live on the same disk, and a home's system xlings may still be a 0.4.x while
// its vendored one is already dated. Getting this backwards is not academic --
// the first cut of the replace-when-behind logic deleted the vendored binary
// and re-acquired from `which xlings`, which on this developer machine turned
// 2026.8.2.1 into 0.4.51: older still, and equally missing the very feature
// the replacement existed to restore. Being behind the pin justifies looking
// for a replacement; it does not justify accepting whatever turns up.
TEST(XlingsVersionPin, DatedSchemeIsNewerThanTheOldOne) {
    EXPECT_FALSE(fb::version_is_older("2026.8.2.1", "0.4.51"));
    EXPECT_TRUE(fb::version_is_older("0.4.51", "2026.8.2.1"));
    EXPECT_TRUE(fb::version_is_older("0.4.51", "0.4.54"));
}

// THE PROBE READS STANDARD OUTPUT, AND ON WINDOWS IT RUNS AT ALL.
//
// The version used to be read through the command string
// `<bin> --version 2>/dev/null`. cmd.exe cannot open `/dev/null`, so on Windows
// the probe printed "The system cannot find the path specified.", never ran
// xlings, and returned an empty version -- which acquire_xlings_binary reads as
// "unknown, keep it", so a Windows home never moved to a newer pin. On Windows
// this test runs a .bat through the real launcher, which is the path that
// failed; elsewhere a shell script. Both print a dotted number on stderr first,
// which must not be taken for the version.
TEST(XlingsVersionPin, ProbeReadsStandardOutputThroughTheLauncher) {
    auto dir = std::filesystem::temp_directory_path()
             / std::format("mcpp probe {}", std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(dir);
#if defined(_WIN32)
    auto fake = dir / "xlings.bat";
    {
        std::ofstream os(fake, std::ios::binary);
        os << "@echo off\r\necho warning 9.9.9 1>&2\r\necho xlings 2026.1.2.3\r\n";
    }
#else
    auto fake = dir / "xlings";
    {
        std::ofstream os(fake, std::ios::binary);
        os << "#!/bin/sh\necho 'warning 9.9.9' 1>&2\nprintf 'xlings 2026.1.2.3\\n'\n";
    }
    std::filesystem::permissions(fake, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
#endif
    EXPECT_EQ(fb::vendored_xlings_version(fake), "2026.1.2.3");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

}  // namespace

// ONE ANSWER TO "WHICH SOURCE" (mcpp#744). The check that decides whether to
// replace a vendored binary used the first source that existed, while the
// replacement ran the whole chain: a released copy older than the pin hid a
// newer xlings on PATH, and mcpp stated that no newer source was available.
namespace {
fb::XlingsSource src(std::string v, std::string origin) {
    return fb::XlingsSource{std::filesystem::path(origin), std::move(v), origin};
}
}  // namespace

TEST(XlingsSource, TheOverrideIsTakenAsAnExplicitChoice) {
    auto c = fb::choose_xlings_source(src("2026.1.1.1", "override"),
                                      src("2026.9.1.1", "released"), src("2026.9.9.1", "path"));
    ASSERT_TRUE(c);
    EXPECT_EQ(c->origin, "override");
}

TEST(XlingsSource, TheNewerOfTheReleasedAndThePathCopyIsTaken) {
    auto a = fb::choose_xlings_source(std::nullopt, src("2026.9.29.1", "released"),
                                      src("2026.9.30.1", "path"));
    ASSERT_TRUE(a);
    EXPECT_EQ(a->origin, "path");
    auto b = fb::choose_xlings_source(std::nullopt, src("2026.9.30.1", "released"),
                                      src("2026.9.29.1", "path"));
    ASSERT_TRUE(b);
    EXPECT_EQ(b->origin, "released");
}

TEST(XlingsSource, TheReleasedCopyWinsATieAndAnUnreadablePathCopy) {
    auto tie = fb::choose_xlings_source(std::nullopt, src("2026.9.30.1", "released"),
                                        src("2026.9.30.1", "path"));
    ASSERT_TRUE(tie);
    EXPECT_EQ(tie->origin, "released");
    auto unreadable = fb::choose_xlings_source(std::nullopt, src("2026.9.30.1", "released"),
                                               src("", "path"));
    ASSERT_TRUE(unreadable);
    EXPECT_EQ(unreadable->origin, "released");
    auto releasedUnreadable = fb::choose_xlings_source(std::nullopt, src("", "released"),
                                                       src("2026.9.30.1", "path"));
    ASSERT_TRUE(releasedUnreadable);
    EXPECT_EQ(releasedUnreadable->origin, "path");
}

TEST(XlingsSource, AMissingSourceLeavesTheOther) {
    auto onlyPath = fb::choose_xlings_source(std::nullopt, std::nullopt, src("1.0", "path"));
    ASSERT_TRUE(onlyPath);
    EXPECT_EQ(onlyPath->origin, "path");
    EXPECT_FALSE(fb::choose_xlings_source(std::nullopt, std::nullopt, std::nullopt));
}

// THE VERSION IS ASKED ONCE. Asking costs a process of xlings (0.35 s
// measured), and every command that loads the configuration asked. Within a
// process the answer is kept; across processes it is kept in a file keyed by
// the binary's path, size and modification time. The stub counts its runs.
namespace {
struct CountingStub {
    std::filesystem::path dir, bin, counter;
    explicit CountingStub(std::string_view version) {
        dir = std::filesystem::temp_directory_path()
            / std::format("mcpp memo {}", std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directories(dir);
        counter = dir / "runs";
        write(version);
    }
    void write(std::string_view version) {
#if defined(_WIN32)
        bin = dir / "xlings.bat";
        std::ofstream os(bin, std::ios::binary | std::ios::trunc);
        os << "@echo off\r\necho run>>\"" << counter.string() << "\"\r\necho xlings "
           << version << "\r\n";
#else
        bin = dir / "xlings";
        {
            std::ofstream os(bin, std::ios::binary | std::ios::trunc);
            os << "#!/bin/sh\necho run >> '" << counter.string() << "'\nprintf 'xlings "
               << version << "\\n'\n";
        }
        std::filesystem::permissions(bin, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
#endif
    }
    int runs() const {
        std::ifstream is(counter);
        int n = 0;
        for (std::string line; std::getline(is, line);) ++n;
        return n;
    }
    ~CountingStub() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};
std::string memo_key(const std::filesystem::path& bin) {
    auto u8 = bin.generic_u8string();
    return std::format("{}\t{}\t{}", std::string(reinterpret_cast<const char*>(u8.data()), u8.size()),
                       std::filesystem::file_size(bin),
                       std::filesystem::last_write_time(bin).time_since_epoch().count());
}
}  // namespace

TEST(XlingsVersionMemo, AskedOncePerProcessAndStoredForTheNext) {
    CountingStub stub("2026.1.2.3");
    const auto memo = stub.dir / "memo";
    EXPECT_EQ(fb::known_xlings_version(stub.bin, memo), "2026.1.2.3");
    EXPECT_EQ(fb::known_xlings_version(stub.bin, memo), "2026.1.2.3");
    EXPECT_EQ(stub.runs(), 1);
    std::ifstream is(memo);
    std::string line;
    std::getline(is, line);
    EXPECT_EQ(line, memo_key(stub.bin) + "\t2026.1.2.3");
}

TEST(XlingsVersionMemo, AStoredAnswerIsReadWithoutRunningTheBinary) {
    CountingStub stub("2026.1.2.3");
    const auto memo = stub.dir / "memo";
    {
        std::ofstream os(memo, std::ios::binary);
        os << "/elsewhere/xlings\t1\t1\t2020.1.1.1\n" << memo_key(stub.bin) << "\t2026.7.7.7\n";
    }
    EXPECT_EQ(fb::known_xlings_version(stub.bin, memo), "2026.7.7.7");
    EXPECT_EQ(stub.runs(), 0);
}

TEST(XlingsVersionMemo, ANewFileIsAskedAgain) {
    CountingStub stub("2026.1.2.3");
    const auto memo = stub.dir / "memo";
    EXPECT_EQ(fb::known_xlings_version(stub.bin, memo), "2026.1.2.3");
    // An update writes a new file: another size, and another modification time.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    stub.write("2026.10.20.30");
    EXPECT_EQ(fb::known_xlings_version(stub.bin, memo), "2026.10.20.30");
    EXPECT_EQ(stub.runs(), 2);
    std::ifstream is(memo);
    int lines = 0;
    for (std::string l; std::getline(is, l);) ++lines;
    EXPECT_EQ(lines, 1) << "the binary's earlier line is replaced, not kept";
}

// ONCE PER PROCESS (mcpp#744). The configuration is loaded from about ten call
// sites, and one `mcpp pack` over a workspace printed its note three times per
// member. A home settled once in a process is not examined again: the note is
// stated once and the version is asked once.
namespace {
class ScopedEnv {
public:
    ScopedEnv(std::string name, const char* value)
        : inner_(std::move(name), value ? std::optional<std::string>(value) : std::nullopt) {}
private:
    mcpp::invocation::ScopedEnv inner_;   // stated for this invocation, as mcpp states its own
};
}  // namespace

TEST(XlingsAcquire, TheNoteIsStatedOncePerProcess) {
    CountingStub stub("2026.1.1.1");
    const auto empty = stub.dir / "empty-path";
    std::filesystem::create_directories(empty);
    // No newer source: no override, an empty PATH, and a test binary that does
    // not run from a release layout (`<prefix>/bin/`).
    ScopedEnv override_("MCPP_VENDORED_XLINGS", nullptr);
    ScopedEnv path("PATH", empty.string().c_str());
    testing::internal::CaptureStderr();
    auto first = fb::acquire_xlings_binary(stub.bin, /*quiet=*/false, "2026.9.30.1");
    auto second = fb::acquire_xlings_binary(stub.bin, /*quiet=*/false, "2026.9.30.1");
    const auto err = testing::internal::GetCapturedStderr();
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    std::size_t notes = 0;
    for (auto at = err.find("no newer source is available"); at != std::string::npos;
         at = err.find("no newer source is available", at + 1))
        ++notes;
    EXPECT_EQ(notes, 1u) << err;
    EXPECT_EQ(stub.runs(), 1) << "the second load asked the version again";
}


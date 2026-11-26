import std;
import mcpplibs.mtest;
import mcpp.surface;

using namespace mcpplibs::mtest;

// SUBSYSTEM-LEVEL: the line of an item and the record it is appended to. That `mcpp emit
// surface` lists the items, and an invocation records those it used, is stated by the e2e suite
// (tests/e2e/test/protocol.cpp).

namespace sf = mcpp::surface;

UTest an_item_reads_back_from_its_line{"Surface/AnItemReadsBackFromItsLine", [] {
    const auto text = sf::line({"flag", sf::flag_name("cache gc", "dry-run")})
                    + sf::line({"key", "build.cache"}) + "\nreason\nkey build.cflags\r\nflag ";
    const auto back = sf::parse(text);
    expect(fatal(eq(back.size(), std::size_t{3})));
    expect(eq(back[0].name, std::string("cache gc --dry-run")));
    expect(eq(back[1].kind, std::string("key")));
    expect(eq(back[2].name, std::string("build.cflags"))) << "a CRLF line reads the same";
    expect(eq(sf::flag_name("", "quiet"), std::string("--quiet")));
}};

UTest an_item_is_recorded_once_per_process{"Surface/AnItemIsRecordedOncePerProcess", [] {
    const auto file = std::filesystem::temp_directory_path()
                    / std::format("mcpp-surface-{}.txt", std::random_device{}());
    std::filesystem::remove(file);
    sf::append_once(file, {"key", "build.cache"});
    sf::append_once(file, {"reason", "unknown-target"});
    sf::append_once(file, {"key", "build.cache"});
    std::ifstream is(file);
    const std::string text{std::istreambuf_iterator<char>(is), {}};
    std::filesystem::remove(file);
    expect(eq(sf::parse(text).size(), std::size_t{2}));
}};

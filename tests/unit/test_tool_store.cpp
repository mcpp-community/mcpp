// The tool store's paths and keys. The key of a tool built from a `path`
// package names its tree by the digest of its content (mcpp#974), which
// test_source_digest.cpp covers.

import std;
import mcpplibs.mtest;
import mcpp.build.tool_store;

using namespace mcpplibs::mtest;

namespace fs = std::filesystem;

// The sub-build scratch (mcpp#641, item 3). Every object path the sub-build
// writes is appended to it, so it spends no length on names the entry already
// records; it still separates two consumers of one entry, and a re-run by the
// same consumer finds the same directory.
UTest is_short_per_consumer_and_stable{"ToolStoreScratch/IsShortPerConsumerAndStable", [] {
    const fs::path cache{"/home/u/.mcpp/build-cache/v1"};
    const fs::path entry = cache / "tool" / "mcpplibs"
        / "installer@0.1.0+path.e285b2f02cb1c429" / "7005eaf9bd8c80ef";
    const auto a1 = mcpp::build::tool_store::scratch_dir(cache, entry, "/work/app");
    const auto a2 = mcpp::build::tool_store::scratch_dir(cache, entry, "/work/app");
    const auto b  = mcpp::build::tool_store::scratch_dir(cache, entry, "/work/other");
    const auto other = mcpp::build::tool_store::scratch_dir(
        cache, entry.parent_path() / "0000000000000000", "/work/app");

    expect(eq(a1, a2));
    expect(ne(a1, b));
    expect(ne(a1, other));
    expect(eq(a1.parent_path(), cache / "tool" / ".build"));
    expect(eq(a1.filename().string().size(), 16u)) << a1;
    // The old scratch lived inside the entry, as `<entry>/build-<16 hex>`.
    expect(lt(a1.generic_string().size(), (entry / "build-0123456789abcdef").generic_string().size()));
}};

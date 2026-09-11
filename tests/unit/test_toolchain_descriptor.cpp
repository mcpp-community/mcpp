#include <gtest/gtest.h>

import std;
import mcpp.platform;
import mcpp.toolchain.model;
import mcpp.toolchain.registry;
import mcpp.toolchain.triple;

using namespace mcpp::toolchain;

namespace {

// A payload root in a temporary directory, with an optional descriptor and an
// optional compiler at a chosen relative path. Nothing here is Android- or
// emsdk-specific: the contract is that A PAYLOAD DESCRIBES ITSELF, and a test
// tied to one payload's layout would be testing the layout.
struct FakePayload {
    std::filesystem::path root;
    explicit FakePayload(std::string_view name) {
        root = std::filesystem::temp_directory_path()
             / std::format("mcpp-desc-{}-{}", name,
                           std::chrono::steady_clock::now()
                               .time_since_epoch().count());
        std::filesystem::create_directories(root);
    }
    ~FakePayload() { std::error_code ec; std::filesystem::remove_all(root, ec); }
    FakePayload(const FakePayload&) = delete;
    FakePayload& operator=(const FakePayload&) = delete;

    void write_descriptor(std::string_view json) const {
        std::ofstream(root / ".mcpp-toolchain.json") << json;
    }
    std::filesystem::path add_compiler(std::string_view rel) const {
        auto p = root / rel;
        std::filesystem::create_directories(p.parent_path());
        std::ofstream(p) << "#!/bin/sh\n";
        return p;
    }
};

XimToolchainPackage bin_shaped_pkg() {
    // The built-in guess for a payload that ships no descriptor: `bin/` and a
    // clang-shaped frontend. This is what "no flag day" means, expressed as an
    // object rather than as a sentence.
    auto spec = parse_toolchain_spec("llvm@22.1.8");
    return to_xim_package(*spec);
}

} // namespace

// ─── Absence is compatibility (C) ───────────────────────────────────────────
//
// A released payload ships no descriptor, and there is no version of this
// feature in which that stops working. The criterion has a denominator: the
// SAME package object resolves the SAME frontend as before the descriptor
// existed, which is what the assertion below measures.
TEST(PayloadDescriptor, NoDescriptorResolvesExactlyAsBefore) {
    FakePayload fp{"none"};
    auto clang = fp.add_compiler("bin/clang++");
    auto pkg = bin_shaped_pkg();

    auto d = read_payload_descriptor(fp.root);
    ASSERT_TRUE(d.has_value()) << d.error();
    EXPECT_FALSE(d->has_value()) << "no file means no answer, not an error";

    auto found = payload_frontend(fp.root, pkg);
    ASSERT_TRUE(found.has_value()) << found.error();
    EXPECT_EQ(*found, clang);
    EXPECT_EQ(payload_frontend_dir(fp.root, pkg), fp.root / "bin");
}

// ─── The engine stops knowing the layout (C) ────────────────────────────────
//
// The criterion is that the guess is NOT CONSULTED when the payload answers,
// and the only way to assert that is a payload whose compiler is NOT where the
// guess would look. Here `bin/` is empty and the compiler is five levels down
// in the NDK's own shape -- so a resolution that succeeds could only have come
// from the descriptor.
TEST(PayloadDescriptor, TheDescriptorAnswersWhereTheGuessCannot) {
    FakePayload fp{"frontend"};
    auto deep = fp.add_compiler(
        "toolchains/llvm/prebuilt/some-host-tag/bin/clang++");
    std::filesystem::create_directories(fp.root / "bin");   // present, empty
    fp.write_descriptor(R"({
        "schema": 1,
        "frontend": "toolchains/llvm/prebuilt/some-host-tag/bin/clang++"
    })");
    auto pkg = bin_shaped_pkg();

    // `some-host-tag` is not a host tag this engine can compute, which is the
    // point: the path is the payload's, not a derivation.
    auto found = payload_frontend(fp.root, pkg);
    ASSERT_TRUE(found.has_value()) << found.error();
    EXPECT_EQ(*found, deep);

    // AND THE MESSAGE NAMES WHERE THE SEARCH HAPPENED. A refusal naming
    // `<root>/bin` while the lookup read the descriptor's path is the failure
    // this repository records most often: the lookup is repaired and the
    // message is not.
    EXPECT_EQ(payload_frontend_dir(fp.root, pkg), deep.parent_path());
}

// A descriptor naming a frontend that is not there is "nothing found where we
// looked" -- empty, the caller's long-standing cue -- and NOT an error. The
// two outcomes have different repairs: install the payload again, versus fix
// the recipe.
TEST(PayloadDescriptor, ANamedFrontendThatIsAbsentIsEmptyNotAnError) {
    FakePayload fp{"absent"};
    fp.add_compiler("bin/clang++");   // the guess WOULD find this
    fp.write_descriptor(R"({"schema": 1, "frontend": "opt/bin/clang++"})");

    auto found = payload_frontend(fp.root, bin_shaped_pkg());
    ASSERT_TRUE(found.has_value()) << found.error();
    EXPECT_TRUE(found->empty())
        << "the payload said where its compiler is; the guess is not a "
           "second opinion";
}

// ─── Present and malformed is refused, naming the file (C) ──────────────────
//
// This is the property that makes the descriptor safe to add. Without it a typo
// reads as "an older payload": the engine falls back to a hardcoded path for a
// layout that has moved, and the build fails somewhere else entirely.
TEST(PayloadDescriptor, EveryMalformedShapeIsRefusedByName) {
    struct Case { std::string_view why, json, mustMention; };
    const Case cases[] = {
        { "not JSON",             "{ not json",                        "JSON" },
        { "not an object",        "[1, 2, 3]",                         "object" },
        { "no schema",            R"({"frontend": "bin/clang++"})",    "schema" },
        { "schema not an int",    R"({"schema": "1"})",                "schema" },
        { "a schema we lack",     R"({"schema": 2})",                  "schema 2" },
        { "frontend not string",  R"({"schema":1,"frontend":42})",     "frontend" },
        { "frontend empty",       R"({"schema":1,"frontend":""})",     "frontend" },
        { "frontend absolute",    R"({"schema":1,"frontend":"/usr/bin/g++"})",
                                                                       "absolute" },
        { "frontend escapes",     R"({"schema":1,"frontend":"../../usr/bin/g++"})",
                                                                       "leaves" },
        { "floor not a string",   R"({"schema":1,"platform_floor":21})", "platform_floor" },
        { "floor empty",          R"({"schema":1,"platform_floor":""})", "platform_floor" },
        { "floor not a version",  R"({"schema":1,"platform_floor":"r30"})",
                                                                       "platform_floor" },
        { "defines not an array", R"({"schema":1,"std_module_defines":"A="})",
                                                                       "std_module_defines" },
        { "defines hold an int",  R"({"schema":1,"std_module_defines":[1]})",
                                                                       "non-string" },
        { "a define is a flag",   R"({"schema":1,"std_module_defines":["-Wl,-rpath,/usr/lib"]})",
                                                                       "flag channel" },
        { "a define has a space", R"({"schema":1,"std_module_defines":["A=b c"]})",
                                                                       "whitespace" },
    };
    for (auto const& c : cases) {
        FakePayload fp{"bad"};
        fp.add_compiler("bin/clang++");
        fp.write_descriptor(c.json);

        auto d = read_payload_descriptor(fp.root);
        ASSERT_FALSE(d.has_value()) << c.why << " was accepted";
        // THE FILE IS NAMED IN EVERY ONE. A message that says only "invalid
        // descriptor" leaves the reader to find which payload wrote it.
        EXPECT_NE(d.error().find(".mcpp-toolchain.json"), std::string::npos)
            << c.why << ": " << d.error();
        EXPECT_NE(d.error().find(c.mustMention), std::string::npos)
            << c.why << ": " << d.error();

        // And the refusal travels: the frontend lookup cannot express it as
        // "not found", so it propagates rather than falling back to the guess
        // -- even though the guess would have succeeded here.
        auto found = payload_frontend(fp.root, bin_shaped_pkg());
        ASSERT_FALSE(found.has_value()) << c.why << " fell back to the guess";
        EXPECT_EQ(found.error(), d.error()) << c.why;
    }
}

// ─── The three answers, read as written ─────────────────────────────────────
TEST(PayloadDescriptor, TheThreeKeysAreCarriedThrough) {
    FakePayload fp{"full"};
    fp.add_compiler("toolchains/llvm/prebuilt/linux-x86_64/bin/clang++");
    fp.write_descriptor(R"({
        "schema": 1,
        "frontend": "toolchains/llvm/prebuilt/linux-x86_64/bin/clang++",
        "platform_floor": "21",
        "std_module_defines": ["__BIONIC_CTYPE_INLINE=", "_FORTIFY_SOURCE=0"]
    })");
    auto d = read_payload_descriptor(fp.root);
    ASSERT_TRUE(d.has_value()) << d.error();
    ASSERT_TRUE(d->has_value());
    EXPECT_EQ((*d)->schema, 1);
    EXPECT_EQ((*d)->frontend,
              "toolchains/llvm/prebuilt/linux-x86_64/bin/clang++");
    EXPECT_EQ((*d)->platformFloor, "21");
    ASSERT_EQ((*d)->stdModuleDefines.size(), 2u);
    EXPECT_EQ((*d)->stdModuleDefines[0], "__BIONIC_CTYPE_INLINE=");
    EXPECT_EQ((*d)->stdModuleDefines[1], "_FORTIFY_SOURCE=0");

    // ALL THREE KEYS ARE OPTIONAL. A payload that answers one question and
    // not the others is a payload, not a malformed file -- otherwise a second
    // SDK would have to fabricate answers it does not have.
    fp.write_descriptor(R"({"schema": 1})");
    auto bare = read_payload_descriptor(fp.root);
    ASSERT_TRUE(bare.has_value()) << bare.error();
    ASSERT_TRUE(bare->has_value());
    EXPECT_TRUE((*bare)->frontend.empty());
    EXPECT_TRUE((*bare)->platformFloor.empty());
    EXPECT_TRUE((*bare)->stdModuleDefines.empty());
}

// ─── Found from the compiler, by walking rather than counting ───────────────
//
// Two of the three answers are consumed where only the compiler path is in
// hand. Counting components is the fact that changes silently when a layout
// does, so the search walks up -- and is bounded, so a compiler that belongs
// to no payload cannot adopt a descriptor from some ancestor directory.
TEST(PayloadDescriptor, FoundFromTheCompilerAndTheWalkIsBounded) {
    FakePayload fp{"walk"};
    auto deep = fp.add_compiler(
        "toolchains/llvm/prebuilt/linux-x86_64/bin/clang++");
    fp.write_descriptor(R"({"schema": 1, "platform_floor": "21"})");

    auto d = payload_descriptor_for_compiler(deep);
    ASSERT_TRUE(d.has_value()) << d.error();
    ASSERT_TRUE(d->has_value()) << "five levels up was not reached";
    EXPECT_EQ((*d)->platformFloor, "21");

    // A refusal still travels through the walk, because a compiler whose
    // payload describes itself wrongly is the same defect wherever it is read.
    fp.write_descriptor(R"({"schema": 1, "platform_floor": 21})");
    auto bad = payload_descriptor_for_compiler(deep);
    ASSERT_FALSE(bad.has_value());
    EXPECT_NE(bad.error().find(".mcpp-toolchain.json"), std::string::npos)
        << bad.error();

    // Beyond the bound there is no answer. Nine levels is one past it.
    auto far = fp.add_compiler("a/b/c/d/e/f/g/h/i/clang++");
    auto none = payload_descriptor_for_compiler(far);
    ASSERT_TRUE(none.has_value()) << none.error();
    EXPECT_FALSE(none->has_value())
        << "an unbounded walk adopts a descriptor from a directory that is "
           "not this payload";
}

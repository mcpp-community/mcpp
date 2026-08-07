// Reading the `subos_info` block xlings writes into a subos's .xlings.json.
//
// This is a CROSS-REPO CONTRACT, so the assertions are about the wire format
// and about what happens when it is absent or newer than we understand —
// not about internal behaviour. The degradation cases carry the weight: a
// subos made before xlings grew the block is the common case on any machine
// that has been around a while, and silence there is what made mcpp#352
// expensive to find in the first place.

#include <gtest/gtest.h>

import std;
import mcpp.platform;
import mcpp.xlings.subos_info;

namespace su = mcpp::xlings::subos;

namespace {

struct Tmp {
    std::filesystem::path dir;
    Tmp() {
        dir = std::filesystem::temp_directory_path()
            / std::format("mcpp_subos_test_{}", std::random_device{}());
        std::filesystem::create_directories(dir);
    }
    ~Tmp() { std::error_code ec; std::filesystem::remove_all(dir, ec); }
    void write(std::string_view body) const {
        std::ofstream(dir / ".xlings.json") << body;
    }
};

TEST(SubosInfo, ReadsRuntimeAndEnvDeclarations) {
    Tmp t;
    t.write(R"({
      "workspace": {},
      "subos_info": {
        "schema_version": 1,
        "runtime": "glibc@2.39",
        "envs": [
          { "binding": "mesa@25.0.7.1", "decls": [
            { "var": "LIBGL_DRIVERS_PATH", "op": "prepend",
              "value": "${subosdir}/usr/lib/dri" },
            { "var": "XDG_DATA_DIRS", "op": "prepend",
              "value": "${subosdir}/share" }
          ]}
        ]
      }
    })");
    auto info = su::read(t.dir);
    EXPECT_TRUE(info.present);
    EXPECT_EQ(info.schema, 1);
    EXPECT_EQ(info.runtime, "glibc@2.39");
    ASSERT_EQ(info.providers.size(), 1u);
    EXPECT_EQ(info.providers[0].binding, "mesa@25.0.7.1");
    ASSERT_EQ(info.providers[0].decls.size(), 2u);
    EXPECT_EQ(info.providers[0].decls[0].var, "LIBGL_DRIVERS_PATH");
    EXPECT_TRUE(info.note.empty());
}

// `${subosdir}` expands against the subos the block was read FROM, not
// against any global notion of "the" subos. That is what lets a program
// built under one subos run correctly under another.
TEST(SubosInfo, ResolvesSubosdirPlaceholder) {
    Tmp t;
    t.write(R"({"subos_info":{"schema_version":1,"runtime":"glibc@2.39",
      "envs":[{"binding":"mesa@1","decls":[
        {"var":"LIBGL_DRIVERS_PATH","op":"prepend","value":"${subosdir}/usr/lib/dri"}]}]}})");
    auto env = su::resolve_env(su::read(t.dir), t.dir);
    ASSERT_EQ(env.size(), 1u);
    EXPECT_EQ(env[0].first, "LIBGL_DRIVERS_PATH");
    // Literal concatenation, NOT a path join. The separator in the declaration
    // belongs to the subos manifest and is substituted verbatim; turning it
    // into the host's would rewrite a value we do not own. On Windows the two
    // spellings differ and the path-join form is the wrong expectation.
    EXPECT_EQ(env[0].second, t.dir.string() + "/usr/lib/dri");
}

// Several providers may contribute to one variable — that is the normal
// shape for the graphics stack, where mesa and a vendor bridge both add an
// EGL vendor directory. `prepend` joins them; it must not drop either.
TEST(SubosInfo, PrependJoinsProvidersInOrder) {
    Tmp t;
    t.write(R"({"subos_info":{"schema_version":1,"runtime":"glibc@2.39","envs":[
      {"binding":"a-mesa@1","decls":[
        {"var":"V","op":"prepend","value":"${subosdir}/one"}]},
      {"binding":"b-vendor@1","decls":[
        {"var":"V","op":"prepend","value":"${subosdir}/two"}]}]}})");
    auto env = su::resolve_env(su::read(t.dir), t.dir);
    ASSERT_EQ(env.size(), 1u);
    const auto sep = mcpp::platform::env::path_list_separator();
    EXPECT_EQ(env[0].second,
              t.dir.string() + "/two" + sep + t.dir.string() + "/one")
        << "both providers must survive, later binding front-most";
}

// The same value arriving twice must not accumulate: nested invocations
// would otherwise grow the variable without bound.
TEST(SubosInfo, PrependDeduplicates) {
    Tmp t;
    t.write(R"({"subos_info":{"schema_version":1,"runtime":"glibc@2.39","envs":[
      {"binding":"a@1","decls":[{"var":"V","op":"prepend","value":"${subosdir}/x"}]},
      {"binding":"b@1","decls":[{"var":"V","op":"prepend","value":"${subosdir}/x"}]}]}})");
    auto env = su::resolve_env(su::read(t.dir), t.dir);
    ASSERT_EQ(env.size(), 1u);
    // One entry, not two. The de-duplication has to split on the PLATFORM's
    // list separator: keyed on ':' it would cut "C:\\x" apart on Windows,
    // match nothing, and grow the list on every nested invocation.
    EXPECT_EQ(env[0].second, t.dir.string() + "/x");
    EXPECT_EQ(env[0].second.find(mcpp::platform::env::path_list_separator()),
              std::string::npos);
}

// `set` replaces rather than joins — xlings's own precedence.
TEST(SubosInfo, SetReplaces) {
    Tmp t;
    t.write(R"({"subos_info":{"schema_version":1,"runtime":"glibc@2.39","envs":[
      {"binding":"a@1","decls":[{"var":"V","op":"prepend","value":"/one"}]},
      {"binding":"b@1","decls":[{"var":"V","op":"set","value":"/two"}]}]}})");
    auto env = su::resolve_env(su::read(t.dir), t.dir);
    ASSERT_EQ(env.size(), 1u);
    EXPECT_EQ(env[0].second, "/two");
}

// A subos made before xlings grew the block. Degrade, and SAY SO — this is
// the common case on an existing machine, and mcpp's own sandbox subos was
// measured in exactly this state.
TEST(SubosInfo, MissingBlockDegradesWithANote) {
    Tmp t;
    t.write(R"({"workspace":{}})");
    auto info = su::read(t.dir);
    EXPECT_FALSE(info.present);
    EXPECT_TRUE(info.runtime.empty());
    EXPECT_FALSE(info.note.empty());
    EXPECT_TRUE(su::resolve_env(info, t.dir).empty());
}

// A schema newer than we understand: read what we can, and say we are behind.
// Refusing outright would make a newer xlings break an older mcpp, which is
// the failure mode the index-floor incident already paid for once.
TEST(SubosInfo, NewerSchemaIsUsedAndAnnounced) {
    Tmp t;
    t.write(R"({"subos_info":{"schema_version":99,"runtime":"glibc@2.44","envs":[]}})");
    auto info = su::read(t.dir);
    EXPECT_TRUE(info.present);
    EXPECT_EQ(info.runtime, "glibc@2.44");
    EXPECT_FALSE(info.note.empty());
}

TEST(SubosInfo, NoFileAtAllIsNotACrash) {
    Tmp t;   // nothing written
    auto info = su::read(t.dir);
    EXPECT_FALSE(info.present);
    EXPECT_FALSE(info.note.empty());
}

TEST(SubosInfo, MalformedJsonDegrades) {
    Tmp t;
    t.write("{ this is not json");
    auto info = su::read(t.dir);
    EXPECT_FALSE(info.present);
    EXPECT_FALSE(info.note.empty());
}

// The family mapping is part of the cross-repo contract: xlings derives it
// from the runtime string too, and the two must agree or a subos and a build
// will disagree about what ABI they are talking about.
TEST(SubosInfo, FamilyOfMirrorsXlings) {
    EXPECT_EQ(su::family_of("glibc@2.39"), "linux-x86_64-glibc");
    EXPECT_EQ(su::family_of("musl@1.2.5"), "linux-x86_64-musl");
    EXPECT_EQ(su::family_of("glibc@2.39", "aarch64"), "linux-aarch64-glibc");
    EXPECT_EQ(su::family_of("wasi-libc@1"), "wasm32-wasi");
    EXPECT_EQ(su::family_of("macos_sdk@14.0", "arm64"), "darwin-arm64");
    EXPECT_EQ(su::family_of("ucrt@10"), "windows-x86_64-ucrt");
    EXPECT_EQ(su::family_of("nonsense@1"), "unknown");
    // No '@' at all is not a binding; it must not be read as one.
    EXPECT_EQ(su::family_of("glibc"), "linux-x86_64-glibc");
}

}  // namespace

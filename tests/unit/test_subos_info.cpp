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
        "envs": {
          "mesa@25.0.7.1": [
            { "var": "LIBGL_DRIVERS_PATH", "op": "prepend",
              "value": "${subosdir}/usr/lib/dri" },
            { "var": "XDG_DATA_DIRS", "op": "prepend",
              "value": "${subosdir}/share" }
          ]
        }
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
      "envs":{"mesa@1":[
        {"var":"LIBGL_DRIVERS_PATH","op":"prepend","value":"${subosdir}/usr/lib/dri"}]}}})");
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
    t.write(R"({"subos_info":{"schema_version":1,"runtime":"glibc@2.39","envs":{
      "a-mesa@1":[{"var":"V","op":"prepend","value":"${subosdir}/one"}],
      "b-vendor@1":[{"var":"V","op":"prepend","value":"${subosdir}/two"}]}}})");
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
    t.write(R"({"subos_info":{"schema_version":1,"runtime":"glibc@2.39","envs":{
      "a@1":[{"var":"V","op":"prepend","value":"${subosdir}/x"}],
      "b@1":[{"var":"V","op":"prepend","value":"${subosdir}/x"}]}}})");
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
    t.write(R"({"subos_info":{"schema_version":1,"runtime":"glibc@2.39","envs":{
      "a@1":[{"var":"V","op":"prepend","value":"/one"}],
      "b@1":[{"var":"V","op":"set","value":"/two"}]}}})");
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


// A VERBATIM capture of what a real xlings wrote, after `xlings install
// graphics` on an NVIDIA host. Reformatted for width and nothing else -- keys,
// nesting and spelling are as found on disk.
//
// This test exists because its absence shipped a broken feature. The first
// version of this file hand-wrote every fixture in a shape the reader also
// expected and xlings never produces: `envs` as an array of {binding, decls}.
// Ten tests passed against a format that does not exist, and against a real
// subos the released build applied no variables at all -- silently, because
// "no providers" and "nothing declared" look identical.
//
// A fixture composed from the same understanding as the parser cannot catch
// that. Only one taken from the writer can.
TEST(SubosInfo, RealXlingsCapture) {
    Tmp t;
    t.write(R"({
      "subos_info": {
        "created_at": "2026-08-08T01:40:00Z",
        "created_by": "xlings 2026.8.7.1",
        "runtime": "glibc@2.39",
        "schema_version": 1,
        "envs": {
          "mesa@25.0.7.1": [
            {"op": "prepend", "value": "${subosdir}/usr/lib/dri", "var": "LIBGL_DRIVERS_PATH"},
            {"op": "prepend", "value": "${subosdir}/share/glvnd/egl_vendor.d", "var": "__EGL_VENDOR_LIBRARY_DIRS"},
            {"op": "prepend", "value": "${subosdir}/share", "var": "XDG_DATA_DIRS"}
          ],
          "nvidia-gl-host-link@0.1.1": [
            {"op": "prepend", "value": "${subosdir}/share/glvnd/egl_vendor.d", "var": "__EGL_VENDOR_LIBRARY_DIRS"}
          ]
        }
      },
      "workspace": {}
    })");

    auto info = su::read(t.dir);
    ASSERT_TRUE(info.present);
    EXPECT_EQ(info.runtime, "glibc@2.39");
    ASSERT_EQ(info.providers.size(), 2u);
    EXPECT_EQ(info.providers[0].binding, "mesa@25.0.7.1");
    EXPECT_EQ(info.providers[1].binding, "nvidia-gl-host-link@0.1.1");

    auto env = su::resolve_env(info, t.dir);
    ASSERT_EQ(env.size(), 3u) << "all three graphics variables must be produced";

    std::map<std::string, std::string> byVar;
    for (auto& [k, v] : env) byVar[k] = v;
    const auto sep = mcpp::platform::env::path_list_separator();
    EXPECT_EQ(byVar["LIBGL_DRIVERS_PATH"], t.dir.string() + "/usr/lib/dri");
    EXPECT_EQ(byVar["XDG_DATA_DIRS"],      t.dir.string() + "/share");
    // Both providers name the same vendor directory; de-duplication must
    // leave exactly one, or libglvnd sees it twice and enumerates the device
    // twice -- which is a defect xlings hit on its own side.
    EXPECT_EQ(byVar["__EGL_VENDOR_LIBRARY_DIRS"],
              t.dir.string() + "/share/glvnd/egl_vendor.d");
    EXPECT_EQ(byVar["__EGL_VENDOR_LIBRARY_DIRS"].find(sep), std::string::npos);
}

// xlings drops a declaration whose op it does not recognise. A reader more
// permissive than its writer eventually applies something the writer meant to
// reject, so this asserts the same refusal rather than a tolerant guess.
TEST(SubosInfo, UnknownOpIsDroppedLikeXlingsDrops) {
    Tmp t;
    t.write(R"({"subos_info":{"schema_version":1,"runtime":"glibc@2.39","envs":{
      "a@1":[{"var":"V","op":"append","value":"/nope"},
             {"var":"W","op":"prepend","value":"/yes"},
             {"var":"","op":"prepend","value":"/no-name"}]}}})");
    auto env = su::resolve_env(su::read(t.dir), t.dir);
    ASSERT_EQ(env.size(), 1u);
    EXPECT_EQ(env[0].first, "W");
    EXPECT_EQ(env[0].second, "/yes");
}




// `prepend` prepends TO the caller's value rather than replacing it. Same
// reason: the pair replaces the variable, so emitting the declared value alone
// discards whatever search path the user had.
TEST(SubosResolveEnv, PrependKeepsTheExportedValue) {
    Tmp t;
    t.write(R"({"workspace":{},"subos_info":{"schema_version":1,"runtime":"glibc@2.39",
        "envs":{"glibc@2.39":[{"var":"LD_LIBRARY_PATH","op":"prepend","value":"/sub/lib"}]}}})");
    auto info = su::read(t.dir);
    auto out = su::resolve_env(
        info, t.dir, [](std::string_view v) -> std::optional<std::string> {
            if (v == "LD_LIBRARY_PATH") return std::string("/user/lib");
            return std::nullopt;
        });
    ASSERT_EQ(out.size(), 1u);
    const auto sep = mcpp::platform::env::path_list_separator();
    EXPECT_EQ(out[0].second, std::string("/sub/lib") + sep + "/user/lib");
}

// Already there: prepending again would grow the list on every nested run.
TEST(SubosResolveEnv, PrependIsIdempotentAgainstTheExportedValue) {
    Tmp t;
    t.write(R"({"workspace":{},"subos_info":{"schema_version":1,"runtime":"glibc@2.39",
        "envs":{"glibc@2.39":[{"var":"LD_LIBRARY_PATH","op":"prepend","value":"/sub/lib"}]}}})");
    auto info = su::read(t.dir);
    const auto sep = mcpp::platform::env::path_list_separator();
    const auto existing = std::string("/sub/lib") + sep + "/user/lib";
    auto out = su::resolve_env(
        info, t.dir, [&](std::string_view v) -> std::optional<std::string> {
            if (v == "LD_LIBRARY_PATH") return existing;
            return std::nullopt;
        });
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].second, existing);
}

// `set` wins over an exported value, and that is deliberate.
//
// This assertion exists to stop the opposite reading from being reintroduced
// -- it was, once, as a fix for mcpp#382, and withdrawn: `set` and "a default
// the user may override" are two intentions, and a subos needs the first for
// variables naming its own configuration. The escape hatch that issue wants is
// a NEW op from xlings, whose wire format this is, not a second meaning for
// this one applied by one consumer.
TEST(SubosResolveEnv, SetWinsOverAnExportedValue) {
    Tmp t;
    t.write(R"({"workspace":{},"subos_info":{"schema_version":1,
        "runtime":"glibc@2.39",
        "envs":{"glibc@2.39":[{"var":"GALLIUM_DRIVER","op":"set","value":"d3d12"}]}}})");
    auto info = su::read(t.dir);
    auto out = su::resolve_env(
        info, t.dir, [](std::string_view v) -> std::optional<std::string> {
            if (v == "GALLIUM_DRIVER") return std::string("llvmpipe");
            return std::nullopt;
        });
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].second, "d3d12");
}

}  // namespace

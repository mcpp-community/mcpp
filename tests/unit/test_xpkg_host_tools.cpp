#include <gtest/gtest.h>

import std;
import mcpp.manifest;
import mcpp.manifest.xpkg;
import mcpp.platform;
import mcpp.platform.axis;

// #355 needed two things from the Form B (xpkg .lua) descriptor parser that it
// simply did not read, and neither failed loudly:
//
//   * `targets.<x>.required_features` was dropped, so a descriptor could not
//     express the cost gate that makes an optional host tool affordable —
//     compat.protobuf's `protoc` pulls in libprotoc's ~157 extra TUs, which the
//     consumers who only want the runtime must never compile. Without the gate
//     the target is either always built or never available.
//   * `deps` values could only be a version STRING, so a Form B descriptor had
//     no syntax at all for requesting a tool from one of its own dependencies.
//
// Both were silent: an unread key is just an absent feature. These tests are
// what makes them loud.

namespace {

mcpp::manifest::Manifest parse_or_fail(std::string_view lua) {
    auto m = mcpp::manifest::synthesize_from_xpkg_lua(
        lua, "compat.demo", "1.0.0", mcpp::platform::HostPlatform::current());
    EXPECT_TRUE(m.has_value()) << (m ? "" : m.error().message);
    return m.value_or(mcpp::manifest::Manifest{});
}

const mcpp::manifest::Target* find_target(const mcpp::manifest::Manifest& m,
                                          std::string_view name) {
    for (auto const& t : m.targets)
        if (t.name == name) return &t;
    return nullptr;
}

}  // namespace

TEST(XpkgHostTools, TargetsCarryRequiredFeatures) {
    auto m = parse_or_fail(R"LUA(
package = {
    spec = "1", name = "demo", namespace = "compat", type = "package",
    mcpp = {
        sources = { "*/src/**.cc" },
        targets = {
            ["demo"]   = { kind = "lib" },
            ["protoc"] = { kind = "bin", main = "src/compiler/main.cc",
                           required_features = { "protoc", "upb" } },
        },
    },
}
)LUA");

    auto const* lib = find_target(m, "demo");
    ASSERT_NE(lib, nullptr);
    EXPECT_EQ(lib->kind, mcpp::manifest::Target::Library);
    EXPECT_TRUE(lib->requiredFeatures.empty());

    auto const* tool = find_target(m, "protoc");
    ASSERT_NE(tool, nullptr);
    EXPECT_EQ(tool->kind, mcpp::manifest::Target::Binary);
    EXPECT_EQ(tool->main, "src/compiler/main.cc");
    EXPECT_EQ(tool->requiredFeatures,
              (std::vector<std::string>{"protoc", "upb"}));
}

TEST(XpkgHostTools, DepsAcceptBothAStringAndATable) {
    // The string form is the long-standing one and must keep working
    // unchanged; the table form is what lets a descriptor request a tool.
    auto m = parse_or_fail(R"LUA(
package = {
    spec = "1", name = "demo", namespace = "compat", type = "package",
    mcpp = {
        sources = { "*/src/**.cc" },
        deps = {
            ["compat.zlib"]     = "1.3.2",
            ["compat.protobuf"] = { version = "35.1", tools = { "protoc" } },
        },
    },
}
)LUA");

    const mcpp::manifest::DependencySpec* zlib = nullptr;
    const mcpp::manifest::DependencySpec* pb   = nullptr;
    for (auto const& [k, spec] : m.dependencies) {
        if (k.find("zlib") != std::string::npos)     zlib = &spec;
        if (k.find("protobuf") != std::string::npos) pb   = &spec;
    }

    ASSERT_NE(zlib, nullptr);
    EXPECT_EQ(zlib->version, "1.3.2");
    EXPECT_TRUE(zlib->tools.empty());

    ASSERT_NE(pb, nullptr);
    EXPECT_EQ(pb->version, "35.1");
    EXPECT_EQ(pb->tools, (std::vector<std::string>{"protoc"}));
}

TEST(XpkgHostTools, UnknownDepKeyIsRecordedRatherThanSwallowed) {
    // A descriptor author writing an unsupported key must be told. Silently
    // ignoring it leaves the dependency half-configured with no signal — the
    // exact failure the per-feature key recording already exists to prevent.
    auto m = parse_or_fail(R"LUA(
package = {
    spec = "1", name = "demo", namespace = "compat", type = "package",
    mcpp = {
        sources = { "*/src/**.cc" },
        deps = {
            ["compat.protobuf"] = { version = "35.1", no_such_key = "x" },
        },
    },
}
)LUA");

    bool recorded = false;
    for (auto const& k : m.xpkgUnknownKeys)
        if (k.find("no_such_key") != std::string::npos) recorded = true;
    EXPECT_TRUE(recorded) << "unknown dep key was swallowed";

    // ...and the keys it DOES understand still take effect.
    for (auto const& [k, spec] : m.dependencies)
        if (k.find("protobuf") != std::string::npos)
            EXPECT_EQ(spec.version, "35.1");
}

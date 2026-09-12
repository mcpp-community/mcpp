#include <gtest/gtest.h>

import std;
import mcpp.manifest;
import mcpp.build.prepare;

namespace cfgpred = mcpp::build::cfgpred;

// `[target.<sel>.runtime] frameworks` (#622 A2). The one Mach-O key that
// names a platform library is the one that must differ between macOS and
// iOS, because `AppKit` is absent from the iOS SDK and `UIKit` from the macOS
// one. The merge appends a matching selector's list after the top-level one,
// as it does for `libraries`; rendering stays Mach-O only, which `flags`
// already guarantees for the merged list.
namespace {

constexpr const char* kSrc = R"(
[package]
name    = "fw"
version = "0.1.0"

[runtime]
frameworks = ["Foundation"]

[target.macos.runtime]
frameworks = ["AppKit"]

[target.'cfg(os = "ios")'.runtime]
frameworks = ["UIKit"]
)";

std::vector<std::string> frameworks_for(std::string_view triple) {
    auto m = mcpp::manifest::parse_string(kSrc);
    EXPECT_TRUE(m.has_value());
    if (!m) return {};
    mcpp::build::merge_conditional_config(*m, cfgpred::context_for(triple));
    return m->runtimeConfig.linkIntent.frameworks;
}

} // namespace

TEST(TargetRuntimeFrameworks, TheSelectorDecidesAndTheTopLevelStays) {
    const std::vector<std::string> mac{"Foundation", "AppKit"};
    const std::vector<std::string> ios{"Foundation", "UIKit"};
    const std::vector<std::string> lin{"Foundation"};
    EXPECT_EQ(frameworks_for("aarch64-macos"), mac);
    EXPECT_EQ(frameworks_for("aarch64-ios-sim"), ios);
    EXPECT_EQ(frameworks_for("x86_64-linux-gnu"), lin);
}

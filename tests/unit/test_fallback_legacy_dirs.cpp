#include <gtest/gtest.h>

import std;
import mcpp.fallback.legacy_dirs;

// The legacy install-dir scan must not cross namespaces.
//
// `Fetcher::install_path` contract is "the verdir for THIS package". Its
// last-resort scan used to match any directory ending in `-x-<shortName>`,
// regardless of which namespace that directory belongs to. A lookup for
// `ocornut:imgui` therefore returned `compat-x-imgui` — a different package
// that merely shares a short name — and the caller then (a) skipped the
// install because the package "already exists" and (b) read the other
// package's tree.
//
// It was unreachable while the two carried unrelated versions
// (`compat:imgui@1.92.8` vs `mcpplibs:imgui@0.0.6`), because install_path also
// matches on version. Aligning package versions to upstream (mcpp-index#163)
// makes them coincide.

namespace {

std::filesystem::path make_xpkgs(std::initializer_list<std::string_view> dirs) {
    auto base = std::filesystem::temp_directory_path()
              / std::format("legacy-dirs-{}",
                            std::chrono::steady_clock::now()
                                .time_since_epoch().count());
    for (auto d : dirs) std::filesystem::create_directories(base / std::string(d));
    return base;
}

} // namespace

TEST(FallbackLegacyDirs, DoesNotReturnAnotherNamespacesDirectory) {
    auto base = make_xpkgs({"compat-x-imgui"});
    // Asking for ocornut:imgui. Only compat's directory exists, and it is NOT
    // an answer to this question.
    auto hit = mcpp::fallback::scan_legacy_install_dirs(
        base, "ocornut.imgui", "imgui", {"ocornut", "mcpplibs"});
    EXPECT_FALSE(hit.has_value())
        << "returned '" << hit.value_or("") << "' for ocornut:imgui";
    std::filesystem::remove_all(base);
}

TEST(FallbackLegacyDirs, StillFindsOwnNamespaceShortNameLayout) {
    auto base = make_xpkgs({"compat-x-imgui", "ocornut-x-imgui"});
    auto hit = mcpp::fallback::scan_legacy_install_dirs(
        base, "ocornut.imgui", "imgui", {"ocornut", "mcpplibs"});
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(*hit, "ocornut-x-imgui");
    std::filesystem::remove_all(base);
}

TEST(FallbackLegacyDirs, StillFindsIndexPrefixedLegacyLayout) {
    // The old layout this arm exists for: <index>-x-<shortName>.
    auto base = make_xpkgs({"mcpplibs-x-tinyhttps"});
    auto hit = mcpp::fallback::scan_legacy_install_dirs(
        base, "mcpplibs.tinyhttps", "tinyhttps", {"mcpplibs", "mcpplibs"});
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(*hit, "mcpplibs-x-tinyhttps");
    std::filesystem::remove_all(base);
}

TEST(FallbackLegacyDirs, QualifiedSuffixNeedsNoPrefixAllowance) {
    // `-x-<ns>.<name>` carries the namespace in the suffix itself, so it is
    // accepted whatever the prefix is — that is the legacy FQN layout.
    auto base = make_xpkgs({"someindex-x-ocornut.imgui"});
    auto hit = mcpp::fallback::scan_legacy_install_dirs(
        base, "ocornut.imgui", "imgui", {"ocornut"});
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(*hit, "someindex-x-ocornut.imgui");
    std::filesystem::remove_all(base);
}

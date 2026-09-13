#include <gtest/gtest.h>

import std;
import mcpp.manifest;
import mcpp.modgraph.graph;
import mcpp.platform.env;
import mcpp.publish.xpkg_emit;
import mcpp.diag;

using namespace mcpp::publish;

namespace {

mcpp::manifest::Manifest minimal_manifest() {
    mcpp::manifest::Manifest m;
    m.package.name        = "mcpplibs.hello";
    m.package.version     = "0.1.0";
    m.package.description = "Modular C++23 hello";
    m.package.license     = "Apache-2.0";
    m.package.repo        = "https://github.com/mcpplibs/hello";
    m.language.standard   = "c++23";
    m.language.importStd = true;
    m.dependencies["mcpplibs.primitives"] = mcpp::manifest::DependencySpec{ .version = "0.0.1", .path = "" };
    return m;
}

mcpp::modgraph::Graph minimal_graph() {
    mcpp::modgraph::Graph g;
    mcpp::modgraph::SourceUnit u;
    u.path = "/x/hello.cppm";
    u.provides = mcpp::modgraph::ModuleId{"mcpplibs.hello"};
    g.units.push_back(u);
    return g;
}

} // namespace

TEST(XpkgEmit, ContainsRequiredFields) {
    auto m   = minimal_manifest();
    auto g   = minimal_graph();
    auto rel = placeholder_release("0.1.0");

    auto out = emit_xpkg(m, g, rel);

    EXPECT_NE(out.find("AUTO-GENERATED"),               std::string::npos);
    EXPECT_NE(out.find("name = \"mcpplibs.hello\""),    std::string::npos);
    EXPECT_NE(out.find("import_std = true"),            std::string::npos);
    EXPECT_NE(out.find("\"mcpplibs.hello\""),           std::string::npos);   // module
    EXPECT_NE(out.find("manifest = \"mcpp.toml\""),     std::string::npos);
    EXPECT_NE(out.find("[\"mcpplibs.primitives\"]"),    std::string::npos);   // dep
}

TEST(XpkgEmit, RejectsLuaInjection) {
    auto m = minimal_manifest();
    m.package.description = "evil\"injection";
    auto g = minimal_graph();
    auto out = emit_xpkg(m, g, placeholder_release("0.1.0"));

    // The escaped form must be present, NOT the raw quote.
    EXPECT_NE(out.find("evil\\\"injection"), std::string::npos);
}

TEST(XpkgEmit, EscapesControlCharacters) {
    auto m = minimal_manifest();
    auto g = minimal_graph();

    // Description with backslash, CR, NUL, BEL, DEL — all must be escaped.
    m.package.description = std::string("a\\b\rc") + std::string(1, '\0')
                          + std::string("d\x07") + std::string(1, '\x7f') + "e";
    auto out = emit_xpkg(m, g, placeholder_release("0.1.0"));

    // Backslash → doubled
    EXPECT_NE(out.find("a\\\\b"), std::string::npos);
    // \r → \r escape
    EXPECT_NE(out.find("\\rc"), std::string::npos);
    // NUL → \0
    EXPECT_NE(out.find("\\0"), std::string::npos);
    // 0x07 (BEL) → \x07
    EXPECT_NE(out.find("\\x07"), std::string::npos);
    // 0x7f (DEL) → \x7f
    EXPECT_NE(out.find("\\x7f"), std::string::npos);
    // No literal control bytes leak through
    EXPECT_EQ(out.find('\r'), std::string::npos);
    EXPECT_EQ(out.find('\0'), std::string::npos);
    EXPECT_EQ(out.find('\x07'), std::string::npos);
}

TEST(XpkgEmit, ReleaseTarballUrl) {
    using namespace mcpp::publish;
    EXPECT_EQ(release_tarball_url("https://github.com/foo/bar",   "bar", "0.1.0"),
              "https://github.com/foo/bar/releases/download/v0.1.0/bar-0.1.0.tar.gz");
    // .git suffix gets stripped
    EXPECT_EQ(release_tarball_url("https://github.com/foo/bar.git", "bar", "0.1.0"),
              "https://github.com/foo/bar/releases/download/v0.1.0/bar-0.1.0.tar.gz");
    // empty repo → empty URL (caller errors out)
    EXPECT_EQ(release_tarball_url("", "bar", "0.1.0"), "");
    // non-http(s) repo URL → empty (don't trust ssh:// / git@ for downloads)
    EXPECT_EQ(release_tarball_url("git@github.com:foo/bar.git", "bar", "0.1.0"), "");
}

#if !defined(_WIN32)
// sha256_of_file uses sha256sum which is not available on Windows
TEST(XpkgEmit, Sha256OfFile) {
    using namespace mcpp::publish;

    // Empty file → known SHA-256
    auto p = std::filesystem::temp_directory_path() / "mcpp_unit_sha_empty";
    { std::ofstream(p).close(); }
    EXPECT_EQ(sha256_of_file(p),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    std::filesystem::remove(p);

    // Known content
    auto p2 = std::filesystem::temp_directory_path() / "mcpp_unit_sha_abc";
    { std::ofstream os(p2); os << "abc"; }
    EXPECT_EQ(sha256_of_file(p2),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    std::filesystem::remove(p2);

    // Non-existent file → empty string
    EXPECT_EQ(sha256_of_file("/no/such/path/zzz"), "");
}
#endif // !defined(_WIN32)

#if defined(__linux__)
TEST(XpkgEmit, Sha256OfFileIgnoresTargetRuntimeLibraryPath) {
    using namespace mcpp::publish;

    auto hostile = std::filesystem::temp_directory_path()
                 / std::format("mcpp_hostile_ld_{}", std::random_device{}());
    std::filesystem::create_directories(hostile);
    { std::ofstream(hostile / "libc.so.6").close(); }

    auto p = std::filesystem::temp_directory_path()
           / std::format("mcpp_unit_sha_hostile_{}", std::random_device{}());
    { std::ofstream(p).close(); }

    {
        mcpp::platform::env::ScopedEnv ld("LD_LIBRARY_PATH", hostile.string());
        EXPECT_EQ(sha256_of_file(p),
                  "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    }

    std::error_code ec;
    std::filesystem::remove(p, ec);
    std::filesystem::remove_all(hostile, ec);
}
#endif // defined(__linux__)

TEST(XpkgEmit, LongBracketSequenceInValueIsHarmless) {
    // We emit `"..."` strings, not `[[...]]`, so a literal `]=]` in
    // user data can't terminate the literal early. Just make sure it
    // round-trips as escaped content (the `]`/`=` chars are printable
    // ASCII and pass through; what matters is the surrounding quotes
    // are intact and no escape is needed for them).
    auto m = minimal_manifest();
    auto g = minimal_graph();
    m.package.description = "trick: ]==] more stuff";
    auto out = emit_xpkg(m, g, placeholder_release("0.1.0"));
    EXPECT_NE(out.find("\"trick: ]==] more stuff\""), std::string::npos);
}

// ── #630 item 7: an OS-only selector is a platform ──────────────────────────
//
// Design record 2026-09-13-630 §8.3's two criteria, both asserted here: an
// OS-only `[target.<selector>]` tool declaration names its block and raises
// no `publish/target-axis-tools` warning; a selector that is not OS-only
// names no block and still raises the warning (the residual case, so the
// advisory is not dead code once the OS-only path exists).

namespace {

// The three platform blocks are rendered back to back, each opened by its
// own header line and closed by "        },\n" before the next one starts —
// see `mcpp::pm::emit_xpkg`. Slicing out one block lets a test ask "does
// THIS platform name the address" without the substring also matching a
// different block that happens to share indentation.
std::string block(const std::string& out, std::string_view header) {
    auto p = out.find(header);
    if (p == std::string::npos) return {};
    auto end = out.find("        },\n", p);
    return out.substr(p, (end == std::string::npos ? out.size() : end) - p);
}

}  // namespace

TEST(XpkgEmit, OsOnlySelectorNamesItsPlatformBlockAndSuppressesTheWarning) {
    constexpr const char* src = R"(
[package]
name    = "gtkapp"
version = "0.1.0"

[target.'cfg(linux)'.xlings.workspace]
"xim:gtk4" = ""
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value()) << (m ? std::string{} : m.error().format());
    auto g = minimal_graph();

    mcpp::diag::reset();
    auto out = emit_xpkg(*m, g, placeholder_release("0.1.0"));

    EXPECT_NE(block(out, "linux   = {").find("xim:gtk4"), std::string::npos)
        << out;
    EXPECT_EQ(block(out, "macosx  = {").find("xim:gtk4"), std::string::npos);
    EXPECT_EQ(block(out, "windows = {").find("xim:gtk4"), std::string::npos);

    // No `publish/target-axis-tools` advisory for THIS section: an OS-only
    // selector is emitted, so there is nothing left to warn about.
    EXPECT_EQ(mcpp::diag::count(mcpp::diag::Severity::Warning), 0u);
    EXPECT_TRUE(mcpp::diag::records().empty());
}

TEST(XpkgEmit, NonOsSelectorNamesNoBlockAndStillWarns) {
    constexpr const char* src = R"(
[package]
name    = "gtkapp"
version = "0.1.0"

[target.'cfg(target_arch = "aarch64")'.xlings.workspace]
"xim:gtk4" = ""
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value()) << (m ? std::string{} : m.error().format());
    auto g = minimal_graph();

    mcpp::diag::reset();
    auto out = emit_xpkg(*m, g, placeholder_release("0.1.0"));

    EXPECT_EQ(block(out, "linux   = {").find("xim:gtk4"), std::string::npos);
    EXPECT_EQ(block(out, "macosx  = {").find("xim:gtk4"), std::string::npos);
    EXPECT_EQ(block(out, "windows = {").find("xim:gtk4"), std::string::npos);

    // The residual case: the advisory still fires, and its text now also
    // states that an OS-only selector WOULD have been emitted.
    ASSERT_EQ(mcpp::diag::count(mcpp::diag::Severity::Warning), 1u);
    auto recs = mcpp::diag::records();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_EQ(recs.front().domain, "publish/target-axis-tools");
    EXPECT_NE(recs.front().what.find("xim:gtk4"), std::string::npos);
    EXPECT_NE(recs.front().what.find("OS-only selector"), std::string::npos)
        << recs.front().what;
}

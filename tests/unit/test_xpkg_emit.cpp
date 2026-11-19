
import std;
import mcpplibs.mtest;
import mcpp.manifest;
import mcpp.pm.descriptor;
import mcpp.toolchain.platform;
import mcpp.invocation;
import mcpp.publish.xpkg_emit;
import mcpp.diag;

using namespace mcpplibs::mtest;

using namespace mcpp::publish;

namespace {

mcpp::manifest::Manifest minimal_manifest() {
    mcpp::manifest::Manifest m;
    m.package.namespace_  = "mcpplibs";
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

} // namespace

UTest contains_required_fields{"XpkgEmit/ContainsRequiredFields", [] {
    auto m   = minimal_manifest();
    auto rel = placeholder_release("0.1.0");

    auto out = emit_xpkg(m, rel);

    expect(ne(out.find("AUTO-GENERATED"), std::string::npos));
    expect(ne(out.find("    name = \"hello\""), std::string::npos));
    // The pointer form (SPEC-001 §3.5): where the manifest is, and nothing the
    // manifest states.
    expect(ne(out.find("    mcpp = \"*/mcpp.toml\",\n"), std::string::npos)) << out;
    expect(eq(out.find("manifest ="), std::string::npos)) << out;
    expect(eq(out.find("modules ="), std::string::npos)) << out;
    expect(eq(out.find("mcpplibs.primitives"), std::string::npos)) << out;
}};

// mcpp#960: what `mcpp publish` writes is what every reader reads. The
// descriptor goes back through the one function the build, `xpkg parse` and
// `mcpp new --template` read it with, over a payload laid out as the archive is.
UTest what_is_written_is_read{"XpkgEmit/WhatIsWrittenIsRead", [] {
    auto m = minimal_manifest();
    const auto out = emit_xpkg(m, placeholder_release("0.1.0"));
    const auto field = mcpp::manifest::extract_mcpp_field(out);
    expect(fatal(field.kind == mcpp::manifest::McppField::StringPath)) << out;
    expect(eq(field.value, std::string("*/mcpp.toml")));

    const auto versionDir = std::filesystem::temp_directory_path()
        / std::format("mcpp-xpkg-emit-{}", std::chrono::steady_clock::now().time_since_epoch().count());
    const auto top = versionDir / "hello-0.1.0";
    std::filesystem::create_directories(top);
    { std::ofstream(top / "mcpp.toml") << "[package]\nname = \"hello\"\nversion = \"0.1.0\"\n"; }
    const auto stated = mcpp::pm::descriptor_manifest(
        out, versionDir, "hello", "0.1.0", mcpp::toolchain::platform::HostPlatform::current());
    expect(fatal(stated.has_value())) << (stated ? std::string{} : stated.error());
    expect(stated->form == mcpp::pm::DescriptorManifest::Form::Pointer);
    expect(stated->manifestPath == top / "mcpp.toml") << stated->manifestPath.string();
    expect(stated->packageRoot == top) << stated->packageRoot.string();
    expect(!mcpp::pm::descriptor_is_inline(out));
    expect(mcpp::pm::descriptor_payload_present(
        out, versionDir, "hello", "0.1.0", mcpp::toolchain::platform::HostPlatform::current()));

    // The table `mcpp publish` wrote before: neither form, and the refusal
    // says which form a payload with a manifest takes.
    const std::string before = "package = { name = \"hello\", mcpp = { schema = \"0.1\", "
                               "language = \"c++23\", manifest = \"mcpp.toml\" } }";
    const auto refused = mcpp::pm::descriptor_manifest(
        before, versionDir, "hello", "0.1.0", mcpp::toolchain::platform::HostPlatform::current());
    expect(fatal(!refused.has_value()));
    expect(ne(refused.error().find("mcpp = \"*/mcpp.toml\""), std::string::npos)) << refused.error();

    std::error_code ec;
    std::filesystem::remove_all(versionDir, ec);
}};

UTest rejects_lua_injection{"XpkgEmit/RejectsLuaInjection", [] {
    auto m = minimal_manifest();
    m.package.description = "evil\"injection";
    auto out = emit_xpkg(m, placeholder_release("0.1.0"));

    // The escaped form must be present, NOT the raw quote.
    expect(ne(out.find("evil\\\"injection"), std::string::npos));
}};

UTest escapes_control_characters{"XpkgEmit/EscapesControlCharacters", [] {
    auto m = minimal_manifest();

    // Description with backslash, CR, NUL, BEL, DEL — all must be escaped.
    m.package.description = std::string("a\\b\rc") + std::string(1, '\0')
                          + std::string("d\x07") + std::string(1, '\x7f') + "e";
    auto out = emit_xpkg(m, placeholder_release("0.1.0"));

    // Backslash → doubled
    expect(ne(out.find("a\\\\b"), std::string::npos));
    // \r → \r escape
    expect(ne(out.find("\\rc"), std::string::npos));
    // NUL → \0
    expect(ne(out.find("\\0"), std::string::npos));
    // 0x07 (BEL) → \x07
    expect(ne(out.find("\\x07"), std::string::npos));
    // 0x7f (DEL) → \x7f
    expect(ne(out.find("\\x7f"), std::string::npos));
    // No literal control bytes leak through
    expect(eq(out.find('\r'), std::string::npos));
    expect(eq(out.find('\0'), std::string::npos));
    expect(eq(out.find('\x07'), std::string::npos));
}};

UTest xpkg_emit_release_tarball_url{"XpkgEmit/ReleaseTarballUrl", [] {
    using namespace mcpp::publish;
    expect(eq(release_tarball_url("https://github.com/foo/bar",   "bar", "0.1.0"), "https://github.com/foo/bar/releases/download/v0.1.0/bar-0.1.0.tar.gz"));
    // .git suffix gets stripped
    expect(eq(release_tarball_url("https://github.com/foo/bar.git", "bar", "0.1.0"), "https://github.com/foo/bar/releases/download/v0.1.0/bar-0.1.0.tar.gz"));
    // empty repo → empty URL (caller errors out)
    expect(eq(release_tarball_url("", "bar", "0.1.0"), ""));
    // non-http(s) repo URL → empty (don't trust ssh:// / git@ for downloads)
    expect(eq(release_tarball_url("git@github.com:foo/bar.git", "bar", "0.1.0"), ""));
}};

// mcpp#947: a release's tag is a template, an archive is named by the
// package's identity, and a descriptor is filed as the index files it.
UTest xpkg_emit_names_of_a_release{"XpkgEmit/TheNamesAPublishedPackageIsFiledUnder", [] {
    using namespace mcpp::publish;
    expect(eq(release_tarball_url("https://github.com/foo/ws", "mlibs.math", "0.1.0", "r0.1.0"),
              "https://github.com/foo/ws/releases/download/r0.1.0/mlibs.math-0.1.0.tar.gz"));
    expect(eq(archive_identity("mlibs", "math"), "mlibs.math"));
    expect(eq(archive_identity("mlibs", "mlibs.math"), "mlibs.math"));
    expect(eq(archive_identity("", "math"), "math"));
    expect(eq(descriptor_file_name("mlibs", "math"), "mlibs.math.lua"));
    expect(eq(descriptor_file_name("mcpplibs", "cmdline"), "cmdline.lua"));
    expect(eq(descriptor_file_name("mcpplibs.capi", "lua"), "mcpplibs.capi.lua.lua"));
    expect(eq(descriptor_file_name("", "opencv"), "opencv.lua"));
    expect(eq(*release_tag("", "mlibs", "math", "0.1.0"), "v0.1.0"));
    expect(eq(*release_tag("{namespace}-{name}-{version}", "mlibs", "math", "0.1.0"), "mlibs-math-0.1.0"));
    expect(!release_tag("v{versoin}", "mlibs", "math", "0.1.0").has_value());
    expect(!release_tag("{version", "mlibs", "math", "0.1.0").has_value());
    expect(!release_tag("a tag {version}", "mlibs", "math", "0.1.0").has_value());
}};

// The descriptor's name is one segment and its namespace carries the path
// (SPEC-001 §3.2), whichever of the two the manifest's name is written as.
UTest xpkg_emit_name_is_one_segment{"XpkgEmit/TheNameIsOneSegment", [] {
    auto m = minimal_manifest();
    m.package.namespace_ = "mlibs";
    m.package.name = "math";
    auto out = emit_xpkg(m, placeholder_release("0.1.0"));
    expect(ne(out.find("namespace = \"mlibs\""), std::string::npos)) << out;
    expect(ne(out.find("    name = \"math\""), std::string::npos)) << out;
    m.package.name = "mlibs.math";
    out = emit_xpkg(m, placeholder_release("0.1.0"));
    expect(ne(out.find("    name = \"math\""), std::string::npos)) << out;
}};

#if !defined(_WIN32)
// sha256_of_file uses sha256sum which is not available on Windows
UTest xpkg_emit_sha256_of_file{"XpkgEmit/Sha256OfFile", [] {
    using namespace mcpp::publish;

    // Empty file → known SHA-256
    auto p = std::filesystem::temp_directory_path() / "mcpp_unit_sha_empty";
    { std::ofstream(p).close(); }
    expect(eq(sha256_of_file(p), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    std::filesystem::remove(p);

    // Known content
    auto p2 = std::filesystem::temp_directory_path() / "mcpp_unit_sha_abc";
    { std::ofstream os(p2); os << "abc"; }
    expect(eq(sha256_of_file(p2), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    std::filesystem::remove(p2);

    // Non-existent file → empty string
    expect(eq(sha256_of_file("/no/such/path/zzz"), ""));
}};
#endif // !defined(_WIN32)

#if defined(__linux__)
UTest sha256_of_file_ignores_target_runtime_library_path{"XpkgEmit/Sha256OfFileIgnoresTargetRuntimeLibraryPath", [] {
    using namespace mcpp::publish;

    auto hostile = std::filesystem::temp_directory_path()
                 / std::format("mcpp_hostile_ld_{}", std::random_device{}());
    std::filesystem::create_directories(hostile);
    { std::ofstream(hostile / "libc.so.6").close(); }

    auto p = std::filesystem::temp_directory_path()
           / std::format("mcpp_unit_sha_hostile_{}", std::random_device{}());
    { std::ofstream(p).close(); }

    {
        mcpp::invocation::ScopedEnv ld("LD_LIBRARY_PATH", hostile.string());
        expect(eq(sha256_of_file(p), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    }

    std::error_code ec;
    std::filesystem::remove(p, ec);
    std::filesystem::remove_all(hostile, ec);
}};
#endif // defined(__linux__)

UTest long_bracket_sequence_in_value_is_harmless{"XpkgEmit/LongBracketSequenceInValueIsHarmless", [] {
    // We emit `"..."` strings, not `[[...]]`, so a literal `]=]` in
    // user data can't terminate the literal early. Just make sure it
    // round-trips as escaped content (the `]`/`=` chars are printable
    // ASCII and pass through; what matters is the surrounding quotes
    // are intact and no escape is needed for them).
    auto m = minimal_manifest();
    m.package.description = "trick: ]==] more stuff";
    auto out = emit_xpkg(m, placeholder_release("0.1.0"));
    expect(ne(out.find("\"trick: ]==] more stuff\""), std::string::npos));
}};

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

UTest os_only_selector_names_its_platform_block_and_suppresses_the_warning{"XpkgEmit/OsOnlySelectorNamesItsPlatformBlockAndSuppressesTheWarning", [] {
    constexpr const char* src = R"(
[package]
name    = "gtkapp"
version = "0.1.0"

[target.'cfg(linux)'.xlings.workspace]
"xim:gtk4" = ""
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << (m ? std::string{} : m.error().format());

    mcpp::diag::reset();
    auto out = emit_xpkg(*m, placeholder_release("0.1.0"));

    expect(ne(block(out, "linux   = {").find("xim:gtk4"), std::string::npos))
        << out;
    expect(eq(block(out, "macosx  = {").find("xim:gtk4"), std::string::npos));
    expect(eq(block(out, "windows = {").find("xim:gtk4"), std::string::npos));

    // No `publish/target-axis-tools` advisory for THIS section: an OS-only
    // selector is emitted, so there is nothing left to warn about.
    expect(eq(mcpp::diag::count(mcpp::diag::Severity::Warning), 0u));
    expect(mcpp::diag::records().empty());
}};

UTest non_os_selector_names_no_block_and_still_warns{"XpkgEmit/NonOsSelectorNamesNoBlockAndStillWarns", [] {
    constexpr const char* src = R"(
[package]
name    = "gtkapp"
version = "0.1.0"

[target.'cfg(target_arch = "aarch64")'.xlings.workspace]
"xim:gtk4" = ""
)";
    auto m = mcpp::manifest::parse_string(src);
    if (auto&& failed = expect(fatal(m.has_value())); !failed) failed << (m ? std::string{} : m.error().format());

    mcpp::diag::reset();
    auto out = emit_xpkg(*m, placeholder_release("0.1.0"));

    expect(eq(block(out, "linux   = {").find("xim:gtk4"), std::string::npos));
    expect(eq(block(out, "macosx  = {").find("xim:gtk4"), std::string::npos));
    expect(eq(block(out, "windows = {").find("xim:gtk4"), std::string::npos));

    // The residual case: the advisory still fires, and its text now also
    // states that an OS-only selector WOULD have been emitted.
    expect(fatal(eq(mcpp::diag::count(mcpp::diag::Severity::Warning), 1u)));
    auto recs = mcpp::diag::records();
    expect(fatal(eq(recs.size(), 1u)));
    expect(eq(recs.front().domain, "publish/target-axis-tools"));
    expect(ne(recs.front().what.find("xim:gtk4"), std::string::npos));
    expect(ne(recs.front().what.find("OS-only selector"), std::string::npos))
        << recs.front().what;
}};


import std;
import mcpplibs.mtest;
import mcpp.toolchain.platform;
import mcpp.toolchain.compat;

using namespace mcpplibs::mtest;

using namespace mcpp::toolchain::compat;

static std::string host_musl() {
    return std::string(mcpp::toolchain::platform::host_arch) + "-linux-musl";
}

// ── canonical spellings pass through unchanged ───────────────────────────────

UTest canonical_families_pass_through{"Compat/CanonicalFamiliesPassThrough", [] {
    auto g = normalize_spec("gcc", "16.1.0");
    expect(fatal(g.has_value()));
    expect(eq(g->family, "gcc"));
    expect(eq(g->version, "16.1.0"));
    expect(g->target.empty());
    expect(!g->changed);

    auto l = normalize_spec("llvm", "20.1.7");
    expect(fatal(l.has_value()));
    expect(eq(l->family, "llvm"));
    expect(!l->changed);

    auto m = normalize_spec("msvc", "system");
    expect(fatal(m.has_value()));
    expect(eq(m->family, "msvc"));
    expect(eq(m->version, "system"));
    expect(!m->changed);
}};

// ── §6.2 alias table: every legacy row round-trips ───────────────────────────

UTest musl_version_suffix_becomes_target{"Compat/MuslVersionSuffixBecomesTarget", [] {
    auto s = normalize_spec("gcc", "15.1.0-musl");
    expect(fatal(s.has_value()));
    expect(eq(s->family, "gcc"));
    expect(eq(s->version, "15.1.0"));
    expect(eq(s->target.str(), host_musl()));
    expect(bool(s->changed));
    expect(!(s->hint.empty()));
}};

UTest musl_gcc_compiler_name_becomes_target{"Compat/MuslGccCompilerNameBecomesTarget", [] {
    auto s = normalize_spec("musl-gcc", "15.1.0");
    expect(fatal(s.has_value()));
    expect(eq(s->family, "gcc"));
    expect(eq(s->version, "15.1.0"));
    expect(eq(s->target.str(), host_musl()));
    expect(bool(s->changed));
}};

UTest triple_named_gcc_becomes_target{"Compat/TripleNamedGccBecomesTarget", [] {
    auto s = normalize_spec("aarch64-linux-musl-gcc", "16.1.0");
    expect(fatal(s.has_value()));
    expect(eq(s->family, "gcc"));
    expect(eq(s->version, "16.1.0"));
    expect(eq(s->target.str(), "aarch64-linux-musl"));
    expect(bool(s->changed));
}};

UTest mingw_and_mingw_cross_collapse_to_windows_gnu{"Compat/MingwAndMingwCrossCollapseToWindowsGnu", [] {
    // One concept, two host-split legacy names → the SAME normalized identity.
    auto native = normalize_spec("mingw", "16.1.0");
    auto cross  = normalize_spec("mingw-cross", "16.1.0");
    expect(fatal(native.has_value()));
    expect(fatal(cross.has_value()));
    expect(eq(native->family, "gcc"));
    expect(eq(cross->family, "gcc"));
    expect(eq(native->target.str(), "x86_64-windows-gnu"));
    expect(eq(cross->target.str(), "x86_64-windows-gnu"));
    expect(bool(native->changed));
    expect(bool(cross->changed));
}};

UTest clang_alias_becomes_llvm{"Compat/ClangAliasBecomesLlvm", [] {
    auto s = normalize_spec("clang", "20.1.7");
    expect(fatal(s.has_value()));
    expect(eq(s->family, "llvm"));
    expect(eq(s->version, "20.1.7"));
    expect(bool(s->changed));
}};

UTest partial_musl_version_suffix{"Compat/PartialMuslVersionSuffix", [] {
    // "gcc 15-musl" — partial version with the legacy suffix.
    auto s = normalize_spec("gcc", "15-musl");
    expect(fatal(s.has_value()));
    expect(eq(s->version, "15"));
    expect(eq(s->target.str(), host_musl()));
}};

UTest xim_namespace_prefix_stripped{"Compat/XimNamespacePrefixStripped", [] {
    auto s = normalize_spec("xim:gcc", "16.1.0");
    expect(fatal(s.has_value()));
    expect(eq(s->family, "gcc"));
    expect(!s->changed);
}};

// ── rejects ─────────────────────────────────────────────────────────────────

UTest rejects_unknown_families{"Compat/RejectsUnknownFamilies", [] {
    expect(!(normalize_spec("tcc", "1.0").has_value()));
    expect(!(normalize_spec("llvm", "20.1.7-musl").has_value()));  // llvm has no musl flavor
    expect(!(normalize_spec("not-a-triple-gcc", "1.0").has_value()));
}};


import std;
import mcpplibs.mtest;
import mcpp.source_kind;
import mcpp.toolchain.model;
import mcpp.toolchain.detect;

using namespace mcpplibs::mtest;

using mcpp::SourceKind;

// ─── T-1. classify() ───────────────────────────────────────────────────────

UTest built_in_table_is_cppm_only{"SourceKind/BuiltInTableIsCppmOnly", [] {
    // Load-bearing, not conservatism. Widening the built-in set widens the
    // DEFAULT source glob with it, so a published package with a vendored
    // MSVC-only `.ixx` under src/ would start compiling it on the next mcpp
    // upgrade — a break its author cannot fix, because that version's tarball
    // has already shipped.
    auto t = mcpp::builtin_extension_table();
    expect(eq(t.moduleInterface, (std::vector<std::string>{".cppm"})));
    expect(eq(mcpp::classify("src/a.ixx", t), SourceKind::Other));
    expect(eq(mcpp::classify("src/a.ccm", t), SourceKind::Other));
    expect(eq(mcpp::classify("src/a.cxxm", t), SourceKind::Other));
}};

UTest classifies_every_built_in_role{"SourceKind/ClassifiesEveryBuiltInRole", [] {
    auto t = mcpp::builtin_extension_table();
    struct Row { const char* path; SourceKind want; };
    const Row rows[] = {
        {"src/a.cppm", SourceKind::ModuleInterface},
        {"src/a.cpp",  SourceKind::Cxx},
        {"src/a.cc",   SourceKind::Cxx},
        {"src/a.cxx",  SourceKind::Cxx},
        {"src/a.mm",   SourceKind::Cxx},
        {"src/a.c",    SourceKind::C},
        {"src/a.m",    SourceKind::C},
        {"src/a.S",    SourceKind::GasAsm},
        {"src/a.s",    SourceKind::GasAsm},
        {"src/a.asm",  SourceKind::NasmAsm},
        {"src/a.h",    SourceKind::Header},
        {"src/a.hpp",  SourceKind::Header},
        {"src/a.hh",   SourceKind::Header},
        {"src/a.hxx",  SourceKind::Header},
        {"src/a.txt",  SourceKind::Other},
        {"src/README", SourceKind::Other},
    };
    for (auto const& r : rows)
        expect(eq(mcpp::classify(r.path, t), r.want)) << r.path;
}};

UTest case_is_never_folded{"SourceKind/CaseIsNeverFolded", [] {
    // `.S` and `.s` are DIFFERENT LANGUAGES here — `.S` goes through the C
    // preprocessor, `.s` does not. Any normalization that lowercases would
    // silently merge them, so extensions are compared literally.
    auto t = mcpp::builtin_extension_table();
    expect(eq(mcpp::classify("a.S", t), SourceKind::GasAsm));
    expect(eq(mcpp::classify("a.s", t), SourceKind::GasAsm));
    expect(eq(mcpp::normalize_extension(".IXX"), ".IXX"));
    expect(eq(mcpp::classify("a.CPPM", t), SourceKind::Other));
}};

UTest configured_extensions_are_additive_and_normalized{"SourceKind/ConfiguredExtensionsAreAdditiveAndNormalized", [] {
    // `ixx` without a dot, a duplicate, and one already built in.
    auto t = mcpp::extension_table_for(
        std::vector<std::string>{"ixx", ".ccm", ".ccm", ".cppm", " .cxxm "});
    expect(eq(t.moduleInterface, (std::vector<std::string>{".cppm", ".ixx", ".ccm", ".cxxm"})));
    expect(eq(mcpp::classify("src/a.ixx", t), SourceKind::ModuleInterface));
    expect(eq(mcpp::classify("src/a.ccm", t), SourceKind::ModuleInterface));
    expect(eq(mcpp::classify("src/a.cxxm", t), SourceKind::ModuleInterface));
    // Built-in roles are untouched by the addition.
    expect(eq(mcpp::classify("src/a.cpp", t), SourceKind::Cxx));
    expect(eq(mcpp::classify("src/a.c", t), SourceKind::C));
}};

UTest reserved_extensions_are_rejected_not_silently_accepted{"SourceKind/ReservedExtensionsAreRejectedNotSilentlyAccepted", [] {
    // Declaring `.c` a module interface has no legitimate use and would route
    // C files to the C++ module rule, failing somewhere that names neither the
    // file nor the key. A hard error, not a warning.
    for (auto const* bad : {".cpp", ".cc", ".cxx", ".c", ".m", ".mm",
                            ".h", ".hpp", ".hh", ".hxx", ".S", ".s", ".asm"}) {
        auto err = mcpp::validate_module_extensions(std::vector<std::string>{bad});
        expect(fatal(err.has_value())) << bad;
        if (auto&& failed = expect(ne(err->find(bad), std::string::npos)); !failed) failed << *err;
    }
    // An unusual-but-free extension is fine: mcpp tells the compiler what the
    // unit is rather than relying on the driver to recognize the suffix.
    expect(!(mcpp::validate_module_extensions(
        std::vector<std::string>{".mpp", ".cppmi"}).has_value()));
}};

UTest validation_rejects_non_extension_shapes{"SourceKind/ValidationRejectsNonExtensionShapes", [] {
    for (auto const* bad : {"", "  ", ".", "src/*.ixx", "a/b.ixx",
                            "foo.bar.ixx"}) {
        expect(mcpp::validate_module_extensions(
            std::vector<std::string>{bad}).has_value()) << "accepted: " << bad;
    }
}};

UTest predicates_agree_with_the_kind{"SourceKind/PredicatesAgreeWithTheKind", [] {
    expect(bool(mcpp::produces_bmi(SourceKind::ModuleInterface)));
    expect(bool(mcpp::links_unconditionally(SourceKind::ModuleInterface)));
    for (auto k : {SourceKind::Cxx, SourceKind::C, SourceKind::GasAsm,
                   SourceKind::NasmAsm, SourceKind::Header, SourceKind::Other}) {
        expect(!mcpp::produces_bmi(k));
        expect(!mcpp::links_unconditionally(k));
    }

    // Scan-exempt: cannot contain import/module, so no P1689 scan.
    expect(mcpp::is_scan_exempt(SourceKind::C));
    expect(mcpp::is_scan_exempt(SourceKind::GasAsm));
    expect(mcpp::is_scan_exempt(SourceKind::NasmAsm));
    expect(!mcpp::is_scan_exempt(SourceKind::ModuleInterface));
    expect(!mcpp::is_scan_exempt(SourceKind::Cxx));

    // The fast path's question: could editing this change the graph's SHAPE?
    // Assembly is absent on purpose — it has no import and no scanned include
    // graph, so editing one changes its object (ninja tracks that) and nothing
    // else. A NEW assembly file is a different question, answered by
    // program_inputs_stale.
    expect(bool(mcpp::affects_graph_shape(SourceKind::ModuleInterface)));
    expect(bool(mcpp::affects_graph_shape(SourceKind::Cxx)));
    expect(bool(mcpp::affects_graph_shape(SourceKind::C)));
    expect(bool(mcpp::affects_graph_shape(SourceKind::Header)));
    expect(!mcpp::affects_graph_shape(SourceKind::GasAsm));
    expect(!mcpp::affects_graph_shape(SourceKind::Other));
}};

UTest default_globs_are_derived_from_the_table{"SourceKind/DefaultGlobsAreDerivedFromTheTable", [] {
    // Two hand-maintained copies of this list used to exist and had already
    // drifted apart (the staging fallback was missing all three assembly
    // extensions). Deriving them is what keeps a declared extension from being
    // classified but never FOUND.
    auto builtin = mcpp::default_source_globs(mcpp::builtin_extension_table());
    expect(eq(builtin, (std::vector<std::string>{
        "src/**/*.cppm", "src/**/*.cpp", "src/**/*.cc", "src/**/*.c",
        "src/**/*.S", "src/**/*.s", "src/**/*.asm"})));

    auto wide = mcpp::default_source_globs(
        mcpp::extension_table_for(std::vector<std::string>{".ixx"}));
    expect(eq(wide.front(), "src/**/*.cppm"));
    expect(ne(std::ranges::find(wide, "src/**/*.ixx"), wide.end()));

    expect(eq(mcpp::default_source_globs_note(mcpp::builtin_extension_table()), "sources [src/**/*.{cppm,cpp,cc,c,S,s,asm}]"));
}};

// ─── T-2. Object naming ────────────────────────────────────────────────────

UTest object_naming_is_monotone_and_collision_free{"SourceKind/ObjectNamingIsMonotoneAndCollisionFree", [] {
    // Historical names are FROZEN: an object's name is part of the internal
    // layout of a global cache entry, so renaming one without changing the
    // cache key produces a HIT on an entry that lacks the object the link then
    // asks for — a missing `.o` at link time, not a cache miss.
    expect(eq(mcpp::object_naming_for("foo.cpp"), mcpp::ObjectNaming::Stem));
    expect(eq(mcpp::object_naming_for("foo.cc"), mcpp::ObjectNaming::Stem));
    expect(eq(mcpp::object_naming_for("foo.c"), mcpp::ObjectNaming::Stem));
    expect(eq(mcpp::object_naming_for("foo.cppm"), mcpp::ObjectNaming::StemDotM));
    expect(eq(mcpp::object_naming_for("foo.S"), mcpp::ObjectNaming::FullFilename));
    expect(eq(mcpp::object_naming_for("foo.asm"), mcpp::ObjectNaming::FullFilename));

    // Everything a project can ADD gets the collision-proof form, so a new
    // extension can never change an existing object's name.
    for (auto const* ext : {"foo.ixx", "foo.ccm", "foo.cxxm", "foo.mpp"})
        expect(eq(mcpp::object_naming_for(ext), mcpp::ObjectNaming::FullFilename))
            << ext;
}};

UTest same_stem_across_module_extensions_never_collides{"SourceKind/SameStemAcrossModuleExtensionsNeverCollides", [] {
    // mcpp#272 proposed giving all four module extensions the `.m` prefix,
    // which makes `foo.cppm` and `foo.ccm` BOTH `foo.m.o`. The per-package
    // collision prefix cannot help: it mirrors the source DIRECTORY, and these
    // two are in the same one.
    auto name = [](std::string_view f) {
        switch (mcpp::object_naming_for(f)) {
            case mcpp::ObjectNaming::Stem:
                return std::filesystem::path(f).stem().string() + ".o";
            case mcpp::ObjectNaming::StemDotM:
                return std::filesystem::path(f).stem().string() + ".m.o";
            case mcpp::ObjectNaming::FullFilename:
                return std::filesystem::path(f).filename().string() + ".o";
        }
        return std::string{};
    };
    std::set<std::string> seen;
    for (auto const* f : {"foo.cppm", "foo.ccm", "foo.cxxm", "foo.ixx",
                          "foo.cpp", "foo.S", "foo.s", "foo.asm"}) {
        auto n = name(f);
        if (auto&& failed = expect(bool(seen.insert(n).second)); !failed) failed << "collision: " << f << " -> " << n;
    }

    // KNOWN GAP, deliberately pinned rather than asserted away: `foo.c` and
    // `foo.cpp` in one directory have always shared `foo.o`. Fixing it renames
    // every C object, which is exactly the cache-layout change described
    // above and needs a cache-key revision to be safe. Tracked separately —
    // this assertion documents the gap so nobody "fixes" the test instead.
    expect(eq(name("foo.c"), name("foo.cpp")))
        << "known gap closed? update this test and bump the cache key";
}};

// ─── T-8. The module-interface language flag ───────────────────────────────

UTest module_interface_lang_flag_is_per_compiler_and_not_interchangeable{"SourceKind/ModuleInterfaceLangFlagIsPerCompilerAndNotInterchangeable", [] {
    // Measured 2026-08-11: `-x c++-module` makes GCC exit with "language
    // c++-module not recognized", and `-x c++` makes Clang emit a 174-byte
    // stub instead of a module BMI. The spelling is a property of the compiler
    // FAMILY, not of the command dialect — gcc and clang share the gnu dialect.
    auto traits_for = [](mcpp::toolchain::CompilerId id) {
        mcpp::toolchain::Toolchain tc;
        tc.compiler = id;
        return mcpp::toolchain::bmi_traits(tc);
    };
    expect(eq(traits_for(mcpp::toolchain::CompilerId::GCC).moduleInterfaceLangFlag, " -x c++"));
    expect(eq(traits_for(mcpp::toolchain::CompilerId::Clang).moduleInterfaceLangFlag, " -x c++-module"));
    expect(eq(traits_for(mcpp::toolchain::CompilerId::MSVC).moduleInterfaceLangFlag, " /interface /TP"));

    // Never empty: mcpp tells the driver EVERY time rather than tracking which
    // suffix each driver version happens to know. That table would expire with
    // every compiler release, and getting it wrong is silent — Clang hands an
    // unrecognized suffix to the linker, warns, and exits 0 with no BMI.
    for (auto id : {mcpp::toolchain::CompilerId::GCC,
                    mcpp::toolchain::CompilerId::Clang,
                    mcpp::toolchain::CompilerId::MSVC})
        expect(!(traits_for(id).moduleInterfaceLangFlag.empty()));
}};

// ─── T-6. Device translation units ─────────────────────────────────────────
//
// A device TU is compiled by a vendor compiler (nvcc, hipcc) that mcpp does
// not drive directly, and no such compiler accepts C++20 modules. The kind
// therefore states the GRAPH ROLE — "not scanned, no BMI" — and says nothing
// about the language, which is what lets one kind cover CUDA C++, HIP, and
// dialects that are not C++ at all.

UTest classifies_device_translation_units{"SourceKind/ClassifiesDeviceTranslationUnits", [] {
    auto t = mcpp::builtin_extension_table();
    expect(eq(mcpp::classify("src/k.cu",  t), SourceKind::Device));
    expect(eq(mcpp::classify("src/k.hip", t), SourceKind::Device));
}};

UTest classifies_sycl_translation_units_by_compiler_not_dialect{"SourceKind/ClassifiesSyclTranslationUnitsByCompilerNotDialect", [] {
    // A `.sycl` unit is ordinary C++ and is still a device TU, because the
    // criterion is which compiler consumes it: a SYCL unit goes to icpx or a
    // clang with the SYCL front end, neither of which mcpp drives and neither
    // of which accepts C++20 modules. Naming it `.cpp` and routing it by glob
    // would make one extension mean two things depending on match order.
    auto t = mcpp::builtin_extension_table();
    expect(eq(mcpp::classify("src/kernels/saxpy.sycl", t), SourceKind::Device));
    expect(mcpp::is_scan_exempt(SourceKind::Device));
    // And the C++ spelling of the same content stays C++: adding the row must
    // not reclassify anything that is not literally named `.sycl`.
    expect(eq(mcpp::classify("src/kernels/saxpy.cpp", t), SourceKind::Cxx));
}};

UTest classifies_shader_and_kernel_languages{"SourceKind/ClassifiesShaderAndKernelLanguages", [] {
    // The criterion is "a separate compiler consumes it", not "NVIDIA ships
    // it". Before this, the list was `.cu` and `.hip` alone, and a shader in a
    // constrained glob was refused with "no role for the extension '.comp'" —
    // so the file never reached MCPP_DEVICE_SOURCES and the rule package that
    // exists to compile it was told there was nothing to compile.
    auto t = mcpp::builtin_extension_table();
    for (const char* p : {"shaders/s.comp", "shaders/s.vert", "shaders/s.frag",
                          "shaders/s.geom", "shaders/s.tesc", "shaders/s.tese",
                          "shaders/s.mesh", "shaders/s.task", "shaders/s.rgen",
                          "shaders/s.rint", "shaders/s.rahit", "shaders/s.rchit",
                          "shaders/s.rmiss", "shaders/s.rcall", "shaders/s.glsl",
                          "shaders/s.hlsl", "kernels/k.cl", "kernels/k.metal"})
        expect(eq(mcpp::classify(p, t), SourceKind::Device)) << p;
}};

UTest shader_extensions_do_not_widen_the_default_glob{"SourceKind/ShaderExtensionsDoNotWidenTheDefaultGlob", [] {
    // The whole safety argument for widening the table rests on this: the
    // default globs are unchanged, so no published package starts compiling a
    // vendored shader on the next upgrade. Asserted over the WHOLE list rather
    // than the two names that were there before, so a future addition cannot
    // pass this file while changing what a default build compiles.
    const auto globs = mcpp::default_source_globs(mcpp::builtin_extension_table());
    const std::vector<std::string> expected{
        "src/**/*.cppm", "src/**/*.cpp", "src/**/*.cc", "src/**/*.c",
        "src/**/*.S", "src/**/*.s", "src/**/*.asm",
    };
    expect(eq(globs, expected));
}};

UTest shader_extensions_cannot_be_claimed_as_module_interfaces{"SourceKind/ShaderExtensionsCannotBeClaimedAsModuleInterfaces", [] {
    // Reserved for the same reason `.cu` is: routing a shader to the C++
    // module rule fails somewhere that names neither the file nor the key.
    for (const char* ext : {".comp", ".hlsl", ".cl", ".metal", ".sycl"}) {
        auto err = mcpp::validate_module_extensions(std::vector<std::string>{ext});
        expect(fatal(err.has_value())) << ext;
        expect(ne(err->find(ext), std::string::npos)) << ext;
    }
}};

UTest shader_objects_use_the_collision_proof_name{"SourceKind/ShaderObjectsUseTheCollisionProofName", [] {
    // `scale.comp` and `scale.vert` are one rename apart in a shader
    // directory, and both would be `scale.o` under the stem-named form.
    expect(eq(mcpp::object_filename_for("shaders/scale.comp", ".o"), "scale.comp.o"));
    expect(eq(mcpp::object_filename_for("shaders/scale.vert", ".o"), "scale.vert.o"));
}};

UTest device_headers_affect_graph_shape{"SourceKind/DeviceHeadersAffectGraphShape", [] {
    // `.cuh` reaches a device TU through the preprocessor, so editing one can
    // change what the graph should be. Leaving it in `Other` is why a project
    // would see "edited the kernel header, nothing rebuilt".
    auto t = mcpp::builtin_extension_table();
    expect(eq(mcpp::classify("src/k.cuh",  t), SourceKind::Header));
    expect(eq(mcpp::classify("src/k.hiph", t), SourceKind::Header));
    expect(bool(mcpp::affects_graph_shape(SourceKind::Header)));
}};

UTest device_predicates{"SourceKind/DevicePredicates", [] {
    // Scan-exempt for the same reason assembly is: a device TU carries no
    // `import`, so there is nothing for P1689 to answer.
    expect(mcpp::is_scan_exempt(SourceKind::Device));
    expect(!mcpp::produces_bmi(SourceKind::Device));
    expect(!mcpp::is_cxx_like(SourceKind::Device));
    expect(!mcpp::links_unconditionally(SourceKind::Device));
    // Absent for the same reason assembly is absent: the content change is
    // tracked by ninja, and a NEW file is `program_inputs_stale`'s question.
    expect(!mcpp::affects_graph_shape(SourceKind::Device));
    expect(eq(mcpp::to_string(SourceKind::Device), "device"));
}};

UTest device_extensions_are_not_in_the_default_glob{"SourceKind/DeviceExtensionsAreNotInTheDefaultGlob", [] {
    // Same compatibility argument as the built-in module-extension table: a
    // published package that vendors a `.cu` it builds elsewhere must not
    // start compiling it on the next mcpp upgrade. Device sources are opted
    // into by naming them in a `kind = "device"` target.
    auto globs = mcpp::default_source_globs(mcpp::builtin_extension_table());
    for (auto const& g : globs) {
        expect(eq(g.find(".cu"), std::string::npos)) << g;
        expect(eq(g.find(".hip"), std::string::npos)) << g;
    }
    expect(eq(mcpp::default_source_globs_note(mcpp::builtin_extension_table())
                  .find("cu"), std::string::npos));
}};

UTest device_objects_use_the_collision_proof_name{"SourceKind/DeviceObjectsUseTheCollisionProofName", [] {
    // `k.cu` and `k.cpp` in one directory are common in a mixed project; the
    // stem-named form would give both `k.o`.
    expect(eq(mcpp::object_filename_for("src/k.cu", ".o"), "k.cu.o"));
    expect(eq(mcpp::object_filename_for("src/k.hip", ".o"), "k.hip.o"));
}};

UTest device_extensions_cannot_be_claimed_as_module_interfaces{"SourceKind/DeviceExtensionsCannotBeClaimedAsModuleInterfaces", [] {
    auto err = mcpp::validate_module_extensions(std::vector<std::string>{".cu"});
    expect(fatal(err.has_value()));
    expect(ne(err->find(".cu"), std::string::npos));
}};

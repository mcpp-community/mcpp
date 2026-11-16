
import std;
import mcpplibs.mtest;
import mcpp.build.flags;
import mcpp.build.distribution;
import mcpp.modgraph.scanner;
import mcpp.build.ninja;
import mcpp.targetside;

using namespace mcpplibs::mtest;

namespace {

struct Tmp {
    std::filesystem::path path;
    Tmp() {
        path = std::filesystem::temp_directory_path()
             / std::format("mcpp_flags_test_{}", std::random_device{}());
        std::filesystem::create_directories(path);
    }
    ~Tmp() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

void touch(const std::filesystem::path& p) {
    std::ofstream(p) << "x";
}

// libatomic (a GCC runtime lib; LLVM ships no equivalent) provides the
// out-of-line __atomic_* libcalls that 16-byte/oversized std::atomic lowers
// to. Compiler drivers don't auto-link it, so mcpp must inject `-latomic`.
// But `--as-needed` does NOT skip a missing library — the linker still has to
// open it — so the flag may only be emitted when a libatomic actually exists
// on the toolchain's link dirs, else it would break toolchains that omit it.

constexpr bool kDynamic = false;  // staticLink arg
constexpr bool kStatic  = true;

UTest emitted_when_libatomic_shared_present{"BuildFlagsAtomic/EmittedWhenLibatomicSharedPresent", [] {
    Tmp dir;
    touch(dir.path / "libatomic.so");
    auto flag = mcpp::build::atomic_link_flag({dir.path}, kDynamic);
    expect(ne(flag.find("-latomic"), std::string::npos));
    expect(ne(flag.find("--as-needed"), std::string::npos));
}};

UTest emitted_when_libatomic_archive_present{"BuildFlagsAtomic/EmittedWhenLibatomicArchivePresent", [] {
    Tmp dir;
    touch(dir.path / "libatomic.a");
    auto flag = mcpp::build::atomic_link_flag({dir.path}, kDynamic);
    expect(ne(flag.find("-latomic"), std::string::npos));
}};

UTest empty_when_libatomic_absent{"BuildFlagsAtomic/EmptyWhenLibatomicAbsent", [] {
    Tmp dir;
    touch(dir.path / "libc++.so.1");  // some other lib, but no libatomic
    auto flag = mcpp::build::atomic_link_flag({dir.path}, kDynamic);
    expect(flag.empty()) << "got: " << flag;
}};

// `-latomic` resolves only `libatomic.so` / `libatomic.a` — a bare
// soname-versioned `libatomic.so.1` (no dev symlink, no archive) is NOT
// link-resolvable, so the flag must stay empty or the link breaks with
// "cannot find -latomic".
UTest empty_when_only_soname_versioned_present{"BuildFlagsAtomic/EmptyWhenOnlySonameVersionedPresent", [] {
    Tmp dir;
    touch(dir.path / "libatomic.so.1");
    touch(dir.path / "libatomic.so.1.2.0");
    auto flag = mcpp::build::atomic_link_flag({dir.path}, kDynamic);
    expect(flag.empty()) << "got: " << flag;
}};

UTest scans_all_dirs_not_just_first{"BuildFlagsAtomic/ScansAllDirsNotJustFirst", [] {
    Tmp a, b;
    touch(b.path / "libatomic.so");  // link-resolvable, in a later dir
    auto flag = mcpp::build::atomic_link_flag({a.path, b.path}, kDynamic);
    expect(ne(flag.find("-latomic"), std::string::npos));
}};

// Cross-platform / cross-linkage: a full-static link (`-static`, e.g. musl
// targets) resolves `-latomic` only from `libatomic.a`. A lone `libatomic.so`
// is not usable, so the flag must stay empty under static linkage to avoid
// "cannot find -latomic".
UTest static_link_empty_when_only_shared_present{"BuildFlagsAtomic/StaticLinkEmptyWhenOnlySharedPresent", [] {
    Tmp dir;
    touch(dir.path / "libatomic.so");
    auto flag = mcpp::build::atomic_link_flag({dir.path}, kStatic);
    expect(flag.empty()) << "got: " << flag;
}};

UTest static_link_emitted_when_archive_present{"BuildFlagsAtomic/StaticLinkEmittedWhenArchivePresent", [] {
    Tmp dir;
    touch(dir.path / "libatomic.a");
    auto flag = mcpp::build::atomic_link_flag({dir.path}, kStatic);
    expect(ne(flag.find("-latomic"), std::string::npos));
}};

// mcpp#226: normalize_include_flags generalizes the old -I-only
// absolutize_include_flags to the whole include/lib-search-path family, in
// BOTH the joined spelling (`-iquotehdr`) and the separated spelling
// (`-isystem` followed by a standalone next element). All four of these
// project-relative paths must resolve to the same "/proj/hdr" target.
//
// The expected spelling is NATIVE (#390): `-I/abs/hdr` written with forward
// slashes keeps them on MSVC, and the old expectation `(root / "hdr").string()`
// was itself the mixed `"/proj\hdr"` shape this family of bugs produced.
// make_preferred() makes the expectation platform-correct on both sides.
UTest normalize_include_flags_rewrites_full_include_family{"BuildFlags/NormalizeIncludeFlagsRewritesFullIncludeFamily", [] {
    std::filesystem::path root = "/proj";
    auto expected = [](const std::filesystem::path& p) {
        auto n = p;
        n.make_preferred();
        return n.string();
    };
    std::vector<std::string> flags = {
        "-Ihdr", "-iquotehdr", "-isystem", "hdr", "-idirafterhdr",
    };

    mcpp::modgraph::normalize_include_flags(root, flags);

    expect(fatal(eq(flags.size(), 5u)));
    expect(eq(flags[0], "-I" + expected(root / "hdr")));
    expect(eq(flags[1], "-iquote" + expected(root / "hdr")));
    expect(eq(flags[2], "-isystem"));                       // prefix itself untouched
    expect(eq(flags[3], expected(root / "hdr")));           // separated element rewritten
    expect(eq(flags[4], "-idirafter" + expected(root / "hdr")));
}};

// Absolute paths and root-relative spellings are left alone (matches the
// pre-#226 -I behavior), for both the joined and separated forms — only the
// separator spelling is normalized to native (#390; a no-op on POSIX).
UTest normalize_include_flags_leaves_absolute_paths_alone{"BuildFlags/NormalizeIncludeFlagsLeavesAbsolutePathsAlone", [] {
    std::filesystem::path root = "/proj";
    auto expected = [](const std::filesystem::path& p) {
        auto n = p;
        n.make_preferred();
        return n.string();
    };
    std::vector<std::string> flags = {
        "-I/abs/hdr", "-isystem", "/abs/hdr", "-DKEEP",
    };

    mcpp::modgraph::normalize_include_flags(root, flags);

    expect(eq(flags[0], "-I" + expected("/abs/hdr")));
    expect(eq(flags[1], "-isystem"));
    expect(eq(flags[2], expected("/abs/hdr")));
    expect(eq(flags[3], "-DKEEP"));
}};

}  // namespace

// #634, item 11 of the triage record: a search-path entry that begins with a
// token the loader expands is relative to a loaded object, not to the package
// that wrote it. Both ldflag normalisers ask this one predicate.
UTest every_loader_token_is_left_as_written{"BuildFlagsLoaderTokens/EveryLoaderTokenIsLeftAsWritten", [] {
    for (std::string_view entry : {"$ORIGIN", "${ORIGIN}", "$ORIGIN/../lib", "$LIB",
                                   "@executable_path", "@executable_path/../Frameworks",
                                   "@loader_path", "@loader_path/", "@rpath",
                                   "@rpath/sub"}) {
        expect(mcpp::build::is_loader_relative_search_path(entry)) << entry;
    }
}};

UTest ordinary_paths_are_not_tokens{"BuildFlagsLoaderTokens/OrdinaryPathsAreNotTokens", [] {
    for (std::string_view entry : {"", "lib", "./lib", "/opt/lib", "rpath",
                                   "@executable_pathology", "@rpathx", "x/@rpath"}) {
        expect(!mcpp::build::is_loader_relative_search_path(entry)) << entry;
    }
}};

// #647 E3: which link line a (host, target) pair takes. The branch used to be
// chosen by `if constexpr` on the host, so an Android row built on a macOS host
// received the Apple SDK line with no `--target` and linked through
// `ld64.lld`. The choice is a function of the host AND the target's object
// format, stated here for every host on every CI machine.
namespace {
using mcpp::build::LinkHost;
using mcpp::build::LinkShape;
using mcpp::build::link_shape;
using Fmt = mcpp::build::dist::Format;
} // namespace

UTest linux_host_takes_the_generic_line_for_every_target{"LinkShape/LinuxHostTakesTheGenericLineForEveryTarget", [] {
    for (auto f : {Fmt::Elf, Fmt::MachO, Fmt::Pe, Fmt::Wasm}) {
        expect(eq(link_shape(LinkHost::Linux, f, false, true), LinkShape::Generic));
        expect(eq(link_shape(LinkHost::Linux, f, false, false), LinkShape::Generic));
    }
}};

UTest mac_os_host_keeps_the_apple_line_for_mach_o_only{"LinkShape/MacOSHostKeepsTheAppleLineForMachOOnly", [] {
    // macOS for macOS, and the iOS rows: the Apple SDK line, as before.
    expect(eq(link_shape(LinkHost::MacOS, Fmt::MachO, false, false), LinkShape::AppleSdk));
    expect(eq(link_shape(LinkHost::MacOS, Fmt::MachO, false, true), LinkShape::AppleSdk));
    // x86_64-linux-android and wasm32-emscripten on a Mac: the line that
    // carries `--target`.
    expect(eq(link_shape(LinkHost::MacOS, Fmt::Elf,  false, true), LinkShape::Generic));
    expect(eq(link_shape(LinkHost::MacOS, Fmt::Wasm, false, true), LinkShape::Generic));
    expect(eq(link_shape(LinkHost::MacOS, Fmt::Elf,  false, false), LinkShape::Generic));
    expect(eq(link_shape(LinkHost::MacOS, Fmt::Pe,   false, true), LinkShape::Generic));
}};

UTest windows_host_separates_pe_from_targets_named_by_flag{"LinkShape/WindowsHostSeparatesPeFromTargetsNamedByFlag", [] {
    expect(eq(link_shape(LinkHost::Windows, Fmt::Pe,  true,  false), LinkShape::MsvcLinkExe));
    expect(eq(link_shape(LinkHost::Windows, Fmt::Pe,  false, true), LinkShape::PeLld));
    expect(eq(link_shape(LinkHost::Windows, Fmt::Pe,  false, false), LinkShape::PeLld));
    // An SDK or retargetable clang aimed at an ELF row: named by `--target`.
    expect(eq(link_shape(LinkHost::Windows, Fmt::Elf, false, true), LinkShape::Generic));
    // The canadian GCC cross to x86_64-linux-musl names its target by prefix
    // and keeps the line its CI job verifies.
    expect(eq(link_shape(LinkHost::Windows, Fmt::Elf, false, false), LinkShape::PeLld));
}};

// ── c_abi_absent_facility_advice — the manifest reads back at the link ──────
//
// `undefined reference to 'fork'` is true and useless: it says a symbol is
// missing without saying whether that is a defect, a missing dependency, or a
// deliberate limit of the environment. The manifest already distinguishes
// those (design 2026-09-20 §5.7.5); these pin that it is read back.

UTest a_link_absence_named_in_the_manifest_is_explained{"CAbiAbsentAdvice/ALinkAbsenceNamedInTheManifestIsExplained", [] {
    std::vector<mcpp::targetside::CAbiAbsentEntry> absent{
        {"fork", mcpp::targetside::CAbiAbsentForm::Link,
         "openkal has no process image duplication"},
    };
    auto a = mcpp::build::c_abi_absent_facility_advice(
        "ld.lld: error: undefined symbol: fork\n", "musl", absent);
    expect(fatal(!a.empty()));
    expect(ne(a.find("fork"), std::string::npos));
    expect(ne(a.find("no process image duplication"), std::string::npos));
    // THE WHOLE CLAUSE, NOT `find("musl")`. The name was interpolated from
    // two conditionals -- an opening paren and the name -- and the closing
    // one was never emitted, so every reader saw `(musl declares that it
    // does not supply ...`. `find("musl")` is true of that sentence too,
    // which is why it took a real link to notice. Assert the rendering.
    expect(ne(a.find("the C library in this graph (musl) declares"), std::string::npos)) << a;
}};

UTest an_unnamed_c_library_leaves_no_empty_parentheses{"CAbiAbsentAdvice/AnUnnamedCLibraryLeavesNoEmptyParentheses", [] {
    // The other side of the same substitution: with no name there must be no
    // parenthetical at all, rather than `graph () declares`.
    std::vector<mcpp::targetside::CAbiAbsentEntry> absent{
        {"fork", mcpp::targetside::CAbiAbsentForm::Link, ""},
    };
    auto a = mcpp::build::c_abi_absent_facility_advice(
        "ld.lld: error: undefined symbol: fork\n", "", absent);
    expect(fatal(!a.empty()));
    expect(ne(a.find("the C library in this graph declares"), std::string::npos)) << a;
    expect(eq(a.find("()"), std::string::npos)) << a;
}};

UTest the_gnu_spelling_is_matched_too{"CAbiAbsentAdvice/TheGnuSpellingIsMatchedToo", [] {
    std::vector<mcpp::targetside::CAbiAbsentEntry> absent{
        {"fork", mcpp::targetside::CAbiAbsentForm::Link, ""},
    };
    auto a = mcpp::build::c_abi_absent_facility_advice(
        "main.o: undefined reference to `fork'\n", "musl", absent);
    expect(!a.empty());
}};

UTest an_absence_that_reaches_the_program_at_run_time_is_not_matched{"CAbiAbsentAdvice/AnAbsenceThatReachesTheProgramAtRunTimeIsNotMatched", [] {
    // `enosys` and `accepted-no-effect` reach a program by construction while
    // it runs; matching them against a link diagnostic would report a
    // coincidence of spelling as an explanation.
    std::vector<mcpp::targetside::CAbiAbsentEntry> absent{
        {"mprotect", mcpp::targetside::CAbiAbsentForm::Enosys, "no protection op"},
        {"tcsetattr", mcpp::targetside::CAbiAbsentForm::AcceptedNoEffect, ""},
    };
    expect(mcpp::build::c_abi_absent_facility_advice(
        "ld.lld: error: undefined symbol: mprotect\n", "musl", absent).empty());
    expect(mcpp::build::c_abi_absent_facility_advice(
        "undefined reference to `tcsetattr'\n", "musl", absent).empty());
}};

UTest a_link_failure_naming_something_else_gets_no_note{"CAbiAbsentAdvice/ALinkFailureNamingSomethingElseGetsNoNote", [] {
    std::vector<mcpp::targetside::CAbiAbsentEntry> absent{
        {"fork", mcpp::targetside::CAbiAbsentForm::Link, ""},
    };
    expect(mcpp::build::c_abi_absent_facility_advice(
        "ld.lld: error: undefined symbol: my_own_function\n", "musl", absent)
                    .empty());
}};

UTest a_library_that_enumerated_nothing_produces_no_note{"CAbiAbsentAdvice/ALibraryThatEnumeratedNothingProducesNoNote", [] {
    expect(mcpp::build::c_abi_absent_facility_advice(
        "ld.lld: error: undefined symbol: fork\n", "musl", {}).empty());
}};

UTest a_longer_symbol_with_the_same_prefix_is_not_explained{"CAbiAbsentAdvice/ALongerSymbolWithTheSamePrefixIsNotExplained", [] {
    // `undefined symbol: open` is a prefix of `undefined symbol: opendir`.
    // A substring search would answer a link failure with a row that has
    // nothing to do with it, and an explanation that is confidently wrong is
    // worse than the linker's own message.
    std::vector<mcpp::targetside::CAbiAbsentEntry> absent{
        {"open", mcpp::targetside::CAbiAbsentForm::Link, "not supplied"},
    };
    expect(mcpp::build::c_abi_absent_facility_advice(
        "ld.lld: error: undefined symbol: opendir\n", "musl", absent).empty());
    expect(!(mcpp::build::c_abi_absent_facility_advice(
        "ld.lld: error: undefined symbol: open\n", "musl", absent).empty()));
}};

UTest the_list_survives_a_write_and_read_beside_build_ninja{"CAbiAbsentAdvice/TheListSurvivesAWriteAndReadBesideBuildNinja", [] {
    // The two paths that report a failed build --- the one with a plan and the
    // fast path, which has none by construction --- must give the same advice,
    // or it appears depending on whether build.ninja happened to be up to
    // date. The list travels between them in a file.
    Tmp dir;
    std::vector<mcpp::targetside::CAbiAbsentEntry> absent{
        {"fork", mcpp::targetside::CAbiAbsentForm::Link, "no image duplication"},
        {"mprotect", mcpp::targetside::CAbiAbsentForm::Enosys, ""},
    };
    mcpp::build::write_c_abi_absent_sidecar(dir.path, "musl", absent);
    auto [name, back] = mcpp::build::read_c_abi_absent_sidecar(dir.path);
    expect(eq(name, "musl"));
    expect(fatal(eq(back.size(), 2u)));
    expect(eq(back[0].name, "fork"));
    expect(eq(back[0].form, mcpp::targetside::CAbiAbsentForm::Link));
    expect(eq(back[0].note, "no image duplication"));
    expect(eq(back[1].form, mcpp::targetside::CAbiAbsentForm::Enosys));
    expect(back[1].note.empty());
    // And the advice built from the read-back list is the advice the plan
    // path would have given.
    expect(!(mcpp::build::c_abi_absent_facility_advice(
        "ld.lld: error: undefined symbol: fork\n", name, back).empty()));
}};

UTest a_directory_no_build_wrote_to_yields_nothing{"CAbiAbsentAdvice/ADirectoryNoBuildWroteToYieldsNothing", [] {
    Tmp dir;
    auto [name, back] = mcpp::build::read_c_abi_absent_sidecar(dir.path);
    expect(name.empty());
    expect(back.empty());
}};

UTest a_graph_that_declares_no_absence_leaves_no_sidecar{"CAbiAbsentAdvice/AGraphThatDeclaresNoAbsenceLeavesNoSidecar", [] {
    // A build whose C library states nothing must not leave a file behind for
    // the next build to read: the fast path would then explain a failure with
    // a list the current graph never declared.
    Tmp dir;
    mcpp::build::write_c_abi_absent_sidecar(dir.path, "musl",
        {{"fork", mcpp::targetside::CAbiAbsentForm::Link, ""}});
    mcpp::build::write_c_abi_absent_sidecar(dir.path, "", {});
    auto [name, back] = mcpp::build::read_c_abi_absent_sidecar(dir.path);
    expect(back.empty());
}};

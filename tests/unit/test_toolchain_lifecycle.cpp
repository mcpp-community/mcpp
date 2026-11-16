
import std;
import mcpplibs.mtest;
import mcpp.config;
import mcpplibs.os.platform;
import mcpp.runtime.binding;
import mcpp.toolchain.lifecycle;
import mcpp.toolchain.post_install;
import mcpp.toolchain.registry;

using namespace mcpplibs::mtest;

using namespace mcpp::toolchain;

// WHAT IS AND IS NOT TESTED HERE, because the difference is not a gap.
//
// `remove_payload_tree()` falls back to moving held FILES aside so the tree
// can be deleted around them. That path is reachable only where a file can be
// renamed while something holds it open — which is Windows. On POSIX the two
// permissions are the same one: a file you cannot unlink is a file you cannot
// rename either, because both need write on the parent directory. There is no
// POSIX state that models "held open by another process".
//
// A POSIX fixture cannot stand in for it either, and the reason is worth
// keeping: the only POSIX way to make a file undeletable is to drop write on
// its parent directory — and this function's SECOND pass adds write back
// across the tree, on purpose, because that is exactly the read-only-payload
// case it must fix. The fixture therefore becomes removable the moment the
// code under test touches it. That is the function working, not a hole.
//
// So the held-file path is gated by e2e 239 on the Windows runner, and what
// is pinned here is what must hold on every platform: the ordinary deletion
// takes the ordinary route, and the sweep is precise about what it eats.
// Nothing here asserts something this platform cannot express.

UTest an_ordinary_payload_is_just_deleted{"ToolchainRemove/AnOrdinaryPayloadIsJustDeleted", [] {
    // The common path must not grow a `.trash-` directory: moving files aside
    // is the fallback, and a fallback that fires always is not a fallback.
    auto dir = std::filesystem::temp_directory_path()
             / std::format("mcpp-rm-ok-{}", std::chrono::steady_clock::now()
                                                .time_since_epoch().count());
    std::filesystem::create_directories(dir / "14.44.35207" / "bin");
    std::ofstream{dir / "14.44.35207" / "bin" / "cl.exe"} << "not a compiler";

    std::error_code ec;
    expect(bool(remove_payload_tree(dir / "14.44.35207", ec)));
    expect(!std::filesystem::exists(dir / "14.44.35207"));

    bool anyTrash = false;
    for (auto& e : std::filesystem::directory_iterator(dir, ec))
        anyTrash |= e.path().filename().string().starts_with(".trash-");
    expect(!anyTrash) << "a deletable payload was parked instead of deleted";

    std::filesystem::remove_all(dir, ec);
}};

UTest the_sweep_does_not_touch_another_version_that_has_no_files_yet{"ToolchainRemove/TheSweepDoesNotTouchAnotherVersionThatHasNoFilesYet", [] {
    // An install populates a version directory over time, so a DIFFERENT
    // version being extracted right now is briefly indistinguishable from a
    // leftover skeleton. Two mcpp processes against one MCPP_HOME is ordinary
    // on a shared or self-hosted runner, and deleting someone else's
    // half-extracted toolchain is not a cleanup.
    //
    // So the file-less sweep applies to the ONE version this command names.
    auto pkgRoot = std::filesystem::temp_directory_path()
                 / std::format("mcpp-sweep3-{}", std::chrono::steady_clock::now()
                                                     .time_since_epoch().count());
    auto mine   = pkgRoot / "14.44.35207";       // the one being removed
    auto theirs = pkgRoot / "14.52.36629";       // someone else, mid-extract
    std::filesystem::create_directories(mine / "bin");
    std::filesystem::create_directories(theirs / "VC" / "Tools");

    sweep_parked_payloads(pkgRoot, mine);

    expect(!std::filesystem::exists(mine)) << "the named skeleton survived";
    expect(std::filesystem::exists(theirs))
        << "swept a version this command was not about — that is someone "
           "else's install being extracted";

    std::error_code ec;
    std::filesystem::remove_all(pkgRoot, ec);
}};

UTest the_sweep_deletes_parked_payloads_and_nothing_else{"ToolchainRemove/TheSweepDeletesParkedPayloadsAndNothingElse", [] {
    // The sweep runs before every install and remove, so it must be precise:
    // `.trash-*` goes, an installed version directory beside it stays.
    auto pkgRoot = std::filesystem::temp_directory_path()
                 / std::format("mcpp-sweep-{}", std::chrono::steady_clock::now()
                                                    .time_since_epoch().count());
    std::filesystem::create_directories(pkgRoot / ".trash-14.44.35207-1" / "sub");
    std::ofstream{pkgRoot / ".trash-14.44.35207-1" / "sub" / "held.dll"} << "x";
    std::filesystem::create_directories(pkgRoot / "14.44.35207" / "bin");
    std::ofstream{pkgRoot / "14.44.35207" / "bin" / "cl.exe"} << "keep me";

    sweep_parked_payloads(pkgRoot, {});

    expect(!std::filesystem::exists(pkgRoot / ".trash-14.44.35207-1"))
        << "parked payload was not swept";
    expect(std::filesystem::exists(pkgRoot / "14.44.35207" / "bin" / "cl.exe"))
        << "the sweep deleted an installed toolset";

    std::error_code ec;
    std::filesystem::remove_all(pkgRoot, ec);
}};

UTest a_directory_skeleton_with_no_files_counts_as_removed{"ToolchainRemove/ADirectorySkeletonWithNoFilesCountsAsRemoved", [] {
    // Windows can refuse to delete an empty DIRECTORY when a process has it
    // as its current directory — mspdbsrv.exe is launched inside the payload,
    // so this is the ordinary tail of a /Zi build with the toolset being
    // removed. Every file is already gone at that point.
    //
    // A toolchain with no files in it is not installed, which is exactly what
    // `remove` promises. Calling that a failure would report the opposite of
    // what happened.
    auto dir = std::filesystem::temp_directory_path()
             / std::format("mcpp-skel-{}", std::chrono::steady_clock::now()
                                               .time_since_epoch().count());
    std::filesystem::create_directories(dir / "14.44.35207" / "bin" / "Hostx64" / "x64");

    std::error_code ec;
    expect(bool(remove_payload_tree(dir / "14.44.35207", ec)))
        << "a payload with no files left was reported as still installed";

    std::filesystem::remove_all(dir, ec);
}};

UTest the_sweep_also_clears_a_file_less_skeleton{"ToolchainRemove/TheSweepAlsoClearsAFileLessSkeleton", [] {
    // The deferral half: whatever the removal could not delete goes on the
    // next lifecycle command, once the process holding it has exited.
    auto pkgRoot = std::filesystem::temp_directory_path()
                 / std::format("mcpp-skel2-{}", std::chrono::steady_clock::now()
                                                    .time_since_epoch().count());
    std::filesystem::create_directories(pkgRoot / "14.44.35207" / "bin" / "x64");
    std::filesystem::create_directories(pkgRoot / "14.52.36629" / "bin");
    std::ofstream{pkgRoot / "14.52.36629" / "bin" / "cl.exe"} << "a real one";

    sweep_parked_payloads(pkgRoot, pkgRoot / "14.44.35207");

    expect(!std::filesystem::exists(pkgRoot / "14.44.35207"))
        << "the empty skeleton survived the sweep";
    expect(std::filesystem::exists(pkgRoot / "14.52.36629" / "bin" / "cl.exe"))
        << "the sweep ate an installed toolset";

    std::error_code ec;
    std::filesystem::remove_all(pkgRoot, ec);
}};

// ─── The declared C runtime payload (mcpp#660) ─────────────────────────────
//
// `ensure_declared_runtime` installs the payload a SubOS declares. The install
// itself needs xlings and the network and is covered end to end (e2e 700);
// what is pinned here is when it must NOT act, because each of these cases
// runs on every build and must start no process and change nothing.

namespace {

mcpp::config::GlobalConfig unreachable_cfg() {
    // A registry that does not exist. None of the cases below may consult it.
    mcpp::config::GlobalConfig cfg;
    cfg.registryDir = std::filesystem::temp_directory_path()
        / "mcpp-declared-runtime-unreachable-registry";
    return cfg;
}

} // namespace

UTest a_binding_that_locates_its_payload_is_left_alone{"DeclaredRuntime/ABindingThatLocatesItsPayloadIsLeftAlone", [] {
    auto cfg = unreachable_cfg();
    mcpp::runtime::RuntimeBinding binding;
    binding.runtimeId = "glibc@2.44.3";
    binding.libraryDirs = {"/store/xim-x-glibc/2.44.3/lib64"};
    const auto before = binding.libraryDirs;
    expect(eq(ensure_declared_runtime(cfg, binding).value_or(true), false));
    expect(eq(binding.libraryDirs, before));
    expect(eq(binding.runtimeId, "glibc@2.44.3"));
}};

// A provider that binds no payload (`ucrt` is an OS component) and an absent
// identity are not requests to install anything.
UTest only_a_glibc_payload_is_installed{"DeclaredRuntime/OnlyAGlibcPayloadIsInstalled", [] {
    auto cfg = unreachable_cfg();
    for (auto id : {"", "ucrt@10.0.26100.0", "macos_sdk@15.0", "glibc"}) {
        mcpp::runtime::RuntimeBinding binding;
        binding.runtimeId = id;
        expect(eq(ensure_declared_runtime(cfg, binding).value_or(true), false)) << id;
        expect(binding.libraryDirs.empty()) << id;
    }
}};

// Off Linux there is no glibc payload to provide, whatever the identity says.
UTest nothing_is_installed_off_linux{"DeclaredRuntime/NothingIsInstalledOffLinux", [] {
    if constexpr (mcpplibs::os::is_linux)
        skip("the Linux path is exercised by e2e 700");
    auto cfg = unreachable_cfg();
    mcpp::runtime::RuntimeBinding binding;
    binding.runtimeId = "glibc@2.44.3";
    expect(eq(ensure_declared_runtime(cfg, binding).value_or(true), false));
}};

// Which toolchains run on a C runtime payload: a GNU gcc and an llvm on
// Linux. Any other toolchain must not cause a runtime download.
UTest only_toolchains_on_a_runtime_payload_need_it{"DeclaredRuntime/OnlyToolchainsOnARuntimePayloadNeedIt", [] {
    XimToolchainPackage gcc;   gcc.ximName = "gcc";   gcc.needsGccPostInstallFixup = true;
    XimToolchainPackage llvm;  llvm.ximName = "llvm";
    XimToolchainPackage musl;  musl.ximName = "musl-gcc";
    XimToolchainPackage msvc;  msvc.ximName = "msvc";
    if constexpr (mcpplibs::os::is_windows) {
        for (auto const* p : {&gcc, &llvm, &musl, &msvc})
            expect(runtime_payload_kind(*p).empty()) << p->ximName;
    } else {
        expect(eq(runtime_payload_kind(gcc), "gcc"));
        expect(eq(runtime_payload_kind(llvm), "llvm"));
        expect(runtime_payload_kind(musl).empty());
        expect(runtime_payload_kind(msvc).empty());
    }
}};


import std;
import mcpplibs.mtest;
import mcpplibs.os.platform;
import mcpp.toolchain.msvc;
import mcpp.toolchain.triple;
import mcpp.toolchain.registry;
import mcpp.build.prepare;

using namespace mcpplibs::mtest;

// The first runs of the hosts the table names, asked as each host asks for its own
// (`pins::kFirstRun*` are this host's).
namespace hosts {
namespace pins = mcpp::toolchain::triple::pins;
constexpr std::string_view kMac          = pins::first_run("macos", "aarch64");
constexpr std::string_view kWinMsvc      = pins::first_run("windows", "x86_64", "msvc");
constexpr std::string_view kWinGnu       = pins::first_run("windows", "x86_64", "no-msvc");
constexpr std::string_view kWinGnuTarget = pins::first_run_target("windows", "x86_64", "no-msvc");
constexpr std::string_view kLinuxX86_64  = pins::first_run("linux", "x86_64");
constexpr std::string_view kLinuxAarch64 = pins::first_run("linux", "aarch64");
constexpr std::string_view kLinuxOther   = pins::first_run("linux", "riscv64");
}  // namespace hosts

// ── has_usable_msvc() ───────────────────────────────────────────────────────
//
// The predicate that decides whether the MSVC ABI is a viable default on this
// machine. A bare Windows box has the UCRT runtime DLLs but neither the MSVC
// STL (Visual Studio's "Desktop development with C++" workload) nor the
// Windows SDK, so a default that targets the MSVC ABI there can never build.

UTest has_usable_msvc_is_false_off_windows{"WindowsDefaults/HasUsableMsvcIsFalseOffWindows", [] {
    if constexpr (mcpplibs::os::is_windows) {
        skip("windows behavior is covered by e2e 182");
    } else {
        expect(!mcpp::toolchain::msvc::has_usable_msvc());
    }
}};

// The predicate must never disagree with the two probes it is defined as.
// A drift here (e.g. someone "optimizing" it down to find_vs_install_path())
// silently reintroduces the half-installed-VS trap it exists to close.
UTest has_usable_msvc_agrees_with_its_parts{"WindowsDefaults/HasUsableMsvcAgreesWithItsParts", [] {
    const bool both = mcpp::toolchain::msvc::find_std_module_source().has_value()
                   && mcpp::toolchain::msvc::find_windows_sdk().has_value();
    expect(eq(mcpp::toolchain::msvc::has_usable_msvc(), both));
}};

// ── First-run pins ──────────────────────────────────────────────────────────
//
// macOS and Windows shared one pin until 2026.8.2.1. They must not: Apple
// ships no GCC, while on Windows clang targets the MSVC ABI and therefore
// needs a Visual Studio that is not preinstalled.

UTest first_run_pins_parse{"WindowsDefaults/FirstRunPinsParse", [] {
    namespace pins = mcpp::toolchain::triple::pins;
    for (auto spec : { hosts::kMac, hosts::kWinMsvc,
                       hosts::kWinGnu, hosts::kLinuxX86_64,
                       hosts::kLinuxAarch64, hosts::kLinuxOther }) {
        auto parsed = mcpp::toolchain::parse_toolchain_spec(std::string(spec));
        expect(fatal(parsed.has_value())) << spec;
        expect(!(parsed->version.empty())) << spec;
    }
}};

// THE DEFAULTS ARE DATA (design 2026-11-21 §3.7): every host the engine names
// has a toolchain, the bootstrap tools have a version on every host, and an
// mcpp revision of ninja names the upstream version it corrects.
UTest defaults_answer_every_host{"WindowsDefaults/DefaultsAnswerEveryHost", [] {
    namespace pins = mcpp::toolchain::triple::pins;
    for (auto spec : { hosts::kMac, hosts::kWinMsvc, hosts::kWinGnu,
                       hosts::kLinuxX86_64, hosts::kLinuxAarch64, hosts::kLinuxOther })
        expect(spec.contains('@')) << "a host without a toolchain default: " << spec;
    expect(eq(hosts::kWinGnuTarget, std::string_view("x86_64-windows-gnu")));
    expect(!pins::tool_version("ninja").empty());
    expect(!pins::tool_version("patchelf").empty());
    expect(eq(pins::install_words("llvm@23.1.3"), std::string("llvm 23.1.3")));
    // This host's answer is its row's.
    expect(eq(pins::host_default_toolchain(false), pins::kFirstRunNoMsvc));
    expect(eq(pins::host_default_toolchain(true), pins::kFirstRunMsvc));
    expect(pins::host_default_toolchain(false).contains('@'));
    expect(pins::host_default_toolchain(true).contains('@'));
}};

// The GNU fallback must land on a target mcpp is willing to build for, and
// the toolchain it names must equal that target's vocabulary pin — otherwise
// the same decision is derived in two places and they will drift.
UTest gnu_fallback_target_is_verified_and_pin_agrees{"WindowsDefaults/GnuFallbackTargetIsVerifiedAndPinAgrees", [] {
    namespace triple = mcpp::toolchain::triple;
    auto t = triple::parse(std::string(hosts::kWinGnuTarget));
    expect(fatal(t.has_value()));
    const auto* known = triple::find_known_target(*t);
    expect(fatal(ne(known, nullptr))) << hosts::kWinGnuTarget;
    expect(eq(known->tier, "verified"));
    expect(eq(known->pin, hosts::kWinGnu));
}};

// ── Intent classification ───────────────────────────────────────────────────
//
// This table decides who may be overruled. Getting it wrong fails in both
// directions: call an explicit choice "mcpp's own" and a project that needs
// the MSVC ABI silently gets a different one; call mcpp's own default
// "explicit" and every user carrying a stale persisted default stays stuck.
UTest origin_classification{"WindowsDefaults/OriginClassification", [] {
    using mcpp::build::TcOrigin;
    using mcpp::build::tc_origin_is_user_explicit;
    expect(tc_origin_is_user_explicit(TcOrigin::ManifestToolchain));
    expect(tc_origin_is_user_explicit(TcOrigin::TargetSection));
    expect(!tc_origin_is_user_explicit(TcOrigin::GlobalDefault));
    expect(!tc_origin_is_user_explicit(TcOrigin::TargetPin));
    expect(!tc_origin_is_user_explicit(TcOrigin::FirstRun));
    expect(!tc_origin_is_user_explicit(TcOrigin::None));
    // A compiler the dependency graph required is not the user's word either —
    // mcpp derived it, so the two paths that revise mcpp's own answers may.
    expect(!tc_origin_is_user_explicit(TcOrigin::GraphRequirement));
}};

// AND IT IS THE ONE ORIGIN THAT MAY NEVER BECOME THE MACHINE'S DEFAULT.
//
// Two branches persist one: the Windows first-run diversion, whose condition is
// `tcSpec.has_value()`, and the MSVC repair, whose gate is "mcpp chose this
// itself". A compiler chosen by `requires = ["mcpp:compiler=…"]` satisfies both
// — so a bare Windows box building ONE llvm-requiring project would have handed
// llvm to every later project that asked for nothing.
//
// THIS IS THE ONLY PLACE THE INVARIANT CAN BE MEASURED WITHOUT A BARE
// WINDOWS MACHINE. The e2e that checks `config.toml`'s sha256 runs where a
// toolchain is already configured, so it never reaches either branch; here the
// rule itself is the subject.
UTest only_mcpps_own_answers_may_be_persisted{"WindowsDefaults/OnlyMcppsOwnAnswersMayBePersisted", [] {
    using mcpp::build::TcOrigin;
    using mcpp::build::tc_origin_may_persist;
    expect(!tc_origin_may_persist(TcOrigin::GraphRequirement));
    expect(bool(tc_origin_may_persist(TcOrigin::GlobalDefault)));
    expect(bool(tc_origin_may_persist(TcOrigin::TargetPin)));
    expect(bool(tc_origin_may_persist(TcOrigin::FirstRun)));
    expect(bool(tc_origin_may_persist(TcOrigin::None)));
}};

// The fallback target must be PE/GNU, not the host's MSVC-ABI triple — the
// whole point is to leave the ABI that needs Visual Studio behind.
UTest gnu_fallback_target_is_windows_gnu{"WindowsDefaults/GnuFallbackTargetIsWindowsGnu", [] {
    namespace triple = mcpp::toolchain::triple;
    auto t = triple::parse(std::string(hosts::kWinGnuTarget));
    expect(fatal(t.has_value()));
    expect(eq(t->os, "windows"));
    expect(eq(t->env, "gnu"));
    expect(t->is_windows_gnu());
}};

UTest native_arm64_uses_llvm_and_other_defaults_remain_stable{"LinuxDefaults/NativeArm64UsesLlvmAndOtherDefaultsRemainStable", [] {
    namespace pins = mcpp::toolchain::triple::pins;
    expect(eq(hosts::kLinuxAarch64, "llvm@23.1.3"));
    expect(eq(hosts::kLinuxX86_64, "gcc@16.2.0"));
    expect(eq(hosts::kLinuxOther, "gcc@16.2.0-musl"));
    auto target = mcpp::toolchain::triple::parse("aarch64-linux-gnu");
    expect(fatal(bool(target)));
    auto row = mcpp::toolchain::triple::find_known_target(*target);
    expect(fatal(bool(row)));
    expect(eq(row->pin, hosts::kLinuxAarch64));
    expect(!target->pin_is_capability());
}};

// A Luban system's /etc/mcpp/config.toml names its own gcc on the host's
// triple (gcc@16.1.0 + aarch64-linux-gnu); the aarch64-linux-gnu row's llvm
// convention replaced it with a download (xim-pkgindex#986, editions CI on
// arm). The configured pair for the host's own target is kept; a cross target
// and a remembered default without a target are not this case.
UTest a_configured_pair_for_the_hosts_own_target_keeps_its_toolchain{"LinuxDefaults/AConfiguredPairForTheHostsOwnTargetKeepsItsToolchain", [] {
    namespace tr = mcpp::toolchain::triple;
    using mcpp::build::TcOrigin;
    using mcpp::build::configured_native_pair;
    auto arm = tr::parse("aarch64-linux-gnu");
    auto x86 = tr::parse("x86_64-linux-gnu");
    auto winGnu = tr::parse("x86_64-windows-gnu");
    expect(fatal(arm && x86 && winGnu));
    expect(bool(configured_native_pair(true, TcOrigin::GlobalDefault, *arm, *arm)));
    expect(!configured_native_pair(false, TcOrigin::GlobalDefault, *arm, *arm)) << "--target names it";
    expect(!configured_native_pair(true, TcOrigin::FirstRun, *arm, *arm)) << "mcpp's own guess";
    expect(!configured_native_pair(true, TcOrigin::GlobalDefault, *winGnu, *x86)) << "a cross target";
}};

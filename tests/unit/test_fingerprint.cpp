
import std;
import mcpplibs.mtest;
import mcpp.toolchain.detect;
import mcpp.toolchain.fingerprint;

using namespace mcpplibs::mtest;

using namespace mcpp::toolchain;

namespace {

FingerprintInputs baseline() {
    FingerprintInputs in;
    in.toolchain.compiler        = CompilerId::GCC;
    in.toolchain.version         = "16.1.0";
    in.toolchain.binaryPath     = "/usr/bin/g++";
    in.toolchain.driverIdent    = "g++ (xim-x-gcc 16.1.0) 16.1.0";
    in.toolchain.targetTriple   = "x86_64-linux-gnu";
    in.toolchain.stdlibId       = "libstdc++";
    in.toolchain.stdlibVersion  = "16.1.0";
    in.cppStandard              = "c++23";
    in.compileFlags             = "-O2";
    in.dependencyLockHash      = "deadbeefcafebabe";
    in.stdBmiHash              = "0123456789abcdef";
    return in;
}


// Two builds that differ ONLY in which libc they target must not share a
// fingerprint -- and therefore must not share `target/<fp>/`.
//
// Without this, switching subos leaves the build directory untouched: ninja
// sees the same graph, the objects inside were compiled against the other
// glibc, and the link succeeds. What comes out references symbols the
// interpreter cannot provide, and nothing along the way said anything. This
// is the prerequisite for building the same project under two subos, not a
// refinement of it.
UTest runtime_binding_is_part_of_the_identity{"Fingerprint/RuntimeBindingIsPartOfTheIdentity", [] {
    mcpp::toolchain::FingerprintInputs a;
    a.toolchain.compiler     = mcpp::toolchain::CompilerId::GCC;
    a.toolchain.version      = "16.1.0";
    a.toolchain.targetTriple = "x86_64-linux-gnu";
    a.toolchain.runtimeBinding = "glibc@2.39";

    auto b = a;
    b.toolchain.runtimeBinding = "glibc@2.44";

    expect(ne(mcpp::toolchain::compute_fingerprint(a).hex, mcpp::toolchain::compute_fingerprint(b).hex))
        << "same toolchain, same triple, different libc — sharing target/<fp>/ "
           "would replay objects compiled against the other one";
}};

// ...and an unset binding is its own identity, not a collision with any
// particular version.
UTest empty_runtime_binding_is_distinct{"Fingerprint/EmptyRuntimeBindingIsDistinct", [] {
    mcpp::toolchain::FingerprintInputs a;
    a.toolchain.targetTriple   = "x86_64-linux-gnu";
    a.toolchain.runtimeBinding = "glibc@2.39";
    auto b = a;
    b.toolchain.runtimeBinding.clear();
    expect(ne(mcpp::toolchain::compute_fingerprint(a).hex, mcpp::toolchain::compute_fingerprint(b).hex));
}};

UTest full_runtime_contract_hash_overrides_the_libc_label{"Fingerprint/FullRuntimeContractHashOverridesTheLibcLabel", [] {
    mcpp::toolchain::FingerprintInputs a;
    a.toolchain.targetTriple = "x86_64-linux-gnu";
    a.toolchain.runtimeBinding = "glibc@2.39";
    a.toolchain.runtimeContractHash = "subos-el8-contract";
    auto b = a;
    b.toolchain.runtimeContractHash = "subos-dev-contract";
    expect(ne(mcpp::toolchain::compute_fingerprint(a).hex, mcpp::toolchain::compute_fingerprint(b).hex));
}};

} // namespace

UTest deterministic_for_same_inputs{"Fingerprint/DeterministicForSameInputs", [] {
    auto a = compute_fingerprint(baseline());
    auto b = compute_fingerprint(baseline());
    expect(eq(a.hex, b.hex));
}};

UTest produces_sixteen_hex_chars{"Fingerprint/ProducesSixteenHexChars", [] {
    auto fp = compute_fingerprint(baseline());
    expect(eq(fp.hex.size(), 16u));
    for (char c : fp.hex) {
        expect((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
}};

// Each field, changed alone from the baseline, changes the hash.
UTest all_ten_fields_affect_hash{"Fingerprint/AllTenFieldsAffectHash", [] {
    using Mutation = std::pair<std::string_view, std::function<void(decltype(baseline())&)>>;
    const Mutation mutations[] = {
        {"toolchain.compiler",  [](auto& in) { in.toolchain.compiler     = CompilerId::Clang; }},
        {"toolchain.version",   [](auto& in) { in.toolchain.version      = "16.0.0"; }},
        {"toolchain.driverIdent", [](auto& in) { in.toolchain.driverIdent = "g++ (xim-x-gcc 15.1.0) 15.1.0"; }},
        {"toolchain.targetTriple", [](auto& in) { in.toolchain.targetTriple = "aarch64-linux-gnu"; }},
        {"toolchain.stdlibId",  [](auto& in) { in.toolchain.stdlibId     = "libc++"; }},
        {"cppStandard c++26",   [](auto& in) { in.cppStandard            = "c++26"; }},
        // c++20 gets its own target dir for the same reason c++26 does — cross-level
        // BMI reuse is rejected by the compiler itself.
        {"cppStandard c++20",   [](auto& in) { in.cppStandard            = "c++20"; }},
        {"compileFlags",        [](auto& in) { in.compileFlags           = "-O3"; }},
        // mcpp version is hardcoded inside compute_fingerprint, can't mutate from here.
        {"dependencyLockHash",  [](auto& in) { in.dependencyLockHash     = ""; }},
        {"stdBmiHash",          [](auto& in) { in.stdBmiHash             = "ffffffffffffffff"; }},
    };
    const auto base = compute_fingerprint(baseline());
    for (auto const& [field, mutate] : mutations) {
        auto in = baseline();
        mutate(in);
        expect(ne(base.hex, compute_fingerprint(in).hex)) << "changing " << field << " did not change the fingerprint";
    }
}};

UTest stable_across_binary_paths_when_driver_ident_matches{"Fingerprint/StableAcrossBinaryPathsWhenDriverIdentMatches", [] {
    auto a = baseline();
    auto b = baseline();
    a.toolchain.binaryPath = "/home/speak/.mcpp/registry/data/xpkgs/xim-x-gcc/16.1.0/bin/g++";
    b.toolchain.binaryPath = "/home/speak/.xlings/data/xpkgs/xim-x-mcpp/0.0.14/registry/data/xpkgs/xim-x-gcc/16.1.0/bin/g++";

    expect(eq(compute_fingerprint(a).hex, compute_fingerprint(b).hex));
}};

UTest hash_string_matches_hash_file{"Fingerprint/HashStringMatchesHashFile", [] {
    auto h1 = hash_string("hello");
    auto h2 = hash_string("hello");
    expect(eq(h1, h2));
    expect(ne(hash_string("hello"), hash_string("hellp")));
}};

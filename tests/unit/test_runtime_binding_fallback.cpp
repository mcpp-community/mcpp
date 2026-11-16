// When nothing declares the runtime, how far may mcpp go on its own?
//
// The rule this replaced asked a directory for "the glibc" and took the first
// entry readdir yielded. That is a CHOICE made by something with no bearing on
// what the artifact loads, and it went wrong the moment a dependency's
// `xim:glibc@>=2.38` floor installed a second payload: the compile side took
// 2.44 while the interpreter still named 2.39.
//
// So the axis is not "may mcpp look at the payload directory" -- it is
// "is there anything to choose between". One payload is an answer; two are a
// question only the subos can settle.
//
// This mattered because the compatibility sources (gcc's specs, clang's cfg,
// the compiler's PT_INTERP) are all things some earlier mechanism wrote, and a
// machine can legitimately have none of them.


import std;
import mcpplibs.mtest;
import mcpp.toolchain.post_install;

using namespace mcpplibs::mtest;

namespace tc = mcpp::toolchain;

namespace {

struct Home {
    std::filesystem::path root, compiler;
    explicit Home(std::initializer_list<const char*> glibcVersions) {
        root = std::filesystem::temp_directory_path()
             / std::format("mcpp_binding_{}", std::random_device{}());
        auto xpkgs = root / "data" / "xpkgs";
        compiler = xpkgs / "xim-x-gcc" / "16.1.0" / "bin" / "g++";
        std::filesystem::create_directories(compiler.parent_path());
        std::ofstream(compiler) << "not an elf";
        for (auto v : glibcVersions)
            std::filesystem::create_directories(
                xpkgs / "xim-x-glibc" / v / "lib64");
    }
    ~Home() { std::error_code ec; std::filesystem::remove_all(root, ec); }
};

UTest one_payload_is_an_answer{"RuntimeBindingFallback/OnePayloadIsAnAnswer", [] {
    Home h{"2.39"};
    expect(eq(tc::baked_runtime_binding(h.compiler), "glibc@2.39"));
}};

// The incident, in one assertion. Two payloads and nothing declaring which:
// silence, not a coin flip.
UTest two_payloads_is_silence{"RuntimeBindingFallback/TwoPayloadsIsSilence", [] {
    Home h{"2.39", "2.44"};
    expect(eq(tc::baked_runtime_binding(h.compiler), ""));
}};

UTest no_payload_is_silence{"RuntimeBindingFallback/NoPayloadIsSilence", [] {
    Home h{};
    expect(eq(tc::baked_runtime_binding(h.compiler), ""));
}};

// A compiler outside any xpkgs tree (a system gcc) has no payload set to read,
// and must not acquire one from somewhere else.
UTest non_payload_compiler_gets_nothing{"RuntimeBindingFallback/NonPayloadCompilerGetsNothing", [] {
    expect(eq(tc::baked_runtime_binding("/usr/bin/g++"), ""));
}};

UTest empty_path_is_silence{"RuntimeBindingFallback/EmptyPathIsSilence", [] {
    expect(eq(tc::baked_runtime_binding({}), ""));
}};

// A record that names a payload which is no longer installed.
//
// specs, cfg and PT_INTERP are all RECORDS of a past state: written at some
// install or some fixup, and never revisited when the payload underneath is
// replaced. CI hit exactly this -- a record said glibc@2.39 while the only
// payload on disk was 2.44 -- and because the probe matches exactly and never
// falls back, nothing resolved, no loader reached the link line, and the
// artifact took the host's. A fossil naming something absent is not an
// authority; it is just old, and the resolution must carry on past it.
struct HomeWithSpecs {
    std::filesystem::path root, compiler;
    HomeWithSpecs(std::initializer_list<const char*> glibcVersions,
                  std::string_view specsNames) {
        root = std::filesystem::temp_directory_path()
             / std::format("mcpp_fossil_{}", std::random_device{}());
        auto xpkgs = root / "data" / "xpkgs";
        auto gccRoot = xpkgs / "xim-x-gcc" / "16.1.0";
        compiler = gccRoot / "bin" / "g++";
        std::filesystem::create_directories(compiler.parent_path());
        std::ofstream(compiler) << "not an elf";
        for (auto v : glibcVersions)
            std::filesystem::create_directories(xpkgs / "xim-x-glibc" / v / "lib64");
        auto sd = gccRoot / "lib" / "gcc" / "x86_64-linux-gnu" / "16.1.0";
        std::filesystem::create_directories(sd);
        std::ofstream(sd / "specs")
            << "*link:\n%{!static:--dynamic-linker " << xpkgs.string()
            << "/xim-x-glibc/" << specsNames << "/lib64/ld-linux-x86-64.so.2}\n";
    }
    ~HomeWithSpecs() { std::error_code ec; std::filesystem::remove_all(root, ec); }
};

UTest record_naming_an_absent_payload_is_skipped{"RuntimeBindingFallback/RecordNamingAnAbsentPayloadIsSkipped", [] {
    HomeWithSpecs h{{"2.44"}, "2.39"};
    // Not "glibc@2.39": that payload is gone. The singleton rule answers.
    expect(eq(tc::baked_runtime_binding(h.compiler), "glibc@2.44"));
}};

// Linux only, and not as an exemption -- the mechanism does not exist
// elsewhere. detect_baked_loader parses GCC specs, whose loader paths are
// POSIX and absolute, and its character whitelist deliberately excludes `\`
// and `:` so that a `%{...}` spec body is never swallowed. A Windows temp
// directory is `C:\Users\...`, so this fixture cannot be spelled there at
// all: the scan stops at the first backslash and the result does not start
// with `/`. Rightly so -- there are no glibc payloads or ld-linux loaders on
// Windows for it to find.
//
// The other cases in this file are platform-neutral (they exercise the
// payload set, not the parser) and keep running everywhere.
UTest record_naming_a_present_payload_wins{"RuntimeBindingFallback/RecordNamingAPresentPayloadWins", [] {
    if constexpr (!std::filesystem::path::preferred_separator ||
                  std::filesystem::path::preferred_separator != '/')
        skip("GCC specs loader paths are POSIX; no such record here");
    HomeWithSpecs h{{"2.39", "2.44"}, "2.39"};
    // Two installed, so the singleton rule cannot answer -- but the record
    // names one that IS there, and that is what the artifact would load.
    expect(eq(tc::baked_runtime_binding(h.compiler), "glibc@2.39"));
}};

UTest stale_record_with_nothing_to_fall_back_on_is_silence{"RuntimeBindingFallback/StaleRecordWithNothingToFallBackOnIsSilence", [] {
    HomeWithSpecs h{{"2.39", "2.44"}, "2.28"};
    expect(eq(tc::baked_runtime_binding(h.compiler), ""));
}};

}  // namespace

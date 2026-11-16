
import std;
import mcpplibs.mtest;
import mcpp.toolchain.linkmodel;
import mcpp.toolchain.model;

using namespace mcpplibs::mtest;

// The toolchain link model is the single resolver for "how do we compile and
// link against this toolchain's C library" (issue #195 / the hermetic link
// model design doc). These tests pin its three contracts:
//   1. loader names come from data (triple map → glob), never a
//      hardcoded x86_64 string;
//   2. the payload link flags include -B (CRT discovery — the driver never
//      consults -L for Scrt1.o/crti.o/crtn.o);
//   3. mode selection mirrors the historical flags.cppm precedence.

namespace {

namespace tc = mcpp::toolchain;

struct Tmp {
    std::filesystem::path path;
    Tmp() {
        path = std::filesystem::temp_directory_path()
             / std::format("mcpp_linkmodel_test_{}", std::random_device{}());
        std::filesystem::create_directories(path);
    }
    ~Tmp() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

void touch(const std::filesystem::path& p) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p) << "x";
}

std::string ident(const std::filesystem::path& p) { return p.string(); }

UTest triple_map{"LoaderFilename/TripleMap", [] {
    expect(eq(tc::loader_filename("x86_64-unknown-linux-gnu"), "ld-linux-x86-64.so.2"));
    expect(eq(tc::loader_filename("aarch64-linux-gnu"), "ld-linux-aarch64.so.1"));
    expect(eq(tc::loader_filename("x86_64-linux-musl"), "ld-musl-x86_64.so.1"));
    expect(eq(tc::loader_filename("aarch64-linux-musl"), "ld-musl-aarch64.so.1"));
    expect(eq(tc::loader_filename("wasm32-unknown-unknown"), ""));
}};

UTest lsb_layout_per_arch{"DistroLoaderPath/LsbLayoutPerArch", [] {
    expect(eq(tc::distro_loader_path("x86_64-linux-gnu"), "/lib64/ld-linux-x86-64.so.2"));
    expect(eq(tc::distro_loader_path("aarch64-linux-gnu"), "/lib/ld-linux-aarch64.so.1"));
    expect(eq(tc::distro_loader_path("mips64-unknown"), ""));
}};

UTest triple_map_hit{"ResolveLoader/TripleMapHit", [] {
    Tmp dir;
    auto lib = dir.path / "lib64";
    touch(lib / "ld-linux-x86-64.so.2");
    expect(eq(tc::resolve_loader(lib, "x86_64-unknown-linux-gnu"), lib / "ld-linux-x86-64.so.2"));
}};

UTest glob_fallback_for_unknown_triple{"ResolveLoader/GlobFallbackForUnknownTriple", [] {
    Tmp dir;
    auto lib = dir.path / "lib";
    touch(lib / "ld-linux-aarch64.so.1");
    auto got = tc::resolve_loader(lib, "");  // no triple info at all
    expect(eq(got, lib / "ld-linux-aarch64.so.1"));
}};

UTest empty_when_nothing_found{"ResolveLoader/EmptyWhenNothingFound", [] {
    Tmp dir;
    auto lib = dir.path / "lib64";
    std::filesystem::create_directories(lib);
    expect(tc::resolve_loader(lib, "x86_64-linux-gnu").empty());
}};

UTest prefers_lib64_then_lib{"PayloadLibDir/PrefersLib64ThenLib", [] {
    Tmp dir;
    touch(dir.path / "lib" / "ld-linux-x86-64.so.2");
    expect(eq(tc::payload_lib_dir_with_loader(dir.path, "x86_64-linux-gnu"), dir.path / "lib"));
    touch(dir.path / "lib64" / "ld-linux-x86-64.so.2");
    expect(eq(tc::payload_lib_dir_with_loader(dir.path, "x86_64-linux-gnu"), dir.path / "lib64"));
}};

// Fabricate a bundled-LLVM-style toolchain: bin/clang++ + bin/clang++.cfg,
// libc++ headers, and a glibc payload.
struct FakeClangWithPayload {
    Tmp dir;
    tc::Toolchain t;
    std::filesystem::path glibcLib;
    FakeClangWithPayload() {
        auto llvm = dir.path / "llvm";
        touch(llvm / "bin" / "clang++");
        touch(llvm / "bin" / "clang++.cfg");
        touch(llvm / "include" / "c++" / "v1" / "version");
        auto glibc = dir.path / "glibc";
        glibcLib = glibc / "lib64";
        touch(glibc / "include" / "features.h");
        touch(glibcLib / "Scrt1.o");
        touch(glibcLib / "ld-linux-x86-64.so.2");
        t.compiler = tc::CompilerId::Clang;
        t.binaryPath = llvm / "bin" / "clang++";
        t.targetTriple = "x86_64-unknown-linux-gnu";
        t.base.payload = tc::PayloadPaths{
            .glibcInclude = glibc / "include",
            .glibcLib = glibcLib,
            .linuxInclude = {},
        };
    }
};

UTest clang_cfg_payload_first_carries_crt_discovery{"LinkModel/ClangCfgPayloadFirstCarriesCrtDiscovery", [] {
    FakeClangWithPayload fx;
    auto lm = tc::resolve_link_model(fx.t);
    expect(eq(lm.mode, tc::CLibMode::PayloadFirst));
    expect(bool(lm.clangWithCfg));
    expect(eq(lm.crtDir, fx.glibcLib));
    expect(eq(lm.loader, fx.glibcLib / "ld-linux-x86-64.so.2"));

    auto link = lm.link_flags(ident);
    // -B is the #195 fix: CRT objects resolve via -B, never -L.
    expect(ne(link.find(" -B" + fx.glibcLib.string()), std::string::npos));
    expect(ne(link.find(" -L" + fx.glibcLib.string()), std::string::npos));
    expect(ne(link.find("--dynamic-linker=" + lm.loader.string()), std::string::npos));

    auto compile = lm.compile_flags(ident);
    expect(ne(compile.find("-nostdlibinc"), std::string::npos));
    expect(ne(compile.find("-isystem"), std::string::npos));
    expect(eq(compile.find("-idirafter"), std::string::npos));
    // `-nostdlibinc` already closes clang's host search; no -isysroot.
    expect(eq(compile.find("-isysroot"), std::string::npos));
}};

UTest clang_driver_model_exposes_cfg_and_headers{"LinkModel/ClangDriverModelExposesCfgAndHeaders", [] {
    FakeClangWithPayload fx;
    auto dm = tc::resolve_clang_driver(fx.t);
    expect(bool(dm.hasCfg));
    expect(fatal(!dm.cxxIncludes.empty()));
    expect(ne(dm.compile_flags(ident).find("--no-default-config"), std::string::npos));
}};

UTest gcc_sysroot_wins_over_payload{"LinkModel/GccSysrootWinsOverPayload", [] {
    Tmp dir;
    auto sysroot = dir.path / "sysroot";
    touch(sysroot / "usr" / "include" / "stdlib.h");
    touch(sysroot / "usr" / "include" / "linux" / "limits.h");
    tc::Toolchain t;
    t.compiler = tc::CompilerId::GCC;
    t.binaryPath = dir.path / "bin" / "g++";
    t.targetTriple = "x86_64-linux-gnu";
    t.base.root = sysroot;
    t.base.payload = tc::PayloadPaths{
        .glibcInclude = dir.path / "glibc" / "include",
        .glibcLib = dir.path / "glibc" / "lib64",
        .linuxInclude = dir.path / "linux" / "include",
    };
    auto lm = tc::resolve_link_model(t);
    expect(eq(lm.mode, tc::CLibMode::Sysroot));
    expect(ne(lm.compile_flags(ident).find("--sysroot="), std::string::npos));
    expect(eq(lm.compile_flags(ident).find("-nostdlibinc"), std::string::npos));
    // --sysroot already replaces the recorded sysroot.
    expect(eq(lm.compile_flags(ident).find("-isysroot"), std::string::npos));
    expect(ne(lm.link_flags(ident).find("--sysroot="), std::string::npos));
    // Kernel headers exist in the sysroot → no supplement.
    expect(lm.systemIncludes.empty());
}};

UTest gcc_sysroot_supplements_missing_kernel_headers{"LinkModel/GccSysrootSupplementsMissingKernelHeaders", [] {
    Tmp dir;
    auto sysroot = dir.path / "sysroot";
    touch(sysroot / "usr" / "include" / "stdlib.h");  // no linux/limits.h
    tc::Toolchain t;
    t.compiler = tc::CompilerId::GCC;
    t.targetTriple = "x86_64-linux-gnu";
    t.base.root = sysroot;
    t.base.payload = tc::PayloadPaths{
        .glibcInclude = dir.path / "glibc" / "include",
        .glibcLib = dir.path / "glibc" / "lib64",
        .linuxInclude = dir.path / "linux" / "include",
    };
    auto lm = tc::resolve_link_model(t);
    expect(fatal(eq(lm.systemIncludes.size(), 1u)));
    // Sysroot-mode supplement renders as -isystem even for GCC.
    expect(ne(lm.compile_flags(ident).find("-isystem"), std::string::npos));
}};

UTest gcc_payload_emits_idirafter_and_its_own_addressing{"LinkModel/GccPayloadEmitsIdirafterAndItsOwnAddressing", [] {
    Tmp dir;
    auto glibcLib = dir.path / "glibc" / "lib64";
    touch(glibcLib / "ld-linux-x86-64.so.2");
    tc::Toolchain t;
    t.compiler = tc::CompilerId::GCC;
    t.targetTriple = "x86_64-linux-gnu";
    t.base.payload = tc::PayloadPaths{
        .glibcInclude = dir.path / "glibc" / "include",
        .glibcLib = glibcLib,
        .linuxInclude = {},
    };
    auto lm = tc::resolve_link_model(t);
    expect(eq(lm.mode, tc::CLibMode::PayloadFirst));
    // Headers still differ by driver: libstdc++'s #include_next wrappers need
    // -idirafter, and that has not changed.
    expect(ne(lm.compile_flags(ident).find("-idirafter"), std::string::npos));
    expect(eq(lm.compile_flags(ident).find("-nostdlibinc"), std::string::npos));

    // The sysroot GCC recorded when it was built is replaced, for headers
    // only, by the C library payload's root: `-isysroot <root>` as two argv
    // words, so `<recorded>/usr/include` is never searched.
    auto tokens = lm.compile_tokens(ident);
    auto it = std::ranges::find(tokens, std::string("-isysroot"));
    expect(fatal(ne(it, tokens.end())));
    expect(fatal(ne(std::next(it), tokens.end())));
    expect(eq(*std::next(it), (dir.path / "glibc").string()));

    // Addressing no longer differs. GCC used to be left to its install-time
    // specs here, which made the RUN side a per-toolchain-install decision
    // while the COMPILE side stayed per-build -- and the two named different
    // glibc versions as soon as a second one was installed. mcpp already
    // refuses to depend on clang's install-time cfg for exactly this reason.
    auto link = lm.link_flags(ident);
    expect(ne(link.find(" -B"), std::string::npos));
    expect(ne(link.find("--dynamic-linker=" + lm.loader.string()), std::string::npos));
    expect(ne(link.find("-Wl,-rpath," + glibcLib.string()), std::string::npos));
}};

UTest nothing_usable_yields_none_and_empty_flags{"LinkModel/NothingUsableYieldsNoneAndEmptyFlags", [] {
    tc::Toolchain t;
    t.compiler = tc::CompilerId::Clang;
    t.targetTriple = "x86_64-linux-gnu";
    auto lm = tc::resolve_link_model(t);
    expect(eq(lm.mode, tc::CLibMode::None));
    expect(lm.compile_flags(ident).empty());
    expect(lm.link_flags(ident).empty());
}};


// 关于 `find_sibling_tool` 的顺序:**不在这里测**。
//
// 它的注释曾写着 "Return the first (highest) version dir",而代码只是取
// directory_iterator 的第一项 —— 没有排序。为它写一条断言的尝试失败了:
// 结果取决于 readdir 顺序,所以那条测试在本机通过、在别的机器上可能失败,
// **通过和失败都不说明问题**。
//
// 正确的做法不是把不确定性钉住,而是让它不再被用于选版本:版本由权威
// (`--runtime` / subos 的 `subos_info.runtime`)给出,`find_sibling_tool`
// 只负责定位包根。确定性的判据在 test_toolchain_probe.cpp —— 给定
// `glibc@2.39`,即使 home 里同时有 2.44,也必须返回 2.39。
//
// 注释本身已改为陈述事实(顺序未定义,调用方不得依赖)。
// 设计:.agents/docs/2026-08-08-payload-version-and-contract-drift-design.md §3.2

}  // namespace

// ─── An SDK target gets CLibMode::None, and the gate is in the MODEL ───────
//
// The C-runtime group reaches the link line through TWO channels -- flags.cppm's
// `link_toolchain_flags` and its `payload_ld` -- both rendering
// `lm.link_flags()`. The comment at the second one records that a reader who
// fixed only the first "saw the identical error and could reasonably conclude
// the fix had not worked". So the gate belongs here, where both read it.
//
// Measured on `--target wasm32-emscripten` with the compile side already
// correct: `wasm-ld: error: unknown argument:
// --dynamic-linker=<glibc>/lib64/ld-linux-x86-64.so.2` -- this host's loader
// handed to a WebAssembly linker.
UTest an_sdk_target_describes_no_c_library{"LinkModel/AnSdkTargetDescribesNoCLibrary", [] {
    for (auto target : {"wasm32-emscripten", "aarch64-linux-android",
                        "x86_64-linux-android"}) {
        auto tc = mcpp::toolchain::Toolchain{};
        tc.compiler     = mcpp::toolchain::CompilerId::Clang;
        tc.targetTriple = target;
        auto lm = mcpp::toolchain::resolve_link_model(tc);
        expect(eq(lm.mode, mcpp::toolchain::CLibMode::None)) << target;
        expect(lm.libDirs.empty()) << target;
        expect(lm.crtDir.empty()) << target;
        // And the rendered flags are empty, which is what both channels emit.
        expect(lm.link_flags([](const std::filesystem::path& p) {
            return p.string(); }).empty()) << target;
    }
}};

// ─── Clang on the MSVC ABI: the toolset and SDK, said to the driver ───────

namespace {
tc::Toolchain clang_msvc() {
    tc::Toolchain t;
    t.compiler          = tc::CompilerId::Clang;
    t.targetTriple      = "x86_64-pc-windows-msvc";
    t.base.msvc.toolsDir      = "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207";
    t.base.msvc.sdkRoot    = "C:/Program Files (x86)/Windows Kits/10";
    t.base.msvc.sdkVersion = "10.0.26100.0";
    return t;
}
} // namespace

UTest clang_on_the_msvc_abi_names_its_toolset_and_sdk{"LinkModel/ClangOnTheMsvcAbiNamesItsToolsetAndSdk", [] {
    auto t = clang_msvc();
    auto words = tc::resolve_link_model(t).msvc_driver_tokens(ident);
    const std::vector<std::string> expected = {
        "-Xmicrosoft-visualc-tools-root", t.base.msvc.toolsDir.string(),
        "-Xmicrosoft-windows-sdk-root",   t.base.msvc.sdkRoot.string(),
        "-Xmicrosoft-windows-sdk-version", "10.0.26100.0",
    };
    expect(eq(words, expected)) << "separate words: the driver rejects the `=` form";
    // Not a C-library mode: the other token sets stay empty.
    auto lm = tc::resolve_link_model(t);
    expect(eq(lm.mode, tc::CLibMode::None));
    expect(lm.compile_tokens(ident).empty());
}};

// mcpp#746. The compiler version travels with the toolset: two runner images
// held different cl.exe builds under one toolset directory name, and a key
// that saw only the directory served one image's std module to the other.
UTest the_compiler_version_is_said_with_the_toolset{"LinkModel/TheCompilerVersionIsSaidWithTheToolset", [] {
    auto t = clang_msvc();
    t.base.msvc.compilerVersion = "19.44.35211";
    auto words = tc::resolve_link_model(t).msvc_driver_tokens(ident);
    expect(fatal(!words.empty()));
    expect(eq(words.back(), "-fms-compatibility-version=19.44.35211"));
    // A second build of cl.exe under the same directory is a different word.
    auto other = t;
    other.base.msvc.compilerVersion = "19.44.35217";
    expect(ne(tc::resolve_link_model(other).msvc_driver_tokens(ident), words));
}};

UTest the_toolset_without_an_sdk_omits_the_sdk_words{"LinkModel/TheToolsetWithoutAnSdkOmitsTheSdkWords", [] {
    auto t = clang_msvc();
    t.base.msvc.sdkRoot.clear();
    t.base.msvc.sdkVersion.clear();
    auto words = tc::resolve_link_model(t).msvc_driver_tokens(ident);
    expect(fatal(eq(words.size(), 2u)));
    expect(eq(words[0], "-Xmicrosoft-visualc-tools-root"));
}};

UTest no_other_row_receives_msvc_driver_words{"LinkModel/NoOtherRowReceivesMsvcDriverWords", [] {
    // cl.exe carries the toolset in its own path and envOverrides; MinGW is
    // not MSVC at all; a row with no toolset recorded says nothing.
    auto cl = clang_msvc();
    cl.compiler = tc::CompilerId::MSVC;
    expect(tc::resolve_link_model(cl).msvc_driver_tokens(ident).empty());

    auto mingw = clang_msvc();
    mingw.targetTriple = "x86_64-w64-mingw32";
    expect(tc::resolve_link_model(mingw).msvc_driver_tokens(ident).empty());

    auto unbound = clang_msvc();
    unbound.base.msvc.toolsDir.clear();
    expect(tc::resolve_link_model(unbound).msvc_driver_tokens(ident).empty());
}};

import std;
import mcpplibs.mtest;
import mcpp.toolchain.model;

using namespace mcpplibs::mtest;

// Whether a compile writes its outputs below an attempt of its own (design
// 2026-11-19 §10.6) is a fact of the toolchain: GCC's by its own number, a
// clang driver's by what it answered at detection, and never a CodeView
// target's, whose objects record their path.

namespace {
mcpp::toolchain::Toolchain toolchain(mcpp::toolchain::CompilerId id, std::string version,
                                     std::string triple = "x86_64-linux-gnu") {
    mcpp::toolchain::Toolchain tc;
    tc.compiler = id;
    tc.version = std::move(version);
    tc.targetTriple = std::move(triple);
    return tc;
}
}  // namespace

UTest gcc_relocates_from_its_auxiliary_naming_change{"OutputRelocation/GccRelocatesFromItsAuxiliaryNamingChange", [] {
    using mcpp::toolchain::CompilerId;
    const auto gcc16 = mcpp::toolchain::output_relocation(toolchain(CompilerId::GCC, "16.1.0"));
    expect(gcc16.relocatable);
    expect(eq(gcc16.auxNaming, std::string_view("dumpbase")));
    expect(!mcpp::toolchain::output_relocation(toolchain(CompilerId::GCC, "10.5.0")).relocatable);
}};

UTest a_clang_driver_relocates_when_it_takes_dumpdir{"OutputRelocation/AClangDriverRelocatesWhenItTakesDumpdir", [] {
    using mcpp::toolchain::CompilerId;
    // The driver's number does not decide: a driver numbered 6 (emcc 6.0.9)
    // takes the word, and the answer is what was asked of it.
    auto emcc = toolchain(CompilerId::Clang, "6.0.9", "wasm32-unknown-emscripten");
    expect(!mcpp::toolchain::output_relocation(emcc).relocatable);
    emcc.takesDumpdir = true;
    const auto relocated = mcpp::toolchain::output_relocation(emcc);
    expect(relocated.relocatable);
    expect(eq(relocated.auxNaming, std::string_view("dumpdir")));
}};

UTest a_codeview_target_writes_in_place{"OutputRelocation/ACodeViewTargetWritesInPlace", [] {
    using mcpp::toolchain::CompilerId;
    auto clang = toolchain(CompilerId::Clang, "23.1.3", "x86_64-pc-windows-msvc");
    clang.takesDumpdir = true;
    expect(!mcpp::toolchain::output_relocation(clang).relocatable);
    expect(!mcpp::toolchain::output_relocation(toolchain(CompilerId::MSVC, "19.44", "x86_64-pc-windows-msvc"))
                .relocatable);
}};

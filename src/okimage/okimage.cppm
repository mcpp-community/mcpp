// mcpp.okimage — what an ok image IS, for the target `<arch>-openkal`
// (openkal SPEC clause 12; design 2026-10-26 part 2, §8.5).
//
// One artifact per architecture, which a loader (`okrun`) runs on every system
// of that architecture. The facts below are the convention's, stated once:
// the compile flags that keep an image runnable everywhere, the link that
// produces the shape the loader accepts, and the note the loader reads. The
// format itself -- its reader, its checker, its signature -- is libokld's, and
// the engine reaches it through `okld`, a program of the dependency graph,
// never through a second implementation here.
//
// WHAT IS NOT HERE: the C library and the C++ run-time inside an image. They
// are packages (`provides = ["mcpp:c-abi=…"]`), and an image with neither is
// legal: its entry is its own `ok_main`.

export module mcpp.okimage;

import std;

export namespace mcpp::okimage {

// The entry of an executable image (SPEC 12.2): `int ok_main(void)`, in the
// image's convention, whose value is the image's status. A C library inside
// the image defines it and calls `main` with `kal_env_arg`'s arguments.
inline constexpr std::string_view kEntry = "ok_main";

// The programs of the graph the engine runs for this target.
inline constexpr std::string_view kTool   = "okld";    // note, check
inline constexpr std::string_view kRunner = "okrun";   // `mcpp run`, `mcpp test`

// FLAGS EVERY TRANSLATION UNIT OF AN IMAGE CARRIES (SPEC 12.2).
//
//   -fPIC              the image is ET_DYN and has no text relocation;
//   -femulated-tls     a variable is found through `__emutls_get_address`,
//                      which the image supplies: one system does not let a
//                      program set the thread pointer, another does not let it
//                      change it;
//   -ffixed-x18        (AArch64) two systems reserve the platform register;
//   -mno-outline-atomics
//                      (AArch64) the outlined helpers read the processor's
//                      features through an operating system's interface;
//   -fvisibility=hidden
//                      an image exports what its source marks
//                      `visibility("default")` and nothing else, as the
//                      reference build does: everything else is the image's
//                      own, so the link can drop what nothing reaches, and the
//                      requirements note names only the interfaces the program
//                      uses, not every one its C library could.
//   -fasynchronous-unwind-tables
//                      every frame, C's included, can be unwound: a C++
//                      exception crosses the C of the unwinder itself
//                      (`_Unwind_RaiseException`) and of the C library. The
//                      hosted triples default to it; `<arch>-unknown-none-elf`
//                      does not -- measured, `_Unwind_Backtrace` in an image
//                      over openkal-llvm-runtime found no frame at all, and
//                      every `throw` terminated.
inline std::vector<std::string> compile_flags(std::string_view arch) {
    std::vector<std::string> out{ "-fPIC", "-femulated-tls", "-fvisibility=hidden",
                                  "-fasynchronous-unwind-tables" };
    if (arch == "aarch64") {
        out.emplace_back("-ffixed-x18");
        out.emplace_back("-mno-outline-atomics");
    }
    return out;
}

// THE LINK, BY `ld.lld` ITSELF (SPEC 12.2). Position-independent, every
// loadable segment aligned to 64 KiB (the largest protection granularity among
// the systems), bound now, no executable stack, both hash tables (a loader may
// use either), what nothing reaches discarded, and the unwind index an
// unwinder finds through `dl_iterate_phdr`.
//
// Measured, a C program over openkal-musl before `--gc-sections` and hidden
// visibility: the whole C library was in the image and exported, and `okld
// check` refused it for importing `__mulxc3` -- from complex arithmetic the
// program never called.
inline std::string link_flags() {
    return " -shared -z max-page-size=65536 -z now -z noexecstack --hash-style=both"
           " --gc-sections --eh-frame-hdr";
}

// The notes every image carries (SPEC 12.4): the requirements, which `okld
// note` writes from the image's strong imports after the link, and the
// signature, all zero until `okld sign`. The same text `okld note-source`
// prints.
inline std::string_view note_source() {
    return
        "# The two notes of an ok image (openkal SPEC 12.4), written by mcpp.\n"
        "\t.section .note.openkal,\"a\",@note\n"
        "\t.p2align 2\n"
        "\t.long 8, 16, 1\n"
        "\t.asciz \"OPENKAL\"\n"
        "\t.long 1, 0\n"
        "\t.quad 0\n"
        "\t.p2align 2\n"
        "\t.long 8, 96, 2\n"
        "\t.asciz \"OPENKAL\"\n"
        "\t.fill 96, 1, 0\n";
}

// The diagnostic for a build of this target with no `okld` to finish it. It
// names the dependency that supplies both programs, and where to write it.
inline std::string no_tool_message(std::string_view triple, std::string_view tool) {
    return std::format(
        "'{}' builds ok images, and `{}` -- the format's tool, which writes an "
        "image's requirements note and checks the convention -- is not provided.\n"
        "       Declare the package that builds it for this machine:\n"
        "\n"
        "           [build-dependencies]\n"
        "           openkal-tools = {{ version = \"0.1.0\", tools = [\"okld\", \"okrun\"] }}\n"
        "\n"
        "       `okrun` is then the runner of `mcpp run` and `mcpp test` for this target.",
        triple, tool);
}

} // namespace mcpp::okimage

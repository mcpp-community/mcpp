// mcpp.build.loader_contract — which dynamic tag an artifact must carry, and
// how each producer spells it.
//
// THE RULE
//
//   executable      DT_RPATH
//   shared library  DT_RUNPATH
//
// and it is measured, not stylistic. DT_RUNPATH is consulted only for the
// object that carries it and for the dlopen() that object performs itself;
// DT_RPATH is consulted for every dlopen anywhere in the process, at any depth.
// A GL program reaches its driver through three to four dlopen() calls it does
// not make — libGLX.so.0 / libEGL.so.1 make them on its behalf — so an
// executable tagged DT_RUNPATH has the right path and cannot reach through it.
// Flipping only the tag, with identical paths, moves egl / gles2 /
// egl-surfaceless from llvmpipe to the GPU.
//
// The second half is equally measured and runs the other way: forcing DT_RPATH
// onto a LIBRARY is harmful. Transitivity pushes that library's search path
// into every lookup below it, and eglInitialize fails outright
// (openxlings/xlings#593). So this is a split, not a flip.
//
// WHY A MODULE
//
// Three producers must agree: the linker command line (dev builds), patchelf
// (`mcpp pack`), and the checker that reads the result back. Spelling the rule
// once is the point — a per-recipe decision is exactly how the ecosystem got
// 1 correct executable out of 73, with the one author who found the problem
// having no way to carry it to the other 72.
//
// This module does not parse ELF. `mcpp.platform.elf_runtime` reads the bytes;
// this decides what they should have said. Non-ELF formats get
// `NotApplicable`, so Mach-O and PE need no branches in the callers.

export module mcpp.build.loader_contract;

import std;
import mcpp.platform.elf_runtime;

export namespace mcpp::build::loader {

// What the artifact IS. Decided by PT_INTERP, never by ELF type: a PIE
// executable is ET_DYN and therefore type-identical to a shared library.
enum class Form { Executable, SharedLibrary, NotElf };

// What it must carry.
enum class RequiredTag { Rpath, Runpath, NotApplicable };

RequiredTag required_tag(Form form) {
    switch (form) {
        case Form::Executable:    return RequiredTag::Rpath;
        case Form::SharedLibrary: return RequiredTag::Runpath;
        case Form::NotElf:        return RequiredTag::NotApplicable;
    }
    return RequiredTag::NotApplicable;
}

// The linker flag that produces `tag`, or nothing when the default already
// does.
//
// MUST BE APPENDED AFTER every other linker argument. GCC specs and clang
// config files hand ld `--enable-new-dtags`, and the last occurrence wins.
// xlings' first linker wrapper put its arguments first, the tag stayed
// DT_RUNPATH, and the only entry point exercised at the time (GLX) worked
// anyway — so the bug shipped looking fixed.
std::optional<std::string_view> link_flag(RequiredTag tag) {
    switch (tag) {
        // GNU ld and lld both default to --enable-new-dtags (DT_RUNPATH) on
        // every distribution mcpp targets, so the executable case is the one
        // that needs saying.
        case RequiredTag::Rpath:          return "-Wl,--disable-new-dtags";
        case RequiredTag::Runpath:        return std::nullopt;
        case RequiredTag::NotApplicable:  return std::nullopt;
    }
    return std::nullopt;
}

// The patchelf argument that produces `tag` alongside `--set-rpath`.
//
// `patchelf --set-rpath` writes DT_RUNPATH by default, which is the same
// defect one layer later: a packaged GL program with a bundled vendor would
// carry the path and be unable to dlopen through it.
std::optional<std::string_view> patchelf_flag(RequiredTag tag) {
    switch (tag) {
        case RequiredTag::Rpath:          return "--force-rpath";
        case RequiredTag::Runpath:        return std::nullopt;
        case RequiredTag::NotApplicable:  return std::nullopt;
    }
    return std::nullopt;
}

std::string_view to_string(RequiredTag tag) {
    switch (tag) {
        case RequiredTag::Rpath:         return "DT_RPATH";
        case RequiredTag::Runpath:       return "DT_RUNPATH";
        case RequiredTag::NotApplicable: return "n/a";
    }
    return "n/a";
}

struct TagFinding {
    std::filesystem::path artifact;
    Form form = Form::NotElf;
    RequiredTag required = RequiredTag::NotApplicable;
    mcpp::platform::elf::SearchPathTag actual =
        mcpp::platform::elf::SearchPathTag::None;

    // Three-valued on purpose. `NotChecked` is not `Ok`: an artifact that
    // could not be read, or that carries no search path at all, has not been
    // shown to satisfy the contract — and a check that can only say pass or
    // fail reports "nothing wrong" for both.
    enum class Status { Ok, Violation, NotChecked };
    Status status = Status::NotChecked;

    std::string explain() const {
        using SearchPathTag = mcpp::platform::elf::SearchPathTag;
        if (status != Status::Violation) return {};
        return std::format(
            "{}: {} carries {} but the loader contract requires {} — a search "
            "path under {} is not reachable from a dlopen() performed by "
            "another object, which is how a graphics program reaches its "
            "driver (it never dlopens the vendor itself)",
            artifact.string(),
            form == Form::Executable ? "executable" : "shared library",
            mcpp::platform::elf::to_string(actual),
            to_string(required),
            mcpp::platform::elf::to_string(actual == SearchPathTag::Both
                                               ? SearchPathTag::Runpath
                                               : actual));
    }
};

// Rule E, evaluated where the artifact lands rather than in a repository
// workflow the user's machine never sees.
TagFinding check_artifact(const std::filesystem::path& artifact) {
    using SearchPathTag = mcpp::platform::elf::SearchPathTag;
    TagFinding out;
    out.artifact = artifact;

    auto facts = mcpp::platform::elf::inspect_elf_runtime(artifact);
    if (!facts) return out;                       // not ELF, or unreadable

    out.form = facts->is_executable() ? Form::Executable : Form::SharedLibrary;
    out.required = required_tag(out.form);
    out.actual = facts->searchPathTag;

    // No search path at all is not a violation: nothing was claimed, so
    // nothing is unreachable. It stays NotChecked so it can never be counted
    // as evidence that the contract holds.
    if (out.actual == SearchPathTag::None) return out;

    // `Both` reads as DT_RUNPATH because that is what the loader does with it,
    // and DT_RPATH-first is the common layout — a checker that stops at the
    // first tag it finds would call this compliant while the loader ignores
    // the tag it matched on.
    const auto effective = out.actual == SearchPathTag::Both
        ? SearchPathTag::Runpath : out.actual;
    const bool ok = (out.required == RequiredTag::Rpath
                        && effective == SearchPathTag::Rpath)
                 || (out.required == RequiredTag::Runpath
                        && effective == SearchPathTag::Runpath);
    out.status = ok ? TagFinding::Status::Ok : TagFinding::Status::Violation;
    return out;
}

} // namespace mcpp::build::loader

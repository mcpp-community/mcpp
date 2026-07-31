// mcpp — Modular C++ Package Manager & Build Tool
// Entry point — delegates to mcpp.cli command dispatch.

import std;
import mcpp.cli;
import mcpp.ui;

int main(int argc, char* argv[]) {
    // Line-buffer stdout even when it is not a TTY.
    //
    // Without this, progress output is block-buffered and the block SIZE is
    // whatever the platform's libc picked — which is not the same number
    // anywhere: musl (the Linux release linkage) hardcodes BUFSIZ = 1024 and
    // ignores st_blksize, Apple libc takes st_blksize, which for a pipe is
    // 65536, and MSVCRT uses 4096. Measured on one 97-member workspace: the
    // same ~13 KB of status output flushed 13 times on Linux and ZERO times on
    // macOS, so the macOS CI log showed only the test binaries' own output (they
    // are separate processes and flush at their own exit) and not one line of
    // mcpp's. When that run was then killed by the job timeout, the whole buffer
    // went with it — a 45-minute step with no attributable output at all.
    //
    // Lives in mcpp.ui because a non-module TU may not open a global module
    // fragment for <cstdio> (Clang rejects `module;` here; GCC accepted it).
    // ui::flush() covers the same ground for the ui layer on Windows, where
    // MSVCRT silently treats _IOLBF as _IOFBF.
    mcpp::ui::set_line_buffered();

    int rc;
    try {
        rc = mcpp::cli::run(argc, argv);
    } catch (const std::exception& e) {
        // Last-resort boundary: without it an escaped exception is
        // std::terminate — on Windows a silent 0xC0000409 that git-bash
        // reports as a bare exit 127 (mcpp#230 wore that mask). Name the
        // real error and exit with a recognizable internal-error code.
        std::println(std::cerr, "error: internal: unhandled exception: {}", e.what());
        rc = 70;   // EX_SOFTWARE
    } catch (...) {
        std::println(std::cerr, "error: internal: unhandled non-standard exception");
        rc = 70;
    }
#ifdef __APPLE__
    // With statically linked libc++ (the macOS release linkage since
    // 0.0.50), static destruction can SIGABRT on exit — same issue xlings
    // guards against. A CLI tool needs no destructor-based cleanup; skip
    // static dtors entirely. _Exit bypasses atexit handlers too, so flush
    // the standard streams explicitly first.
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(rc);
#else
    return rc;
#endif
}

// mcpp — Modular C++ Package Manager & Build Tool
// Entry point — delegates to mcpp.cli command dispatch.

import std;
import mcpp.cli;
import mcpp.ui;
import mcpp.home;
import mcpp.platform;

int main(int argc, char* argv[]) {
    // Before anything is written: `mcpp … | head` ends mcpp in order, and no
    // program mcpp starts inherits SIGPIPE ignored (part 2 design, §6.4).
    mcpp::platform::process::guard_broken_pipe();

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
    // The status line's clock and `Finished` count from here (build progress
    // design 2026-09-29, §4.4), and every line, mcpp.log's verbose records
    // included, is written through mcpp.ui from the first one on.
    mcpp::ui::mark_command_start();
    mcpp::ui::init();

    int rc;
    try {
        rc = mcpp::cli::run(argc, argv);
    } catch (const mcpp::home::Error& e) {
        mcpp::ui::error(e.what());
        rc = 4;
    } catch (const std::exception& e) {
        // Last-resort boundary: without it an escaped exception is
        // std::terminate — on Windows a silent 0xC0000409 that git-bash
        // reports as a bare exit 127 (mcpp#230 wore that mask). Name the
        // real error and exit with a recognizable internal-error code.
        mcpp::ui::error(std::format("internal: unhandled exception: {}", e.what()));
        rc = 70;   // EX_SOFTWARE
    } catch (...) {
        mcpp::ui::error("internal: unhandled non-standard exception");
        rc = 70;
    }
    // A command that returned early, on an error, may have left its live
    // region on the screen; the shell's prompt must not follow it on the
    // status line.
    mcpp::ui::close_region();
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

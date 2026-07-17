// mcpp — Modular C++ Package Manager & Build Tool
// Entry point — delegates to mcpp.cli command dispatch.

import std;
import mcpp.cli;

int main(int argc, char* argv[]) {
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

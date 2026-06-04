// mcpp — Modular C++ Package Manager & Build Tool
// Entry point — delegates to mcpp.cli command dispatch.

import std;
import mcpp.cli;

int main(int argc, char* argv[]) {
    int rc = mcpp::cli::run(argc, argv);
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

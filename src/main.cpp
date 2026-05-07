// mcpp — Modular C++ Package Manager & Build Tool
// Entry point — delegates to mcpp.cli command dispatch.

import std;
import mcpp.cli;

int main(int argc, char* argv[]) {
    return mcpp::cli::run(argc, argv);
}

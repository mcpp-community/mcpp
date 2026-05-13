// mcpp.toolchain.llvm - xlings LLVM package mapping.

export module mcpp.toolchain.llvm;

import std;

export namespace mcpp::toolchain::llvm {

bool is_alias(std::string_view compiler);
std::string package_name();
std::vector<std::string> frontend_candidates();
std::vector<std::string> list_aliases();

} // namespace mcpp::toolchain::llvm

namespace mcpp::toolchain::llvm {

bool is_alias(std::string_view compiler) {
    return compiler == "llvm" || compiler == "clang";
}

std::string package_name() {
    return "llvm";
}

std::vector<std::string> frontend_candidates() {
    return {"clang++"};
}

std::vector<std::string> list_aliases() {
    return {"llvm", "clang"};
}

} // namespace mcpp::toolchain::llvm

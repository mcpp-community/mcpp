// mcpp.toolchain.devicehost — which host compilers a device toolchain accepts.
//
// WHY THIS IS READ RATHER THAN TABULATED
//
// nvcc refuses host compilers newer than a bound that changes with every CUDA
// release, and the bound is not documentation: it is a preprocessor guard in
// the toolkit's own `crt/host_config.h`. A table transcribed into mcpp would
// be a copy of one release, correct until the next one and wrong silently
// afterwards, and it would have to grow a row for every future toolkit.
// Reading the guard means a toolkit mcpp has never heard of still answers.
//
// WHAT IT IS FOR
//
// mcpp supplies the host compiler, so it is the one build system in a position
// to know both sides of this pairing before either compiler runs. CMake
// forwards `-ccbin` and lets nvcc fail; the diagnostic then names a compiler
// the user did not choose and a bound they cannot see. Refusing earlier, with
// both versions and the bound in the message, is the whole of the benefit.
//
// The parse is deliberately narrow: two guards, no macro evaluation. A file
// this module cannot make sense of yields no bound, and no bound means the
// check does not run — an unreadable header must not invent a refusal.

export module mcpp.toolchain.devicehost;

import std;

export namespace mcpp::toolchain {

// The host-compiler bounds one device toolkit declares. Zero means "the header
// said nothing about this family", never "no version is allowed".
struct HostCompilerBounds {
    int gccMax   = 0;   // greatest accepted __GNUC__
    int clangMax = 0;   // greatest accepted clang major

    bool known() const { return gccMax != 0 || clangMax != 0; }
};

// Parse the two guards out of a `crt/host_config.h`.
HostCompilerBounds parse_host_config(std::string_view header);

// Is `major` of `family` ("gcc" | "clang") within the bounds? A family the
// header said nothing about is accepted: silence is not a refusal.
bool host_compiler_accepted(const HostCompilerBounds& b,
                            std::string_view family, int major);

} // namespace mcpp::toolchain

namespace mcpp::toolchain {

namespace {

// The first run of digits at or after `pos`, or 0.
int digits_after(std::string_view s, std::size_t pos) {
    while (pos < s.size() && !std::isdigit(static_cast<unsigned char>(s[pos]))) {
        // Stop at a line break: a number on the next line belongs to another
        // statement, and reading across one is how a parse this narrow would
        // start inventing answers.
        if (s[pos] == '\n') return 0;
        ++pos;
    }
    int v = 0, n = 0;
    while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
        v = v * 10 + (s[pos] - '0');
        ++pos; ++n;
    }
    return n == 0 ? 0 : v;
}

} // namespace

HostCompilerBounds parse_host_config(std::string_view header) {
    HostCompilerBounds b;

    // `#if __GNUC__ > 12` guards the "gcc versions later than 12" error, so
    // the greatest accepted major is the operand itself.
    if (auto p = header.find("__GNUC__ > "); p != std::string_view::npos)
        b.gccMax = digits_after(header, p + 10);

    // The clang guard states its bound in prose rather than in the condition:
    // "clang version must be less than 15 and greater than 3.2".
    if (auto p = header.find("clang version must be less than ");
        p != std::string_view::npos) {
        int exclusive = digits_after(header, p + 31);
        if (exclusive > 0) b.clangMax = exclusive - 1;
    }
    return b;
}

bool host_compiler_accepted(const HostCompilerBounds& b,
                            std::string_view family, int major)
{
    if (major <= 0) return true;                 // unknown version: no claim
    if (family == "gcc")   return b.gccMax   == 0 || major <= b.gccMax;
    if (family == "clang" || family == "llvm")
        return b.clangMax == 0 || major <= b.clangMax;
    return true;
}

} // namespace mcpp::toolchain

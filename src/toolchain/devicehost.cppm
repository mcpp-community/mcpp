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

// The plan nvcc states for one compilation: the search path it will use, and
// the programs it will invoke by bare name.
//
// WHY THE PLAN IS ASKED FOR RATHER THAN ASSUMED
//
// nvcc runs its back-end stages -- cicc, ptxas, fatbinary, nvlink -- as bare
// names, on a PATH it prepends itself from an `nvcc.profile` beside its own
// binary. Where those stages live is therefore not a property of the toolkit
// layout that mcpp could tabulate: it is whatever that profile says. When the
// profile is unreachable -- a container or sandbox that replaces /etc, where
// the profile is a symlink into it -- nvcc states no PATH, keeps the ambient
// one, and fails at the first stage with `sh: 1: cicc: not found`. That
// message names neither nvcc nor the profile, and the toolkit is present and
// intact, so every obvious check passes. `--dryrun` prints the same plan
// without running it, which is how the question is answered before a
// compilation is attempted.
struct DeviceDryRun {
    // The PATH nvcc assigns itself. Empty means it assigned none, in which
    // case the ambient PATH is what its stages will be resolved against.
    std::string              searchPath;
    // Stages invoked by bare name, in first-appearance order, deduplicated.
    // A stage named by an absolute or relative path resolves on its own and
    // is not collected.
    std::vector<std::string> programs;
};

// Parse the `#$` lines of `nvcc --dryrun` output.
DeviceDryRun parse_dryrun(std::string_view text);

// Is `major` of `family` ("gcc" | "clang") within the bounds? A family the
// header said nothing about is accepted: silence is not a refusal.
bool host_compiler_accepted(const HostCompilerBounds& b,
                            std::string_view family, int major);

// ── The driver a device runtime will meet ──────────────────────────────────
//
// A device runtime must not be NEWER than the driver it runs against, and the
// driver is the one component that cannot be redistributed: it is in ABI
// lockstep with a kernel module. So the toolkit version a build uses is bounded
// by a fact about the machine, and that fact is knowable before anything is
// compiled.
//
// ⚠️⚠️ MEASURED, 2026-09-05, on a host whose driver reports CUDA 12.4: a
// binary built with the 13.3 payload COMPILES AND LINKS CLEANLY and then fails
// at the first allocation with
//
//     cudaMalloc: CUDA driver version is insufficient for CUDA runtime version
//
// while the same source built with the 12.9 payload prints the right answer.
// Everything that could have caught it earlier was silent -- which is the whole
// reason this is checked rather than left to happen.
//
// ⭐ The comparison is separated from the acquisition on purpose. Which
// function asks the driver its version is a vendor's business and belongs to a
// rule package; whether one version may meet another is a relation, and that
// is what lives here.

// A dotted version reduced to (major, minor). Absent or unreadable parts are
// zero, which makes an unreadable version compare as older rather than as a
// refusal.
struct DeviceVersion {
    int major = 0;
    int minor = 0;
    bool known() const { return major != 0; }
};

DeviceVersion parse_device_version(std::string_view text);

// May a runtime built against `toolkit` run on a machine whose driver supports
// up to `driver`?
//
// The rule is minor-version compatibility, which is the vendor's and not
// invented here: within one major version an application built against any
// minor runs on a driver supporting that major. Across majors it does not.
//
// ⚠️ Either side unknown yields TRUE. A check that cannot reach an answer must
// not manufacture a refusal -- the same rule the host-compiler bound follows.
bool driver_accepts_toolkit(DeviceVersion toolkit, DeviceVersion driver);

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

DeviceDryRun parse_dryrun(std::string_view text) {
    DeviceDryRun plan;

    for (std::size_t pos = 0; pos <= text.size(); ) {
        const auto eol  = text.find('\n', pos);
        const auto stop = eol == std::string_view::npos ? text.size() : eol;
        std::string_view line = text.substr(pos, stop - pos);
        pos = stop + 1;   // past the end after the last line: the loop stops

        // Every line nvcc contributes is prefixed; anything else is a
        // diagnostic and says nothing about the plan.
        constexpr std::string_view kPrefix = "#$ ";
        if (!line.starts_with(kPrefix)) continue;
        line.remove_prefix(kPrefix.size());
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.remove_prefix(1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.remove_suffix(1);
        if (line.empty()) continue;

        // The first token ends at whitespace or at the `=` of an assignment,
        // whichever comes first. An assignment is `NAME=value` with no space
        // before the `=`; a command is anything else.
        std::size_t end = 0;
        while (end < line.size() && line[end] != ' ' && line[end] != '\t'
               && line[end] != '=')
            ++end;

        if (end < line.size() && line[end] == '=') {
            if (line.substr(0, end) == "PATH")
                plan.searchPath = std::string(line.substr(end + 1));
            continue;
        }

        std::string_view program = line.substr(0, end);
        // A stage nvcc names by path resolves without the search path.
        if (program.find('/') != std::string_view::npos
            || program.find('\\') != std::string_view::npos
            || program.starts_with("\"")) continue;
        if (std::ranges::find(plan.programs, program) == plan.programs.end())
            plan.programs.emplace_back(program);
    }
    return plan;
}

DeviceVersion parse_device_version(std::string_view text) {
    DeviceVersion v;
    std::size_t i = 0;
    while (i < text.size() && !std::isdigit(static_cast<unsigned char>(text[i]))) ++i;
    int  acc = 0;
    bool any = false;
    for (; i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])); ++i) {
        acc = acc * 10 + (text[i] - '0');
        any = true;
    }
    if (!any) return v;
    v.major = acc;
    if (i < text.size() && text[i] == '.') {
        ++i;
        acc = 0;
        for (; i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])); ++i)
            acc = acc * 10 + (text[i] - '0');
        v.minor = acc;
    }
    return v;
}

bool driver_accepts_toolkit(DeviceVersion toolkit, DeviceVersion driver) {
    if (!toolkit.known() || !driver.known()) return true;   // no claim
    if (toolkit.major != driver.major) return toolkit.major < driver.major;
    // Same major: minor-version compatibility covers it.
    return true;
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

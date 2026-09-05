// bench.engines.engine — implementation.
//
// `module bench.engines.engine;` with no `export`: an implementation unit, so nothing below
// reaches an importer's BMI. The interface is the adapter contract every
// engine implements; the shared probe helpers below are not part of it.
module bench.engines.engine;

import std;
import bench.protocol;
import bench.spec;
import bench.platform;

namespace bench::engines {

std::string resolve_cxx(std::string_view compiler) {
    if (compiler.empty() || compiler == "default") return {};
    if (compiler.find('/') != std::string_view::npos ||
        compiler.find('\\') != std::string_view::npos)
        return std::string(compiler);
    if (compiler == "gcc")   return "g++";
    if (compiler == "clang") return "clang++";
    return std::string(compiler);
}

std::string first_line(std::string_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n' || text[i] == '\r') break;
        if (text[i] == '\x1b') {
            // CSI = ESC '[' , parameter bytes 0x30-0x3F, intermediate bytes
            // 0x20-0x2F, then ONE final byte 0x40-0x7E. Scanning straight for a
            // byte in @-~ stops on the '[' itself, which leaves "0m" behind in
            // every colour reset — the exact residue this used to produce.
            ++i;
            if (i < text.size() && text[i] == '[') ++i;
            while (i < text.size() && text[i] >= '\x20' && text[i] <= '\x3f') ++i;
            // land on the final byte; the loop's own ++i steps past it
            continue;
        }
        out += text[i];
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
    return out;
}

bool looks_uninstalled(std::string_view banner) {
    return banner.find("is not installed") != std::string_view::npos;
}

std::string uninstalled_reason(std::string_view program, std::string_view banner) {
    return std::format("{} resolves to a shim that reports it is not installed: {}",
                       program, banner);
}

Availability probe_program(std::string_view program,
                                  const std::vector<std::string>& version_argv) {
    platform::RunResult r;
    const auto captured = platform::run_capture(version_argv, {}, &r);
    if (!captured)
        return {false, std::format("{} not found on PATH", program)};
    if (r.exit_code != 0)
        return {false, std::format("{} present but `{}` exited {}", program,
                                   version_argv.size() > 1 ? version_argv[1] : "--version",
                                   r.exit_code)};
    auto banner = first_line(*captured);
    // A SHIM THAT ANSWERS FOR A PROGRAM IT DOES NOT HAVE. xlings installs
    // `bazel`, `mcpp` and friends as shims on PATH; ask one for its version
    // when the package is not installed and it prints
    //
    //     [error] xlings: 'bazel' is not installed
    //
    // and exits ZERO. Taken at face value that is "present, version =
    // <error message>", so every cell for that engine ran, failed, and was
    // recorded as a FINDING against the engine rather than as "not installed
    // here" — 18 cells per macOS job.
    if (looks_uninstalled(banner))
        return {false, uninstalled_reason(program, banner)};
    return {true, banner.empty() ? std::string(program) : banner};
}

}  // namespace bench::engines

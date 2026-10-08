// mcpp.toolchain.msvc_coroutines — the note appended to a failed compile when
// the compiler does not support C++20 coroutines on the MSVC ABI it targets.
//
// THE COMPILER DECIDES, AND MCPP FOLLOWS IT. clang 23 does not predefine
// `__cpp_impl_coroutine` for `i686-pc-windows-msvc` (the 32-bit x86 Microsoft
// ABI) and reports code that uses coroutines there with
// `-Wcoroutines-unsupported-target`; 22.1.8 and every other target measured
// still predefine it (LLVM 23.1.3 Part 3, §2.1). mcpp neither defines the macro
// nor rewrites the standard library around its absence. It explains the
// failure, because the failure does not explain itself:
//
//   * the MSVC STL keys `<coroutine>` on the macro, so the header is empty;
//   * `<generator>` (C++23) uses `coroutine_handle` without checking, so the
//     std module stops with twenty errors inside `generator`;
//   * code that uses coroutines stops at `use of undeclared identifier 'std'`
//     and `std::coroutine_traits type was not found`, before clang's own
//     coroutine diagnostic can run (measured, probe PR #788).
//
// ASKED ONLY AFTER A FAILURE, OF THE COMPILER THAT FAILED. The command is read
// from the failure itself, so no record has to be written in advance, and a
// successful build never reaches this module:
//
//   * a compile ninja ran: the output carries the command after `FAILED:` and
//     the compiler's diagnostics, so the symptom is read from it (`advice`);
//     the plan path and the fast path share that one function;
//   * the std module precompile: its error carries the `command:` line but not
//     the compiler's diagnostics, which reach the terminal directly (measured
//     on #781's Windows CI). The answer there rests on the command alone
//     (`std_module_advice`): the MSVC STL's `std.ixx` at C++23 or later
//     includes <generator>, which cannot compile without the macro.
//
// When the compiler or the STL changes, the probe stops matching and the note
// disappears.

export module mcpp.toolchain.msvc_coroutines;

import std;
import mcpp.platform;
import mcpp.toolchain.triple;

export namespace mcpp::toolchain::msvc_coroutines {

// Which of the two failures the output shows.
enum class Symptom {
    None,
    StdModuleGenerator,  // the std module stopped inside the STL's <generator>
    CoroutineUse,        // the program's own code uses the coroutine library
};

// The compile command a failure output names.
struct FailedCommand {
    std::string compiler;  // the driver as written on the command line
    std::string target;    // the value of --target=
    std::string standard;  // the value of -std=, possibly empty
};

// What the probe learned from the compiler.
struct Probe {
    bool        predefinesImplCoroutine = true;
    std::string version;   // __clang_version__, trimmed; may be empty
};

using ProbeFn = std::function<std::optional<Probe>(const FailedCommand&)>;

Symptom symptom_in(std::string_view output);

// The last command line in `output` that names a clang driver and a
// `--target=`. Double-quoted words are honoured (Windows paths with spaces).
std::optional<FailedCommand> failed_command_in(std::string_view output);

// The note, or empty when the output is not this failure. `probe` is called
// only after the symptom and the command have both matched.
std::string advice(std::string_view output, const ProbeFn& probe);

// The note for a failed std module precompile, from its error message: the
// `command:` line compiles the MSVC STL's `std.ixx` or `std.compat.ixx` at
// C++23 or later for a `*-windows-msvc` target, and the probe finds the macro
// undefined. Empty otherwise.
std::string std_module_advice(std::string_view message, const ProbeFn& probe);
std::string std_module_advice(std::string_view message);

// The probe that runs the compiler: `-dM -E` of an empty unit, with the same
// target and standard and without the driver's configuration file.
std::optional<Probe> run_probe(const FailedCommand& cmd);

// `advice` with `run_probe`.
std::string advice(std::string_view output);

} // namespace mcpp::toolchain::msvc_coroutines

namespace mcpp::toolchain::msvc_coroutines {

namespace {

std::vector<std::string> split_words(std::string_view line) {
    std::vector<std::string> words;
    std::string cur;
    bool quoted = false, any = false;
    for (char c : line) {
        if (c == '"') { quoted = !quoted; any = true; continue; }
        if (!quoted && (c == ' ' || c == '\t' || c == '\r')) {
            if (any) { words.push_back(std::move(cur)); cur.clear(); any = false; }
            continue;
        }
        cur += c;
        any = true;
    }
    if (any) words.push_back(std::move(cur));
    return words;
}

bool names_clang_driver(std::string_view word) {
    auto slash = word.find_last_of("/\\");
    auto base = slash == std::string_view::npos ? word : word.substr(slash + 1);
    if (base.ends_with(".exe")) base.remove_suffix(4);
    return base == "clang" || base == "clang++" || base == "clang-cl"
        || base.ends_with("-clang") || base.ends_with("-clang++");
}

bool is_error_line(std::string_view line) {
    return line.find("error:") != std::string_view::npos;
}

// An error reported at `<...>/generator:<line>:<col>` or `<...>\generator:...`.
bool error_inside_generator(std::string_view line) {
    if (!is_error_line(line)) return false;
    for (std::string_view sep : {"/generator:", "\\generator:"})
        if (line.find(sep) != std::string_view::npos) return true;
    return false;
}

bool error_names_coroutine_library(std::string_view line) {
    if (!is_error_line(line)) return false;
    for (std::string_view name : {"coroutine_traits", "'coroutine_handle'",
                                  "'suspend_always'", "'suspend_never'",
                                  "'noop_coroutine'"})
        if (line.find(name) != std::string_view::npos) return true;
    return false;
}

template <class F>
void for_each_line(std::string_view text, F&& f) {
    while (!text.empty()) {
        auto nl = text.find('\n');
        f(text.substr(0, nl));
        if (nl == std::string_view::npos) break;
        text.remove_prefix(nl + 1);
    }
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '"')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '"' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

} // namespace

Symptom symptom_in(std::string_view output) {
    bool generator = false, use = false;
    for_each_line(output, [&](std::string_view line) {
        if (error_inside_generator(line)) generator = true;
        else if (error_names_coroutine_library(line)) use = true;
    });
    if (generator) return Symptom::StdModuleGenerator;
    if (use) return Symptom::CoroutineUse;
    return Symptom::None;
}

std::optional<FailedCommand> failed_command_in(std::string_view output) {
    std::optional<FailedCommand> found;
    for_each_line(output, [&](std::string_view line) {
        if (auto at = line.find("command: "); at == 0) line.remove_prefix(9);
        auto words = split_words(line);
        if (words.empty() || !names_clang_driver(words.front())) return;
        FailedCommand cmd;
        cmd.compiler = words.front();
        for (auto const& w : words) {
            if (w.starts_with("--target=")) cmd.target = w.substr(9);
            else if (w.starts_with("-std=")) cmd.standard = w.substr(5);
        }
        if (!cmd.target.empty()) found = std::move(cmd);
    });
    return found;
}

namespace {

bool targets_msvc_abi(const FailedCommand& cmd) {
    auto triple = mcpp::toolchain::triple::parse(cmd.target);
    return triple && triple->os == "windows" && triple->env == "msvc";
}

// C++23 or later: `c++23`, `gnu++2b`, `c++26`, `c++2c`, `c++latest`.
bool at_least_cxx23(std::string_view standard) {
    auto plus = standard.find("++");
    if (plus == std::string_view::npos) return false;
    auto level = standard.substr(plus + 2);
    if (level == "latest") return true;
    if (level.size() != 2) return false;
    if (level[0] == '2' && level[1] >= 'b' && level[1] <= 'z') return true;
    return level >= "23" && level <= "99";
}

std::string render(Symptom symptom, const FailedCommand& cmd, const Probe& probed);

} // namespace

std::string advice(std::string_view output, const ProbeFn& probe) {
    const auto symptom = symptom_in(output);
    if (symptom == Symptom::None) return {};
    auto cmd = failed_command_in(output);
    if (!cmd || !targets_msvc_abi(*cmd)) return {};
    auto probed = probe(*cmd);
    if (!probed || probed->predefinesImplCoroutine) return {};
    return render(symptom, *cmd, *probed);
}

std::string std_module_advice(std::string_view message, const ProbeFn& probe) {
    std::optional<FailedCommand> cmd;
    bool stlModule = false;
    for_each_line(message, [&](std::string_view line) {
        if (!line.starts_with("command: ")) return;
        if (auto c = failed_command_in(line)) {
            cmd = std::move(c);
            for (auto const& w : split_words(line.substr(9)))
                if (w.ends_with("std.ixx") || w.ends_with("std.compat.ixx")) stlModule = true;
        }
    });
    if (!cmd || !stlModule || !targets_msvc_abi(*cmd) || !at_least_cxx23(cmd->standard))
        return {};
    auto probed = probe(*cmd);
    if (!probed || probed->predefinesImplCoroutine) return {};
    return render(Symptom::StdModuleGenerator, *cmd, *probed);
}

std::string std_module_advice(std::string_view message) {
    return std_module_advice(message, run_probe);
}

namespace {

std::string render(Symptom symptom, const FailedCommand& cmd, const Probe& probed) {
    auto triple = mcpp::toolchain::triple::parse(cmd.target);

    const std::string compiler = probed.version.empty()
        ? std::string("this clang") : std::format("clang {}", probed.version);
    const std::string section = triple ? triple->str() : cmd.target;
    std::string note = std::format(
        "\n"
        "note: {} does not support C++20 coroutines on {} (this Microsoft\n"
        "      ABI): it does not predefine __cpp_impl_coroutine there, so the\n"
        "      MSVC STL's <coroutine> is empty.",
        compiler, cmd.target);
    if (symptom == Symptom::StdModuleGenerator) {
        note +=
            " The C++23 std module includes\n"
            "      <generator>, which needs it, and cannot be compiled.\n";
    } else {
        note +=
            " Code that uses coroutines cannot be\n"
            "      compiled for this target with this compiler.\n";
    }
    note +=
        "      mcpp follows the compiler and does not enable coroutines here.\n"
        "      Options:\n";
    if (symptom == Symptom::StdModuleGenerator) {
        note +=
            "        (a) build the package as C++20, where `import std` and\n"
            "            `import std.compat` do not include <generator> (this\n"
            "            changes the standard of every target of the package):\n"
            "                [package]\n"
            "                standard = \"c++20\"\n"
            "        (b) optionally, use an LLVM that still enables coroutines\n"
            "            for this target:\n";
    } else {
        note +=
            "        (a) optionally, use an LLVM that still enables coroutines\n"
            "            for this target:\n";
    }
    note += std::format(
        "                [target.{}]\n"
        "                toolchain = \"llvm@22.1.8\"\n"
        "            the newer compiler treats coroutines on this ABI as\n"
        "            unsupported, so code that uses them here is at your own\n"
        "            risk.\n"
        "      Defining __cpp_impl_coroutine yourself is not a fix: it switches\n"
        "      on a feature the compiler has declared unsupported for this ABI.\n",
        section);
    return note;
}

} // namespace

std::optional<Probe> run_probe(const FailedCommand& cmd) {
    std::error_code ec;
    const auto empty = std::filesystem::temp_directory_path(ec)
        / std::format("mcpp-coroutine-probe-{}.cpp", std::random_device{}());
    if (ec) return std::nullopt;
    { std::ofstream touch(empty); if (!touch) return std::nullopt; }
    std::vector<std::string> argv{cmd.compiler, "--no-default-config",
                                  "--target=" + cmd.target};
    if (!cmd.standard.empty()) argv.push_back("-std=" + cmd.standard);
    for (std::string_view a : {"-dM", "-E", "-x", "c++"}) argv.emplace_back(a);
    argv.push_back(empty.string());
    auto r = mcpp::platform::process::capture_host_tool_stdout(argv);
    std::filesystem::remove(empty, ec);
    if (r.exit_code != 0 || r.output.empty()) return std::nullopt;

    Probe p;
    p.predefinesImplCoroutine =
        r.output.find("#define __cpp_impl_coroutine ") != std::string::npos;
    constexpr std::string_view kVersion = "#define __clang_version__ ";
    if (auto at = r.output.find(kVersion); at != std::string::npos) {
        auto rest = std::string_view(r.output).substr(at + kVersion.size());
        rest = rest.substr(0, rest.find('\n'));
        rest = trim(rest);
        p.version = std::string(rest.substr(0, rest.find(' ')));
    }
    return p;
}

std::string advice(std::string_view output) {
    return advice(output, run_probe);
}

} // namespace mcpp::toolchain::msvc_coroutines

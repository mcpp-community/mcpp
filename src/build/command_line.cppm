// mcpp.build.command_line — the spelling of a command that a shell reads: a
// ninja rule, a hook, a command printed for a person to paste.
//
// Provides shell-safe argument quoting for command construction:
//   POSIX:   single-quote wrapping  ('arg')
//   Windows: double-quote wrapping  ("arg")
//
// NOTE on Windows: do NOT use quote() for the FIRST token in a
// popen/system command string — cmd.exe strips a leading " pair.
// Use the raw path string as the first token; quote() is safe for
// arguments only.

export module mcpp.build.command_line;

import std;
import mcpplibs.os.platform;

export namespace mcpp::build::command_line {

// Platform-aware shell argument quoting.
std::string quote(std::string_view s);

// The two halves of `quote`, callable regardless of host. Exposed so the
// Windows command-line shape can be built and unit-tested from any platform
// — the cmd.exe quoting rules are the easiest thing in mcpp to get wrong and
// the hardest to notice, since a Linux/macOS run never executes that code.
std::string quote_windows(std::string_view s);
std::string quote_posix(std::string_view s);

// AN ARGUMENT OF A COMMAND PRINTED FOR A PERSON TO PASTE (mcpp#832). Left as
// it is when the shell would read it as one word anyway, so the common
// message reads as it always did; quoted by this host's shell rules when it
// would not -- a space, a quote, a glob or a shell metacharacter. Every
// suggested command that carries a path builds the argument with this.
std::string paste_arg(std::string_view s);

// An argument that must survive TWO parsers: cmd.exe's, and then the child
// program's own argv parsing.
//
// `quote_windows` answers only the second. Its `\"` escape belongs to the
// MSVCRT argv rules, and cmd.exe does not know it: to cmd every `"` simply
// toggles a quote state, so a payload carrying an EVEN number of them before a
// metacharacter leaves that character unquoted. Measured on windows-2022 with
// a JSON argument -- `{"targets":["xim:shaderc@>=2026.3"],"yes":true}` puts
// four quotes before the `>`, and cmd read it as a REDIRECTION, answering
//
//   The filename, directory name, or volume label syntax is incorrect.
//
// which arrived as a package-provisioning failure naming a package.
//
// The answer escapes exactly what needs it: quote for the child, then walk the
// result tracking the quote state CMD sees -- every `"` toggles it, because cmd
// does not know MSVCRT's `\"` -- and prefix `^` to any metacharacter that falls
// outside a quoted region. Inside one it is already inert, and `^` there is a
// literal character rather than an escape.
//
// ESCAPING EVERY METACHARACTER INCLUDING THE QUOTES WAS TRIED FIRST AND
// BROKE EVERY PAYLOAD. It is defensible on paper -- with no bare `"`, cmd never
// enters a quoted region and each metacharacter is escaped rather than quoted
// -- and Windows CI answered `exit 1` for every package fetch, including the
// ones whose JSON contains no metacharacter at all. So the rule here is the
// conservative one: a payload with nothing to escape comes out byte-identical
// to `quote_windows`, and only the character that is actually unprotected
// acquires a caret.
//
// `%` IS NOT ESCAPED AND CANNOT BE. Variable expansion happens before caret
// processing, and the batch-file escape (`%%`) is not available on a command
// line. Callers passing text that may contain `%` need a different mechanism;
// the JSON arguments this exists for do not.
std::string quote_windows_through_cmd(std::string_view s);

// Host-selecting: `quote_windows_through_cmd` on Windows, `quote_posix`
// elsewhere. Use this wherever an argument reaches a shell and may contain
// metacharacters -- notably JSON, and any version constraint spelled `>=`.
std::string quote_through_shell(std::string_view s);

// An argument vector as a person reads it in a message: an argument with
// nothing a shell would read specially stands as written, any other is quoted
// for the host's shell, so the text can be pasted to run the same program.
std::string display(const std::vector<std::string>& argv);

// Silent redirect — stdout + stderr → /dev/null (or NUL on Windows), and the
// error stream alone, for a command a shell reads.
constexpr std::string_view silent_redirect = mcpplibs::os::is_windows ? ">nul 2>&1" : ">/dev/null 2>&1";
constexpr std::string_view null_redirect   = mcpplibs::os::is_windows ? "2>nul" : "2>/dev/null";

// ─── The command interpreter of Windows (host-independent, for testing) ────
//
// A command that is one by nature (a `[hooks]` command, a toolchain's
// environment script) reaches cmd.exe as
//
//     cmd.exe /d /s /c "<command>"
//
// /s makes cmd's rule for the rest of the line unconditional (strip exactly
// the outer pair), so the command arrives verbatim however many quotes it
// contains; /d skips AutoRun so a user's registry-installed hook cannot alter
// it. The switches stay bare and the command is wrapped once: cmd reads the
// line by its own grammar, not by the argument rules of CreateProcess, so the
// arguments are given to it as one string (mcpplibs.os `one_string`).
// The arguments after `cmd.exe`, and the whole line they make.
std::vector<std::string> windows_shell_arguments(std::string_view command);
std::string windows_shell_command_line(std::string_view command);

// An `env LD_LIBRARY_PATH=<dirs> ` prefix for a ninja command on Linux: the
// toolchain's own runtime directories for a host process, without the
// inherited value (which may carry a target program's). "" elsewhere and for
// no directories.
std::string clean_ld_library_path_prefix(const std::vector<std::filesystem::path>& dirs);

} // namespace mcpp::build::command_line

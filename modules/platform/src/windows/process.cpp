// mcpp.platform.process on Windows: the launchers whose programs are started
// through cmd.exe command lines (windows_shell_command_line) by
// mcpp.platform.windows.bounded_process, and the C runtime's pipe primitives.
//
// Windows KEEPS the proven shell path. The env-binding hazard does not exist
// here (env goes through _putenv_s, not a prefix), so the launch primitive is
// not swapped on a platform that cannot be iterated on locally.
module;
#include <cstdio>
#include <stdio.h>     // _popen, _pclose

module mcpp.platform.process;

import std;
import mcpp.platform.common;
import mcpp.platform.env;
import mcpp.platform.shell;
import mcpp.platform.windows.bounded_process;

namespace mcpp::platform::process {

void set_umask_022() {
    // Nothing: the ACL inherited from the directory decides.
}

int normalize_exit_code(int rc) { return rc; }

std::FILE* open_pipe(const std::string& command) { return ::_popen(command.c_str(), "r"); }
int close_pipe(std::FILE* pipe) { return ::_pclose(pipe); }

namespace {

// Build a shell command line from an argv vector. EVERY token is quoted,
// including the program — a payload under `C:\Program Files\...` or a home
// directory with a space in the user name is otherwise cut at the first space
// and reported as `'C:\Program' is not recognized`. One derivation: the
// tested, host-independent shaper (windows_command_from_argv).
std::string command_from_argv(const std::vector<std::string>& argv) {
    return windows_command_from_argv(argv);
}

} // namespace

int run_exec(const std::vector<std::string>& argv,
             const std::vector<std::pair<std::string, std::string>>& extraEnv,
             int* spawn_error)
{
    if (spawn_error) *spawn_error = 0;
    if (argv.empty()) return 127;
    // THE SAME OWNERSHIP AS THE POSIX BRANCH, EXPRESSED IN THIS PLATFORM'S TERMS.
    //
    // `std::system` gave the child away: it runs through a cmd.exe mcpp does not
    // hold a handle to, so terminating mcpp left the tree running exactly as the
    // POSIX branch did before its process group. A job object with
    // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE is the equivalent unit — it takes the
    // whole tree, which matters here for the same reason the group does there:
    // the child starts compilers of its own.
    //
    // The command line is built by the same `windows_shell_command_line` the
    // rest of this file uses, so the cmd.exe quoting rule has ONE derivation.
    // Re-deriving it here is how `/d /s /c` loses an argument.
    std::string prefix = mcpp::platform::env::build_env_prefix(extraEnv);
    // wrap only — run_exec inherits stdio on purpose (see finalize_shell_command).
    std::string cmd = windows_shell_command_line(prefix + command_from_argv(argv));
    auto child = mcpp::platform::winproc::spawn_background(cmd.c_str(), nullptr, 1);
    if (!child.ok) {
        const int refused = static_cast<int>(child.refused);
        if (spawn_error) *spawn_error = refused;
        else std::fputs(spawn_failure(argv.front(), refused).c_str(), stderr);
        return 127;
    }
    mcpp::platform::winproc::guard_job_on_signal(child.job);
    int code = 127;
    mcpp::platform::winproc::wait_background(child.process, &code);
    mcpp::platform::winproc::unguard_job(child.job);
    // Closes both handles; the child has already exited, so this is cleanup
    // rather than a kill.
    mcpp::platform::winproc::background_stop(child.job, child.process, 0);
    return code;
}

int run_foreground(const std::vector<std::string>& argv,
                   const std::vector<std::pair<std::string, std::string>>& extraEnv,
                   int* spawn_error)
{
    if (spawn_error) *spawn_error = 0;
    if (argv.empty()) return 127;
    std::string prefix = mcpp::platform::env::build_env_prefix(extraEnv);
    std::string cmd = windows_shell_command_line(prefix + command_from_argv(argv));
    unsigned long refused = 0;
    const int code = mcpp::platform::winproc::run_foreground(cmd.c_str(), &refused);
    if (code == -1 && refused != 0) {
        if (spawn_error) *spawn_error = static_cast<int>(refused);
        else std::fputs(spawn_failure(argv.front(), static_cast<int>(refused)).c_str(), stderr);
        return 127;
    }
    return code;
}

RunResult capture_exec(
    const std::vector<std::string>& argv,
    const std::vector<std::pair<std::string, std::string>>& extraEnv,
    std::string_view cwd,
    int* spawn_error)
{
    RunResult result;
    if (spawn_error) *spawn_error = 0;
    if (argv.empty()) { result.exit_code = 127; return result; }
    std::string cmd = command_from_argv(argv) + " 2>&1";
    if (!cwd.empty()) {
        // cmd.exe `cd` without /d does not switch drives — a project on a
        // different drive than mcpp's own cwd would run the child (the
        // build.mcpp contract's only cwd consumer) in the wrong directory.
        cmd = "cd /d " + mcpp::platform::shell::quote(cwd) + " && " + cmd;
    }
    return capture_with_env(cmd, extraEnv);
}

RunResult capture_stdout(
    const std::vector<std::string>& argv,
    const std::vector<std::pair<std::string, std::string>>& extraEnv,
    int* spawn_error)
{
    RunResult result;
    if (spawn_error) *spawn_error = 0;
    if (argv.empty()) { result.exit_code = 127; return result; }
    // cmd.exe reports a program it cannot find on stderr, which the redirect
    // discards, and returns 9009; the output stays empty either way.
    return capture_with_env(windows_stdout_probe_command(argv), extraEnv);
}

} // namespace mcpp::platform::process

// mcpp.run — every program mcpp starts, and how mcpp ends when it is asked to.
//
// One launcher over mcpplibs.os (design 2026-10-26 §25.5): a program is named
// by an argument vector, receives this invocation's environment (mcpp.invocation)
// with the caller's additions, runs in a unit of its own that ends with mcpp,
// and is read, bounded and stopped as the caller asks. A command interpreter is
// started only for a command that is one by nature -- a `[hooks]` command, a
// toolchain's environment script -- through `shell`.
//
// THERE ARE NO SIGNAL HANDLERS. mcpplibs.os takes the requests to end over
// (SIGINT, SIGTERM, SIGHUP; a console's control events) and sets a word; a
// watcher thread waits on it, ends the units of every program mcpp is running,
// writes the files an interrupt leaves, runs the registered cleanups and then
// ends mcpp as the request would have. A write to a closed standard output is
// refused rather than fatal; `end_for_closed_output` ends mcpp in the same
// order and then as the system ends such a program (SIGPIPE, 141 to a shell).
export module mcpp.run;

import std;

export namespace mcpp::run {

// Variables added to a started program's environment (a value replaces
// whatever the variable held).
using Env = std::vector<std::pair<std::string, std::string>>;

struct RunResult {
    int         exit_code = 0;
    std::string output;
    std::string error_output;        // Streams::output_error_kept: its standard error
    bool        timed_out = false;   // a bound or `should_stop` ended it
    int         spawn_error = 0;     // errno-style when it could not be started, else 0
};

// ─── This process ────────────────────────────────────────────────────────

// Called once, first thing in main: the requests to end are taken over and
// the watcher is started.
void install();

// The files an interrupt leaves (design 2026-10-26 §5.4): when a request ends
// mcpp, each `(path, content)` replaces its file, through a temporary name
// beside it and a rename, after the programs mcpp started are ended. Setting
// replaces the previous set.
void set_interrupt_files(const std::vector<std::pair<std::filesystem::path, std::string>>& files);

// True once mcpp has begun to end at a request or a closed output. A record
// written from then on would replace the one the request leaves, so a writer
// of the files above writes nothing while it is true.
bool ending();
void clear_interrupt_files();

// A cleanup the watcher runs before mcpp ends at a request (a terminal's mode
// restored). Returns its handle for `remove_cleanup`.
int add_cleanup(std::function<void()> cleanup);
void remove_cleanup(int handle);

// mcpp's own standard output has no reader: the programs it started are ended,
// the interrupt files written, the cleanups run, and mcpp ends as the system
// ends such a program. Returns only where the system has no such ending
// (Windows), where mcpp goes on as its writes are refused.
void end_for_closed_output();

// ─── Starting a program ──────────────────────────────────────────────────
//
// A SPAWN FAILURE IS REPORTED EXACTLY ONCE AND NEVER DROPPED (#544). When the
// program could not be started a launcher returns 127 and either stores an
// errno-style code in `*spawn_error` (ENOENT, EACCES, ENOEXEC, EIO) and prints
// nothing, because a caller that asked owns the report, or, with
// `spawn_error == nullptr`, reports it itself -- on stderr for the launchers
// that inherit it, into `output` for the capturing ones. `*spawn_error` is 0
// whenever the program was started, whatever it then did.

// The program on mcpp's standard streams, in a unit of its own; its exit code
// (128 + the signal that ended it, as a shell reports it).
int run_exec(const std::vector<std::string>& argv, const Env& extraEnv = {},
             int* spawn_error = nullptr);

// THE PROGRAM A USER RUNS (`mcpp run`): mcpp becomes it (POSIX: its image
// replaced), so it owns the terminal, its signals and its exit status as it
// would if the shell had started it. Returns only when it could not be
// started: 127, under the contract above. Elsewhere the program is started in
// a unit bound to mcpp and mcpp ends with its status.
int run_foreground(const std::vector<std::string>& argv, const Env& extraEnv = {},
                   int* spawn_error = nullptr);

// Its standard output and standard error together, in the order written.
RunResult capture_exec(const std::vector<std::string>& argv, const Env& extraEnv = {},
                       std::string_view cwd = {}, int* spawn_error = nullptr);

// A PROGRAM PROBE: its standard output only, an empty standard input, its
// standard error discarded. A program that cannot be started yields 127 and
// EMPTY output: a probe parses what it captured.
RunResult capture_stdout(const std::vector<std::string>& argv, const Env& extraEnv = {},
                         int* spawn_error = nullptr);
// capture_stdout for a host tool: the target's loader search variable is not
// given to it (a target program's LD_LIBRARY_PATH poisons system tools).
RunResult capture_host_tool_stdout(const std::vector<std::string>& argv);
// capture_exec for a host tool.
RunResult capture_host_tool(const std::vector<std::string>& argv, const Env& extraEnv = {});

// Bounded: the unit is ended once `deadline` passes and `*timed_out` is set. A
// zero deadline is no bound.
int run_exec_deadline(const std::vector<std::string>& argv, const Env& extraEnv,
                      std::chrono::milliseconds deadline, bool* timed_out,
                      int* spawn_error = nullptr);
RunResult capture_exec_deadline(const std::vector<std::string>& argv, const Env& extraEnv,
                                std::chrono::milliseconds deadline, bool* timed_out,
                                std::string_view cwd = {}, int* spawn_error = nullptr);

// Each line of the program's output (both streams, without the line end) to
// `on_line` as it arrives; a line the program did not end arrives after it
// does. Its input is empty, and `deadline` bounds the run (zero: no bound).
// Returns the exit code. The general form, with an idle bound, a stop
// condition and carriage returns as line ends, is `execute`.
int stream_exec(const std::vector<std::string>& argv, const Env& extraEnv,
                std::chrono::milliseconds deadline,
                const std::function<void(std::string_view line)>& on_line, bool* timed_out,
                int* spawn_error = nullptr);

// ─── Any program, as the caller states ───────────────────────────────────

// What a started program reads.
enum class Input { none, inherit };
// Where its two output streams go: mcpp's own (inherit); read together
// (merged); its standard output read and its standard error discarded
// (output: a probe); its standard output read and its standard error on
// mcpp's (output_error_shown); both read, apart (output_error_kept: an output
// parsed as records, whose errors only a failure reports); nowhere (discard).
enum class Streams { inherit, merged, output, output_error_shown, output_error_kept, discard };

struct Options {
    Env                       env;                    // additions; a value replaces
    std::vector<std::string>  unset;                  // variables it does not receive
    std::string               cwd;                    // empty: mcpp's
    bool                      host_tool = false;      // the target's loader search variable withheld
    Input                     input = Input::none;
    Streams                   streams = Streams::merged;
    std::chrono::milliseconds total{0};               // zero: no bound
    std::chrono::milliseconds idle{0};                // zero: no bound
    // Each line of the read output as it arrives, instead of `output`.
    std::function<void(std::string_view line)> on_line;
    bool                      split_on_cr = false;
    std::function<bool()>     should_stop;
    // A program that could not be started is reported (into `output` when it
    // is read with its errors, else on stderr) unless the caller owns the
    // report; `spawn_error` is set either way.
    bool                      report_spawn_failure = true;
    // The arguments given as one string after the program's name (Windows):
    // `Shell::one_string`, for the command interpreter.
    bool                      one_string = false;
};
RunResult execute(const std::vector<std::string>& argv, const Options& options = {});

// ─── A command for the command interpreter ───────────────────────────────

// The argument vector that runs `command` through the host's interpreter:
// `<sh> -c <command>` on POSIX (/bin/sh, else Android's /system/bin/sh, else
// $PREFIX/bin/sh), `cmd.exe /d /s /c "<command>"` on Windows, given to it as one
// string (mcpplibs.os `one_string`) so that its own grammar reads the command.
struct Shell {
    std::vector<std::string> argv;
    bool one_string = false;
};
Shell shell(std::string_view command);

// A command line mcpp composed as text and records as such (a std module's
// build commands, kept in its metadata): its standard output read, its own
// redirections deciding the rest, an empty standard input.
RunResult capture_shell(std::string_view command, const Env& extraEnv = {});

// A user-authored command (`[hooks]`) on mcpp's streams, in `cwd`, bounded.
// 127 when the interpreter could not be started.
int run_shell_deadline(std::string_view command, std::string_view cwd,
                       std::chrono::milliseconds deadline, bool* timed_out);
// A command captured with additions to its environment, bounded: a toolchain's
// environment script, which only the interpreter can run.
RunResult capture_shell_deadline(std::string_view command, const Env& extraEnv,
                                 std::chrono::milliseconds deadline, bool* timed_out,
                                 std::string_view cwd = {});

// ─── A command mcpp owns for longer than one call (#496) ─────────────────
//
// A project `[hooks] during_build` command is owned for the length of the
// BUILD: started here, polled while the build runs, stopped afterwards, and
// ended with mcpp if a request ends mcpp first. Its unit is the command and
// everything it starts: the command is user-authored shell, so the thing that
// must end is a tree.
struct BackgroundCommand {
    bool ok = false;
    std::shared_ptr<void> state;   // opaque
};
BackgroundCommand start_shell_background(std::string_view command, std::string_view cwd,
                                         bool inheritStdio);
// True while it runs. When it has ended and `exitCode` is given, the code is
// written there.
bool background_running(const BackgroundCommand& child, int* exitCode = nullptr);
// Asked to end, `grace` given, then its unit ended.
void stop_background(const BackgroundCommand& child, std::chrono::milliseconds grace);
// Ended with mcpp at a request (the watcher's registry), from now on / no more.
void guard_background_on_signal(const BackgroundCommand& child);
void clear_background_guard(const BackgroundCommand& child);

}  // namespace mcpp::run

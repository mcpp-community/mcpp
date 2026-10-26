// mcpp.platform.windows.bounded_process — a child process with a real
// deadline on Windows.
//
// WHY THIS EXISTS
//
// `capture_exec_deadline` enforced its deadline on POSIX only; everywhere else
// it fell through to the unbounded launcher. So every timeout knob was a
// silent no-op on Windows — `mcpp test --timeout`, `--build-timeout`, and
// `[build] build_program_timeout`. A knob that reports nothing and does
// nothing is worse than an absent one: the user sets it, the build still
// hangs, and nothing connects the two.
//
// WHY A JOB OBJECT
//
// Not thoroughness — correctness. The child inherits the capture pipe's write
// handle, and so does anything IT spawns. Killing only the child leaves a
// grandchild holding that handle, and the parent's drain then blocks forever:
// the timeout would "fire" and hang anyway. A Job with
// JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE takes the whole tree down at once, which
// closes every inherited handle and lets the drain finish.
//
// WHY THE INTERFACE HAS NO `std` TYPES IN IT
//
// This is a hard constraint, not a style preference. Measured on GCC 16.1.0
// while adding this module: a NEW module that (a) is imported by
// `mcpp.platform.process` and (b) names `std` types in its EXPORTED
// interface makes every BMI downstream of `mcpp.platform.process` come back as
//
//     mcpp.manifest.xpkg: error: failed to read compiled module cluster 192:
//     Bad file data
//
// deterministically, in a clean build, and identically under `ninja -j1` (so
// it is not a write/read race). Bisected precisely:
//
//     module exists but nothing imports it  -> builds
//     imported, exports only `int probe()`  -> builds
//     imported, `import std;` in the purview-> builds
//     imported, exports std::string/…       -> every downstream BMI corrupt
//
// So `std` may be used freely INSIDE this module; it must not appear in what
// the module exports. The output is therefore delivered through a callback
// instead of returned as a string, and the environment arrives as an array of
// `"K=V"` C strings.
//
// The same constraint is why the caller does the marshalling: see
// `mcpp.platform.process`.
//
// The declarations are the same on every system; the definitions are
// Windows's (windows/bounded_process.cpp), and a call from another system is a
// link error.

export module mcpp.platform.windows.bounded_process;

import std;

export namespace mcpp::platform::winproc {

// Every member is a builtin type — see the note above.
struct DeadlineRun {
    // False on every non-Windows build, and on Windows when the process could
    // not be created at all. The caller must fall back to its unbounded path
    // rather than treating this as a failed child: "could not spawn it" and
    // "it ran and failed" are different answers and must not share an exit
    // code.
    bool supported = false;
    int  exit_code = 0;
    bool timed_out = false;
    // Non-zero when `supported` is false BECAUSE CreateProcess was attempted
    // and refused: its GetLastError() value. Zero with `supported` false means
    // this build has no bounded launcher. Same contract as the POSIX peer,
    // whose value is an errno; the caller reports the number verbatim and
    // does not spawn again (#544).
    int  spawn_error = 0;
};

// ─── Starting a child is safe for concurrent callers ─────────────────────
//
// `CreateProcess` with `bInheritHandles = TRUE` hands a child EVERY inheritable
// handle of its parent, and a capture pipe's write end has to be inheritable for
// its own child. A child started by thread B while thread A holds that write end
// keeps it open for as long as it lives, and A's reader then waits for end of
// file until B's unrelated child exits. (`SetHandleInformation` cannot close the
// window: a handle is inheritable from its creation to the moment the parent
// closes it, and the start of the child lies between the two.)
//
// So the starts that can run at the same time as another, the ones below and
// the `_popen` calls of mcpp.platform.process, hold one process-wide section
// from the creation of the pipe to the parent's close of its write end. The
// alternative, `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`, would restrict what OUR
// launchers pass and leave `_popen` and any third party passing everything; the
// section is one rule for all of them.
//
// On another platform the class exists and does nothing.
class LaunchSection {
public:
    LaunchSection();
    ~LaunchSection();
    // Leaves the section before the destructor does.
    void release();
    LaunchSection(const LaunchSection&) = delete;
    LaunchSection& operator=(const LaunchSection&) = delete;
private:
    bool held_ = false;
};

// Receives stdout+stderr as it arrives. Called on the calling thread only.
//
// A NULL sink means "do not capture": the child inherits the caller's stdio and
// is still bounded. Same contract as the POSIX peer, and for the same reason —
// an uncaptured bounded run (`run_exec_deadline`) must keep its output LIVE and
// keep the child's stdout a console, or console-detecting children drop their
// colors and every line waits for exit.
using OutputSink = void (*)(void* ctx, const char* data, unsigned long len);
using StopPoll = bool (*)(void* ctx);

// `commandLine` is already quoted for CreateProcess (callers pass the output
// of windows_command_from_argv). `envEntries` is `envCount` NUL-terminated
// "KEY=VALUE" strings applied on top of the current environment. `cwd` may be
// null. A non-positive `deadlineMs` without a sink is rejected with
// supported=false — "no bound" belongs on the caller's untimed path, which
// needs none of this.
//
// `idleMs`, when positive, kills the child once it has written nothing for that
// long; the POSIX peer states the reason (#648). With `idleMs` positive a
// non-positive `deadlineMs` means "no total bound", and so it does with a sink
// and neither bound. There is no `ownGroup`
// parameter: every child here runs in a job object with
// JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE already, which is what the POSIX flag
// buys there.
DeadlineRun capture_with_deadline(const char*        commandLine,
                                  const char* const* envEntries,
                                  unsigned long      envCount,
                                  const char*        cwd,
                                  long long          deadlineMs,
                                  long long          idleMs,
                                  OutputSink         sink,
                                  void*              ctx,
                                  StopPoll           stop);

// ─── A child that outlives the call that started it (#496) ───────────────
//
// The peer of unixproc::spawn_background, and the same contract: a project
// `[hooks] during_build` command is owned for the length of the BUILD, so the
// caller gets a handle it can poll and stop later. Builtins only, like
// DeadlineRun — HANDLEs travel as integers rather than as a std type.
//
// The job object does here what a process group does on POSIX, and does it
// better in one respect: JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE means the tree
// dies when the last handle to the job closes, which includes mcpp exiting for
// ANY reason — Ctrl-C, an unhandled exception, or being killed outright. POSIX
// needs an explicit signal handler for the same guarantee and still cannot
// cover `kill -9`.
struct BackgroundChild {
    bool               ok      = false;
    unsigned long long job     = 0;   // HANDLE to the job object
    unsigned long long process = 0;   // HANDLE to the child
    // GetLastError() from a refused CreateProcess. Carried rather than dropped
    // for the reason the POSIX peer carries errno: the code in hand at the
    // failure is the whole difference between "not found" and "not executable",
    // and a caller that maps a spawn failure onto an exit status needs it.
    unsigned long      refused = 0;
};

// `inheritStdio == 0` sends the child's output to NUL. A spanning hook writes
// CONCURRENTLY with ninja and would otherwise interleave into the middle of a
// compiler diagnostic.
BackgroundChild spawn_background(const char* commandLine,
                                 const char* cwd,
                                 int         inheritStdio);

// 1 = still running, 0 = exited, -1 = unknown. When it returns 0, `exitCode`
// (if given) receives the process's exit code. Same contract as the POSIX
// peer, and for the same caller: the `loop` supervisor has to tell "the
// player finished the track" from "the command does not exist".
int background_running(unsigned long long process, int* exitCode);

// Closes the job, which terminates the whole tree at once.
//
// `graceMs` is accepted for signature parity with the POSIX peer and is NOT
// spent: Windows has no portable graceful stop for a child with no console and
// no window of its own, and the call that looks like one
// (GenerateConsoleCtrlEvent) addresses a process group attached to THIS
// console — see the implementation for what that cost. The asymmetry with the
// POSIX side's SIGTERM-then-grace is real and is declared here rather than
// papered over.
void background_stop(unsigned long long job, unsigned long long process,
                     long long graceMs);

// Ctrl-C. The job already covers process death, so this exists only so that a
// deliberate interrupt stops the tree BEFORE mcpp unwinds, rather than as a
// side effect of it exiting.
// A REGISTRY AND NOT ONE SLOT, for the reason the POSIX peer gives: a spanning
// `[hooks]` command and the build's own ninja are guarded at the same time, and
// a single slot lets the second registration disarm the first.
//
// SAFE FOR CONCURRENT CALLERS, as the POSIX peer is: claiming and releasing a
// slot is serialized by a mutex and the console handler takes no lock. The
// capacity is 256.
void guard_job_on_signal(unsigned long long job);
void unguard_job(unsigned long long job);
void clear_job_guard();

// The files a Ctrl-C or a Ctrl-Break leaves (the POSIX peer states why): the
// console handler, after it has ended the guarded jobs, writes each
// `contents[i]` to `temps[i]` and moves it over `paths[i]`. While files are set
// the handler is installed. At most four files.
void set_interrupt_files(const std::vector<std::string>& paths,
                         const std::vector<std::string>& temps,
                         const std::vector<std::string>& contents);
void clear_interrupt_files();

// Wait for a child started by `spawn_background` and return its exit code.
// Blocking, so an owned run costs no polling latency; `background_running` stays
// for the supervisor that must not block.
int wait_background(unsigned long long process, int* exitCode);

// ─── The program a user runs (`mcpp run`) ────────────────────────────────
//
// Started in the console mcpp was started from and in mcpp's own process
// group, so the console's input, Ctrl-C and Ctrl-Break reach the program and
// the program decides what they mean. While it runs, mcpp ignores Ctrl-C and
// Ctrl-Break through a handler of its own, which a child does not inherit, so
// that mcpp survives to return the program's status. The kill-on-close job
// still ends the program's tree when mcpp itself ends for any other reason.
// Returns the program's exit code, or -1 when it could not be started, with
// GetLastError() in `*refused`.
int run_foreground(const char* commandLine, unsigned long* refused);

} // namespace mcpp::platform::winproc

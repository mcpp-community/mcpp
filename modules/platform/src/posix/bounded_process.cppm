// mcpp.platform.unix.bounded_process — a child process with a deadline on
// POSIX. The peer of mcpp.platform.windows.bounded_process.
//
// WHY THE TWO SIDES ARE SEPARATE MODULES WITH ONE SHARED SIGNATURE
//
// They share no code — this is posix_spawn + a pipe + waitpid + SIGKILL, the
// other is CreateProcess + a Job object + WaitForSingleObject. What they share
// is a CONTRACT: "run this, capture its output, kill it at the deadline, and
// tell me whether you killed it". Putting that contract in one signature and
// each implementation behind its own module is what lets
// `mcpp.platform.process` dispatch once, with `if constexpr`, instead of
// carrying the platform question through twenty-five separate `#if` blocks.
//
// THE INTERFACE NAMES NO `std` TYPE — SAME HARD CONSTRAINT AS THE WINDOWS
// SIDE. See mcpp.platform.windows.bounded_process for the measurements: a new
// module imported by mcpp.platform.process whose EXPORTS mention std types
// corrupts every BMI downstream of it under GCC 16.1. `std` inside the module
// is fine; `std` in what it exports is not.
//
// The POSIX side does not strictly need the constraint (it is reached through
// the same dispatch, so it would be one more module in the same position — and
// the failure is silent enough that "probably fine" is not worth finding out).
// Keeping both sides identical also means the dispatcher marshals once, not
// twice.
//
// The declarations are the same on every system; the definitions are Linux's
// and macOS's (posix/bounded_process.cpp, with linux/ and macos/ for the pipe),
// and a call from another system is a link error.

export module mcpp.platform.unix.bounded_process;

import std;

export namespace mcpp::platform::unixproc {

// Every member is a builtin type. Same reason as the Windows peer.
struct DeadlineRun {
    // False on every non-POSIX build, and on POSIX when the child could not be
    // spawned. "Could not spawn" and "ran and failed" must not share an exit
    // code, so the caller falls back rather than reporting a failure.
    bool supported = false;
    int  exit_code = 0;
    bool timed_out = false;
    // Non-zero when `supported` is false BECAUSE the spawn was attempted and
    // refused: the errno posix_spawnp returned. Zero with `supported` false
    // means this build has no bounded launcher at all. The two need opposite
    // treatment one layer up — the first is reported with its errno, the
    // second falls back to the unbounded launcher — and before this field
    // existed both read the same, so the caller spawned a second time and
    // discarded the errno the first attempt had in hand (#544).
    int  spawn_error = 0;
};

using OutputSink = void (*)(void* ctx, const char* data, unsigned long len);
using StopPoll = bool (*)(void* ctx);

// ─── Starting a child is safe for concurrent callers ─────────────────────
//
// Two threads that start children at the same time must not pass one child's
// pipe to the other. A child inherits every descriptor of its parent that is not
// close-on-exec, so a child started by thread B while thread A holds the write
// end of A's capture pipe keeps that end open for as long as it lives. A's
// reader then waits for end of file until B's unrelated child exits: a compile
// that finished at once is reported only when a longer one does, and a deadline
// the reader is meant to enforce is enforced late.
//
// The pipes are therefore created close-on-exec, and `posix_spawn`'s `dup2`
// action, which clears the flag on the descriptor it creates, is the only way a
// descriptor reaches the child. Where the platform has `pipe2` (Linux) that is
// one atomic call. Where it does not (macOS) the flag is set by a second call,
// which leaves a window in which another thread's child could be started; every
// start on such a platform therefore holds one process-wide section, from the
// creation of the pipe to the parent's close of its write end. On Linux the
// section is empty.
//
// `make_pipe` returns 0 on success and -1 on failure, as `pipe` does. Both ends
// are close-on-exec. The caller holds a `LaunchSection` across this call and the
// start of the child.
int make_pipe(int fds[2]);

class LaunchSection {
public:
    LaunchSection();
    ~LaunchSection();
    // Leaves the section before the destructor does: a caller that goes on to
    // wait for its child must not hold it for the child's whole life.
    void release();
    LaunchSection(const LaunchSection&) = delete;
    LaunchSection& operator=(const LaunchSection&) = delete;
private:
    bool held_ = false;
};

// `argvEntries` is `argvCount` NUL-terminated strings; `envEntries` is
// `envCount` "KEY=VALUE" strings applied on top of the current environment.
// `cwd` may be null. A non-positive `deadlineMs` is rejected with
// supported=false: "no bound" belongs on the caller's untimed path.
//
// A NULL `sink` means "do not capture": the child INHERITS the caller's stdio
// and is still bounded. That is not an optimization — it is the
// `run_exec_deadline` contract. Routing an uncaptured run through a pipe would
// (a) delay every line until the child exits, which is the opposite of what a
// bounded `mcpp test` run is for, and (b) make the child's stdout a pipe
// rather than a terminal, so gtest and friends silently drop their colors.
//
// `idleMs`, when positive, is a second bound: the child is killed once it has
// written nothing for that long. It is meaningful only with a sink, and it is
// what lets a long, progressing child (a download that prints progress) run to
// completion while a wedged one does not (#648). With `idleMs` positive a
// non-positive `deadlineMs` means "no total bound", and so it does with a sink
// and neither bound: a build's ninja, read line by line for its progress.
//
// `ownGroup` non-zero places the child in a process group of its own, kills
// the GROUP at a bound, and registers the group with the signal guard for the
// length of the call, so terminating mcpp takes the child's descendants with
// it. It is opt-in because a child in a background group that reads the
// terminal is stopped by SIGTTIN, and the uncaptured callers of this function
// hand the terminal to their child.
DeadlineRun capture_with_deadline(const char* const* argvEntries,
                                  unsigned long      argvCount,
                                  const char* const* envEntries,
                                  unsigned long      envCount,
                                  const char*        cwd,
                                  long long          deadlineMs,
                                  long long          idleMs,
                                  int                ownGroup,
                                  OutputSink         sink,
                                  void*              ctx,
                                  StopPoll           stop);

// ─── A child that outlives the call that started it (#496) ───────────────
//
// `capture_with_deadline` owns its child for the length of one call. A project
// `[hooks] during_build` command is owned for the length of the BUILD, so the
// caller needs a handle it can poll and stop later. Every member is a builtin,
// same constraint as DeadlineRun.
//
// `group`, NOT a pid. The child is placed in a process group of its own
// (posix_spawnattr_setpgroup) and stopped with killpg, because the thing being
// started is a user-authored SHELL command: `sh -c 'player & wait'` makes the
// writer a grandchild, and `kill(pid)` reaches only the shell. A background
// player that survives its build, from a process the user cannot name, is the
// worst outcome this API has — so the group is the unit throughout.
struct BackgroundChild {
    bool      ok    = false;
    long long group = 0;   // the child's process-group id (== its pid)
};

// `inheritStdio == 0` sends the child's output to /dev/null. A spanning hook
// writes CONCURRENTLY with ninja and would otherwise interleave into the middle
// of a compiler diagnostic.
BackgroundChild spawn_background(const char* const* argvEntries,
                                 unsigned long      argvCount,
                                 const char*        cwd,
                                 int                inheritStdio);

// 1 = still running, 0 = exited, -1 = unknown. When it returns 0, `exitCode`
// (if given) receives the shell convention: the status, or 128+signal.
//
// The code is part of the answer rather than a second call, because the only
// caller that needs it — the `loop` supervisor — has to distinguish "the
// player finished the track" from "the command does not exist". Restarting the
// first forever is the feature; restarting the second forever is a spin.
int background_running(long long group, int* exitCode);

// SIGTERM, `graceMs`, then SIGKILL — to the GROUP. Reaps the direct child.
void background_stop(long long group, long long graceMs);

// ─── Ctrl-C ──────────────────────────────────────────────────────────────
//
// Its own process group is what makes killpg possible AND what stops the
// terminal's SIGINT from reaching the child: Ctrl-C would kill mcpp and leave
// the player running. Both halves are required, so the group that must not
// outlive us is registered here for the duration.
//
// The handler does the minimum that is async-signal-safe: killpg (which is),
// then the default action.
// A REGISTRY AND NOT ONE SLOT, BECAUSE THERE IS MORE THAN ONE OWNER.
//
// A spanning `[hooks]` command is guarded for the length of the build, and the
// build's own ninja is guarded for the length of its run — concurrently. With a
// single slot the second registration overwrites the first, so killing mcpp
// mid-build would take down ninja and leave the hook's process running, which
// is the exact outcome the guard exists to prevent.
//
// Fixed capacity and no allocation: the handler reads this array and may not
// allocate. Registering past capacity fails loudly rather than silently
// dropping a group, because a dropped group is an orphan nobody will find.
//
// SAFE FOR CONCURRENT CALLERS. Children are started from several threads at once
// (a workspace's build programs are compiled concurrently), so claiming and
// releasing a slot is serialized by a mutex, and two threads can no longer claim
// the same slot. The handler takes no lock: it reads slots that are written one
// `sig_atomic_t` at a time. The capacity is 256, well above any job count, since
// one group is held per running child.
void guard_group_on_signal(long long group);
void unguard_group(long long group);
void clear_group_guard();
// Whether no process group is guarded. A caller about to replace mcpp with
// another program (`mcpp run`) requires it: a guarded group is a child that
// would lose its owner.
bool group_guard_idle();

// THE TERMINAL MODE A SIGNAL RESTORES (build output design revision 3,
// §5.14). `--play-game` reads keys from the terminal without echo. A Ctrl-C,
// a termination or a hangup that ends mcpp must leave the terminal in the
// mode it found, and the handler installed while ninja runs is the one above,
// so the saved mode is restored there, with tcsetattr, which is
// async-signal-safe. `fd`'s current mode is saved; the caller changes the mode
// after this returns. A process killed outright cannot restore anything.
void guard_terminal_mode(int fd);
// Restores the saved mode and stops guarding it.
void unguard_terminal_mode();

// THE FILES A SIGNAL LEAVES (design 2026-10-26 §5.4). A build interrupted while
// it writes its directory says so in its result record: the interrupted form is
// rendered beforehand, and the handler -- after it has stopped the guarded
// groups -- writes each `contents[i]` to `temps[i]` and renames it over
// `paths[i]` (open, write, close and rename are async-signal-safe). While files
// are set the handler is installed. Setting replaces the previous set; at most
// four files.
void set_interrupt_files(const std::vector<std::string>& paths,
                         const std::vector<std::string>& temps,
                         const std::vector<std::string>& contents);
void clear_interrupt_files();

// A WRITE WHOSE READER IS GONE ENDS mcpp AS A SIGNAL ABOVE DOES (design
// 2026-10-26 part 2, §6.4): the guarded groups killed, the interrupt files
// written, the terminal's mode restored, and then SIGPIPE's own default, so the
// shell sees the status it always saw (141) for `mcpp … | head`.
//
// A handler and not an ignore. openkal ignores SIGPIPE for its own writes, and
// an ignored disposition survives exec: every program mcpp starts -- a build
// script's `yes | head` included -- would begin with it ignored, and a shell
// cannot undo an ignore it was started with. A handled signal is reset to its
// default by exec. Installed once, before anything is written.
void guard_broken_pipe();

} // namespace mcpp::platform::unixproc

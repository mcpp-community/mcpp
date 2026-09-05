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

module;

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdio>       // fputs, stderr — the out-of-slots diagnostic
#endif

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

// Receives stdout+stderr as it arrives. Called on the calling thread only.
//
// A NULL sink means "do not capture": the child inherits the caller's stdio and
// is still bounded. Same contract as the POSIX peer, and for the same reason —
// an uncaptured bounded run (`run_exec_deadline`) must keep its output LIVE and
// keep the child's stdout a console, or console-detecting children drop their
// colors and every line waits for exit.
using OutputSink = void (*)(void* ctx, const char* data, unsigned long len);

// `commandLine` is already quoted for CreateProcess (callers pass the output
// of windows_command_from_argv). `envEntries` is `envCount` NUL-terminated
// "KEY=VALUE" strings applied on top of the current environment. `cwd` may be
// null. A non-positive `deadlineMs` is rejected with supported=false — "no
// bound" belongs on the caller's untimed path, which needs none of this.
DeadlineRun capture_with_deadline(const char*        commandLine,
                                  const char* const* envEntries,
                                  unsigned long      envCount,
                                  const char*        cwd,
                                  long long          deadlineMs,
                                  OutputSink         sink,
                                  void*              ctx);

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
void guard_job_on_signal(unsigned long long job);
void unguard_job(unsigned long long job);
void clear_job_guard();

// Wait for a child started by `spawn_background` and return its exit code.
// Blocking, so an owned run costs no polling latency; `background_running` stays
// for the supervisor that must not block.
int wait_background(unsigned long long process, int* exitCode);

} // namespace mcpp::platform::winproc

namespace mcpp::platform::winproc {

#if defined(_WIN32)

namespace {

// A `\0`-separated, `\0\0`-terminated block: the current environment with
// `envEntries` applied on top.
//
// The override match is case-INSENSITIVE because Windows environment names
// are: passing `Path=` alongside an existing `PATH=` would otherwise leave two
// entries and let the loader pick.
std::string environment_block(const char* const* envEntries,
                              unsigned long      envCount) {
    auto upper = [](std::string s) {
        for (auto& c : s)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return s;
    };

    std::vector<std::string> overriddenUpper;
    overriddenUpper.reserve(envCount);
    for (unsigned long i = 0; i < envCount; ++i) {
        std::string entry(envEntries[i]);
        auto eq = entry.find('=');
        overriddenUpper.push_back(upper(entry.substr(0, eq == std::string::npos
                                                        ? entry.size() : eq)));
    }

    std::vector<std::string> entries;
    if (LPCH env = ::GetEnvironmentStringsA()) {
        for (const char* p = env; *p; ) {
            std::string entry(p);
            p += entry.size() + 1;
            // Drive-letter entries ("=C:=C:\path") start with '=' and have no
            // name to compare; they must survive verbatim or relative paths
            // resolve differently in the child.
            auto eq = entry.find('=', 1);
            if (eq != std::string::npos) {
                if (std::ranges::find(overriddenUpper, upper(entry.substr(0, eq)))
                    != overriddenUpper.end())
                    continue;
            }
            entries.push_back(std::move(entry));
        }
        ::FreeEnvironmentStringsA(env);
    }
    for (unsigned long i = 0; i < envCount; ++i)
        entries.emplace_back(envEntries[i]);

    std::string block;
    for (auto const& e : entries) { block += e; block.push_back('\0'); }
    // An empty block still needs its terminator or CreateProcess reads past
    // the buffer.
    block.push_back('\0');
    return block;
}

struct Handle {
    HANDLE h = nullptr;
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { reset(); }
    void reset() {
        if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
        h = nullptr;
    }
};

} // namespace

DeadlineRun capture_with_deadline(const char*        commandLine,
                                  const char* const* envEntries,
                                  unsigned long      envCount,
                                  const char*        cwd,
                                  long long          deadlineMs,
                                  OutputSink         sink,
                                  void*              ctx)
{
    DeadlineRun out;
    if (deadlineMs <= 0 || !commandLine || !*commandLine) return out;

    const bool capture = (sink != nullptr);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    Handle readEnd, writeEnd;
    if (capture) {
        if (!::CreatePipe(&readEnd.h, &writeEnd.h, &sa, 0)) return out;
        // Only the WRITE end may cross into the child. An inheritable read end
        // there would keep the pipe alive past the child's exit and the drain
        // below would never see EOF.
        if (!::SetHandleInformation(readEnd.h, HANDLE_FLAG_INHERIT, 0)) return out;
    }

    Handle job;
    job.h = ::CreateJobObjectA(nullptr, nullptr);
    if (job.h) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
        jeli.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ::SetInformationJobObject(job.h, JobObjectExtendedLimitInformation,
                                  &jeli, sizeof(jeli));
    }

    // A CAPTURED child gets stdin from NUL, never from the console.
    //
    // This is not tidiness: mcpp's Windows launchers have always sealed stdin
    // (see this file's peer, mcpp.platform.process, and the bug it names —
    // xlings / xim / curl / git children blocking on terminal input during
    // bootstrap, forcing the user to hammer Enter). The path this replaces
    // went through `_popen` with `< NUL` appended, so inheriting the console's
    // stdin here would quietly bring that hang back.
    //
    // An UNCAPTURED child keeps the real stdin, matching run_exec: `mcpp run`
    // hands the terminal to the program on purpose.
    Handle nulIn;
    if (capture) {
        nulIn.h = ::CreateFileA("NUL", GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                OPEN_EXISTING, 0, nullptr);
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    if (capture) {
        si.dwFlags    = STARTF_USESTDHANDLES;
        si.hStdOutput = writeEnd.h;
        si.hStdError  = writeEnd.h;
        // If NUL could not be opened, fall back to the console handle rather
        // than handing the child an invalid one — a child with no stdin at all
        // fails in ways that look nothing like "stdin was not sealed".
        si.hStdInput  = (nulIn.h && nulIn.h != INVALID_HANDLE_VALUE)
                            ? nulIn.h
                            : ::GetStdHandle(STD_INPUT_HANDLE);
    }
    // else: no STARTF_USESTDHANDLES — the child inherits our console.

    PROCESS_INFORMATION pi{};
    std::string cmdBuf(commandLine);        // CreateProcessA may modify it
    auto envBlock = environment_block(envEntries, envCount);

    // CREATE_SUSPENDED so the child joins the job BEFORE it can spawn
    // anything — a grandchild created in that gap would escape the kill.
    // CREATE_NO_WINDOW only when capturing: an uncaptured run is meant to be
    // seen, and suppressing the console for it would hide the output this
    // branch exists to show.
    BOOL ok = ::CreateProcessA(
        nullptr, cmdBuf.data(), nullptr, nullptr, /*bInheritHandles=*/TRUE,
        CREATE_SUSPENDED | (capture ? CREATE_NO_WINDOW : 0u),
        envBlock.data(),
        (cwd && *cwd) ? cwd : nullptr,
        &si, &pi);
    if (!ok) { out.spawn_error = static_cast<int>(::GetLastError()); return out; }

    Handle proc;   proc.h   = pi.hProcess;
    Handle thread; thread.h = pi.hThread;
    if (job.h) ::AssignProcessToJobObject(job.h, proc.h);
    ::ResumeThread(thread.h);

    // The parent must drop its copy of the write end or the pipe never reaches
    // EOF, even after every child has exited.
    writeEnd.reset();

    const auto until = std::chrono::steady_clock::now()
                     + std::chrono::milliseconds(deadlineMs);
    std::array<char, 4096> buf{};
    bool killed = false;

    auto drain_available = [&]() -> bool {
        if (!capture) return false;
        DWORD avail = 0;
        if (!::PeekNamedPipe(readEnd.h, nullptr, 0, nullptr, &avail, nullptr))
            return false;
        if (avail == 0) return false;
        DWORD want = static_cast<DWORD>(
            std::min<std::size_t>(buf.size(), static_cast<std::size_t>(avail)));
        DWORD got = 0;
        if (!::ReadFile(readEnd.h, buf.data(), want, &got, nullptr) || got == 0)
            return false;
        if (sink) sink(ctx, buf.data(), static_cast<unsigned long>(got));
        return true;
    };

    for (;;) {
        if (drain_available()) continue;   // empty the pipe before sleeping

        if (::WaitForSingleObject(proc.h, 0) == WAIT_OBJECT_0) {
            while (drain_available()) { /* tail */ }
            break;
        }

        if (!killed && std::chrono::steady_clock::now() >= until) {
            killed = true;
            // Closing the job takes the whole tree with it. TerminateProcess
            // alone would leave grandchildren holding the pipe open.
            if (job.h) job.reset();
            else       ::TerminateProcess(proc.h, 1);
            ::WaitForSingleObject(proc.h, 5000);
            continue;
        }

        ::Sleep(20);
    }

    DWORD code = 0;
    ::GetExitCodeProcess(proc.h, &code);
    out.exit_code = static_cast<int>(code);
    out.timed_out = killed;
    out.supported = true;
    return out;
}

// ─── Background children ─────────────────────────────────────────────────

namespace {

// Read by a console control handler, which runs on a thread of the OS's
// choosing. Only the handle is shared, and closing a job handle is atomic from
// the caller's point of view.
constexpr int kMaxGuardedJobs = 8;
volatile unsigned long long g_guardedJobs[kMaxGuardedJobs] = {};

BOOL WINAPI background_console_handler(DWORD) {
    // TerminateJobObject, not CloseHandle: this handler races `background_stop`
    // on the normal path, and terminating is idempotent while closing the same
    // handle twice is not. The handle stays valid for whoever closes it.
    for (int i = 0; i < kMaxGuardedJobs; ++i) {
        const auto job = g_guardedJobs[i];
        if (job)
            ::TerminateJobObject(
                reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(job)), 1);
    }
    return FALSE;   // FALSE = also run the default handler, i.e. still exit
}

} // namespace

BackgroundChild spawn_background(const char* commandLine,
                                 const char* cwd,
                                 int         inheritStdio)
{
    BackgroundChild out;
    if (!commandLine || !*commandLine) return out;

    HANDLE job = ::CreateJobObjectA(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
        jeli.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ::SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                  &jeli, sizeof(jeli));
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    Handle nulIo;
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    if (!inheritStdio) {
        nulIo.h = ::CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                OPEN_EXISTING, 0, nullptr);
        if (nulIo.h && nulIo.h != INVALID_HANDLE_VALUE) {
            si.dwFlags    = STARTF_USESTDHANDLES;
            si.hStdInput  = nulIo.h;
            si.hStdOutput = nulIo.h;
            si.hStdError  = nulIo.h;
        }
    }

    PROCESS_INFORMATION pi{};
    std::string cmdBuf(commandLine);        // CreateProcessA may modify it

    // CREATE_SUSPENDED so the child joins the job BEFORE it can spawn anything
    // — a grandchild created in that gap would escape the kill, which for a
    // background player is the difference between "stops" and "plays forever".
    //
    // CREATE_NEW_PROCESS_GROUP is the peer of POSIX_SPAWN_SETPGROUP: the child
    // stops receiving the console's Ctrl-C, which is what makes the guard
    // below necessary and what stops a stray Ctrl-C from half-killing the tree.
    const BOOL ok = ::CreateProcessA(
        nullptr, cmdBuf.data(), nullptr, nullptr,
        /*bInheritHandles=*/TRUE,
        CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP
            | (inheritStdio ? 0u : CREATE_NO_WINDOW),
        nullptr, (cwd && *cwd) ? cwd : nullptr, &si, &pi);
    if (!ok) {
        out.refused = ::GetLastError();
        if (job) ::CloseHandle(job);
        return out;
    }
    if (job) ::AssignProcessToJobObject(job, pi.hProcess);
    ::ResumeThread(pi.hThread);
    ::CloseHandle(pi.hThread);

    out.ok      = true;
    out.job     = static_cast<unsigned long long>(
                      reinterpret_cast<std::uintptr_t>(job));
    out.process = static_cast<unsigned long long>(
                      reinterpret_cast<std::uintptr_t>(pi.hProcess));
    return out;
}

int background_running(unsigned long long process, int* exitCode) {
    if (!process) return -1;
    auto h = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(process));
    const DWORD r = ::WaitForSingleObject(h, 0);
    if (r == WAIT_TIMEOUT)  return 1;
    if (r != WAIT_OBJECT_0) return -1;
    if (exitCode) {
        DWORD code = 0;
        ::GetExitCodeProcess(h, &code);
        *exitCode = static_cast<int>(code);
    }
    return 0;
}

void background_stop(unsigned long long job, unsigned long long process,
                     long long graceMs)
{
    auto procH = process ? reinterpret_cast<HANDLE>(
                               static_cast<std::uintptr_t>(process))
                         : nullptr;
    auto jobH  = job ? reinterpret_cast<HANDLE>(
                           static_cast<std::uintptr_t>(job))
                     : nullptr;

    // NO POLITE ASK HERE, AND `graceMs` IS DELIBERATELY UNSPENT.
    //
    // The obvious "ask first" is
    //
    //     ::GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, ::GetProcessId(procH));
    //
    // and it is wrong in a way that does not show up locally. That call
    // addresses a process GROUP attached to this console, not a process; when
    // the id does not name a live group of ours — which it does not, once the
    // child has already exited, and `start /b`-style commands exit at once —
    // the event reaches everything sharing the console instead. Measured on
    // the Windows e2e runner: the whole suite died eleven seconds into the
    // hooks test with exit code -1073741510 (0xC000013A,
    // STATUS_CONTROL_C_EXIT) and printed no summary at all, because mcpp had
    // Ctrl-Break'd its own console.
    //
    // Windows has no portable graceful stop for a child with no console and no
    // window of its own. The job IS the mechanism; the POSIX peer's
    // SIGTERM-then-grace has no equivalent here, and the asymmetry is stated
    // in the declaration rather than faked with a call that reaches too far.
    (void)graceMs;

    // Closing a KILL_ON_JOB_CLOSE job takes the whole tree. TerminateProcess
    // on the child alone would leave whatever it started behind — which for
    // `start /b cmd /c player` is the player.
    if (jobH)  ::CloseHandle(jobH);
    if (procH) ::CloseHandle(procH);
}

void guard_job_on_signal(unsigned long long job) {
    if (!job) return;
    for (int i = 0; i < kMaxGuardedJobs; ++i) {
        if (g_guardedJobs[i] == 0) {
            g_guardedJobs[i] = job;
            ::SetConsoleCtrlHandler(background_console_handler, TRUE);
            return;
        }
    }
    std::fputs("mcpp: internal: more than 8 concurrently guarded jobs; the "
               "newest is NOT guarded and may outlive mcpp\n", stderr);
}

void unguard_job(unsigned long long job) {
    if (!job) return;
    bool any = false;
    for (int i = 0; i < kMaxGuardedJobs; ++i) {
        if (g_guardedJobs[i] == job) g_guardedJobs[i] = 0;
        else if (g_guardedJobs[i] != 0) any = true;
    }
    if (!any) ::SetConsoleCtrlHandler(background_console_handler, FALSE);
}

void clear_job_guard() {
    for (int i = 0; i < kMaxGuardedJobs; ++i) g_guardedJobs[i] = 0;
    ::SetConsoleCtrlHandler(background_console_handler, FALSE);
}

int wait_background(unsigned long long process, int* exitCode) {
    HANDLE h = reinterpret_cast<HANDLE>(process);
    if (!h) return -1;
    ::WaitForSingleObject(h, INFINITE);
    DWORD code = 0;
    if (!::GetExitCodeProcess(h, &code)) return -1;
    if (exitCode) *exitCode = static_cast<int>(code);
    return 0;
}

#else

DeadlineRun capture_with_deadline(const char*, const char* const*, unsigned long,
                                  const char*, long long, OutputSink, void*) {
    // Not Windows: the POSIX launcher in mcpp.platform.process owns this.
    return {};
}

BackgroundChild spawn_background(const char*, const char*, int) { return {}; }
int  background_running(unsigned long long, int*) { return -1; }
void background_stop(unsigned long long, unsigned long long, long long) {}
void guard_job_on_signal(unsigned long long) {}
void unguard_job(unsigned long long) {}
void clear_job_guard() {}
int  wait_background(unsigned long long, int*) { return -1; }

#endif

} // namespace mcpp::platform::winproc

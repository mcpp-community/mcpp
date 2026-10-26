// mcpp.platform.windows.bounded_process on Windows.
module;
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>       // fputs, stderr — the out-of-slots diagnostic

module mcpp.platform.windows.bounded_process;

import std;

namespace mcpp::platform::winproc {

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

// The one section every child start shares (see LaunchSection).
std::mutex g_launchMutex;

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

LaunchSection::LaunchSection() {
    g_launchMutex.lock();
    held_ = true;
}

LaunchSection::~LaunchSection() { release(); }

void LaunchSection::release() {
    if (!held_) return;
    held_ = false;
    g_launchMutex.unlock();
}

DeadlineRun capture_with_deadline(const char*        commandLine,
                                  const char* const* envEntries,
                                  unsigned long      envCount,
                                  const char*        cwd,
                                  long long          deadlineMs,
                                  long long          idleMs,
                                  OutputSink         sink,
                                  void*              ctx,
                                  StopPoll           stop)
{
    DeadlineRun out;
    const bool idleBound = idleMs > 0 && sink != nullptr;
    // A child with no bound at all is accepted only with a sink, as on POSIX.
    if ((deadlineMs <= 0 && !idleBound && sink == nullptr) || !commandLine || !*commandLine)
        return out;

    const bool capture = (sink != nullptr);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    // From the creation of the pipe to the parent's close of its write end: no
    // other child may be started while this pipe is inheritable.
    LaunchSection launch;
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
    launch.release();

    const auto started = std::chrono::steady_clock::now();
    const auto until = deadlineMs > 0
        ? started + std::chrono::milliseconds(deadlineMs)
        : std::chrono::steady_clock::time_point::max();
    auto lastOutput = started;
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
        lastOutput = std::chrono::steady_clock::now();
        return true;
    };

    for (;;) {
        // A stream of heartbeats cannot defer a caller's progress deadline.
        if (!killed && (std::chrono::steady_clock::now() >= until || (stop && stop(ctx)))) {
            killed = true;
            if (job.h) job.reset();
            else ::TerminateProcess(proc.h, 1);
        }
        if (drain_available()) continue;   // empty the pipe before sleeping

        if (::WaitForSingleObject(proc.h, 0) == WAIT_OBJECT_0) {
            while (drain_available()) { /* tail */ }
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        const bool overIdle = idleBound
            && now - lastOutput >= std::chrono::milliseconds(idleMs);
        if (!killed && (now >= until || overIdle)) {
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
constexpr int kMaxGuardedJobs = 256;
volatile unsigned long long g_guardedJobs[kMaxGuardedJobs] = {};
// Serializes the WRITERS of the registry and of the handler it installs; the
// console handler never takes it.
std::mutex g_guardMutex;

// The files an interrupt leaves; every set lives until exit (the POSIX peer
// states why).
struct InterruptFiles {
    std::vector<std::string> paths, temps, contents;
};
std::atomic<InterruptFiles*> g_interrupt{nullptr};
std::vector<std::unique_ptr<InterruptFiles>> g_interruptSets;

void write_interrupt_files() {
    auto* set = g_interrupt.load();
    if (!set) return;
    for (std::size_t i = 0; i < set->paths.size(); ++i) {
        HANDLE f = ::CreateFileA(set->temps[i].c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;
        DWORD written = 0;
        const BOOL ok = ::WriteFile(f, set->contents[i].data(),
                                    static_cast<DWORD>(set->contents[i].size()), &written, nullptr);
        ::CloseHandle(f);
        if (ok && written == set->contents[i].size())
            ::MoveFileExA(set->temps[i].c_str(), set->paths[i].c_str(), MOVEFILE_REPLACE_EXISTING);
        else
            ::DeleteFileA(set->temps[i].c_str());
    }
}

bool handler_needed() {
    if (g_interrupt.load()) return true;
    for (int i = 0; i < kMaxGuardedJobs; ++i)
        if (g_guardedJobs[i] != 0) return true;
    return false;
}

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
    write_interrupt_files();
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
    BOOL ok = FALSE;
    {
        // This child inherits every inheritable handle, another thread's
        // capture pipe included, unless it is started outside the window that
        // pipe is open in (see LaunchSection).
        LaunchSection launch;
        ok = ::CreateProcessA(
            nullptr, cmdBuf.data(), nullptr, nullptr,
            /*bInheritHandles=*/TRUE,
            CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP
                | (inheritStdio ? 0u : CREATE_NO_WINDOW),
            nullptr, (cwd && *cwd) ? cwd : nullptr, &si, &pi);
    }
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
    {
        std::lock_guard lock(g_guardMutex);
        for (int i = 0; i < kMaxGuardedJobs; ++i) {
            if (g_guardedJobs[i] == 0) {
                g_guardedJobs[i] = job;
                ::SetConsoleCtrlHandler(background_console_handler, TRUE);
                return;
            }
        }
    }
    std::fputs("mcpp: internal: more than 256 concurrently guarded jobs; the "
               "newest is NOT guarded and may outlive mcpp\n", stderr);
}

void unguard_job(unsigned long long job) {
    if (!job) return;
    std::lock_guard lock(g_guardMutex);
    for (int i = 0; i < kMaxGuardedJobs; ++i)
        if (g_guardedJobs[i] == job) g_guardedJobs[i] = 0;
    if (!handler_needed()) ::SetConsoleCtrlHandler(background_console_handler, FALSE);
}

void clear_job_guard() {
    std::lock_guard lock(g_guardMutex);
    for (int i = 0; i < kMaxGuardedJobs; ++i) g_guardedJobs[i] = 0;
    if (!handler_needed()) ::SetConsoleCtrlHandler(background_console_handler, FALSE);
}

void set_interrupt_files(const std::vector<std::string>& paths,
                         const std::vector<std::string>& temps,
                         const std::vector<std::string>& contents) {
    if (paths.empty() || paths.size() > 4 || temps.size() != paths.size()
        || contents.size() != paths.size())
        return;
    std::lock_guard lock(g_guardMutex);
    g_interruptSets.push_back(std::make_unique<InterruptFiles>(InterruptFiles{paths, temps, contents}));
    g_interrupt.store(g_interruptSets.back().get());
    ::SetConsoleCtrlHandler(background_console_handler, TRUE);
}

void clear_interrupt_files() {
    std::lock_guard lock(g_guardMutex);
    g_interrupt.store(nullptr);
    if (!handler_needed()) ::SetConsoleCtrlHandler(background_console_handler, FALSE);
}

namespace {
BOOL WINAPI ignore_interrupt(DWORD type) {
    return (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) ? TRUE : FALSE;
}
} // namespace

int run_foreground(const char* commandLine, unsigned long* refused) {
    if (refused) *refused = 0;
    if (!commandLine || !*commandLine) return -1;

    HANDLE job = ::CreateJobObjectA(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ::SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                  &jeli, sizeof(jeli));
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::string cmdBuf(commandLine);        // CreateProcessA may modify it

    // A handler, not SetConsoleCtrlHandler(nullptr, TRUE): the null form sets
    // an attribute the child inherits, and the child would then ignore the
    // Ctrl-C it is meant to receive.
    ::SetConsoleCtrlHandler(ignore_interrupt, TRUE);
    BOOL ok = FALSE;
    {
        LaunchSection launch;
        // No CREATE_NEW_PROCESS_GROUP: that flag is what keeps the console's
        // Ctrl-C from the child. CREATE_SUSPENDED so the child joins the job
        // before it can start anything.
        ok = ::CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr,
                              /*bInheritHandles=*/TRUE, CREATE_SUSPENDED,
                              nullptr, nullptr, &si, &pi);
    }
    if (!ok) {
        if (refused) *refused = ::GetLastError();
        if (job) ::CloseHandle(job);
        ::SetConsoleCtrlHandler(ignore_interrupt, FALSE);
        return -1;
    }
    if (job) ::AssignProcessToJobObject(job, pi.hProcess);
    ::ResumeThread(pi.hThread);
    ::CloseHandle(pi.hThread);

    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hProcess);
    if (job) ::CloseHandle(job);
    ::SetConsoleCtrlHandler(ignore_interrupt, FALSE);
    return static_cast<int>(code);
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

} // namespace mcpp::platform::winproc

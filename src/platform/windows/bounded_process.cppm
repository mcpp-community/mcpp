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
// ⚠️ WHY THE INTERFACE HAS NO `std` TYPES IN IT
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

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    if (capture) {
        si.dwFlags    = STARTF_USESTDHANDLES;
        si.hStdOutput = writeEnd.h;
        si.hStdError  = writeEnd.h;
        si.hStdInput  = ::GetStdHandle(STD_INPUT_HANDLE);
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
    if (!ok) return out;

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

#else

DeadlineRun capture_with_deadline(const char*, const char* const*, unsigned long,
                                  const char*, long long, OutputSink, void*) {
    // Not Windows: the POSIX launcher in mcpp.platform.process owns this.
    return {};
}

#endif

} // namespace mcpp::platform::winproc

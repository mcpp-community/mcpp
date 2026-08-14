// bench.platform:posix — process launch, wall-clock timing and host facts on
// POSIX (Linux + macOS).
//
// SHAPE: the ENTIRE body is inside `#if !defined(_WIN32)`. On Windows this file
// still compiles — it just declares the partition and exports nothing. The peer
// partition exports the same names, so exactly one definition of each exists in
// any build and the compiler selects the platform for us. No stubs, no dead
// branches, no `if constexpr` dispatch at the call sites.
//
// This is the convention used by xlings' src/platform/*.cppm; bench follows it
// so the two codebases read the same way.
module;

#if !defined(_WIN32)
#include <errno.h>
#include <fcntl.h>
#include <signal.h>   // kill/SIGKILL for the run_process timeout
#include <spawn.h>
#include <sys/wait.h>
#include <stdlib.h>   // setenv / unsetenv — needed on Darwin too, where they
                      // live in <_stdlib.h> and are NOT reachable through the
                      // other POSIX headers this file pulls in
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#else
#include <stdio.h>
#include <string.h>
#endif
extern "C" char** environ;
#endif

export module bench.platform:posix;

import std;

#if !defined(_WIN32)

namespace bench::platform_impl {

export constexpr std::string_view OS_NAME =
#if defined(__APPLE__)
    "macos";
#else
    "linux";
#endif

static unsigned long long now_ns() {
    struct timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<unsigned long long>(ts.tv_sec) * 1000000000ULL
         + static_cast<unsigned long long>(ts.tv_nsec);
}

// Runs argv in `cwd`, child stdout+stderr to `log` (empty → discarded).
// Returns the exit status, or -1 if the child could not be started; the
// distinction matters because "could not start" is what tells probe() an engine
// is absent rather than broken.
//
// `timeout_s` <= 0 means wait forever. A build engine CAN hang — bazel fetching
// a module from a registry that never answers is the one seen here — and a
// benchmark that hangs with it is worse than one that fails: the CI job burns
// its whole budget and the log says nothing, because the harness only prints a
// cell once the cell is over. Two jobs sat 25 minutes inside one child that way,
// on a cell whose sibling finished in four.
export int run_process(const std::vector<std::string>& argv,
                       const std::filesystem::path&    cwd,
                       const std::filesystem::path&    log,
                       double*                         out_wall_s,
                       double                          timeout_s   = 0.0,
                       bool*                           out_timeout = nullptr) {
    if (out_wall_s)  *out_wall_s  = 0.0;
    if (out_timeout) *out_timeout = false;
    if (argv.empty()) return -1;

    std::vector<char*> raw;
    raw.reserve(argv.size() + 1);
    for (const auto& a : argv) raw.push_back(const_cast<char*>(a.c_str()));
    raw.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    if (::posix_spawn_file_actions_init(&actions) != 0) return -1;

    // chdir must happen in the CHILD. A process-wide chdir here would race with
    // everything else the harness does and would leave the wrong cwd behind on
    // any early return.
    if (!cwd.empty()) {
        const std::string cwd_s = cwd.string();
        if (::posix_spawn_file_actions_addchdir_np(&actions, cwd_s.c_str()) != 0) {
            ::posix_spawn_file_actions_destroy(&actions);
            return -1;
        }
    }

    const std::string log_s = log.empty() ? std::string("/dev/null") : log.string();
    // APPEND, not truncate. A cell runs configure, then a seed build, then N
    // timed builds — all into one log path. Truncating meant the build's output
    // erased the configure's, so when a cold cell came back at 0.60s the log
    // held one line ("build ok, spent 0.111s") and nothing about the configure
    // that had just been asked to set the output directory. The runner clears
    // the file once per cell (see Runner::measure), which is the right grain.
    const int flags = log.empty() ? O_WRONLY : (O_WRONLY | O_CREAT | O_APPEND);
    ::posix_spawn_file_actions_addopen(&actions, 1, log_s.c_str(), flags, 0644);
    ::posix_spawn_file_actions_adddup2(&actions, 1, 2);

    // The child leads its OWN process group, so a timeout can kill everything it
    // started rather than just the tool itself.
    //
    // A build engine is a process tree: ninja and a pool of compilers, bazel and
    // a server, xmake and its own children. Killing only the direct child leaves
    // that pool running — reparented, invisible, and still holding the CPU while
    // the NEXT cells are being timed. A single timeout would then inflate every
    // measurement after it, with nothing in the report to show why. That is the
    // expensive shape: not a failure, a quietly wrong number.
    //
    // It must be a new group and not the harness's: `kill(-pid)` on a shared
    // group reaches the harness too.
    posix_spawnattr_t attr;
    bool own_group = false;
    if (::posix_spawnattr_init(&attr) == 0) {
        if (::posix_spawnattr_setpgroup(&attr, 0) == 0 &&
            ::posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP) == 0)
            own_group = true;
    }

    const unsigned long long t0 = now_ns();
    ::pid_t pid = 0;
    const int rc = ::posix_spawnp(&pid, raw[0], &actions, own_group ? &attr : nullptr,
                                  raw.data(), ::environ);
    ::posix_spawn_file_actions_destroy(&actions);
    ::posix_spawnattr_destroy(&attr);
    if (rc != 0) return -1;

    int status = 0;
    if (timeout_s <= 0.0) {
        while (::waitpid(pid, &status, 0) < 0) {
            if (errno != EINTR) return -1;
        }
    } else {
        // Poll rather than alarm/sigtimedwait: the harness must not install a
        // signal handler, because the child inherits the disposition and a
        // compiler that ignores SIGALRM is a compiler that behaves differently
        // under measurement than in real use.
        //
        // 20ms is well under the noise floor of anything timed here (the
        // fastest measured cell is a ~10ms noop) and costs ~50 wakeups a second
        // on a machine already running a compiler.
        const auto deadline_ns = t0 + static_cast<unsigned long long>(timeout_s * 1e9);
        for (;;) {
            const ::pid_t r = ::waitpid(pid, &status, WNOHANG);
            if (r == pid) break;
            if (r < 0) { if (errno == EINTR) continue; return -1; }
            if (now_ns() >= deadline_ns) {
                // SIGKILL, not SIGTERM: the thing being killed is a build tool
                // that may have spawned a job server and a pool of compilers,
                // and a polite signal it chooses to handle leaves the harness
                // waiting on exactly the hang it is trying to escape.
                //
                // THE GROUP, not just the child — that is what the spawn above
                // set up. `kill(-pid)` reaches the compilers the tool started;
                // killing the tool alone leaves them running and stealing CPU
                // from every cell measured afterwards. Falls back to the single
                // process when the group could not be set (the flag is POSIX,
                // but this must not depend on it succeeding).
                if (own_group) ::kill(-pid, SIGKILL);
                ::kill(pid, SIGKILL);
                while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
                if (out_wall_s)  *out_wall_s  = static_cast<double>(now_ns() - t0) / 1e9;
                if (out_timeout) *out_timeout = true;
                // 124 is what `timeout(1)` reports, so the number is already
                // familiar; `out_timeout` is what callers actually branch on,
                // since a build tool may legitimately exit 124 on its own.
                return 124;
            }
            struct timespec nap{0, 20 * 1000 * 1000};
            ::nanosleep(&nap, nullptr);
        }
    }
    if (out_wall_s) *out_wall_s = static_cast<double>(now_ns() - t0) / 1e9;

    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

// Environment mutation is a platform concern (setenv here, _putenv_s on
// Windows), so it lives with the other platform primitives rather than being
// #if'd at a call site.
export void set_env(const std::string& key, const std::string& value) {
    ::setenv(key.c_str(), value.c_str(), /*overwrite*/ 1);
}

export void unset_env(const std::string& key) { ::unsetenv(key.c_str()); }

export int cpu_logical() {
    const long n = ::sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? static_cast<int>(n) : 1;
}

#if defined(__APPLE__)

export int cpu_physical() {
    int    value = 0;
    std::size_t len = sizeof(value);
    if (::sysctlbyname("hw.physicalcpu", &value, &len, nullptr, 0) == 0 && value > 0)
        return value;
    return cpu_logical();
}

export std::string cpu_model() {
    char        buf[256] = {};
    std::size_t len      = sizeof(buf);
    if (::sysctlbyname("machdep.cpu.brand_string", buf, &len, nullptr, 0) != 0) return {};
    return std::string(buf);
}

export std::uint64_t ram_bytes() {
    std::uint64_t value = 0;
    std::size_t   len   = sizeof(value);
    if (::sysctlbyname("hw.memsize", &value, &len, nullptr, 0) == 0) return value;
    return 0;
}

// Apple Silicon is performance/efficiency by construction. `hw.nperflevels`
// states it directly; on Intel Macs it is absent and the answer is no.
export bool heterogeneous_cpu() {
    int    value = 0;
    std::size_t len = sizeof(value);
    if (::sysctlbyname("hw.nperflevels", &value, &len, nullptr, 0) == 0) return value > 1;
    return false;
}

#else  // Linux

static std::string read_cpuinfo_field(std::string_view key) {
    std::ifstream in("/proc/cpuinfo");
    if (!in) return {};
    std::string line;
    while (std::getline(in, line)) {
        if (!std::string_view(line).starts_with(key)) continue;
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        auto value = std::string_view(line).substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
            value.remove_prefix(1);
        return std::string(value);
    }
    return {};
}

export int cpu_physical() {
    // "cpu cores" is per-socket. Multi-socket would need the full topology walk;
    // over-counting would be worse than falling back to the logical count, which
    // is never wrong, only imprecise.
    const auto s = read_cpuinfo_field("cpu cores");
    if (!s.empty()) {
        if (const int n = std::atoi(s.c_str()); n > 0) return n;
    }
    return cpu_logical();
}

export std::string cpu_model() { return read_cpuinfo_field("model name"); }

export std::uint64_t ram_bytes() {
    const long pages = ::sysconf(_SC_PHYS_PAGES);
    const long size  = ::sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && size > 0)
        return static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(size);
    return 0;
}

// Hybrid x86 (P-cores + E-cores) reports differing per-CPU max frequencies.
// Cheapest reliable signal short of CPUID: on a homogeneous part every
// cpuinfo_max_freq is identical. No cpufreq at all → cannot tell → say no,
// because a false "heterogeneous" would misread every parallelism figure.
export bool heterogeneous_cpu() {
    const int n = cpu_logical();
    if (n <= 1) return false;
    long first = -1;
    for (int i = 0; i < n; ++i) {
        std::ifstream in(std::format(
            "/sys/devices/system/cpu/cpu{}/cpufreq/cpuinfo_max_freq", i));
        if (!in) return false;
        long v = 0;
        if (!(in >> v)) return false;
        if (first < 0) first = v;
        else if (v != first) return true;
    }
    return false;
}

#endif  // __APPLE__

}  // namespace bench::platform_impl

#endif  // !_WIN32

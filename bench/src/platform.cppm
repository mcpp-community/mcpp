// bench.platform — the suite's single door to the operating system.
//
// The per-platform partitions each guard their whole body with one macro and
// export the SAME names, so exactly one definition of each survives in any
// build. This module re-exports them and adds the parts that need no platform
// knowledge. Consequence: `#if defined(_WIN32)` appears in the two partitions
// and nowhere else — runner, engines, fixtures and analysis contain no platform
// conditionals at all.
export module bench.platform;

import std;

export import :posix;
export import :windows;

export namespace bench::platform {

// Platform-selected primitives, lifted out of the partitions.
using platform_impl::OS_NAME;
using platform_impl::cpu_logical;
using platform_impl::cpu_physical;
using platform_impl::cpu_model;
using platform_impl::ram_bytes;
using platform_impl::heterogeneous_cpu;
using platform_impl::run_process;
using platform_impl::set_env;
using platform_impl::unset_env;

// Sets an environment variable for the lifetime of the guard and restores the
// previous state — including "was not set at all", which is distinct from "was
// empty" to a child process. Engines use this to toggle a build flag for one
// measured cell without leaking it into the next.
class ScopedEnv {
public:
    ScopedEnv(std::string key, const std::string& value);
    ~ScopedEnv();
    ScopedEnv(const ScopedEnv&)            = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    std::string key_;
    std::string previous_;
    bool        had_previous_{};
};

struct RunResult {
    double wall_s{};
    int    exit_code{};
    // Set when the child was killed for exceeding its deadline. A separate flag
    // rather than a reserved exit code: 124 is the `timeout(1)` convention and
    // is a perfectly legal thing for a build tool to exit with on its own, so
    // "hung" and "exited 124" must stay distinguishable.
    bool   timed_out{};
    // WHY the child could not be launched, in the OS's own words. Empty unless
    // `started()` is false.
    //
    // "could not start the process" is true and useless: on Windows it covers a
    // program that is not on PATH, a cwd that does not exist, a bad handle and
    // an ACL, and those are four different fixes. A windows/clang cell reported
    // exactly that message while `xmake --version` had just succeeded in the
    // same job — the only difference between the two calls being a cwd and a
    // log path — and there was no way to tell which from CI. One
    // GetLastError/errno turns that into a sentence.
    std::string start_error;
    [[nodiscard]] bool ok() const;
    // Distinguishes "could not start" from "started and failed" — the whole
    // basis for reporting an engine as unavailable rather than broken.
    //
    // NOT `exit_code >= 0`. That was the test, and on Windows it is wrong in
    // the one case that matters: a child that CRASHES exits with a status like
    // 0xC0000135 (a DLL it needs is missing) or 0xC0000005 (access violation),
    // and `GetExitCodeProcess` hands back a DWORD that becomes a NEGATIVE int.
    // The harness then reported
    //
    //     could not start the process (no log written) —
    //     check the engine's program path
    //
    // for an xmake that `xlings install` had just installed successfully and
    // that the harness's own probe had just run. The advice was wrong, the
    // diagnosis was wrong, and it cost two matrix cycles chasing a PATH that was
    // never the problem.
    //
    // Launch failure is now its own fact, reported by the platform layer,
    // instead of being inferred from the shape of a number that has a different
    // meaning on each OS.
    [[nodiscard]] bool started() const;
    bool launch_failed{};
};

// Run argv, discarding the child's output unless a log path is given. The
// harness never lets build noise reach its own stdout: the report IS the
// output, and a mixed stream cannot be parsed.
//
// `timeout_s` <= 0 waits forever, which is right for a version probe and wrong
// for a build — see run_process.
RunResult run(const std::vector<std::string>& argv,
              const std::filesystem::path&    cwd = {},
              const std::filesystem::path&    log = {},
              double                          timeout_s = 0.0);

// The last `lines` lines of a file, for showing WHY a cell failed.
//
// Without this a red benchmark is as uninformative as the green one it
// replaces: the harness records `see .../logs/cmake-cold.log`, and on a CI
// runner that file is deleted with the machine. Every module cell in the matrix
// failed for weeks behind exactly that sentence.
// Does the log contain any of these markers? Used to decide how much of it is
// worth showing — a crash needs far more context than a compile error.
bool log_mentions(const std::filesystem::path& p,
                  std::initializer_list<std::string_view> markers);

// The lines anywhere in `p` that look like a cause, not a progress report.
//
// A tail cannot answer "why did this fail" for a tool that prints a line per
// translation unit: the error scrolled past hundreds of lines ago and the last
// 20 are all `[ 2%]: generating.module.deps ...`. That is exactly how an
// `xmake exited 255` cell reached CI with nothing to diagnose it by.
//
// Deliberately a keyword sieve rather than per-engine parsing: every engine
// here is a different program with a different diagnostic format, and one that
// is merely APPROXIMATELY right on all of them beats four that are exactly
// right until a tool changes its wording. False positives cost a line of noise;
// a false negative costs a matrix cycle.
std::string log_grep(const std::filesystem::path& p,
                     std::initializer_list<std::string_view> markers,
                     std::size_t max = 12);

std::string tail_of(const std::filesystem::path& p, std::size_t lines = 20);

bool have_program(const std::vector<std::string>& version_argv);

// Run argv and return its combined stdout+stderr, or nullopt if it could not be
// started. Goes through a temp file rather than a pipe: a pipe needs
// platform-specific plumbing on both sides, and the outputs captured here are
// version banners — a few dozen bytes, once per engine.
// `result`, when given, receives the child's RunResult so a caller needing both
// the output and the exit status does not have to run the command twice.
std::optional<std::string> run_capture(const std::vector<std::string>& argv,
                                       const std::filesystem::path& cwd = {},
                                       RunResult* result = nullptr);

struct HostFacts {
    std::string   os;
    std::string   arch;
    std::string   cpu_model;
    int           logical_cores{};
    int           physical_cores{};
    bool          heterogeneous{};
    std::uint64_t ram_bytes{};
};

HostFacts host_facts();

// --- portable helpers: std::filesystem needs no per-platform split ---------

void remove_tree(const std::filesystem::path& p);

// mtime bump with no content change — the `touch-*` scenarios turn on exactly
// this distinction, so it must not rewrite the file.
bool touch(const std::filesystem::path& p);

std::string iso_now();

}  // namespace bench::platform

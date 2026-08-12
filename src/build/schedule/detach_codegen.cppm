// mcpp.build.schedule.detach_codegen — the GCC strategy: let importers start
// when the BMI lands, and let code generation finish off the critical path.
//
// WHY. mcpp's own cold build is 79.9 s and its critical path is 79.73 s — 100%
// of the makespan, at an average of 3.94 concurrent jobs on 32 hardware threads.
// Nothing outside the graph's shape moves it: -j8 → -j32 buys 1.4%, cmake and
// xmake build the same sources in 94.5 s and 94.6 s, and clang only scales the
// constant (32.2 s makespan, 32.15 s critical path — the same 100%).
//
// `-ftime-report` on the chain's heaviest link says where the time goes:
//
//     phase opt and generate    14.08s  86%     <- code generation
//     phase parsing              1.32s   8%
//     template instantiation     0.95s   6%
//     module import              0.51s   3%
//
// 86% of a module interface compile is code generation, and NO IMPORTER NEEDS A
// BYTE OF IT.
//
// THE STRATEGY IS PER COMPILER, and the two are complementary rather than
// alternatives — see mcpp::toolchain::BmiSplit. This module implements the gcc
// one (DetachCodegen). Clang needs nothing from here: `--precompile` already
// splits the work into two ordinary edges.
//
// WHY WATCHING THE FILE IS SOUND FOR GCC, AND NOT A HEURISTIC. GCC writes the
// BMI to `<name>.gcm~` and rename()s it into place — verified with strace:
//
//     openat("gcm.cache/x.gcm~", O_RDWR|O_CREAT|O_TRUNC) = 5
//     rename("gcm.cache/x.gcm~", "gcm.cache/x.gcm")      = 0
//
// so the final path is atomically complete-or-absent. Confirmed three further
// ways: the early snapshot is byte-identical to the finished file, and a real
// downstream importer compiles against it and exits 0. Clang, by contrast,
// writes the BMI straight to the final path with O_TRUNC — which is exactly why
// it gets the other strategy instead of this one.
//
// MEASURED (same build dir, compiler, flags, compiler-concurrency cap and
// sources; the ONLY difference is the graph's shape):
//
//     baseline  wall=80.51s  ninja -j32   compilers<=32
//     split     wall=39.23s  ninja -j192  compilers<=32
//
// ⚠️ FOUR HAZARDS, EVERY ONE OF WHICH BIT DURING DEVELOPMENT:
//
//  1. THE COMPILER MUST NOT INHERIT ninja's PIPE. ninja finishes an edge when
//     the pipe reaches EOF, NOT when its direct child exits. An inherited pipe
//     makes the early exit invisible — every BMI edge is logged with the FULL
//     compile duration, and the arm reads as "the idea does not work". The
//     compiler's stdio goes to a file, replayed by phase 2.
//  2. ninja's -j MUST EXCEED THE COMPILER CAP. A detached compiler no longer
//     holds a ninja slot, so with -j equal to the cap the slots fill with edges
//     that are merely sleeping and the ready frontier starves — the schedule
//     degenerates to the baseline. Real concurrency is bounded by the semaphore
//     below, never by -j.
//  3. FAILURES ARRIVE LATE. A compiler that fails during code generation has
//     already had its BMI edge reported successful. Phase 2 must REPLAY that
//     failure or it surfaces as undefined symbols at link time.
//  4. THE GRAPH MUST DECLARE ITS SHAPE. build.ninja is shared mutable state and
//     the fast path replays it, so "is this graph split" belongs in the
//     `# mcpp:graph=` line — see mcpp.build.graph_shape.
//
// NOT POSIX-ONLY. What this needs is a process that outlives the current one,
// which is a spawn, not a fork; the supervisor is `mcpp` itself re-invoked.
// What is compiler-specific is the PREMISE (atomic BMI publication), not the
// platform.
module;

// The global module fragment is the ONLY place a module interface unit may
// #include. These were briefly written after `module :private;`, which GCC
// rejects with the unhelpful "module already declared".
#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

export module mcpp.build.schedule.detach_codegen;

import std;
import mcpp.build.stage;

export namespace mcpp::build::schedule::detach {

struct CompileRequest {
    // The BMI this compile publishes. Empty for a unit that produces none, in
    // which case phase 1 simply waits like an ordinary edge.
    std::filesystem::path bmi;
    // `<slot>.log` and `<slot>.rc` live beside this path.
    std::filesystem::path slot;
    // Absolute path to the mcpp binary, re-invoked as the supervisor. A spawn
    // rather than a fork is what keeps this portable.
    std::filesystem::path self;
    // Directory of concurrency tokens. Empty disables the cap (hazard 2).
    std::filesystem::path semaphore;
    int                   maxCompilers{0};
    // The compiler invocation, as ONE shell command line.
    //
    // Not a token list: the backend has already joined and quoted the flags for
    // ninja, and splitting that string back into argv would need to reimplement
    // the shell's rules — the exact assumption ("one flag element == one argv
    // token") that has been wrong here before. ninja runs every command through
    // a shell already, so going through one costs no portability.
    std::string command;
    // The file `command` was read from; handed to the supervisor unchanged, so
    // there is one representation and no re-quoting.
    std::filesystem::path commandFile;
    // Where this edge's header dependencies come from, and where ninja expects
    // to find them.
    //
    // MEASURED: the compiler's own `-MMD` file is written at 16.39s of a 16.55s
    // compile — AFTER the BMI is published at 2.36s. This edge is over before
    // it exists, so it cannot use it. The P1689 SCAN has already run and writes
    // an equivalent one; compared on real modules the only prerequisites it
    // lacks are `.gcm` BMIs, which are ninja's through dyndep. Header coverage
    // is exact.
    //
    // COPIED, not pointed at: ninja DELETES a depfile once it has folded it
    // into .ninja_deps, and the scan would not regenerate it unless the scan
    // itself reran.
    std::filesystem::path depFrom;
    std::filesystem::path depTo;
};

// Phase 1 — returns 0 as soon as the BMI is published, leaving code generation
// running. Returns the compiler's status if it exits before publishing one.
int compile_release_at_bmi(const CompileRequest& req);

// The supervisor. Runs the compiler to completion with its output redirected,
// then records the status. Never invoked directly by a build edge.
int supervise(const std::filesystem::path& slot,
              const std::filesystem::path& semaphoreToken,
              std::string_view command);

// Phase 2 — blocks until the compiler for `slot` finished, replays what it
// wrote, and propagates its status. `object`, when given, must exist: a
// compiler that reports success without producing its output is a failure this
// must not pass on.
int await_unit(const std::filesystem::path& slot, const std::filesystem::path& object);

}  // namespace mcpp::build::schedule::detach

// ---------------------------------------------------------------------------

namespace mcpp::build::schedule::detach {
namespace {

std::filesystem::path suffixed(const std::filesystem::path& base, std::string_view s) {
    return std::filesystem::path{base.string() + std::string(s)};
}

bool file_exists(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::exists(p, ec);
}

std::optional<int> read_rc(const std::filesystem::path& slot) {
    std::ifstream in(suffixed(slot, ".rc"));
    if (!in) return std::nullopt;
    int rc = 0;
    if (!(in >> rc)) return std::nullopt;
    return rc;
}

// Temp file + rename, so a reader never observes half a number. The same
// guarantee, for the same reason, that makes watching the BMI path sound.
void write_rc(const std::filesystem::path& slot, int rc) {
    const auto tmp = suffixed(slot, ".rc.tmp");
    { std::ofstream out(tmp, std::ios::trunc); out << rc << '\n'; }
    std::error_code ec;
    std::filesystem::rename(tmp, suffixed(slot, ".rc"), ec);
}

// A counting semaphore made of directories. `mkdir` is atomic on every
// filesystem mcpp targets, it needs no daemon and no shared memory, and a
// crashed holder leaves a directory that is trivially reclaimable. A holder
// never waits for another token, so this cannot deadlock.
std::filesystem::path acquire_token(const std::filesystem::path& dir, int cap) {
    if (dir.empty() || cap <= 0) return {};
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    for (;;) {
        for (int i = 0; i < cap; ++i) {
            const auto tok = dir / std::to_string(i);
            if (std::filesystem::create_directory(tok, ec) && !ec) return tok;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// Keep only the first rule of a make-style depfile.
//
// GCC emits several: the real one, then `.PHONY:` entries and a `CXX_IMPORTS`
// section. Handing those to ninja makes it believe in prerequisites that are
// not files. This is the C++ counterpart of the `awk 'NR==1{print;next}
// /^[^ ]/{exit} {print}'` that used to live in the generated command — having
// it here is also what lets the rule stop depending on a POSIX shell.
// `target` REPLACES the one in the source file, and that is not cosmetic: the
// scanner names the OBJECT, this edge's output is the BMI, and ninja silently
// treats an edge whose depfile names something else as permanently dirty. The
// symptom is a no-op build that recompiles all 140 module interfaces in 25s and
// reports success — nothing warns.
void copy_first_rule(const std::filesystem::path& from, const std::filesystem::path& to,
                     std::string_view target) {
    if (from.empty() || to.empty()) return;
    std::ifstream in(from);
    if (!in) return;
    std::ofstream out(to, std::ios::trunc);
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (first) {
            const auto colon = line.find(':');
            out << target << (colon == std::string::npos ? ":" : line.substr(colon))
                << '\n';
            first = false;
            continue;
        }
        if (!line.empty() && !std::isspace(static_cast<unsigned char>(line[0]))) break;
        out << line << '\n';
    }
}

// The BMI equivalence check, which used to be a POSIX shell one-liner inside the
// generated ninja command — and was therefore skipped entirely on Windows.
// Having it here is what brings cascade suppression to every platform.
void settle_bmi(const std::filesystem::path& bmi) {
    if (bmi.empty()) return;
    const auto backup = suffixed(bmi, ".bak");
    if (!file_exists(backup)) return;
    std::error_code ec;
    if (stage::bmi_equivalent(bmi, backup))
        std::filesystem::rename(backup, bmi, ec);   // keep the old mtime: no cascade
    else
        std::filesystem::remove(backup, ec);
}

#if defined(_WIN32)

std::string join_command(const std::vector<std::string>& argv) {
    std::string cmd;
    for (const auto& a : argv) {
        if (!cmd.empty()) cmd += ' ';
        const bool quote = a.find_first_of(" \t\"") != std::string::npos;
        if (!quote) { cmd += a; continue; }
        cmd += '"';
        for (char c : a) { if (c == '"') cmd += '\\'; cmd += c; }
        cmd += '"';
    }
    return cmd;
}

bool spawn_detached(const std::vector<std::string>& argv) {
    auto cmd = join_command(argv);
    STARTUPINFOA si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                                     DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
                                     nullptr, nullptr, &si, &pi);
    if (!ok) return false;
    ::CloseHandle(pi.hProcess); ::CloseHandle(pi.hThread);
    return true;
}

int run_to_completion(std::string_view command,
                      const std::filesystem::path& logPath) {
    const std::vector<std::string> argv{"cmd.exe", "/c", std::string(command)};
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE log = ::CreateFileA(logPath.string().c_str(), GENERIC_WRITE,
                               FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOA si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = log;
    si.hStdInput = ::CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ, &sa,
                                 OPEN_EXISTING, 0, nullptr);
    PROCESS_INFORMATION pi{};
    auto cmd = join_command(argv);
    const BOOL ok = ::CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                     0, nullptr, nullptr, &si, &pi);
    if (log != INVALID_HANDLE_VALUE) ::CloseHandle(log);
    if (si.hStdInput != INVALID_HANDLE_VALUE) ::CloseHandle(si.hStdInput);
    if (!ok) return 127;
    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hProcess); ::CloseHandle(pi.hThread);
    return static_cast<int>(code);
}

#else

std::vector<char*> to_argv(const std::vector<std::string>& argv) {
    std::vector<char*> out;
    out.reserve(argv.size() + 1);
    for (const auto& a : argv) out.push_back(const_cast<char*>(a.c_str()));
    out.push_back(nullptr);
    return out;
}

bool spawn_detached(const std::vector<std::string>& argv) {
    posix_spawn_file_actions_t fa;
    ::posix_spawn_file_actions_init(&fa);
    // HAZARD 1 also applies to the supervisor: holding ninja's pipe open would
    // keep the edge alive long after this process exits.
    ::posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    ::posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    ::posix_spawn_file_actions_adddup2(&fa, 1, 2);

    posix_spawnattr_t at;
    ::posix_spawnattr_init(&at);
#ifdef POSIX_SPAWN_SETSID
    // Its own session, so a Ctrl-C on the build does not take the supervisor
    // with it mid-write and leave a half-written object behind.
    ::posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSID);
#endif
    auto raw = to_argv(argv);
    pid_t pid = 0;
    const int rc = ::posix_spawnp(&pid, raw[0], &fa, &at, raw.data(), environ);
    ::posix_spawn_file_actions_destroy(&fa);
    ::posix_spawnattr_destroy(&at);
    return rc == 0;
}

int run_to_completion(std::string_view command,
                      const std::filesystem::path& logPath) {
    const std::vector<std::string> argv{"/bin/sh", "-c", std::string(command)};
    posix_spawn_file_actions_t fa;
    ::posix_spawn_file_actions_init(&fa);
    ::posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    ::posix_spawn_file_actions_addopen(&fa, 1, logPath.c_str(),
                                       O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ::posix_spawn_file_actions_adddup2(&fa, 1, 2);
    auto raw = to_argv(argv);
    pid_t pid = 0;
    const int rc = ::posix_spawnp(&pid, raw[0], &fa, nullptr, raw.data(), environ);
    ::posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return 127;
    int status = 0;
    ::waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status)
         : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

#endif

}  // namespace

int compile_release_at_bmi(const CompileRequest& req) {
    if (req.command.empty() || req.self.empty()) return 2;

    std::error_code ec;
    if (!req.slot.parent_path().empty())
        std::filesystem::create_directories(req.slot.parent_path(), ec);
    std::filesystem::remove(suffixed(req.slot, ".rc"), ec);
    std::filesystem::remove(suffixed(req.slot, ".rc.tmp"), ec);

    // Keep the previous BMI for the equivalence check AND get it out of the
    // way, so its mere presence can never be mistaken for the new one landing.
    if (!req.bmi.empty()) {
        const auto backup = suffixed(req.bmi, ".bak");
        std::filesystem::remove(backup, ec);
        if (file_exists(req.bmi)) std::filesystem::rename(req.bmi, backup, ec);
    }

    const auto token = acquire_token(req.semaphore, req.maxCompilers);

    // The supervisor reads the SAME argv file rather than receiving the command
    // on its own command line: one representation, no re-quoting, and no limit
    // on how long a compiler command may be.
    std::vector<std::string> sup{req.self.string(), "bmi-supervise",
                                 "--slot", req.slot.string(),
                                 "--command-file", req.commandFile.string()};
    if (!token.empty()) { sup.push_back("--token"); sup.push_back(token.string()); }
    if (!spawn_detached(sup)) return 2;

    for (;;) {
        if (!req.bmi.empty() && file_exists(req.bmi)) {
            settle_bmi(req.bmi);
            copy_first_rule(req.depFrom, req.depTo, req.bmi.string());
            return 0;                       // importers may proceed
        }
        if (const auto rc = read_rc(req.slot)) {
            if (*rc != 0) {                 // failed before publishing a BMI
                std::ifstream in(suffixed(req.slot, ".log"));
                if (in) std::cerr << in.rdbuf();
            } else {
                copy_first_rule(req.depFrom, req.depTo, req.bmi.string());
            }
            return *rc;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

int supervise(const std::filesystem::path& slot,
              const std::filesystem::path& semaphoreToken,
              std::string_view command) {
    const int rc = run_to_completion(command, suffixed(slot, ".log"));
    if (!semaphoreToken.empty()) {
        std::error_code ec;
        std::filesystem::remove(semaphoreToken, ec);
    }
    write_rc(slot, rc);
    return 0;   // the supervisor's own status is not the compiler's
}

int await_unit(const std::filesystem::path& slot, const std::filesystem::path& object) {
    // BOUNDED. An unbounded wait here turns "phase 1 never started a compiler"
    // into a build that hangs forever with no output — which is strictly worse
    // than a failure, because nothing says what to look at. If no supervisor
    // ever opened the log, there is nothing to wait for and this fails at once.
    using clock = std::chrono::steady_clock;
    const auto started = clock::now();
    constexpr auto kNoSupervisorGrace = std::chrono::seconds(10);
    constexpr auto kHardLimit         = std::chrono::hours(2);

    std::optional<int> rc;
    while (!(rc = read_rc(slot))) {
        const auto waited = clock::now() - started;
        if (!file_exists(suffixed(slot, ".log")) && waited > kNoSupervisorGrace) {
            std::println(std::cerr,
                         "mcpp: no compiler was started for {} — phase 1 did not run",
                         slot.string());
            return 1;
        }
        if (waited > kHardLimit) {
            std::println(std::cerr, "mcpp: timed out waiting for the compiler for {}",
                         slot.string());
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // HAZARD 3: the compiler's diagnostics went to a file so phase 1 could exit
    // early. Replaying them here is the only thing that keeps them.
    {
        std::ifstream in(suffixed(slot, ".log"));
        if (in) std::cerr << in.rdbuf();
    }
    if (*rc != 0) return *rc;

    if (!object.empty() && !file_exists(object)) {
        std::println(std::cerr, "mcpp: compiler reported success but {} is missing",
                     object.string());
        return 1;
    }
    return 0;
}

}  // namespace mcpp::build::schedule::detach

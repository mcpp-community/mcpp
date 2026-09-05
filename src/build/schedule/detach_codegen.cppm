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
// FOUR HAZARDS, EVERY ONE OF WHICH BIT DURING DEVELOPMENT:
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
// filesystem mcpp targets, it needs no daemon and no shared memory. A holder
// never waits for another token, so this cannot deadlock between holders.
//
// WHAT IT CAN DO IS OUTLIVE ITS HOLDER. A token is released by the
// supervisor that took it; a supervisor killed before its cleanup (Ctrl-C, the
// OOM killer, a reboot) leaves the directory behind, and nothing in this file
// ever reclaims one. The reclaim is therefore done ONCE PER BUILD, in prepare,
// before ninja is spawned — the only moment at which no token can have a live
// owner. Doing it here instead would race every other compiler in the build.
//
// The wait is bounded anyway. If a token is somehow still unreleasable, the
// honest outcome is a failure that names the directory, not a build that stops
// producing output and never returns.
std::filesystem::path acquire_token(const std::filesystem::path& dir, int cap) {
    if (dir.empty() || cap <= 0) return {};
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto started = std::chrono::steady_clock::now();
    constexpr auto kLimit = std::chrono::hours(2);
    for (;;) {
        for (int i = 0; i < cap; ++i) {
            const auto tok = dir / std::to_string(i);
            if (std::filesystem::create_directory(tok, ec) && !ec) return tok;
        }
        if (std::chrono::steady_clock::now() - started > kLimit) {
            // Proceeds WITHOUT the cap rather than failing: the semaphore bounds
            // memory pressure, it is not a correctness property, so an unbounded
            // compile is a worse build and a failed one is no build at all.
            std::println(std::cerr,
                         "mcpp: no compiler slot became free in 2h — all {} tokens in {} "
                         "are held by processes that are gone. Continuing without the "
                         "concurrency cap; remove that directory to restore it.",
                         cap, dir.string());
            return {};
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
// A BMI's IDENTITY, so publication can be detected without the file ever having
// to be absent.
//
// THE OLD DESIGN CREATED THE HOLE IT WAS TRYING TO AVOID. Phase 1 used to
// `rename(bmi, bmi.bak)` before spawning the compiler — the comment said it was
// so "its mere presence can never be mistaken for the new one landing". That is
// a real hazard, but the cure left the module with NO BMI ON DISK from that
// rename until the compiler republished: measured at ~208 ms of a single
// incremental rebuild. Any importer scheduled inside that window dies with
//
//     error: failed to read compiled module: No such file or directory
//     note: imports must be built before being imported
//
// reproducible at `-j1`, on four of six scenarios of the generated fixture.
//
// The previous BMI is now COPIED aside instead, so the file is continuously
// readable and GCC's own atomic rename is what replaces it. Publication is
// detected by the identity below changing, which is exactly the question the
// existence check was a poor proxy for.
struct BmiIdentity {
    bool                                    present{};
    std::uintmax_t                          size{};
    std::filesystem::file_time_type         mtime{};
    bool operator==(const BmiIdentity&) const = default;
};

BmiIdentity bmi_identity(const std::filesystem::path& p) {
    BmiIdentity id;
    if (p.empty()) return id;
    std::error_code ec;
    // ASSIGN NOTHING BEFORE CHECKING `ec`. `file_size` returns
    // `static_cast<uintmax_t>(-1)` when it fails, so writing it into the struct
    // first makes "this file is missing" compare UNEQUAL to a default-built
    // identity — which is exactly the sentinel used for "there was no previous
    // BMI". Phase 1 then saw a difference on its very first poll and returned
    // before the compiler had produced anything, and every object edge failed
    // with `no compiler was started … phase 1 did not run`.
    const auto size = std::filesystem::file_size(p, ec);
    if (ec) return id;
    const auto mtime = std::filesystem::last_write_time(p, ec);
    if (ec) return id;
    id.present = true;
    id.size    = size;
    id.mtime   = mtime;
    return id;
}

// Put the previous BMI back. Used when the compile failed: the unit still has
// the BMI it had before, and leaving it parked in `.bak` would strand every
// importer on a file that does not exist.
void restore_backup(const std::filesystem::path& bmi) {
    if (bmi.empty()) return;
    const auto backup = suffixed(bmi, ".bak");
    if (!file_exists(backup)) return;
    std::error_code ec;
    std::filesystem::rename(backup, bmi, ec);
}

void settle_bmi(const std::filesystem::path& bmi) {
    if (bmi.empty()) return;
    const auto backup = suffixed(bmi, ".bak");
    if (!file_exists(backup)) return;
    std::error_code ec;
    // Equivalent → put the PREVIOUS file back, so its mtime does not advance and
    // ninja's restat stops the cascade. The rename is atomic, so the BMI is
    // readable throughout: there is no moment at which importers see nothing.
    if (stage::bmi_equivalent(bmi, backup))
        std::filesystem::rename(backup, bmi, ec);
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
    // `cmd.exe /c` DOES NOT USE CreateProcess ARGUMENT QUOTING.
    //
    // This built the line with `join_command({"cmd.exe", "/c", command})`, which
    // treats the whole compiler invocation as ONE argv element: it wraps it in
    // quotes and escapes every interior `"` as `\"`. cmd.exe parses none of
    // that — its /c rule is about counting quotes in the raw string — so a
    // command containing quoted paths came out mangled, and the compiler ran
    // with flags missing rather than failing outright. On a windows-host cross
    // build that surfaced as
    //
    //     failed: gcm.cache/mcpp.libs.json.gcm
    //     src/libs/json.cppm:3: fatal error: json.hpp: No such file or directory
    //
    // i.e. `-I` gone, reported as a missing header (#425).
    //
    // ninja concatenates verbatim (`subprocess-win32.cc`), and this edge exists
    // to run exactly what ninja would have run. Do the same.
    std::string line = "cmd.exe /c ";
    line += command;

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE log = ::CreateFileA(logPath.string().c_str(), GENERIC_WRITE,
                               FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    // The command, in the log, before it runs. Phase 2 replays this file, so a
    // failure arrives with the exact invocation attached — without it a remote
    // failure can only be guessed at, which is how #425 cost a CI cycle to even
    // localise.
    if (log != INVALID_HANDLE_VALUE) {
        const std::string banner = "+ " + line + "\r\n";
        DWORD wrote = 0;
        ::WriteFile(log, banner.data(), static_cast<DWORD>(banner.size()), &wrote, nullptr);
    }
    STARTUPINFOA si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = log;
    si.hStdInput = ::CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ, &sa,
                                 OPEN_EXISTING, 0, nullptr);
    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessA(nullptr, line.data(), nullptr, nullptr, TRUE,
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

    // Keep the previous BMI for the equivalence check, and LEAVE THE ORIGINAL
    // IN PLACE — see BmiIdentity for why moving it away is what broke importers.
    // Snapshot through the SAME function in both cases, so "no previous BMI"
    // and "the BMI as it is now" are directly comparable.
    const BmiIdentity before = bmi_identity(req.bmi);
    if (!req.bmi.empty()) {
        const auto backup = suffixed(req.bmi, ".bak");
        std::filesystem::remove(backup, ec);
        if (before.present) {
            std::filesystem::copy_file(
                req.bmi, backup, std::filesystem::copy_options::overwrite_existing, ec);
            // AND CARRY THE MTIME ACROSS. `copy_file` stamps the copy with
            // the time of the copy, and `settle_bmi` restores this file when the
            // new BMI turns out equivalent — precisely so the mtime does NOT
            // advance and ninja's restat stops the cascade. Without this line
            // the restore moves the mtime forward instead, every importer is
            // rebuilt, and the optimisation is silently off: `touch-hub` came
            // back at 12.61s against a 12.37s cold build, with every cell
            // reporting `ok`. A status column cannot catch that; the number can.
            std::filesystem::last_write_time(backup, before.mtime, ec);
        }
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

    // BOUNDED, for the same reason `await_unit` is — and this loop waits on the
    // SAME supervisor. The spawn succeeding only says a process was created; if
    // it then dies without writing `<slot>.rc` (the OOM killer is the realistic
    // one here, since this strategy deliberately runs ninja at 6x the compiler
    // cap and each module compile peaks near a gigabyte), neither exit below can
    // ever be taken and the build hangs forever at 2 ms per poll with nothing on
    // stdout to say what it is waiting for.
    //
    // Bounding phase 2 and not phase 1 left the hang reachable from the earlier
    // half of the same mechanism.
    using clock = std::chrono::steady_clock;
    const auto  started            = clock::now();
    constexpr auto kNoSupervisorGrace = std::chrono::seconds(10);
    constexpr auto kHardLimit         = std::chrono::hours(2);

    for (;;) {
        // Published = the file's identity is no longer the one we snapshotted.
        // For a unit with no previous BMI that reduces to "it now exists".
        if (!req.bmi.empty() && bmi_identity(req.bmi) != before) {
            settle_bmi(req.bmi);
            copy_first_rule(req.depFrom, req.depTo, req.bmi.string());
            return 0;                       // importers may proceed
        }
        if (const auto rc = read_rc(req.slot)) {
            if (*rc != 0) {                 // failed before publishing a BMI
                std::ifstream in(suffixed(req.slot, ".log"));
                if (in) std::cerr << in.rdbuf();
                // Nothing was published, so the previous BMI is the truth. Put
                // it back rather than leaving the unit with no BMI at all.
                restore_backup(req.bmi);
            } else {
                // THE COMPILER CAN FINISH BETWEEN THE TWO CHECKS ABOVE.
                //
                // The loop tests `file_exists(bmi)` first and `rc` second, so a
                // unit whose compile is shorter than one poll interval lands
                // here: no BMI seen, then a zero rc. This path used to return
                // success WITHOUT settling — which left the previous BMI parked
                // in `<name>.gcm.bak` and skipped the equivalence check
                // entirely, so the restat suppression that stops the cascade
                // never ran for that unit.
                //
                // Observed as `.bak` files surviving a completed build, and as
                //
                //     fx.unit_0: error: failed to read compiled module:
                //                       No such file or directory
                //     fx.unit_0: note: imports must be built before being imported
                //
                // in an importer — reproducible at `-j1`, so it was never a
                // race between compilers, only between this loop's two checks.
                settle_bmi(req.bmi);
                copy_first_rule(req.depFrom, req.depTo, req.bmi.string());
            }
            return *rc;
        }
        const auto waited = clock::now() - started;
        // No log file means no supervisor ever opened one. Distinguished from
        // the hard limit because the two need different advice: this one is
        // "the process is not there", not "it is taking too long".
        if (!file_exists(suffixed(req.slot, ".log")) && waited > kNoSupervisorGrace) {
            std::println(std::cerr,
                         "mcpp: the compiler supervisor for {} never started "
                         "(no log after {}s) — restoring the previous BMI",
                         req.slot.string(),
                         std::chrono::duration_cast<std::chrono::seconds>(waited).count());
            restore_backup(req.bmi);
            return 1;
        }
        if (waited > kHardLimit) {
            std::println(std::cerr,
                         "mcpp: timed out waiting for {} to publish its BMI",
                         req.slot.string());
            restore_backup(req.bmi);
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

int supervise(const std::filesystem::path& slot,
              const std::filesystem::path& semaphoreToken,
              std::string_view command) {
    // EVERY EXIT FROM HERE MUST LEAVE AN `.rc`, INCLUDING THE ONES THAT DO
    // NOT RUN A COMPILER. Phase 1 and phase 2 both wait on that file; a
    // supervisor that bails without writing one is indistinguishable from one
    // that was never started, and both waiters can only end on a timeout. The
    // caller used to return 2 for an unreadable command file before reaching
    // this function, which is precisely that case.
    if (command.empty()) {
        std::println(std::cerr, "mcpp: bmi-supervise got an empty command for {}",
                     slot.string());
        if (!semaphoreToken.empty()) {
            std::error_code ec;
            std::filesystem::remove(semaphoreToken, ec);
        }
        write_rc(slot, 2);
        return 0;
    }
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

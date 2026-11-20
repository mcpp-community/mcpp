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
//     unlink("gcm.cache/x.gcm")                       = 0
//     rename("gcm.cache/x.gcm~", "gcm.cache/x.gcm")      = 0
//
// GCC unlinks the previous final path immediately before that rename, so the
// path can briefly be absent. Only a PRESENT changed identity is publication.
// Confirmed three further
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
export module mcpp.build.schedule.detach_codegen;

import std;
import mcpplibs.os.fs;
import mcpplibs.os.process;
import mcpplibs.os.env;
import mcpplibs.os.platform;
import mcpplibs.os.stdio;
import mcpplibs.os.text;
import mcpplibs.os.machine;
import mcpp.build.depfile;
import mcpp.build.command_line;
import mcpp.run;
import mcpp.build.stage;
import mcpp.buildopt.keep;
import mcpp.build.schedule.compiler_transaction;
import mcpp.build.schedule.relocation;
import mcpp.diag;
import mcpp.home;
import mcpp.home.files;
import mcpp.invocation;
import mcpp.toolchain.fingerprint;

export namespace mcpp::build::schedule::detach {

inline constexpr std::string_view kRunEnv = "MCPP_COMPILER_RUN";
// The cap of an executor that runs outside a generation (Ninja started by
// hand), when its command line names none.
inline constexpr std::string_view kCapEnv = "MCPP_COMPILER_CAP";

// One runtime generation, outside the build identity. Its actor barrier covers
// every helper that may write state, not only helpers holding compiler slots.
// The generation carries the compiler cap: build.ninja names no number of
// compilers, so the graph is the same whatever the job count (e2e 861), and
// every executor of the generation reads the cap from its owner record.
// `mcpp clean` (design 2026-11-19 §10.3 D): before a configuration's output
// directory is removed, its compiler-run generations are retired under the
// gate a build takes. Its outputs are being removed, so there is nothing an
// old generation's recovery would protect and nothing is observed. A
// generation whose owner or actor lock is held belongs to a running build:
// the error names it, and the configuration is not cleaned.
std::expected<void, std::string> retire_compiler_runs(const std::filesystem::path& outputDir);

class RunScope {
public:
    static std::expected<RunScope, std::string> begin(const std::filesystem::path& outputDir, int compilerCap);
    RunScope(RunScope&&) noexcept;
    RunScope& operator=(RunScope&&) noexcept;
    ~RunScope();
    void environment(std::vector<std::pair<std::string, std::string>>& env) const;
    std::expected<void, std::string> finish();
    const std::filesystem::path& directory() const;
    bool failed() const;
private:
    struct State;
    std::unique_ptr<State> state_;
    explicit RunScope(std::unique_ptr<State> state);
};

struct CompileRequest {
    // The BMI this compile publishes. Empty for a unit that produces none, in
    // which case phase 1 simply waits like an ordinary edge.
    std::filesystem::path bmi;
    std::filesystem::path object;
    // `<slot>.log` and `<slot>.rc` live beside this path.
    std::filesystem::path slot;
    // Absolute path to the mcpp binary, re-invoked as the supervisor. A spawn
    // rather than a fork is what keeps this portable.
    std::filesystem::path self;
    // Standalone permit directory. Managed drives use their generation's
    // stable slots; no compiler acquires a second permit (hazard 2).
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
    // The BMI as ninja spells it, which is what the copied depfile must name.
    // `bmi` is the path this edge OPENS, and on Windows it is the absolute,
    // extended-length form (mcpplibs::os::fs::extended); ninja treats
    // an edge whose depfile names anything but its own output spelling as
    // permanently dirty. Empty means `bmi` is already that spelling.
    std::string bmiTarget;
    // The object as the command spells it, and how the compile is relocated
    // (`--relocate`, design 2026-11-19 §10.6): empty writes in place.
    std::string objectTarget;
    std::string relocate;
};

// Phase 1 — returns 0 as soon as the BMI is published, leaving code generation
// running. Returns the compiler's status if it exits before publishing one.
int compile_release_at_bmi(const CompileRequest& req);

// The supervisor. Runs the compiler to completion with its output redirected,
// then records the status. Never invoked directly by a build edge.
int supervise(const std::filesystem::path& slot,
              const std::filesystem::path& semaphore, int maxCompilers,
              const std::filesystem::path& object, std::string_view command,
              const std::filesystem::path& relocation = {});

// Phase 2 — blocks until the compiler for `slot` finished, replays what it
// wrote, and propagates its status. `object`, when given, must exist: a
// compiler that reports success without producing its output is a failure this
// must not pass on.
int await_unit(const std::filesystem::path& slot, const std::filesystem::path& object);

// Ordinary compiler invocations share the detached compilers' actual bound.
// Their original argv and streams are retained; no shell is introduced.
// `relocate` non-empty: the compile writes its outputs below an attempt of
// its own and this executor publishes them (design 2026-11-19 §10.6); the
// value is the toolchain's auxiliary naming (`OutputRelocation::auxNaming`),
// or `none`.
//
// `reading` is what the executor reads for the compiler (design 2026-11-19
// §20): the record of the files the compile read when the compiler writes one
// that is not the Make rule `depfile` (cl.exe's `/sourceDependencies`, given to
// ninja as lines of this executor's output once the compile succeeds), and the
// encoding of the compiler's text when it is not UTF-8, in which case its two
// streams are read through one pipe and written on as UTF-8.
struct Reading {
    std::filesystem::path         sourceDependencies;
    mcpplibs::os::text::encoding  text = mcpplibs::os::text::encoding::utf8;
};
int run_capped(const std::vector<std::string>& argv,
               const std::filesystem::path& semaphore, int maxCompilers,
               const std::filesystem::path& output = {},
               const std::filesystem::path& depfile = {},
               const std::filesystem::path& bmi = {}, bool object = false,
               std::string_view relocate = {}, const Reading& reading = {});

}  // namespace mcpp::build::schedule::detach

// ---------------------------------------------------------------------------

namespace mcpp::build::schedule::detach {
namespace {
namespace transactions = mcpp::build::schedule::transaction;
namespace relocation = mcpp::build::schedule::relocation;

// A name no other attempt has: this process's identity and a sequence of its
// own (design 2026-11-19 §10.6).
std::optional<std::string> attempt_name() {
    const auto self = mcpplibs::os::process::self();
    if (!self || !self->id || !self->started_ns) return std::nullopt;
    static std::atomic<unsigned long long> sequence{0};
    return std::format("{}-{}-{}", self->id, self->started_ns, sequence++);
}

std::filesystem::path suffixed(const std::filesystem::path& base, std::string_view s) {
    return std::filesystem::path{base.string() + std::string(s)};
}

bool file_exists(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::exists(p, ec);
}

struct Owner {
    bool managed = false;
    std::filesystem::path generation;
    mcpplibs::os::process::program program;
    int cap = 0;  // the generation's compiler cap

    static Owner inherited() {
        Owner owner;
        const auto value = mcpp::invocation::get(kRunEnv);
        if (!value || value->empty()) return owner;
        owner.managed = true;
        owner.generation = mcpp::home::from_utf8(*value);
        std::ifstream record(owner.generation / "owner");
        std::string version;
        if (!(record >> version >> owner.program.id >> owner.program.started_ns)
            || version != "mcpp-compiler-run-v1") owner.program = {};
        else if (!(record >> owner.cap)) owner.cap = 0;
        return owner;
    }
    bool active() const {
        if (!managed) return true; // standalone internal helpers
        if (program.id == 0 || program.started_ns == 0
            || !mcpplibs::os::process::alive(program)) return false;
        // A surviving file is not a surviving owner. A normal drive may also
        // have ended while its process remains alive to report the outcome.
        auto released = mcpplibs::os::fs::lock::acquire(generation / "owner.lock",
            mcpplibs::os::fs::lock::kind::exclusive, false);
        return !released && released.error().code == mcpplibs::os::errc::again;
    }
    std::filesystem::path permits(const std::filesystem::path& fallback) const {
        return managed ? generation.parent_path() / "slots" : fallback;
    }
};

struct Actor {
    Owner owner = Owner::inherited();
    std::filesystem::path path;
    std::optional<mcpplibs::os::fs::lock> lock;
    bool ready = false;
    Actor() {
        if (!owner.active()) return;
        if (!owner.managed) { ready = true; return; }
        const auto self = mcpplibs::os::process::self();
        if (!self || self->id == 0 || self->started_ns == 0) return;
        static std::atomic<unsigned long long> sequence{0};
        const auto actors = owner.generation / "actors";
        std::error_code ec;
        std::filesystem::create_directories(actors, ec);
        if (ec) return;
        const auto name = std::format("actor-{}-{}-{}.lock", self->id, self->started_ns, sequence++);
        path = actors / name;
        auto registered = mcpplibs::os::fs::lock::acquire(path,
            mcpplibs::os::fs::lock::kind::exclusive, false);
        if (registered) lock.emplace(std::move(*registered));
        // Registration precedes the final owner check. A late helper may
        // create a record after closure, but cannot write build state.
        ready = lock.has_value() && owner.active();
    }
    ~Actor() {
        lock.reset();
        if (!path.empty()) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    }
};

struct InterruptCleanup {
    int handle;
    ~InterruptCleanup() { mcpp::run::remove_cleanup(handle); }
};

bool actors_idle(const std::filesystem::path& generation) {
    const auto actors = generation / "actors";
    std::error_code ec;
    if (!std::filesystem::exists(actors, ec)) return !ec;
    std::filesystem::directory_iterator entries(actors, ec);
    if (ec) return false;
    for (const auto& entry : entries) {
        const auto name = mcpp::home::spelling(entry.path().filename());
        if (!name.ends_with(".lock")) continue;
        auto free = mcpplibs::os::fs::lock::acquire(entry.path(),
            mcpplibs::os::fs::lock::kind::exclusive, false);
        if (!free) return false;
    }
    return true;
}

bool drain_actors(const std::filesystem::path& generation) {
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        if (actors_idle(generation)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < limit);
    return false;
}

void withdraw_outputs(const std::filesystem::path& slot,
                      const std::filesystem::path& object = {}) {
    std::error_code ec;
    std::ifstream named(suffixed(slot, ".bmi"));
    if (std::string bmi; named && std::getline(named, bmi) && !bmi.empty())
        std::filesystem::remove(bmi, ec);
    if (!object.empty()) std::filesystem::remove(object, ec);
}

// The status a supervisor left. `.rc` appears by a rename once its number is
// written, so one that holds no number is a write that failed (a full disk):
// the supervisor's status is lost, and the unit is taken as failed rather than
// waited for until the hard limit.
std::optional<int> read_rc(const std::filesystem::path& slot) {
    std::ifstream in;
    if (transactions::open_settled(in, suffixed(slot, ".rc")) != transactions::Opened::yes) return std::nullopt;
    int rc = 0;
    if (!(in >> rc)) {
        std::println(std::cerr, "mcpp: the status of the compiler for {} was not written "
                                "(is the disk full?)", slot.string());
        return 1;
    }
    return rc;
}

// Temp file + rename, so a reader never observes half a number. The same
// guarantee, for the same reason, that makes watching the BMI path sound.
void record_supervisor_error(const std::filesystem::path& slot,std::string_view reason);
bool write_rc(const std::filesystem::path& slot, int rc,std::string* diagnostic=nullptr) {
    std::string localDiagnostic;
    auto* detail=diagnostic?diagnostic:&localDiagnostic;
    const bool written=transactions::replace_record(suffixed(slot,".rc"),std::to_string(rc)+"\n",detail);
    // An early supervisor failure has no caller-owned diagnostic. Keep its
    // status-publication error beside the compiler log for the waiter.
    if(!written&&!diagnostic)
        record_supervisor_error(slot,std::format("cannot publish compiler status: {}",localDiagnostic));
    return written;
}

// Stable kernel lock slots, shared by ordinary and detached compilers. The
// lock ends with its holder even after SIGKILL; its file is never unlinked as
// a way of reclaiming a live permit. No holder acquires a second permit.
struct Permit {
    std::optional<mcpplibs::os::fs::lock> lock;
    std::filesystem::path reservation;
    // The machine's budget slot (SPEC-017 O11), held for the compile, taken
    // before the generation's slot; empty when the slot is the one the runner
    // holds for this process, or no budget is stated.
    std::optional<mcpplibs::os::fs::lock> budget;
    std::optional<mcpplibs::os::fs::lock> heldClaim;
};

// ONE BUDGET FOR THE MACHINE (SPEC-017 O11, design 2026-11-20 §5.5). A runner
// that states MTEST_BUDGET holds a slot file of that directory for the process
// it started (MTEST_BUDGET_HELD); the first compiler of this build runs on it,
// and every other compiler locks a free slot file before it starts. Four test
// processes each starting a parallel build put 7 to 9 processes in the run
// queue of 4 processors (measured, design §3.1): the work of a slot that is
// not free waits instead.
std::expected<void, int> take_budget_slot(Permit& permit, const std::filesystem::path& permits) {
    const auto budget = mcpp::invocation::get("MTEST_BUDGET");
    if (!budget || budget->empty()) return {};
    const auto held = mcpp::invocation::get("MTEST_BUDGET_HELD").value_or("");
    const std::filesystem::path dir = mcpp::home::from_utf8(*budget);
    // The held slot is claimed by one compiler of the build at a time.
    if (!held.empty()) {
        auto claim = mcpplibs::os::fs::lock::acquire(permits / "budget-held.lock",
                                                     mcpplibs::os::fs::lock::kind::exclusive, false);
        if (claim) { permit.heldClaim.emplace(std::move(*claim)); return {}; }
    }
    std::vector<std::filesystem::path> slots;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
        if (e.path().filename().string().starts_with("slot-")
            && mcpp::home::spelling(e.path()) != held)
            slots.push_back(e.path());
    if (slots.empty()) return {};   // a budget of one slot: the held one, claimed by another compiler
    std::ranges::sort(slots);
    for (;;) {
        for (const auto& f : slots) {
            auto slot = mcpplibs::os::fs::lock::acquire(f, mcpplibs::os::fs::lock::kind::exclusive, false);
            if (slot) { permit.budget.emplace(std::move(*slot)); return {}; }
        }
        // The held slot may have been released by the compiler that claimed it.
        if (!held.empty()) {
            auto claim = mcpplibs::os::fs::lock::acquire(permits / "budget-held.lock",
                                                         mcpplibs::os::fs::lock::kind::exclusive, false);
            if (claim) { permit.heldClaim.emplace(std::move(*claim)); return {}; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// THE MEMORY A COMPILER MAY START BELOW (design 2026-11-20 §5.5, mcpp#802): read
// after the slots are held, before the compiler starts. Below the threshold the
// compiler waits for memory a running compiler gives back; none is ended. A
// minute without enough is said and the compiler starts: waiting for memory no
// compiler of this machine holds would wait forever.
void await_memory() {
    std::uint64_t threshold = 0;
    if (const auto v = mcpp::invocation::get("MCPP_MEMORY_THRESHOLD"))
        std::from_chars(v->data(), v->data() + v->size(), threshold);
    if (threshold == 0) return;
    const auto started = std::chrono::steady_clock::now();
    for (;;) {
        const auto available = mcpplibs::os::machine::memory_available();
        if (!available || *available >= threshold) return;
        if (std::chrono::steady_clock::now() - started > std::chrono::minutes(1)) {
            std::println(std::cerr, "mcpp: {} MiB available, under the memory threshold of {} MiB for a "
                                    "minute; the compiler starts ([build] memory_threshold)",
                         *available >> 20, threshold >> 20);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}
std::expected<Permit, int>
acquire_permit(const Actor& actor, const std::filesystem::path& requested, int requestedCap) {
    const auto dir = actor.owner.permits(requested);
    const int cap = [&] {
        if (actor.owner.managed) return actor.owner.cap;
        if (requestedCap > 0) return requestedCap;
        int fromEnv = 0;
        if (const auto value = mcpp::invocation::get(kCapEnv))
            std::from_chars(value->data(), value->data() + value->size(), fromEnv);
        return fromEnv;
    }();
    if (!actor.ready) return std::unexpected(130);
    if (dir.empty() || cap <= 0) {
        if (actor.owner.managed) return std::unexpected(2);
        Permit free;
        await_memory();
        return free;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        std::println(std::cerr, "mcpp: cannot create compiler semaphore {}: {}", dir.string(), ec.message());
        return std::unexpected(1);
    }
    Permit budgeted;
    if (auto took = take_budget_slot(budgeted, dir); !took) return std::unexpected(took.error());
    const auto started = std::chrono::steady_clock::now();
    constexpr auto kLimit = std::chrono::hours(2);
    std::optional<std::chrono::steady_clock::time_point> unexplainedSince;
    for (;;) {
        if (!actor.owner.active()) return std::unexpected(130);
        bool orphan = false, unexplained = false;
        for (int i = 0; i < cap; ++i) {
            auto permit = mcpplibs::os::fs::lock::acquire(dir / std::format("slot-{}.lock", i),
                mcpplibs::os::fs::lock::kind::exclusive, false);
            if (permit) {
                const auto reservation = dir / std::format("slot-{}.reservation", i);
                if (actor.owner.managed) {
                    const auto available = transactions::reservation_available(reservation);
                    if (!available) { std::println(std::cerr,"mcpp: {}",available.error()); return std::unexpected(1); }
                    if (!*available) { orphan = true; continue; }
                }
                budgeted.lock.emplace(std::move(*permit));
                budgeted.reservation = reservation;
                await_memory();
                return budgeted;
            }
            // On macOS a slot another compiler holds is now and then reported
            // "not found" from inside this executor, though the directory and
            // the file exist and the next attempt reports it held (measured on
            // #889; the kernel itself answers EAGAIN, mcpplibs/openkal-macos#45).
            // It is treated as held while the directory exists, unless every
            // round reports it for five seconds.
            std::error_code dirError;
            if (permit.error().code == mcpplibs::os::errc::not_found
                && std::filesystem::is_directory(dir, dirError)) { unexplained = true; continue; }
            if (permit.error().code != mcpplibs::os::errc::again) {
                std::println(std::cerr, "mcpp: {}", permit.error().message());
                return std::unexpected(1);
            }
        }
        if (!unexplained) unexplainedSince.reset();
        else if (!unexplainedSince) unexplainedSince = std::chrono::steady_clock::now();
        else if (std::chrono::steady_clock::now() - *unexplainedSince > std::chrono::seconds(5)) {
            std::println(std::cerr, "mcpp: the compiler slots in {} have reported themselves missing for 5s", dir.string());
            return std::unexpected(1);
        }
        if (orphan && std::chrono::steady_clock::now()-started > std::chrono::seconds(5)) {
            std::println(std::cerr,"mcpp: a previous compiler unit is occupied or unknown; its slot remains reserved");
            return std::unexpected(1);
        }
        if (std::chrono::steady_clock::now() - started > kLimit) {
            std::println(std::cerr,
                         "mcpp: no compiler slot became free in 2h — all {} tokens in {} "
                         "are still occupied; refusing to exceed the concurrency cap.",
                         cap, dir.string());
            return std::unexpected(1);
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
// The previous BMI is now COPIED aside instead, so this adapter does not
// introduce a missing-file interval. GCC itself unlinks the old path before
// its rename; that interval must not release importers. Publication is
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
    if (mcpp::buildopt::keep::keeps_previous(bmi, backup))
        std::filesystem::rename(backup, bmi, ec);
    else
        std::filesystem::remove(backup, ec);
}

// The supervisor, left running when this edge returns: its streams are the
// null device (HAZARD 1 applies to it as well -- holding ninja's pipe open
// would keep the edge alive long after this process exits), and it runs in a
// unit of its own, so the terminal's interrupt of the build is not delivered
// to it mid-write.
bool spawn_detached(const std::vector<std::string>& argv, bool declare_inside,
                    std::optional<mcpplibs::os::process::program>* identity = nullptr) {
    namespace process = mcpplibs::os::process;
    process::spawn_options o;
    o.program = argv.front();
    o.args.assign(argv.begin() + 1, argv.end());
    o.in = o.out = o.err = process::stdio::null;
    o.own_unit = !declare_inside;
    auto p = process::process::spawn(o);
    if (!p) return false;
    if(identity) {
        const auto described=process::describe(p->id());
        if(described&&described->who.id&&described->who.started_ns)*identity=described->who;
    }
    p->detach();
    return true;
}

// `command` as ninja would have run it, through the host's interpreter (the
// command is ninja's text: `cmd.exe /d /s /c "<command>"` on Windows, inside
// the one pair of quotes `/s` removes, #425 #832), its output and errors into
// `logPath`.
int run_to_completion(std::string_view command,
                      const std::filesystem::path& logPath, const Owner& owner,
                      mcpplibs::os::process::owned_unit* unit=nullptr) {
    // Created before the compiler starts: its existence is what tells the
    // waiters that a supervisor started.
    std::ofstream log(logPath, std::ios::binary | std::ios::trunc);
    log.flush();
    if(!log)return 2;
    if(!owner.active())return 130;
    auto sh = mcpp::run::shell(command);
    auto r = mcpp::run::execute(sh.argv, {
        .should_stop = [&] { return !owner.active(); }, .one_string = sh.one_string,
        .declared_unit = unit });
    // A failure carries the command it ran. Phase 2 replays this file, so the
    // failure arrives with the exact invocation attached -- without it a
    // remote failure can only be guessed at, which is how #425 cost a CI cycle
    // to even localise.
    if (r.exit_code != 0) log << "+ " << command << '\n';
    log << r.output;
    if(!log)return 2;
    return owner.active() ? r.exit_code : 130;
}

void record_supervisor_error(const std::filesystem::path& slot,std::string_view reason) {
    // Detached supervisors have null stdio. Keep the failure beside the
    // compiler's ordinary log so either waiter can report why status is absent.
    std::ofstream log(suffixed(slot,".log"),std::ios::app);
    if(log)log << "mcpp: " << reason << '\n';
}

// What the waiter observed when it concluded that the controller is gone: one
// line, without a path, so a failure states which observation decided it.
void report_controller_loss(const std::filesystem::path& slot,transactions::Controller controller,bool supervising) {
    std::error_code ec;
    const bool log=std::filesystem::exists(suffixed(slot,".log"),ec);
    std::println(std::cerr,"mcpp: compiler controller observation: controller {}, supervisor {}, status absent, log {}",
                 transactions::describe(controller),supervising ? "running" : "ended",log ? "present" : "absent");
}

void replay_supervisor_log(const std::filesystem::path& slot) {
    std::ifstream log(suffixed(slot,".log"));
    if(log)std::cerr << log.rdbuf();
}

std::expected<void,std::string> retire_owned(mcpplibs::os::process::owned_unit& unit,const Owner& owner) {
    const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    for(;;) {
        if(!owner.active()||mcpp::run::ending()) {
            unit.terminate();
            return std::unexpected("compiler run ended before unit retirement");
        }
        auto state=unit.retire(owner.program);
        if(!state)return std::unexpected(state.error().message());
        if(*state==mcpplibs::os::process::unit_state::empty)return {};
        if(std::chrono::steady_clock::now()>=limit) {
            unit.terminate();
            return std::unexpected("compiler descendants did not retire; outputs and reservation retained");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

}  // namespace

struct RunScope::State {
    // False where this program cannot declare a compiler unit (§10.3 H): the
    // build then compiles as an unmanaged helper does, and there is no
    // generation to recover, export or retire; the helpers read the cap from
    // `kCapEnv`, as an executor outside a generation does.
    bool managed = true;
    int cap = 0;
    std::filesystem::path root, generation;
    std::optional<mcpp::home::FileLock> gate;
    std::optional<mcpplibs::os::fs::lock> owner;
    std::unique_ptr<transactions::Witnesses> witnesses;
    bool closed = false;
    std::string failure;
    std::expected<void, std::string> close() {
        if (closed) return failure.empty() ? std::expected<void, std::string>{}
                                          : std::unexpected(failure);
        closed = true;
        if (generation.empty()) return {};
        if (witnesses) witnesses->stop();
        owner.reset();
        if (!drain_actors(generation)) {
            failure = "previous compiler helpers have not stopped; refusing to start another generation";
            return std::unexpected(failure);
        }
        auto recovered = transactions::recover_generation(generation,witnesses ? &witnesses->observers() : nullptr);
        if (!recovered) { failure=recovered.error(); return std::unexpected(failure); }
        std::error_code ec;
        std::filesystem::remove_all(generation, ec);
        if(ec) {failure="cannot retire completed compiler runtime state";return std::unexpected(failure);}
        if(witnesses&&witnesses->failed()) {
            failure="a compiler controller ended before completion; incomplete outputs were recovered";
            return std::unexpected(failure);
        }
        return {};
    }
    ~State() { (void)close(); }
};

RunScope::RunScope(std::unique_ptr<State> state) : state_(std::move(state)) {}
RunScope::RunScope(RunScope&&) noexcept = default;
RunScope& RunScope::operator=(RunScope&&) noexcept = default;
RunScope::~RunScope() = default;

namespace {
// Where a configuration's compiler-run state lies:
// `<work>/.mcpp/compiler-runs/<hash of the canonical output directory>/`.
struct RunsLocation { std::filesystem::path canonical, work, root; };
std::expected<RunsLocation, std::string> locate_runs(const std::filesystem::path& outputDir) {
    std::error_code ec;
    const auto spelled = std::filesystem::absolute(outputDir, ec).lexically_normal();
    if (ec) return std::unexpected("cannot identify the compiler-run output directory");
    RunsLocation at;
    at.canonical = std::filesystem::weakly_canonical(spelled, ec);
    if (ec) return std::unexpected("cannot identify the compiler-run output directory");
    // Locate work BEFORE resolving output symlinks: target/<triple> may point
    // at a disposable cache. Locks remain beside target/, independently of
    // clean and MCPP_HOME; the canonical output still identifies the slots.
    const auto target = spelled.parent_path().parent_path();
    at.work = std::filesystem::weakly_canonical(
        target.filename() == "target" ? target.parent_path() : spelled.parent_path(), ec);
    if (ec) return std::unexpected("cannot identify the compiler-run work directory");
    at.root = at.work / ".mcpp" / "compiler-runs" / mcpp::toolchain::hash_string(mcpp::home::spelling(at.canonical));
    return at;
}
}  // namespace

std::expected<void, std::string> retire_compiler_runs(const std::filesystem::path& outputDir) {
    auto located = locate_runs(outputDir);
    if (!located) return std::unexpected(located.error());
    std::error_code ec;
    if (!std::filesystem::is_directory(located->root, ec)) return {};
    auto gate = mcpp::home::FileLock::acquire(located->root, "gate.lock", "",
        std::chrono::milliseconds(300), {});
    if (!gate) return std::unexpected(std::format(
        "an mcpp is building {}: it holds the compiler-run gate of {}; it was not cleaned",
        mcpp::home::spelling(outputDir), mcpp::home::spelling(located->root)));
    std::filesystem::directory_iterator runs(located->root, ec);
    if (ec) return std::unexpected("cannot inspect the compiler runs of " + mcpp::home::spelling(outputDir));
    for (const auto& entry : runs) {
        if (!entry.path().filename().native().starts_with(std::filesystem::path("run-").native())) continue;
        {
            // Released before the directory goes: a lock held on a file of
            // it keeps it on Windows.
            auto owner = mcpplibs::os::fs::lock::acquire(entry.path() / "owner.lock",
                mcpplibs::os::fs::lock::kind::exclusive, false);
            if (!owner || !actors_idle(entry.path())) return std::unexpected(std::format(
                "an mcpp is building {}: the helpers of {} hold its locks; it was not cleaned",
                mcpp::home::spelling(outputDir), mcpp::home::spelling(entry.path())));
        }
        std::filesystem::remove_all(entry.path(), ec);
        if (ec) return std::unexpected(std::format("cannot retire {}: {}",
            mcpp::home::spelling(entry.path()), ec.message()));
    }
    return {};
}

std::expected<RunScope, std::string> RunScope::begin(const std::filesystem::path& outputDir, int compilerCap) {
    if (compilerCap <= 0) return std::unexpected("a compiler-run generation needs a compiler cap");
    const auto self = mcpplibs::os::process::self();
    if (!self || self->id == 0 || self->started_ns == 0)
        return std::unexpected("cannot identify the compiler-run owner and its birth time");
    std::error_code ec;
    auto located = locate_runs(outputDir);
    if (!located) return std::unexpected(located.error());
    const auto canonical = located->canonical;
    auto state = std::make_unique<State>();
    mcpp::home::mark_project_state(located->work / ".mcpp");
    state->root = located->root;
    std::filesystem::create_directories(state->root, ec);
    if (ec) return std::unexpected("cannot create compiler-run runtime state: " + ec.message());
    state->gate = mcpp::home::FileLock::acquire(state->root, "gate.lock", "",
        std::chrono::milliseconds(300), {});
    if (!state->gate) return std::unexpected("cannot acquire the compiler-run generation gate");
    std::filesystem::directory_iterator previous(state->root, ec);
    if (ec) return std::unexpected("cannot inspect previous compiler runs");
    for (const auto& entry : previous) {
        if (!entry.path().filename().native().starts_with(std::filesystem::path("run-").native())) continue;
        const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        bool free = false;
        do {
            auto oldOwner = mcpplibs::os::fs::lock::acquire(entry.path() / "owner.lock",
                mcpplibs::os::fs::lock::kind::exclusive, false);
            if (oldOwner && actors_idle(entry.path())) { free = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while (std::chrono::steady_clock::now() < limit);
        if (!free) return std::unexpected(std::format(
            "an mcpp is building this configuration: the helpers of {} hold its locks, from this "
            "process space or another that shares the directory; its state was retained\n"
            "       When that build has ended, build again.", mcpp::home::spelling(entry.path())));
        // The owner lock and every actor lock are free: what cannot be observed
        // from here is settled as abandoned (§10.3 C).
        auto recovered=transactions::recover_generation(entry.path(),nullptr,true);
        if(!recovered)return std::unexpected(recovered.error());
        if(recovered->unobservable>0)
            mcpp::diag::degraded("compiler-run/unobservable", std::format(
                "{} compiler unit{} of an interrupted build cannot be observed from this process space "
                "(state: {}); their unpublished outputs were discarded",
                recovered->unobservable, recovered->unobservable==1?"":"s", mcpp::home::spelling(entry.path())),
                "a compiler that outlived its controller in that process space is not found; a late "
                "write is caught by the next build, which compiles that unit again");
        std::filesystem::remove_all(entry.path(), ec);
        if (ec) return std::unexpected("cannot retire previous compiler-run state");
    }
    // Old owners, actors and units are quiet before cached output recovery;
    // no Ninja reader from this generation has started yet.
    // What the recovered attempts wrote below themselves goes with them
    // (§10.6); a file an orphan still holds stays until a later build.
    relocation::sweep(canonical);
    if(auto admitted=transactions::admit_cached_outputs(canonical);!admitted)
        return std::unexpected(admitted.error());
    // WHETHER A UNIT CAN BE DECLARED HERE IS A PROPERTY OF THE ENVIRONMENT
    // (§10.3 H), asked once for the build. Where it cannot, every compile of
    // the build runs as an unmanaged helper does -- bounded by the slots, not
    // declared as a unit -- and this is said once, rather than each compile
    // failing without a word (an Android app sandbox, 2026-11-18).
    if (!mcpplibs::os::process::units_observable()) {
        mcpp::diag::degraded("compiler-run/no-unit",
            "this environment gives a program nothing to declare a compiler unit from; "
            "the compilers of this build are not declared as units",
            "an interrupted build's compilers are not found by the next build, and a compiler "
            "that outlives its controller may write an output after it");
        state->managed = false;
        state->cap = compilerCap;
        state->gate.reset();
        return RunScope(std::move(state));
    }
    static std::atomic<unsigned long long> sequence{0};
    state->generation = state->root / std::format("run-{}-{}-{}", self->id, self->started_ns, sequence++);
    std::filesystem::create_directories(state->generation, ec);
    if (ec) return std::unexpected("cannot create a compiler-run generation");
    auto owner = mcpplibs::os::fs::lock::acquire(state->generation / "owner.lock",
        mcpplibs::os::fs::lock::kind::exclusive, false);
    if (!owner) return std::unexpected("cannot lock the compiler-run owner: " + owner.error().message());
    state->owner.emplace(std::move(*owner));
    auto witnesses = transactions::Witnesses::start(state->generation);
    if (!witnesses) return std::unexpected(witnesses.error());
    state->witnesses = std::move(*witnesses);
    const auto record = std::format("mcpp-compiler-run-v1\n{} {} {}\n", self->id, self->started_ns, compilerCap);
    if (!transactions::replace_record(state->generation / "owner", record))
        return std::unexpected("cannot publish compiler-run ownership");
    return RunScope(std::move(state));
}

void RunScope::environment(std::vector<std::pair<std::string, std::string>>& env) const {
    std::erase_if(env, [](const auto& entry) { return entry.first == kRunEnv; });
    if (!state_->managed) {
        std::erase_if(env, [](const auto& entry) { return entry.first == kCapEnv; });
        env.emplace_back(kCapEnv, std::to_string(state_->cap));
        return;
    }
    env.emplace_back(kRunEnv, mcpp::home::spelling(state_->generation));
}
std::expected<void, std::string> RunScope::finish() { return state_->close(); }
const std::filesystem::path& RunScope::directory() const { return state_->generation; }
bool RunScope::failed() const {return state_->witnesses&&state_->witnesses->failed();}

int compile_release_at_bmi(const CompileRequest& req) {
    if (req.command.empty() || req.self.empty()) return 2;
    Actor actor;
    if (!actor.ready) return 130;

    std::error_code ec;
    if(!req.slot.parent_path().empty())std::filesystem::create_directories(req.slot.parent_path(),ec);
    if(ec)return 1;
    CompileRequest actual = req;
    // THE ATTEMPT'S OWN PATHS (§10.6): its command names them, through a map
    // of its own for the BMI, and is handed to the supervisor in a file of the
    // attempt's, which outlives this edge as the supervisor does.
    relocation::Plan plan;
    if (!req.relocate.empty()) {
        const auto name = attempt_name();
        if (!name) { std::println(std::cerr, "mcpp: relocating the compile of {}: this process's identifier or birth cannot be read", req.objectTarget); return 1; }
        auto planned = relocation::relocate(actual.command,
            {req.objectTarget, "", req.bmiTarget.empty() ? req.bmi.string() : req.bmiTarget, true},
            *name, req.relocate == "none" ? std::string_view{} : std::string_view(req.relocate));
        if (!planned) { std::println(std::cerr, "mcpp: relocating the compile of {}: {}", req.objectTarget, planned.error()); return 1; }
        plan = std::move(*planned);
        if (plan.relocated()) {
            actual.commandFile = plan.directory / "command";
            std::ofstream file(actual.commandFile, std::ios::binary | std::ios::trunc);
            file << actual.command;
            file.close();
            if (auto saved = relocation::save(plan); !saved || !file) {
                std::println(std::cerr, "mcpp: relocating the compile of {}: its command cannot be written", req.objectTarget);
                return 1;
            }
        }
    }
    std::optional<transactions::Attempt> attempt;
    if(actor.owner.managed) {
        auto prepared=transactions::Attempt::prepare(actor.owner.generation,false,req.bmi,req.object,req.slot,req.depTo);
        if(!prepared) {std::println(std::cerr,"mcpp: {}",prepared.error());return 1;}
        attempt.emplace(std::move(*prepared));
        actual.bmi=attempt->bmi;actual.object=attempt->object;actual.slot=attempt->slot;actual.depTo=attempt->dep;
        if(plan.relocated()) {
            if(!attempt->relocate(std::filesystem::absolute(plan.directory))) {std::println(std::cerr,"mcpp: cannot record the relocation of {}",req.objectTarget);return 1;}
        } else if(auto copied=attempt->prepare_backup();!copied) {std::println(std::cerr,"mcpp: {}",copied.error());return 1;}
    }
    std::filesystem::remove(suffixed(actual.slot, ".rc"), ec);
    if(attempt&&ec)return 1;
    std::filesystem::remove(suffixed(actual.slot, ".rc.tmp"), ec);
    if(attempt&&ec)return 1;
    std::filesystem::remove(suffixed(actual.slot, ".log"), ec);
    if(attempt&&ec)return 1;
    if(attempt) {
        std::filesystem::remove(suffixed(actual.slot,".completed"),ec);
        if(ec)return 1;
    }

    // Keep the previous BMI for the equivalence check, and LEAVE THE ORIGINAL
    // IN PLACE — see BmiIdentity for why moving it away is what broke importers.
    // Snapshot through the SAME function in both cases, so "no previous BMI"
    // and "the BMI as it is now" are directly comparable.
    // A relocated attempt watches its own BMI, which did not exist before it.
    const auto watched = plan.relocated() ? relocation::written_bmi(plan) : actual.bmi;
    const BmiIdentity before = plan.relocated() ? BmiIdentity{} : bmi_identity(actual.bmi);
    if (!attempt && !plan.relocated() && !actual.bmi.empty()) {
        const auto backup = suffixed(actual.bmi, ".bak");
        std::filesystem::remove(backup, ec);
        if (before.present) {
            std::filesystem::copy_file(
                actual.bmi, backup, std::filesystem::copy_options::overwrite_existing, ec);
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

    // The BMI this slot publishes, for phase 2 to withdraw if the compile
    // fails (see `await_unit`).
    if (!actual.bmi.empty()) std::ofstream(suffixed(actual.slot, ".bmi"), std::ios::trunc) << actual.bmi.string();
    // The attempt's directory, for phase 2 to discard once the compiler ended.
    std::filesystem::remove(suffixed(actual.slot, ".relocation"), ec);
    if (plan.relocated())
        std::ofstream(suffixed(actual.slot, ".relocation"), std::ios::trunc)
            << mcpp::home::spelling(std::filesystem::absolute(plan.directory));

    // The supervisor reads the SAME argv file rather than receiving the command
    // on its own command line: one representation, no re-quoting, and no limit
    // on how long a compiler command may be.
    std::vector<std::string> sup{actual.self.string(), "bmi-supervise",
                                 "--slot", actual.slot.string(),
                                 "--command-file", actual.commandFile.string()};
    if (!actual.semaphore.empty()) { sup.push_back("--sem"); sup.push_back(actual.semaphore.string()); }
    if (actual.maxCompilers > 0) { sup.push_back("--cap"); sup.push_back(std::to_string(actual.maxCompilers)); }
    if (!actual.object.empty()) { sup.push_back("--object"); sup.push_back(actual.object.string()); }
    if (plan.relocated()) { sup.push_back("--relocation"); sup.push_back(std::filesystem::absolute(plan.directory).string()); }
    if (!actor.owner.active()) return 130;
    std::optional<mcpplibs::os::process::program> supervisor;
    if (!spawn_detached(sup,actor.owner.managed,&supervisor)) {
        if(!attempt&&!plan.relocated())restore_backup(actual.bmi);
        return 2;
    }

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
    constexpr auto kNoSupervisorGrace = transactions::kStartupGrace;
    constexpr auto kHardLimit         = std::chrono::hours(2);
    auto nextControllerCheck=started;

    for (;;) {
        if (!actor.owner.active()) return 130;
        // GCC unlinks an existing BMI immediately before renaming its new
        // temporary file. Absence is a transition, never publication: treating
        // it as a changed identity releases importers inside that unlink gap.
        const auto published = bmi_identity(watched);
        if (published.present && published != before) {
            if(plan.relocated()) {
                if(attempt&&!attempt->publication())return 1;
                if(auto done=relocation::publish_bmi(plan);!done) {std::println(std::cerr,"mcpp: {}",done.error());return 1;}
            } else if(attempt) {
                if(!attempt->publication())return 1;
                if(auto settled=attempt->settle();!settled) {std::println(std::cerr,"mcpp: {}",settled.error());return 1;}
            } else settle_bmi(actual.bmi);
            copy_first_rule(actual.depFrom, actual.depTo,
                            actual.bmiTarget.empty() ? actual.bmi.string() : actual.bmiTarget);
            return actor.owner.active() ? 0 : 130; // importers may proceed only in this run
        }
        if (const auto rc = read_rc(actual.slot)) {
            if (*rc != 0) {                 // failed before publishing a BMI
                std::ifstream in(suffixed(actual.slot, ".log"));
                if (in) std::cerr << in.rdbuf();
                // Nothing was published, so the previous BMI is the truth. Put
                // it back rather than leaving the unit with no BMI at all.
                if(!attempt&&!plan.relocated())restore_backup(actual.bmi);
            } else if (plan.relocated()) {
                // The compile ended within one poll: its BMI is published now.
                if(attempt&&!attempt->publication())return 1;
                auto done=relocation::publish_bmi(plan);
                if(!done) {std::println(std::cerr,"mcpp: {}",done.error());return 1;}
                if(!*done) {
                    std::println(std::cerr,"mcpp: compiler reported success but {} is missing",actual.bmi.string());
                    return 1;
                }
                copy_first_rule(actual.depFrom, actual.depTo,
                                actual.bmiTarget.empty() ? actual.bmi.string() : actual.bmiTarget);
            } else {
                if (!actual.bmi.empty() && (!file_exists(actual.bmi)
                    || (attempt && bmi_identity(actual.bmi)==before))) {
                    std::println(std::cerr, "mcpp: compiler reported success but {} is missing",
                                 actual.bmi.string());
                    if(!attempt)restore_backup(actual.bmi);
                    if (!attempt && !actual.object.empty()) std::filesystem::remove(actual.object, ec);
                    return 1;
                }
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
                if(attempt) {
                    if(!attempt->publication())return 1;
                    if(auto settled=attempt->settle();!settled) {std::println(std::cerr,"mcpp: {}",settled.error());return 1;}
                } else settle_bmi(actual.bmi);
                copy_first_rule(actual.depFrom, actual.depTo,
                                actual.bmiTarget.empty() ? actual.bmi.string() : actual.bmiTarget);
            }
            return actor.owner.active() ? *rc : 130;
        }
        if(attempt && clock::now()>=nextControllerCheck) {
            nextControllerCheck=clock::now()+std::chrono::milliseconds(50);
            const auto controller=transactions::controller_state(*attempt);
            const bool supervising=!supervisor||mcpplibs::os::process::alive(*supervisor);
            if(controller==transactions::Controller::ended||!supervising) {
                // The final status can be published between the first read and
                // this liveness observation. The next iteration validates it.
                if(read_rc(actual.slot))continue;
                replay_supervisor_log(actual.slot);
                report_controller_loss(actual.slot,controller,supervising);
                std::println(std::cerr,"mcpp: compiler controller ended without a final status; retaining its outputs until unit recovery");
                return 1;
            }
        }
        const auto waited = clock::now() - started;
        // No log file means no supervisor ever opened one. Distinguished from
        // the hard limit because the two need different advice: this one is
        // "the process is not there", not "it is taking too long".
        if (!file_exists(suffixed(actual.slot, ".log")) && waited > kNoSupervisorGrace) {
            std::println(std::cerr,
                         "mcpp: the compiler supervisor for {} never started "
                         "(no log after {}s) — restoring the previous BMI",
                         actual.slot.string(),
                         std::chrono::duration_cast<std::chrono::seconds>(waited).count());
            if(!attempt&&!plan.relocated())restore_backup(actual.bmi);
            return 1;
        }
        if (waited > kHardLimit) {
            std::println(std::cerr,
                         "mcpp: timed out waiting for {} to publish its BMI",
                         actual.slot.string());
            if(!attempt&&!plan.relocated())restore_backup(actual.bmi);
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

int supervise(const std::filesystem::path& slot,
              const std::filesystem::path& semaphore, int maxCompilers,
              const std::filesystem::path& object, std::string_view command,
              const std::filesystem::path& relocationDir) {
    auto actor = std::make_shared<Actor>();
    if (!actor->ready) return 130;
    // A relocated compile's outputs but its BMI, which phase 1 publishes the
    // moment it is complete (§10.6): published once the compiler has ended.
    auto publish_outputs = [&](int rc) {
        if (relocationDir.empty() || rc != 0 || !actor->owner.active()) return rc;
        auto plan = relocation::load(relocationDir);
        auto done = plan ? relocation::publish(*plan, false) : std::expected<void, std::string>(std::unexpected(plan.error()));
        if (done) return rc;
        record_supervisor_error(slot, "publishing the compile's outputs: " + done.error());
        return 1;
    };
    if(actor->owner.managed) {
        auto loaded=transactions::for_slot(actor->owner.generation,slot);
        if(!loaded||!*loaded) {std::println(std::cerr,"mcpp: missing or invalid prepared compiler attempt");return 2;}
        auto attempt=std::move(**loaded);
        const auto who=mcpplibs::os::process::self();
        if(!who||!who->id||!who->started_ns
            ||!transactions::publish(attempt.directory/"controller","mcpp-controller-v1",{
                std::to_string(who->id),std::to_string(who->started_ns),transactions::spelling(actor->path)})) {
            (void)write_rc(slot,2);return 2;
        }
        {std::ofstream started(suffixed(slot,".log"),std::ios::trunc);if(!started){(void)write_rc(slot,2);return 2;}}
        if(command.empty()) {(void)write_rc(slot,2);return 0;}
        auto permit=acquire_permit(*actor,semaphore,maxCompilers);
        if(!permit) {(void)write_rc(slot,permit.error());return permit.error();}
        auto created=mcpplibs::os::process::owned_unit::create();
        if(!created) {(void)write_rc(slot,2);return 2;}
        auto unit=std::make_shared<mcpplibs::os::process::owned_unit>(std::move(*created));
        InterruptCleanup cleanup{mcpp::run::add_cleanup([actor,unit] {unit->terminate();})};
        if(auto registered=attempt.register_unit(*unit,actor->path,permit->reservation);!registered) {
            record_supervisor_error(slot,registered.error());(void)write_rc(slot,2);return 2;
        }
        // Every exit leaves a status, a cancelled one included.
        if(!actor->owner.active()) {(void)write_rc(slot,130);return 130;}
        if(!transactions::publish(attempt.directory/"admitted","mcpp-admitted-v1",{})) {
            (void)write_rc(slot,2);return 2;
        }
        const int ran=run_to_completion(command,suffixed(slot,".log"),actor->owner,unit.get());
        if(auto retired=retire_owned(*unit,actor->owner);!retired) {
            record_supervisor_error(slot,retired.error());return 1;
        }
        const int rc=publish_outputs(ran);
        if(!attempt.retired(rc)) {record_supervisor_error(slot,"cannot publish compiler retirement certificate");return 1;}
        std::string status_failure;
        if(!write_rc(slot,rc,&status_failure)) {
            record_supervisor_error(slot,std::format("cannot publish compiler final status: {}",status_failure));
            return 1;
        }
        if(!status_failure.empty())
            std::println(std::cerr,"mcpp: {}",status_failure);
        if(!transactions::release_reservation(permit->reservation,attempt.directory)) {
            record_supervisor_error(slot,"cannot release compiler reservation");return 1;
        }
        return 0;
    }
    auto settled = std::make_shared<std::atomic<bool>>(false);
    auto settle = [actor, settled, slot, object](int rc) {
        if (settled->exchange(true)) return;
        if (rc != 0 || !actor->owner.active()) withdraw_outputs(slot, object);
        write_rc(slot, rc);
    };
    // The callback retains the actor lease until its last state write, even
    // when interrupt cleanup and the normal return race on different threads.
    InterruptCleanup cleanup{mcpp::run::add_cleanup([settle] { settle(130); })};
    { std::ofstream started(suffixed(slot, ".log"), std::ios::trunc); }
    // EVERY EXIT FROM HERE MUST LEAVE AN `.rc`, INCLUDING THE ONES THAT DO
    // NOT RUN A COMPILER. Phase 1 and phase 2 both wait on that file; a
    // supervisor that bails without writing one is indistinguishable from one
    // that was never started, and both waiters can only end on a timeout. The
    // caller used to return 2 for an unreadable command file before reaching
    // this function, which is precisely that case.
    if (command.empty()) {
        std::println(std::cerr, "mcpp: bmi-supervise got an empty command for {}",
                     slot.string());
        settle(2);
        return 0;
    }
    auto permit = acquire_permit(*actor, semaphore, maxCompilers);
    if (!permit) {
        if (actor->owner.active()) settle(permit.error());
        return permit.error();
    }
    // Acquire in the supervisor: no lock-handle handoff, and no interval in
    // which an early-returning phase 1 could release the compiler's permit.
    if (!actor->owner.active()) return 130;
    const int rc = publish_outputs(run_to_completion(command, suffixed(slot, ".log"), actor->owner));
    settle(mcpp::run::ending() ? 130 : rc);
    return 0;   // the supervisor's own status is not the compiler's
}

int await_unit(const std::filesystem::path& slot, const std::filesystem::path& object) {
    Actor actor;
    if (!actor.ready) return 130;
    std::optional<transactions::Attempt> attempt;
    if(actor.owner.managed) {
        auto pointer=transactions::read(suffixed(slot,".transaction"),"mcpp-attempt-pointer-v1");
        if(!pointer) {std::println(std::cerr,"mcpp: {}",pointer.error());return 1;}
        bool current=false;
        if(*pointer) {
            if((**pointer).size()!=1)return 1;
            current=transactions::named((**pointer)[0]).parent_path()==actor.owner.generation/"attempts";
        }
        if(!current) {
            auto cached=transactions::cached_result(slot,object);
            if(!cached) {std::println(std::cerr,"mcpp: {}",cached.error());return 1;}
            if(*cached)return **cached;
            std::println(std::cerr,"mcpp: no verified completed compiler state for this cached object");
            return 1;
        }
        auto loaded=transactions::for_slot(actor.owner.generation,slot);
        if(!loaded||!*loaded)return 1;
        attempt.emplace(std::move(**loaded));
    }

    // BOUNDED. An unbounded wait here turns "phase 1 never started a compiler"
    // into a build that hangs forever with no output — which is strictly worse
    // than a failure, because nothing says what to look at. If no supervisor
    // ever opened the log, there is nothing to wait for and this fails at once.
    using clock = std::chrono::steady_clock;
    const auto started = clock::now();
    constexpr auto kNoSupervisorGrace = transactions::kStartupGrace;
    constexpr auto kHardLimit         = std::chrono::hours(2);

    std::optional<int> rc;
    auto nextControllerCheck=started;
    while (!(rc = read_rc(slot))) {
        if (!actor.owner.active()) return 130;
        if(attempt&&clock::now()>=nextControllerCheck) {
            nextControllerCheck=clock::now()+std::chrono::milliseconds(50);
            const auto controller=transactions::controller_state(*attempt);
            if(controller==transactions::Controller::ended) {
                // A controller may publish its status and exit after the read
                // in the loop condition. Let the normal result path inspect it.
                if((rc=read_rc(slot)))break;
                replay_supervisor_log(slot);
                report_controller_loss(slot,controller,true);
                std::println(std::cerr,"mcpp: compiler controller ended without a status; unit recovery will retain or invalidate its outputs");
                return 1;
            }
        }
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
    // The compiler has ended and phase 1 has returned: a relocated attempt's
    // directory holds nothing more to publish (§10.6).
    {
        std::error_code ec;
        std::string directory;
        if (std::ifstream named(suffixed(slot, ".relocation")); named && std::getline(named, directory) && !directory.empty())
            relocation::discard(mcpp::home::from_utf8(directory));
        std::filesystem::remove(suffixed(slot, ".relocation"), ec);
    }
    if(attempt) {
        if(*rc!=0)return *rc;
        auto retired=transactions::read(attempt->directory/"retired","mcpp-retired-v1");
        const auto output=transactions::identity(attempt->object);
        if(!retired||!*retired||(**retired).size()!=3||(**retired)[0]!="0"
            ||!output||(*output)[0]!=(**retired)[1]||(*output)[1]!=(**retired)[2]) {
            std::println(std::cerr,"mcpp: compiler result has no matching quiet-unit output proof");
            return 1;
        }
        return 0;
    }

    // A FAILURE IS NOT KEPT. Phase 1 reported success when the BMI was
    // published, so ninja holds that edge done; were the record of the failed
    // compile left in place, every later build would replay it here without
    // compiling again, a failure of the moment (a full disk) included. The
    // record and the published BMI are withdrawn, and the next build compiles
    // the unit again.
    auto withdraw = [&] {
        std::error_code ec;
        std::ifstream named(suffixed(slot, ".bmi"));
        if (std::string bmi; named && std::getline(named, bmi) && !bmi.empty())
            std::filesystem::remove(bmi, ec);
        std::filesystem::remove(suffixed(slot, ".rc"), ec);
    };
    if (*rc != 0) { withdraw(); return *rc; }

    if (!object.empty() && !file_exists(object)) {
        std::println(std::cerr, "mcpp: compiler reported success but {} is missing",
                     object.string());
        withdraw();
        return 1;
    }
    return 0;
}

// WHAT AN ORDINARY COMPILE WROTE BEFORE, HELD WHILE THE COMPILER RUNS.
//
// The previous file under another name beside its own; when the compiler has
// written one that stays (`mcpp::buildopt::keep::keeps_previous` for a BMI, which reads
// its format and the directory's cutoff; `keeps_previous_object` for an
// object, whose bytes decide; both record the answer in the build directory),
// the previous file takes its place again and the output's time has not
// moved, so ninja's `restat` ends the rebuild at this edge: no importer is
// compiled for the BMI, nothing is archived or linked for the object. In every
// other outcome -- a different file, a failed or interrupted compiler, nothing
// written before -- the held file is removed and the compiler's output stands.
//
// For the BMI this was a shell fragment of the generated command (`cp -p`,
// `bmi-equal`, `mv`), which a host without a POSIX shell did not run; here
// every host does. A BMI is copied, since a compiler that fails must leave the
// one its importers were compiled against; an object is renamed, since nothing
// reads it while its own edge runs.
struct Previous {
    enum class Kind { Bmi, Object };
    Kind kind;
    std::filesystem::path file, held;
    Previous(Kind k, const std::filesystem::path& of) : kind(k) {
        std::error_code ec;
        if (of.empty() || !std::filesystem::is_regular_file(of, ec)) return;
        auto to = suffixed(of, k == Kind::Bmi ? ".bak" : ".prev");
        if (k == Kind::Object) {
            std::filesystem::rename(of, to, ec);
            if (ec) return;
        } else {
            std::filesystem::copy_file(of, to, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) return;
            const auto time = std::filesystem::last_write_time(of, ec);
            if (!ec) std::filesystem::last_write_time(to, time, ec);
            if (ec) { std::filesystem::remove(to, ec); return; }
        }
        file = of;
        held = std::move(to);
    }
    Previous(const Previous&) = delete;
    Previous& operator=(const Previous&) = delete;
    void settle(bool compiled) {
        if (held.empty()) return;
        std::error_code ec;
        if (compiled && (kind == Kind::Bmi ? mcpp::buildopt::keep::keeps_previous(file, held)
                                           : mcpp::buildopt::keep::keeps_previous_object(file, held))) {
            std::filesystem::rename(held, file, ec);
            if (!ec) { held.clear(); return; }
        }
        // No output at all (the compiler was not started, or failed and its
        // output was removed): the object it had is the one there is.
        if (kind == Kind::Object && !std::filesystem::exists(file, ec)) std::filesystem::rename(held, file, ec);
        else std::filesystem::remove(held, ec);
        held.clear();
    }
    ~Previous() { settle(false); }
};

namespace {

// THE NAME OF AN ENTRY AS THE FILE SYSTEM SPELLS IT, for a name the record
// spelled in lower case (§20 T1): the entry whose name is the same without
// ASCII case, else the one the system opens for that name.
std::string on_disk_name(const std::string& dir, const std::string& name) {
    namespace fs = std::filesystem;
    const auto directory = mcpplibs::os::fs::from_utf8(dir);
    std::error_code ec;
    for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        std::string entry = mcpplibs::os::fs::utf8(it->path().filename());
        if (mcpp::build::depfile::detail::same_text(entry, name, true)) return entry;
    }
    const auto wanted = directory / mcpplibs::os::fs::from_utf8(name);
    ec.clear();
    for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code same;
        if (fs::equivalent(it->path(), wanted, same)) return mcpplibs::os::fs::utf8(it->path().filename());
    }
    return name;
}

// The compiler's record of the files it read, given to ninja as lines of this
// executor's output (§20 T1: `kInputsPrefix` and a path, which the edge reads
// as `deps = msvc`), and then removed. A failed compile gives none: ninja
// reads none for a failed edge.
std::expected<void, std::string> write_inputs(const std::filesystem::path& record, bool compiled) {
    std::error_code ec;
    if (!compiled) {
        std::filesystem::remove(record, ec);
        return {};
    }
    std::ifstream in(record, std::ios::binary);
    if (!in) return std::unexpected(std::format("the compiler wrote no record of the files it read ({})",
                                                mcpp::home::spelling(record)));
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    in.close();
    mcpp::build::depfile::SourceRecord source{.record = text};
    source.buildDir = mcpplibs::os::fs::generic_utf8(std::filesystem::current_path(ec));
    for (const char* variable : {"INCLUDE", "EXTERNAL_INCLUDE"})
        if (auto value = mcpp::invocation::get(variable))
            for (auto const& dir : mcpplibs::os::env::split_list(*value))
                source.toolchainDirs.push_back(mcpplibs::os::fs::generic_utf8(dir));
    source.foldCase = mcpplibs::os::conventions().case_insensitive_names;
    source.spelled = on_disk_name;
    auto inputs = mcpp::build::depfile::inputs_from_source_dependencies(source);
    if (!inputs) return std::unexpected(std::format("{}: {}", mcpp::home::spelling(record), inputs.error()));
    std::string lines;
    for (auto const& path : *inputs) {
        lines += mcpp::build::depfile::kInputsPrefix;
        lines += ' ';
        lines += path;
        lines += '\n';
    }
    if (!lines.empty() && !mcpplibs::os::stdio::write_output(lines))
        return std::unexpected(std::string("the files it read cannot be written to ninja"));
    std::filesystem::remove(record, ec);
    return {};
}

// The compiler's text, decoded where it is read (§20 T2): a line on.
void write_line(std::string_view line) {
    std::string out(line);
    out += '\n';
    (void)mcpplibs::os::stdio::write_output(out);
}

}  // namespace

int run_capped(const std::vector<std::string>& argv,
               const std::filesystem::path& semaphore, int maxCompilers,
               const std::filesystem::path& output, const std::filesystem::path& depfile,
               const std::filesystem::path& bmi, bool object, std::string_view relocate,
               const Reading& reading) {
    if (argv.empty()) { std::println(std::cerr, "mcpp: __compiler-cap: no compiler command was given"); return 2; }
    auto actor = std::make_shared<Actor>();
    if (!actor->ready) return 130;
    // EVERY RETURN BUT THE COMPILER'S OWN STATUS AND A CANCELLATION SAYS
    // WHICH STEP ENDED IT AND WHY (§10.3 G): ninja shows the line, and a
    // helper that ended with 1 or 2 and nothing on its error stream left a
    // failed build with no cause (Termux, 2026-11-18).
    const auto out_name=output.empty() ? std::string("a compile") : mcpp::home::spelling(output);
    const bool decoding = reading.text != mcpplibs::os::text::encoding::utf8;
    // The compiler's record of the files it read, given to ninja (§20 T1);
    // the status the edge ends with.
    auto read_inputs = [&](int compiled) {
        if (reading.sourceDependencies.empty()) return compiled;
        auto given = write_inputs(reading.sourceDependencies, compiled == 0 && actor->owner.active());
        if (given) return compiled;
        std::println(std::cerr, "mcpp: giving ninja the files the compile of {} read: {}", out_name, given.error());
        return 1;
    };
    auto ended=[&](int status,std::string_view step,std::string_view why) {
        std::println(std::cerr,"mcpp: {} for {}: {}",step,out_name,why);return status;
    };
    // THE ATTEMPT'S OWN PATHS (§10.6), decided before anything of the
    // compile is held or recorded.
    std::vector<std::string> command = argv;
    relocation::Plan plan;
    if (!relocate.empty()) {
        const auto name = attempt_name();
        if (!name) return ended(1, "relocating the compile's outputs", "this process's identifier or birth cannot be read");
        auto planned = relocation::relocate(command, {output.string(), depfile.string(), bmi.string(), object},
                                            *name, relocate == "none" ? std::string_view{} : relocate);
        if (!planned) return ended(1, "relocating the compile's outputs", planned.error());
        plan = std::move(*planned);
    }
    // An attempt in place holds what the compile wrote before (`Previous`);
    // a relocated one leaves it where it is until it publishes.
    std::optional<Previous> previousBmi, previousObject;
    if (!plan.relocated()) {
        previousBmi.emplace(Previous::Kind::Bmi, bmi);
        previousObject.emplace(Previous::Kind::Object, object ? output : std::filesystem::path{});
    }
    // Before the transaction certifies what the compiler retired: the
    // certificate names the output as it stands.
    auto settle = [&](bool compiled) -> std::expected<void, std::string> {
        if (!plan.relocated()) { previousBmi->settle(compiled); previousObject->settle(compiled); return {}; }
        auto published = compiled ? relocation::publish(plan) : std::expected<void, std::string>{};
        relocation::discard(plan.directory);
        return published;
    };
    if(actor->owner.managed) {
        auto prepared=transactions::Attempt::prepare(actor->owner.generation,true,{},output,{},depfile);
        if(!prepared)return ended(1,"preparing the compiler transaction",prepared.error());
        auto attempt=std::move(*prepared);
        if(plan.relocated()&&!attempt.relocate(std::filesystem::absolute(plan.directory)))
            return ended(1,"recording the compile's relocation","its record cannot be written");
        const auto who=mcpplibs::os::process::self();
        if(!who||!who->id||!who->started_ns)
            return ended(1,"identifying the compiler controller","this process's identifier or birth cannot be read");
        if(!transactions::publish(attempt.directory/"controller","mcpp-controller-v1",{
                std::to_string(who->id),std::to_string(who->started_ns),transactions::spelling(actor->path)}))
            return ended(1,"recording the compiler controller","its record cannot be written");
        auto permit=acquire_permit(*actor,semaphore,maxCompilers);if(!permit)return permit.error();
        auto created=mcpplibs::os::process::owned_unit::create();
        if(!created)return ended(2,"declaring the compiler unit",created.error().message());
        auto unit=std::make_shared<mcpplibs::os::process::owned_unit>(std::move(*created));
        InterruptCleanup cleanup{mcpp::run::add_cleanup([actor,unit] {unit->terminate();})};
        if(auto registered=attempt.register_unit(*unit,actor->path,permit->reservation);!registered)
            return ended(2,"registering the compiler unit",registered.error());
        if(!actor->owner.active())return 130;
        if(!transactions::publish(attempt.directory/"admitted","mcpp-admitted-v1",{}))
            return ended(2,"recording the compiler's admission","its record cannot be written");
        const auto result=mcpp::run::execute(command,{
            .input=mcpp::run::Input::inherit,.streams=decoding?mcpp::run::Streams::merged:mcpp::run::Streams::inherit,
            .on_line=decoding?std::function<void(std::string_view)>(write_line):nullptr,
            .should_stop=[&] {return !actor->owner.active();},.declared_unit=unit.get(),.text=reading.text});
        if(auto retired=retire_owned(*unit,actor->owner);!retired)
            return ended(1,"retiring the compiler unit",retired.error());
        // A compile whose record cannot be given to ninja failed: files it
        // read that ninja does not know of would leave the edge untracked.
        const int status=read_inputs(result.exit_code);
        if(auto settled=settle(status==0&&actor->owner.active());!settled)
            return ended(1,"publishing the compile's outputs",settled.error());
        if(!attempt.retired(status)||!attempt.result(status))
            return ended(1,"certifying the compiler's result","its record cannot be written");
        if(!transactions::release_reservation(permit->reservation,attempt.directory))
            return ended(1,"releasing the compiler slot","its reservation cannot be released");
        return status;
    }
    // An interrupted compile in place leaves no output a later build would
    // take for complete; a relocated one leaves its own directory only.
    const auto withdrawn = plan.relocated() ? plan.directory : output;
    InterruptCleanup cleanup{mcpp::run::add_cleanup([actor, withdrawn] {
        std::error_code ec;
        if (!withdrawn.empty()) std::filesystem::remove_all(withdrawn, ec);
    })};
    auto permit = acquire_permit(*actor, semaphore, maxCompilers);
    if (!permit) return permit.error();
    if (!actor->owner.active()) return 130;
    const auto result = mcpp::run::execute(command, {
        .input = mcpp::run::Input::inherit,
        .streams = decoding ? mcpp::run::Streams::merged : mcpp::run::Streams::inherit,
        .on_line = decoding ? std::function<void(std::string_view)>(write_line) : nullptr,
        .should_stop = [&] { return !actor->owner.active(); },
        .text = reading.text});
    const int status = read_inputs(result.exit_code);
    if (!actor->owner.active() || status != 0) {
        std::error_code ec;
        if (!withdrawn.empty()) std::filesystem::remove_all(withdrawn, ec);
        return actor->owner.active() ? status : 130;
    }
    if (auto settled = settle(true); !settled) return ended(1, "publishing the compile's outputs", settled.error());
    return status;
}

}  // namespace mcpp::build::schedule::detach

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
import mcpp.build.command_line;
import mcpp.run;
import mcpp.build.stage;
import mcpp.build.schedule.compiler_transaction;
import mcpp.home;
import mcpp.home.files;
import mcpp.invocation;
import mcpp.toolchain.fingerprint;

export namespace mcpp::build::schedule::detach {

inline constexpr std::string_view kRunEnv = "MCPP_COMPILER_RUN";

// One runtime generation, outside the build identity. Its actor barrier covers
// every helper that may write state, not only helpers holding compiler slots.
class RunScope {
public:
    static std::expected<RunScope, std::string> begin(const std::filesystem::path& outputDir);
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
};

// Phase 1 — returns 0 as soon as the BMI is published, leaving code generation
// running. Returns the compiler's status if it exits before publishing one.
int compile_release_at_bmi(const CompileRequest& req);

// The supervisor. Runs the compiler to completion with its output redirected,
// then records the status. Never invoked directly by a build edge.
int supervise(const std::filesystem::path& slot,
              const std::filesystem::path& semaphore, int maxCompilers,
              const std::filesystem::path& object, std::string_view command);

// Phase 2 — blocks until the compiler for `slot` finished, replays what it
// wrote, and propagates its status. `object`, when given, must exist: a
// compiler that reports success without producing its output is a failure this
// must not pass on.
int await_unit(const std::filesystem::path& slot, const std::filesystem::path& object);

// Ordinary compiler invocations share the detached compilers' actual bound.
// Their original argv and streams are retained; no shell is introduced.
int run_capped(const std::vector<std::string>& argv,
               const std::filesystem::path& semaphore, int maxCompilers,
               const std::filesystem::path& output = {},
               const std::filesystem::path& depfile = {});

}  // namespace mcpp::build::schedule::detach

// ---------------------------------------------------------------------------

namespace mcpp::build::schedule::detach {
namespace {
namespace transactions = mcpp::build::schedule::transaction;

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
};
std::expected<Permit, int>
acquire_permit(const Actor& actor, const std::filesystem::path& requested, int cap) {
    const auto dir = actor.owner.permits(requested);
    if (!actor.ready) return std::unexpected(130);
    if (dir.empty() || cap <= 0) {
        if (actor.owner.managed) return std::unexpected(2);
        return Permit{};
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        std::println(std::cerr, "mcpp: cannot create compiler semaphore {}: {}", dir.string(), ec.message());
        return std::unexpected(1);
    }
    const auto started = std::chrono::steady_clock::now();
    constexpr auto kLimit = std::chrono::hours(2);
    for (;;) {
        if (!actor.owner.active()) return std::unexpected(130);
        bool orphan = false;
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
                return Permit{std::optional<mcpplibs::os::fs::lock>{std::move(*permit)},reservation};
            }
            if (permit.error().code != mcpplibs::os::errc::again) {
                std::println(std::cerr, "mcpp: {}", permit.error().message());
                {   // [temp] probe 885 macOS: what the failing lock saw.
                    std::error_code pec;
                    const auto lockFile = dir / std::format("slot-{}.lock", i);
                    std::println(std::cerr, "probe885: dir='{}' exists={} lockexists={} cwd='{}' env='{}'",
                                 dir.string(), std::filesystem::exists(dir, pec), std::filesystem::exists(lockFile, pec),
                                 std::filesystem::current_path(pec).string(),
                                 mcpp::invocation::get(kRunEnv).value_or("<unset>"));
                    for (const auto& name : mcpplibs::os::fs::preopens()) std::println(std::cerr, "probe885: preopen '{}'", name);
                    std::ofstream touch(dir / "probe-touch");
                    std::println(std::cerr, "probe885: ofstream create in dir: {}", touch.good());
                    auto again = mcpplibs::os::fs::lock::acquire(lockFile, mcpplibs::os::fs::lock::kind::exclusive, false);
                    std::println(std::cerr, "probe885: second acquire: {}", again ? std::string("ok") : again.error().message());
                }
                return std::unexpected(1);
            }
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
    if (stage::bmi_equivalent(bmi, backup))
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

std::expected<RunScope, std::string> RunScope::begin(const std::filesystem::path& outputDir) {
    const auto self = mcpplibs::os::process::self();
    if (!self || self->id == 0 || self->started_ns == 0)
        return std::unexpected("cannot identify the compiler-run owner and its birth time");
    std::error_code ec;
    const auto spelled = std::filesystem::absolute(outputDir, ec).lexically_normal();
    if (ec) return std::unexpected("cannot identify the compiler-run output directory");
    const auto canonical = std::filesystem::weakly_canonical(spelled, ec);
    if (ec) return std::unexpected("cannot identify the compiler-run output directory");
    auto state = std::make_unique<State>();
    // Locate work BEFORE resolving output symlinks: target/<triple> may point
    // at a disposable cache. Locks remain beside target/, independently of
    // clean and MCPP_HOME; the canonical output still identifies the slots.
    const auto target = spelled.parent_path().parent_path();
    const auto work = std::filesystem::weakly_canonical(
        target.filename() == "target" ? target.parent_path() : spelled.parent_path(), ec);
    if (ec) return std::unexpected("cannot identify the compiler-run work directory");
    state->root = work / ".mcpp" / "compiler-runs"
        / mcpp::toolchain::hash_string(mcpp::home::spelling(canonical));
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
        if (!free) return std::unexpected("previous compiler helpers are still active; their state was retained");
        if(auto recovered=transactions::recover_generation(entry.path());!recovered)return std::unexpected(recovered.error());
        std::filesystem::remove_all(entry.path(), ec);
        if (ec) return std::unexpected("cannot retire previous compiler-run state");
    }
    // Old owners, actors and units are quiet before cached output recovery;
    // no Ninja reader from this generation has started yet.
    if(auto admitted=transactions::admit_cached_outputs(canonical);!admitted)
        return std::unexpected(admitted.error());
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
    const auto record = std::format("mcpp-compiler-run-v1\n{} {}\n", self->id, self->started_ns);
    if (!transactions::replace_record(state->generation / "owner", record))
        return std::unexpected("cannot publish compiler-run ownership");
    return RunScope(std::move(state));
}

void RunScope::environment(std::vector<std::pair<std::string, std::string>>& env) const {
    std::erase_if(env, [](const auto& entry) { return entry.first == kRunEnv; });
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
    std::optional<transactions::Attempt> attempt;
    if(actor.owner.managed) {
        auto prepared=transactions::Attempt::prepare(actor.owner.generation,false,req.bmi,req.object,req.slot,req.depTo);
        if(!prepared) {std::println(std::cerr,"mcpp: {}",prepared.error());return 1;}
        attempt.emplace(std::move(*prepared));
        actual.bmi=attempt->bmi;actual.object=attempt->object;actual.slot=attempt->slot;actual.depTo=attempt->dep;
        if(auto copied=attempt->prepare_backup();!copied) {std::println(std::cerr,"mcpp: {}",copied.error());return 1;}
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
    const BmiIdentity before = bmi_identity(actual.bmi);
    if (!attempt && !actual.bmi.empty()) {
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

    // The supervisor reads the SAME argv file rather than receiving the command
    // on its own command line: one representation, no re-quoting, and no limit
    // on how long a compiler command may be.
    std::vector<std::string> sup{actual.self.string(), "bmi-supervise",
                                 "--slot", actual.slot.string(),
                                 "--command-file", actual.commandFile.string()};
    if (!actual.semaphore.empty()) { sup.push_back("--sem"); sup.push_back(actual.semaphore.string()); }
    if (actual.maxCompilers > 0) { sup.push_back("--cap"); sup.push_back(std::to_string(actual.maxCompilers)); }
    if (!actual.object.empty()) { sup.push_back("--object"); sup.push_back(actual.object.string()); }
    if (!actor.owner.active()) return 130;
    std::optional<mcpplibs::os::process::program> supervisor;
    if (!spawn_detached(sup,actor.owner.managed,&supervisor)) {
        if(!attempt)restore_backup(actual.bmi);
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
    constexpr auto kNoSupervisorGrace = std::chrono::seconds(10);
    constexpr auto kHardLimit         = std::chrono::hours(2);
    auto nextControllerCheck=started;

    for (;;) {
        if (!actor.owner.active()) return 130;
        // GCC unlinks an existing BMI immediately before renaming its new
        // temporary file. Absence is a transition, never publication: treating
        // it as a changed identity releases importers inside that unlink gap.
        const auto published = bmi_identity(actual.bmi);
        if (published.present && published != before) {
            if(attempt) {
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
                if(!attempt)restore_backup(actual.bmi);
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
            if(!attempt)restore_backup(actual.bmi);
            return 1;
        }
        if (waited > kHardLimit) {
            std::println(std::cerr,
                         "mcpp: timed out waiting for {} to publish its BMI",
                         actual.slot.string());
            if(!attempt)restore_backup(actual.bmi);
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

int supervise(const std::filesystem::path& slot,
              const std::filesystem::path& semaphore, int maxCompilers,
              const std::filesystem::path& object, std::string_view command) {
    auto actor = std::make_shared<Actor>();
    if (!actor->ready) return 130;
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
        const int rc=run_to_completion(command,suffixed(slot,".log"),actor->owner,unit.get());
        if(auto retired=retire_owned(*unit,actor->owner);!retired) {
            record_supervisor_error(slot,retired.error());return 1;
        }
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
    const int rc = run_to_completion(command, suffixed(slot, ".log"), actor->owner);
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
    constexpr auto kNoSupervisorGrace = std::chrono::seconds(10);
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

int run_capped(const std::vector<std::string>& argv,
               const std::filesystem::path& semaphore, int maxCompilers,
               const std::filesystem::path& output, const std::filesystem::path& depfile) {
    if (argv.empty()) return 2;
    auto actor = std::make_shared<Actor>();
    if (!actor->ready) return 130;
    if(actor->owner.managed) {
        auto prepared=transactions::Attempt::prepare(actor->owner.generation,true,{},output,{},depfile);
        if(!prepared) {std::println(std::cerr,"mcpp: {}",prepared.error());return 1;}
        auto attempt=std::move(*prepared);
        const auto who=mcpplibs::os::process::self();
        if(!who||!who->id||!who->started_ns
            ||!transactions::publish(attempt.directory/"controller","mcpp-controller-v1",{
                std::to_string(who->id),std::to_string(who->started_ns),transactions::spelling(actor->path)}))return 1;
        auto permit=acquire_permit(*actor,semaphore,maxCompilers);if(!permit)return permit.error();
        auto created=mcpplibs::os::process::owned_unit::create();
        if(!created)return 2;
        auto unit=std::make_shared<mcpplibs::os::process::owned_unit>(std::move(*created));
        InterruptCleanup cleanup{mcpp::run::add_cleanup([actor,unit] {unit->terminate();})};
        if(auto registered=attempt.register_unit(*unit,actor->path,permit->reservation);!registered) {
            std::println(std::cerr,"mcpp: {}",registered.error());return 2;
        }
        if(!actor->owner.active())return 130;
        if(!transactions::publish(attempt.directory/"admitted","mcpp-admitted-v1",{}))return 2;
        const auto result=mcpp::run::execute(argv,{
            .input=mcpp::run::Input::inherit,.streams=mcpp::run::Streams::inherit,
            .should_stop=[&] {return !actor->owner.active();},.declared_unit=unit.get()});
        if(auto retired=retire_owned(*unit,actor->owner);!retired) {std::println(std::cerr,"mcpp: {}",retired.error());return 1;}
        if(!attempt.retired(result.exit_code)||!attempt.result(result.exit_code))return 1;
        if(!transactions::release_reservation(permit->reservation,attempt.directory))return 1;
        return result.exit_code;
    }
    InterruptCleanup cleanup{mcpp::run::add_cleanup([actor, output] {
        if (!output.empty()) {
            std::error_code ec;
            std::filesystem::remove(output, ec);
        }
    })};
    auto permit = acquire_permit(*actor, semaphore, maxCompilers);
    if (!permit) return permit.error();
    if (!actor->owner.active()) return 130;
    const auto result = mcpp::run::execute(argv, {
        .input = mcpp::run::Input::inherit,
        .streams = mcpp::run::Streams::inherit,
        .should_stop = [&] { return !actor->owner.active(); }});
    if (!actor->owner.active() || result.exit_code != 0) {
        if (!output.empty()) {
            std::error_code ec;
            std::filesystem::remove(output, ec);
        }
        return actor->owner.active() ? result.exit_code : 130;
    }
    return result.exit_code;
}

}  // namespace mcpp::build::schedule::detach

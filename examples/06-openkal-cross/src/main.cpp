// One source, four machines, and a program that asks each of them what it is.
//
//     mcpp run                                  this machine
//     mcpp build --target x86_64-linux          Linux   x86_64,  any host
//     mcpp build --target aarch64-linux         Linux   aarch64, any host
//     mcpp build --target aarch64-macos         macOS   aarch64, any host
//     mcpp build --target x86_64-windows        Windows x86_64,  any host
//
// ⭐ EVERY ONE OF THEM DECLINES THE THIRD SEGMENT, AND THE FOURTH LINE COULD
// NOT BE WRITTEN UNTIL 2026.8.26.2. `aarch64-linux` used to complete to
// `aarch64-linux-gnu` — a row registered but not supported — and refuse, while
// `aarch64-linux-musl` built. A request that names no C library now resolves to
// a row that exists; see docs/16.
//
// Nothing below is conditional on a platform. There is no preprocessor
// directive in this file, and no branch on a target name. What differs between
// the four builds is which packages the dependency graph resolved, and the
// only trace of that difference in the source is that the program ASKS about
// capabilities instead of assuming them.
//
// That distinction is the subject of the example. A portable program written
// against a conventional stack is a program that compiles under several sets of
// `#if`s; a program written against a named interface is one source that
// queries what it landed on. The queries below are chosen so that each one
// exercises a different layer of the target side, and so that a build which
// silently reached the host's own libraries instead of the resolved ones would
// answer differently.
//
// ⚠️ WHAT IS ASKED HERE AND WHAT IS NOT.
//
// A capability word says how an implementation behaves WITHIN an interface it
// provides. Whether it provides the interface at all is a different question,
// answered earlier and by something else: the dependency graph, and failing
// that the linker. So this file may ask a filesystem how it compares names, and
// may not ask a machine whether it has a filesystem — on one that does not,
// `kal::fs::properties()` is an undefined symbol, and that is clause 6.1 of the
// specification working rather than failing. A program for such a machine is a
// different program, and the README says what it writes instead.

import std;

import openkal.env;
import openkal.fs;
import openkal.task;
import openkal.time;
import openkal.types;

namespace {

// A row of the report. Collected into a container first rather than printed as
// it is produced, because the container is what requires the allocator, and the
// allocator is `openkal.memory` rather than the host's.
struct fact {
    std::string subject;
    std::string value;
};

// Records that its destructor ran. Unwinding is the one part of the C++ runtime
// that links successfully whether or not it works: a program whose unwinder is
// absent still builds, and fails only when something is thrown. Running a
// destructor during the unwind is what separates the two.
struct scope_marker {
    bool* ran;
    ~scope_marker() { *ran = true; }
};

struct unwound {};

bool destructor_ran_during_unwind() {
    bool ran = false;
    try {
        scope_marker marker{&ran};
        throw unwound{};
    } catch (const unwound&) {
    }
    return ran;
}

std::string yes_no(bool b) { return b ? "yes" : "no"; }

// The first command-line argument, or a placeholder. `kal::env` reports the
// argument vector the platform actually delivered, which on Windows is derived
// from a single command-line string and on Linux from the stack the kernel
// prepared. The program does not need to know which.
std::string program_name() {
    if (kal::env::arg_count() == 0) return "(none)";
    kal_uintptr len = 0;
    const char* p = kal::env::arg(0, &len);
    if (p == nullptr || len == 0) return "(empty)";
    return std::string(p, static_cast<std::size_t>(len));
}

} // namespace

int main() {
    std::vector<fact> facts;

    // ── The platform interface ──────────────────────────────────────────────
    //
    // A preopened directory is the only filesystem root a program is given on a
    // capability-oriented platform. The count is a property of an implementation
    // that HAS a filesystem — how many roots it handed over — and not a way of
    // discovering whether there is one.
    facts.push_back({"preopened directories",
                     std::format("{}", kal::fs::preopen_count())});
    facts.push_back({"case-sensitive paths",
                     yes_no(kal::fs::has(kal::fs::case_sensitive))});
    facts.push_back({"symbolic links",
                     yes_no(kal::fs::has(kal::fs::links))});
    facts.push_back({"atomic rename",
                     yes_no(kal::fs::has(kal::fs::atomic_rename))});

    // ── Time ────────────────────────────────────────────────────────────────
    //
    // Two clocks with different guarantees. A wall clock may be absent, which
    // is the ordinary state of a machine that has just been powered on and has
    // no battery-backed counter; the monotonic clock is always present because
    // the specification requires it.
    facts.push_back({"wall clock",
                     yes_no(kal::time::has(kal::time::wall_available))});
    facts.push_back({"monotonic granularity (ns)",
                     std::format("{}", kal::time::granularity())});

    const auto before = kal::time::monotonic();
    kal::time::sleep(1'000'000);            // one millisecond
    const auto after = kal::time::monotonic();
    facts.push_back({"monotonic advanced", yes_no(after > before)});

    // ── Concurrency ─────────────────────────────────────────────────────────
    //
    // Reported rather than used. Every implementation reached here provides
    // `openkal.task`; what varies is whether its scheduler preempts and whether
    // anything runs in parallel. A program that spawns a thread without asking
    // does not fail to compile on a machine with one core and a cooperative
    // scheduler; it fails to return.
    facts.push_back({"preemptive scheduling",
                     yes_no(kal::task::has(kal::task::preemptive))});
    facts.push_back({"parallel execution",
                     yes_no(kal::task::has(kal::task::parallel))});
    facts.push_back({"thread-local storage",
                     yes_no(kal::task::has(kal::task::thread_local_storage))});

    // ── The C++ runtime ─────────────────────────────────────────────────────
    facts.push_back({"unwinding runs destructors",
                     yes_no(destructor_ran_during_unwind())});

    // ── The standard library ────────────────────────────────────────────────
    //
    // A sort and a fold, present so that the report is not the only thing the
    // allocator and the algorithm headers are asked to do.
    std::vector<int> sample{9, 3, 7, 1, 8, 2};
    std::ranges::sort(sample);
    const int total = std::accumulate(sample.begin(), sample.end(), 0);
    facts.push_back({"sorted sample",
                     std::format("{} (sum {})", sample, total)});

    // ── Output ──────────────────────────────────────────────────────────────
    //
    // One column width for every row, computed rather than hard-coded, so that
    // the output of the three builds can be compared line by line.
    std::size_t width = 0;
    for (const auto& f : facts) width = std::max(width, f.subject.size());

    std::println("{}", program_name());
    std::println("{}", std::string(width + 22, '-'));
    for (const auto& f : facts)
        std::println("{:<{}}   {}", f.subject, width, f.value);

    return 0;
}

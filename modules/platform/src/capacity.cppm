// mcpp.platform.capacity — how much machine is actually available.
//
// Exists because `nproc` is the wrong number to build with, in two separate ways
// that both have measurements behind them:
//
//   MEMORY. A C++23 module compile is not cheap in RAM. Measured on this
//   repository with GCC 16.1 at -O2:
//       src/build/prepare.cppm   peak RSS 1,057 MB
//       src/build/plan.cppm      peak RSS   561 MB
//   ninja's default job count is `nproc + 2`. On a 64-core / 32 GB machine that
//   is 66 concurrent compiles against ~0.5-1 GB each — the machine swaps, and a
//   swapping build is far slower than a smaller job count would have been. The
//   default is not merely un-tuned there; it is actively harmful.
//
//   HETEROGENEITY. An i9-13900K reports 32 logical CPUs, but they are 8 P-cores
//   (SMT, 16 threads) plus 16 E-cores. E-cores deliver roughly 40% of a P-core's
//   compile throughput and SMT siblings roughly 25%. Treating 32 threads as 32
//   equal workers overestimates usable parallelism by more than 2x.
//
// The interface deliberately names no `std` type: under GCC 16.1 a newly added
// module whose EXPORTS mention std types can poison the BMIs of everything
// downstream of it, and the failures point at unrelated modules. Integers only.
export module mcpp.platform.capacity;

export namespace mcpp::platform::capacity {

struct HostCapacity {
    int           logicalCores   = 1;
    int           physicalCores  = 1;
    // True when the CPU mixes core classes (Intel P/E, Apple performance +
    // efficiency). Recorded rather than inferred: every parallelism figure has
    // to be read against it.
    bool          heterogeneous  = false;
    unsigned long long totalBytes     = 0;
    unsigned long long availableBytes = 0;   // falls back to total when unknown
};

HostCapacity host_capacity();

// Job count for `auto`. See the module comment for why this is not `nproc`.
//
//   cpu_budget = heterogeneous ? physicalCores : logicalCores
//   mem_budget = (available - reserve) / per_job
//   jobs       = clamp(min(cpu, mem), 1, ceiling)
//
// `perJobBytes` and `reserveBytes` are parameters rather than constants so a
// project whose translation units are heavier (or lighter) than mcpp's can say
// so without patching this file.
int recommended_jobs(const HostCapacity& cap,
                     unsigned long long perJobBytes  = 768ull * 1024 * 1024,
                     unsigned long long reserveBytes = 2ull * 1024 * 1024 * 1024,
                     int ceiling = 64);

} // namespace mcpp::platform::capacity

// ─── Implementation ────────────────────────────────────────────────────────

namespace mcpp::platform::capacity {

int recommended_jobs(const HostCapacity& cap, unsigned long long perJobBytes,
                     unsigned long long reserveBytes, int ceiling) {
    // A heterogeneous machine's logical count is not a count of equal workers,
    // so fall back to physical cores there rather than pretending E-cores and
    // SMT siblings are whole CPUs.
    int cpuBudget = cap.heterogeneous ? cap.physicalCores : cap.logicalCores;
    if (cpuBudget < 1) cpuBudget = 1;

    int memBudget = cpuBudget;
    if (perJobBytes > 0 && cap.availableBytes > reserveBytes) {
        const unsigned long long usable = cap.availableBytes - reserveBytes;
        const unsigned long long fits   = usable / perJobBytes;
        memBudget = fits > 0 ? static_cast<int>(fits > 1000000 ? 1000000 : fits) : 1;
    } else if (perJobBytes > 0) {
        // Less memory available than the reserve: still make progress, but one
        // job at a time. Refusing to build would be worse than building slowly.
        memBudget = 1;
    }

    int jobs = cpuBudget < memBudget ? cpuBudget : memBudget;
    if (jobs < 1) jobs = 1;
    if (ceiling > 0 && jobs > ceiling) jobs = ceiling;
    return jobs;
}

} // namespace mcpp::platform::capacity

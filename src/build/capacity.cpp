// mcpp.build.capacity: the job budget, and the host measured by mcpplibs.os.
module mcpp.build.capacity;

import std;
import mcpplibs.os.machine;


// ─── Implementation ────────────────────────────────────────────────────────

namespace mcpp::build::capacity {

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

} // namespace mcpp::build::capacity

namespace mcpp::build::capacity {

// The measurements are mcpplibs.os's (machine::processors, memory and
// memory_available); the budget above is this module's.
HostCapacity host_capacity() {
    namespace machine = mcpplibs::os::machine;
    HostCapacity cap;
    const auto p = machine::processors();
    cap.logicalCores  = static_cast<int>(p.logical);
    cap.physicalCores = static_cast<int>(p.physical);
    cap.heterogeneous = p.heterogeneous;
    cap.totalBytes     = machine::memory().value_or(0);
    cap.availableBytes = machine::memory_available().value_or(cap.totalBytes);
    if (cap.availableBytes == 0) cap.availableBytes = cap.totalBytes;
    return cap;
}

} // namespace mcpp::build::capacity

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
module;

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>
#include <mach/mach.h>
#else
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>   // atoi
#include <string.h>
#endif

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

#if defined(_WIN32)

// The processor relationship table needs the two-call pattern: its length is not
// knowable up front, so ask for the size, allocate, then ask again.
static bool win_core_facts(int& physical, bool& hybrid) {
    DWORD bytes = 0;
    ::GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
    if (bytes == 0) return false;
    auto* buf = static_cast<unsigned char*>(::malloc(bytes));
    if (!buf) return false;
    bool ok = false;
    if (::GetLogicalProcessorInformationEx(
            RelationProcessorCore,
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf), &bytes)) {
        int count = 0, firstClass = -1;
        DWORD off = 0;
        while (off < bytes) {
            auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf + off);
            if (info->Size == 0) break;
            ++count;
            const int cls = static_cast<int>(info->Processor.EfficiencyClass);
            if (firstClass < 0) firstClass = cls;
            else if (cls != firstClass) hybrid = true;
            off += info->Size;
        }
        if (count > 0) { physical = count; ok = true; }
    }
    ::free(buf);
    return ok;
}

HostCapacity host_capacity() {
    HostCapacity cap;
    SYSTEM_INFO si{};
    ::GetSystemInfo(&si);
    if (si.dwNumberOfProcessors > 0) cap.logicalCores = static_cast<int>(si.dwNumberOfProcessors);
    cap.physicalCores = cap.logicalCores;
    win_core_facts(cap.physicalCores, cap.heterogeneous);

    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (::GlobalMemoryStatusEx(&ms)) {
        cap.totalBytes     = ms.ullTotalPhys;
        cap.availableBytes = ms.ullAvailPhys;
    }
    return cap;
}

#elif defined(__APPLE__)

HostCapacity host_capacity() {
    HostCapacity cap;
    const long n = ::sysconf(_SC_NPROCESSORS_ONLN);
    if (n > 0) cap.logicalCores = static_cast<int>(n);

    int value = 0;
    size_t len = sizeof(value);
    cap.physicalCores = (::sysctlbyname("hw.physicalcpu", &value, &len, nullptr, 0) == 0 && value > 0)
                      ? value : cap.logicalCores;

    // Apple Silicon is performance + efficiency by construction; hw.nperflevels
    // says so directly and is absent on Intel Macs.
    value = 0; len = sizeof(value);
    if (::sysctlbyname("hw.nperflevels", &value, &len, nullptr, 0) == 0)
        cap.heterogeneous = value > 1;

    unsigned long long mem = 0; len = sizeof(mem);
    if (::sysctlbyname("hw.memsize", &mem, &len, nullptr, 0) == 0) cap.totalBytes = mem;

    // Free + inactive is the honest "could be handed to a new process" figure on
    // Darwin; wired and active are not available in any useful sense.
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (::host_statistics64(::mach_host_self(), HOST_VM_INFO64,
                            reinterpret_cast<host_info64_t>(&vm), &count) == KERN_SUCCESS) {
        const unsigned long long page = static_cast<unsigned long long>(::getpagesize());
        cap.availableBytes = (static_cast<unsigned long long>(vm.free_count)
                            + static_cast<unsigned long long>(vm.inactive_count)) * page;
    }
    if (cap.availableBytes == 0) cap.availableBytes = cap.totalBytes;
    return cap;
}

#else   // Linux and other POSIX

static bool read_meminfo_kb(const char* key, unsigned long long& out) {
    FILE* f = ::fopen("/proc/meminfo", "r");
    if (!f) return false;
    char line[256];
    bool found = false;
    const size_t klen = ::strlen(key);
    while (::fgets(line, sizeof(line), f)) {
        if (::strncmp(line, key, klen) != 0) continue;
        unsigned long long kb = 0;
        if (::sscanf(line + klen, " %llu", &kb) == 1) { out = kb * 1024ull; found = true; }
        break;
    }
    ::fclose(f);
    return found;
}

static int read_cpuinfo_cores() {
    FILE* f = ::fopen("/proc/cpuinfo", "r");
    if (!f) return 0;
    char line[256];
    int cores = 0;
    while (::fgets(line, sizeof(line), f)) {
        if (::strncmp(line, "cpu cores", 9) != 0) continue;
        const char* colon = ::strchr(line, ':');
        if (colon) cores = ::atoi(colon + 1);
        break;
    }
    ::fclose(f);
    return cores;
}

// Hybrid x86 reports differing per-CPU maximum frequencies. Cheapest reliable
// signal short of CPUID; when cpufreq is absent the answer is "cannot tell",
// which must be reported as NOT heterogeneous — a false positive here would
// halve the job count on an ordinary homogeneous server.
static bool detect_hybrid(int logical) {
    if (logical <= 1) return false;
    long first = -1;
    for (int i = 0; i < logical; ++i) {
        char path[128];
        ::snprintf(path, sizeof(path),
                   "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
        FILE* f = ::fopen(path, "r");
        if (!f) return false;
        long v = 0;
        const int got = ::fscanf(f, "%ld", &v);
        ::fclose(f);
        if (got != 1) return false;
        if (first < 0) first = v;
        else if (v != first) return true;
    }
    return false;
}

HostCapacity host_capacity() {
    HostCapacity cap;
    const long n = ::sysconf(_SC_NPROCESSORS_ONLN);
    if (n > 0) cap.logicalCores = static_cast<int>(n);

    const int cores = read_cpuinfo_cores();
    cap.physicalCores = cores > 0 ? cores : cap.logicalCores;
    cap.heterogeneous = detect_hybrid(cap.logicalCores);

    const long pages = ::sysconf(_SC_PHYS_PAGES);
    const long psize = ::sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && psize > 0)
        cap.totalBytes = static_cast<unsigned long long>(pages)
                       * static_cast<unsigned long long>(psize);

    // MemAvailable is the kernel's own estimate of what a new workload can get
    // without swapping — strictly better than MemFree, which excludes reclaimable
    // page cache and would make every warm machine look starved.
    if (!read_meminfo_kb("MemAvailable:", cap.availableBytes))
        cap.availableBytes = cap.totalBytes;
    return cap;
}

#endif

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

// mcpp.platform.capacity on linux: what the machine has, from the system's own services.
module;
#include <stdio.h>
#include <stdlib.h>   // atoi
#include <string.h>
#include <unistd.h>

module mcpp.platform.capacity;

import std;

namespace mcpp::platform::capacity {

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

} // namespace mcpp::platform::capacity

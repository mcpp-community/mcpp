// mcpp.platform.capacity on windows: what the machine has, from the system's own services.
module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdlib.h>   // malloc / free — clang does not get these from windows.h

module mcpp.platform.capacity;

import std;

namespace mcpp::platform::capacity {


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

} // namespace mcpp::platform::capacity

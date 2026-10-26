// mcpp.platform.capacity on macos: what the machine has, from the system's own services.
module;
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>
#include <mach/mach.h>

module mcpp.platform.capacity;

import std;

namespace mcpp::platform::capacity {

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

} // namespace mcpp::platform::capacity

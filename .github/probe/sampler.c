// Samples the counts mcpplibs.os reads for memory_available on macOS (free + inactive), beside
// the other reclaimable counts and the kernel's own percentage (kern.memorystatus_level).
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <unistd.h>

int main(int argc, char** argv) {
    int samples = argc > 1 ? atoi(argv[1]) : 1;
    vm_size_t page = 0;
    host_page_size(mach_host_self(), &page);
    for (int i = 0; i < samples; ++i) {
        vm_statistics64_data_t vm;
        mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
        host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm, &count);
        int level = -1;
        size_t len = sizeof level;
        sysctlbyname("kern.memorystatus_level", &level, &len, NULL, 0);
        const double mib = (double)page / (1024.0 * 1024.0);
        printf("free+inactive %7.0f MiB | free %6.0f inactive %6.0f speculative %6.0f purgeable %6.0f "
               "external %6.0f compressor %6.0f wired %6.0f | memorystatus_level %d%%\n",
               (vm.free_count + vm.inactive_count) * mib, vm.free_count * mib, vm.inactive_count * mib,
               vm.speculative_count * mib, vm.purgeable_count * mib, vm.external_page_count * mib,
               vm.compressor_page_count * mib, vm.wire_count * mib, level);
        fflush(stdout);
        if (i + 1 < samples) usleep(200000);
    }
    return 0;
}

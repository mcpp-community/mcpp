// mcpp.platform.capacity::recommended_jobs — the `--jobs auto` formula.
//
// Asserted against SYNTHETIC capacities, not the machine running the test: a
// test that asked the host how many cores it has would assert nothing (it would
// just restate the answer) and would produce a different verdict on every CI
// runner.
#include <gtest/gtest.h>

import mcpp.platform.capacity;

using mcpp::platform::capacity::HostCapacity;
using mcpp::platform::capacity::recommended_jobs;

namespace {
constexpr unsigned long long GiB = 1024ull * 1024 * 1024;
}

// A homogeneous machine with ample RAM should use its logical CPUs: SMT siblings
// still contribute, just less than a full core.
TEST(RecommendedJobs, HomogeneousAndAmpleMemoryUsesLogicalCores) {
    HostCapacity cap{.logicalCores = 16, .physicalCores = 8, .heterogeneous = false,
                     .totalBytes = 64 * GiB, .availableBytes = 60 * GiB};
    EXPECT_EQ(recommended_jobs(cap), 16);
}

// The case that motivated the whole function: 32 "cores" on a 13900K are 8
// P-cores + 16 E-cores, and treating them as 32 equal workers overestimates
// usable parallelism by more than 2x.
TEST(RecommendedJobs, HeterogeneousFallsBackToPhysicalCores) {
    HostCapacity cap{.logicalCores = 32, .physicalCores = 24, .heterogeneous = true,
                     .totalBytes = 64 * GiB, .availableBytes = 60 * GiB};
    EXPECT_EQ(recommended_jobs(cap), 24);
}

// The dangerous shape: many cores, little RAM. ninja's default here would be 66
// concurrent compiles against ~0.75 GB each and the machine would swap.
TEST(RecommendedJobs, MemoryBoundMachineIsCappedByRam) {
    HostCapacity cap{.logicalCores = 64, .physicalCores = 64, .heterogeneous = false,
                     .totalBytes = 32 * GiB, .availableBytes = 32 * GiB};
    // (32 - 2) / 0.75 = 40 → below the 64 CPUs, so memory decides.
    EXPECT_EQ(recommended_jobs(cap), 40);
    EXPECT_LT(recommended_jobs(cap), cap.logicalCores);
}

// Must still make progress rather than refusing to build.
TEST(RecommendedJobs, TinyMemoryStillYieldsOneJob) {
    HostCapacity cap{.logicalCores = 8, .physicalCores = 8, .heterogeneous = false,
                     .totalBytes = 2 * GiB, .availableBytes = 1 * GiB};
    EXPECT_EQ(recommended_jobs(cap), 1);
}

TEST(RecommendedJobs, NeverExceedsTheCeiling) {
    HostCapacity cap{.logicalCores = 256, .physicalCores = 256, .heterogeneous = false,
                     .totalBytes = 1024 * GiB, .availableBytes = 1000 * GiB};
    EXPECT_EQ(recommended_jobs(cap), 64);
    EXPECT_EQ(recommended_jobs(cap, 768ull * 1024 * 1024, 2 * GiB, /*ceiling=*/8), 8);
}

TEST(RecommendedJobs, NeverReturnsZero) {
    HostCapacity cap{.logicalCores = 0, .physicalCores = 0, .heterogeneous = false,
                     .totalBytes = 0, .availableBytes = 0};
    EXPECT_GE(recommended_jobs(cap), 1);
}

// A project with heavier translation units can say so without patching mcpp.
TEST(RecommendedJobs, PerJobEstimateIsAParameter) {
    HostCapacity cap{.logicalCores = 32, .physicalCores = 32, .heterogeneous = false,
                     .totalBytes = 32 * GiB, .availableBytes = 32 * GiB};
    const int light = recommended_jobs(cap, 256ull * 1024 * 1024);
    const int heavy = recommended_jobs(cap, 4ull * 1024 * 1024 * 1024);
    EXPECT_GT(light, heavy);
    EXPECT_LE(heavy, 8);
}

// The real machine must at least produce something sane — a weak assertion on
// purpose, since the value is host-dependent.
TEST(RecommendedJobs, HostQueryIsPlausible) {
    const auto cap = mcpp::platform::capacity::host_capacity();
    EXPECT_GE(cap.logicalCores, 1);
    EXPECT_GE(cap.physicalCores, 1);
    EXPECT_LE(cap.physicalCores, cap.logicalCores);
    EXPECT_GE(recommended_jobs(cap), 1);
}

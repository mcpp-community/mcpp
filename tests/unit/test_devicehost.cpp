#include <gtest/gtest.h>

import std;
import mcpp.toolchain.devicehost;

using mcpp::toolchain::parse_host_config;
using mcpp::toolchain::host_compiler_accepted;

namespace {
// The two guards as CUDA 12.0 writes them, reduced to what the parse reads.
constexpr std::string_view kCuda120 = R"(
#if __GNUC__ > 12
#error -- unsupported GNU version! gcc versions later than 12 are not supported! The nvcc flag '-allow-unsupported-compiler' can be used to override this version check
#endif /* __GNUC__ > 12 */
#if defined(__clang__)
#error -- unsupported clang version! clang version must be less than 15 and greater than 3.2 .
#endif
)";
} // namespace

TEST(DeviceHost, ReadsBothGuardsOutOfTheVendorHeader) {
    auto b = parse_host_config(kCuda120);
    EXPECT_TRUE(b.known());
    EXPECT_EQ(b.gccMax,   12);
    EXPECT_EQ(b.clangMax, 14);   // "less than 15" is an exclusive bound
}

TEST(DeviceHost, AcceptsWithinTheBoundAndRefusesAbove) {
    auto b = parse_host_config(kCuda120);
    EXPECT_TRUE (host_compiler_accepted(b, "gcc",   12));
    EXPECT_FALSE(host_compiler_accepted(b, "gcc",   13));
    EXPECT_FALSE(host_compiler_accepted(b, "gcc",   16));   // mcpp's own payload
    EXPECT_TRUE (host_compiler_accepted(b, "clang", 14));
    EXPECT_FALSE(host_compiler_accepted(b, "clang", 18));
    EXPECT_TRUE (host_compiler_accepted(b, "llvm",  14));   // mcpp's family name
}

TEST(DeviceHost, AnUnreadableHeaderMakesNoClaim) {
    // A refusal invented from a file the parse did not understand would be
    // worse than the failure it prevents: the user cannot act on it.
    auto b = parse_host_config("nothing to see here");
    EXPECT_FALSE(b.known());
    EXPECT_TRUE(host_compiler_accepted(b, "gcc",   99));
    EXPECT_TRUE(host_compiler_accepted(b, "clang", 99));
}

TEST(DeviceHost, SilenceAboutOneFamilyIsNotARefusalOfIt) {
    auto b = parse_host_config("#if __GNUC__ > 11\n#error nope\n#endif\n");
    EXPECT_EQ(b.gccMax, 11);
    EXPECT_EQ(b.clangMax, 0);
    EXPECT_FALSE(host_compiler_accepted(b, "gcc",   12));
    EXPECT_TRUE (host_compiler_accepted(b, "clang", 20));
}

TEST(DeviceHost, AnUnknownVersionMakesNoClaimEither) {
    auto b = parse_host_config(kCuda120);
    EXPECT_TRUE(host_compiler_accepted(b, "gcc", 0));
}

// ── The plan nvcc states, and the one thing that goes missing from it ──────
//
// Both fixtures are real `nvcc --dryrun` output, CUDA 12.0, with the
// temporary paths shortened. They differ in exactly one line: the working
// host states a PATH, and the sandbox -- whose /etc is replaced, so the
// `nvcc.profile` symlinked into it is gone -- states none. Every other line,
// including the stages nvcc will invoke, is identical.

using mcpp::toolchain::parse_dryrun;

namespace {

constexpr std::string_view kPlanWithProfile = R"(#$ _NVVM_BRANCH_=nvvm
#$ _SPACE_= 
#$ _HERE_=/usr/lib/nvidia-cuda-toolkit/bin
#$ _TARGET_SIZE_=64
#$ NVVMIR_LIBRARY_DIR=/usr/lib/nvidia-cuda-toolkit/libdevice
#$ PATH=/usr/lib/nvidia-cuda-toolkit/bin:/usr/local/bin:/usr/bin:/bin
#$ LIBRARIES=  -L/usr/lib/x86_64-linux-gnu/stubs
#$ gcc -D__CUDA_ARCH_LIST__=520 -E -x c++ -m64 "/tmp/X" -o "/tmp/X" 
#$ cudafe++ --c++17 --gnu_version=130300 --m64 "/tmp/X" 
#$ cicc --c++17 -arch compute_52 -m64 "/tmp/X" -o "/tmp/X"
#$ ptxas -arch=sm_52 -m64 "/tmp/X"  -o "/tmp/X" 
#$ fatbinary -64 --cicc-cmdline="-ftz=0 " "--image3=kind=elf,sm=52,file=/tmp/X" 
#$ rm /tmp/X
)";

// The same run with the PATH assignment removed: what nvcc emits when it
// cannot read its own profile.
constexpr std::string_view kPlanWithoutProfile = R"(#$ _NVVM_BRANCH_=nvvm
#$ _SPACE_= 
#$ _HERE_=/usr/lib/nvidia-cuda-toolkit/bin
#$ _TARGET_SIZE_=64
#$ LIBRARIES=  -L/usr/lib/x86_64-linux-gnu/stubs
#$ gcc -D__CUDA_ARCH_LIST__=520 -E -x c++ -m64 "/tmp/X" -o "/tmp/X" 
#$ cudafe++ --c++17 --gnu_version=130300 --m64 "/tmp/X" 
#$ cicc --c++17 -arch compute_52 -m64 "/tmp/X" -o "/tmp/X"
#$ ptxas -arch=sm_52 -m64 "/tmp/X"  -o "/tmp/X" 
#$ fatbinary -64 --cicc-cmdline="-ftz=0 " "--image3=kind=elf,sm=52,file=/tmp/X" 
#$ rm /tmp/X
)";

} // namespace

TEST(DeviceDryRun, CollectsTheStagesAndThePathNvccStates) {
    auto plan = parse_dryrun(kPlanWithProfile);
    EXPECT_EQ(plan.searchPath,
              "/usr/lib/nvidia-cuda-toolkit/bin:/usr/local/bin:/usr/bin:/bin");
    EXPECT_EQ(plan.programs,
              (std::vector<std::string>{"gcc", "cudafe++", "cicc", "ptxas",
                                        "fatbinary", "rm"}));
}

TEST(DeviceDryRun, TheMissingProfileShowsUpAsAnAbsentPathAssignment) {
    // This is the whole of the difference the check keys on. The stages are
    // the same; only the path they will be resolved against is gone.
    auto broken = parse_dryrun(kPlanWithoutProfile);
    auto intact = parse_dryrun(kPlanWithProfile);
    EXPECT_TRUE(broken.searchPath.empty());
    EXPECT_EQ(broken.programs, intact.programs);
}

TEST(DeviceDryRun, AssignmentsAreNotMistakenForStages) {
    auto plan = parse_dryrun(kPlanWithProfile);
    // `LIBRARIES=  -L...` and `_SPACE_= ` both parse as assignments, and
    // neither names a program. Reading either as a stage would report a
    // missing tool that nvcc never intended to run.
    for (auto const& p : plan.programs) {
        EXPECT_EQ(p.find('='), std::string::npos);
        EXPECT_NE(p, "LIBRARIES");
        EXPECT_NE(p, "_SPACE_");
    }
}

TEST(DeviceDryRun, StagesNamedByPathAreLeftAlone) {
    // A stage nvcc spells out resolves without the search path, so it is not
    // a candidate for "cannot be found on PATH".
    auto plan = parse_dryrun("#$ PATH=/bin\n"
                             "#$ /opt/cuda/bin/cicc --c++17\n"
                             "#$ ptxas -arch=sm_52\n");
    EXPECT_EQ(plan.programs, (std::vector<std::string>{"ptxas"}));
}

TEST(DeviceDryRun, TextThatIsNotAPlanYieldsNoStages) {
    // A spawn that failed because there is no nvcc lands here. No stages
    // means no finding: the probe reached no answer and invents none.
    auto plan = parse_dryrun("mcpp: failed to spawn 'nvcc': No such file\n");
    EXPECT_TRUE(plan.programs.empty());
    EXPECT_TRUE(plan.searchPath.empty());
}

// ── The driver a device runtime will meet ──────────────────────────────────
//
// Measured 2026-09-05 on a host whose driver reports CUDA 12.4: a binary built
// with the 13.3 payload compiles and links cleanly and then fails at the first
// allocation; the same source built with the 12.9 payload prints the right
// answer. These assert the relation that turns that into a message before
// anything is compiled.

using mcpp::toolchain::parse_device_version;
using mcpp::toolchain::driver_accepts_toolkit;

TEST(DeviceDriver, ReadsAVersionOutOfSurroundingText) {
    // The two real shapes: `nvcc --version` ends with "release 12.9, V12.9.86",
    // and `nvidia-smi`'s header carries "CUDA Version: 12.4".
    auto a = parse_device_version("12.9, V12.9.86");
    EXPECT_EQ(a.major, 12);
    EXPECT_EQ(a.minor, 9);
    auto b = parse_device_version(" 12.4     |");
    EXPECT_EQ(b.major, 12);
    EXPECT_EQ(b.minor, 4);
}

TEST(DeviceDriver, MinorVersionCompatibilityHolds) {
    // Within one major, any minor runs. This is the vendor's rule, and it is
    // why the 12.9 payload works against a driver that serves 12.4 -- the case
    // a naive "toolkit must be <= driver" check would have refused.
    EXPECT_TRUE(driver_accepts_toolkit(parse_device_version("12.9"),
                                       parse_device_version("12.4")));
    EXPECT_TRUE(driver_accepts_toolkit(parse_device_version("12.0"),
                                       parse_device_version("12.4")));
}

TEST(DeviceDriver, ANewerMajorIsRefused) {
    EXPECT_FALSE(driver_accepts_toolkit(parse_device_version("13.3"),
                                        parse_device_version("12.4")));
}

TEST(DeviceDriver, AnOlderMajorIsAccepted) {
    EXPECT_TRUE(driver_accepts_toolkit(parse_device_version("11.8"),
                                       parse_device_version("12.4")));
}

TEST(DeviceDriver, EitherSideUnknownMakesNoClaim) {
    // The same rule the host-compiler bound follows: a check that cannot reach
    // an answer must not manufacture a refusal. A machine with no driver, or a
    // toolkit whose version could not be read, is not a machine with a defect.
    EXPECT_TRUE(driver_accepts_toolkit(parse_device_version("13.3"),
                                       parse_device_version("no gpu here")));
    EXPECT_TRUE(driver_accepts_toolkit(parse_device_version(""),
                                       parse_device_version("12.4")));
}

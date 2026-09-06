// The island, written against the HIP API. Nothing here is visible to the
// module graph: a device translation unit is never scanned and never produces
// a BMI.
//
// On the NVIDIA platform every name below is an inline wrapper over the CUDA
// one -- `hipMalloc` resolves to `cudaMalloc` through the header -- so this
// file and its CUDA sibling in tests/cuda-consumer compile to the same device
// code through different spellings. That is the point of the fixture: the
// engine path, the rule and the seam are the same, and only the API differs.
//
// It uses no C++ standard library. That is a deliberate property rather than
// an accident of a small example: an island that pulls in libstdc++ links a
// second copy of the C++ runtime into a program whose own copy came from
// mcpp's toolchain, which is the failure where one is linked and the other is
// loaded.
#include "saxpy/saxpy.h"
#include <hip/hip_runtime.h>
#include <cstdio>

namespace {

__global__ void saxpy_kernel(float a, const float* x, const float* y,
                             float* out, unsigned n) {
    unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) out[i] = a * x[i] + y[i];
}

// THE NAME OF THE DEVICE, RECORDED BY THE RUN.
//
// Set only after a successful call, and read through the seam. The CPU
// fallback produces the same four numbers as this island does, so without a
// name printed by the program nothing distinguishes a device run from a silent
// fallback -- which is the one confusion an example about heterogeneous
// compute must not leave in place.
char g_ran_on[256] = "";

} // namespace

extern "C" const char* saxpy_device_name(void) { return g_ran_on; }

extern "C" int saxpy_device(float a, const float* x, const float* y,
                            float* out, unsigned n) {
    float *dx = nullptr, *dy = nullptr, *dout = nullptr;
    const size_t bytes = static_cast<size_t>(n) * sizeof(float);
    int rc = -1;

    if (hipError_t e = hipMalloc(&dx, bytes); e != hipSuccess) {
        std::fprintf(stderr, "hipMalloc: %s\n", hipGetErrorString(e));
        goto done;
    }
    if (hipMalloc(&dy,   bytes) != hipSuccess) goto done;
    if (hipMalloc(&dout, bytes) != hipSuccess) goto done;
    if (hipMemcpy(dx, x, bytes, hipMemcpyHostToDevice) != hipSuccess) goto done;
    if (hipMemcpy(dy, y, bytes, hipMemcpyHostToDevice) != hipSuccess) goto done;

    hipLaunchKernelGGL(saxpy_kernel, dim3((n + 255) / 256), dim3(256), 0, 0,
                       a, dx, dy, dout, n);
    // The launch is asynchronous, so its own return value reports only whether
    // the launch was accepted. A kernel compiled for an architecture this
    // device does not have fails HERE, with `no kernel image is available for
    // execution on the device` — which is the runtime failure the accelerator
    // dimension of an artifact's identity exists to turn into a build-time one.
    if (hipError_t e = hipGetLastError(); e != hipSuccess) {
        std::fprintf(stderr, "launch: %s\n", hipGetErrorString(e));
        goto done;
    }
    if (hipDeviceSynchronize() != hipSuccess) goto done;
    if (hipMemcpy(out, dout, bytes, hipMemcpyDeviceToHost) != hipSuccess) goto done;
    rc = 0;

done:
    hipFree(dx); hipFree(dy); hipFree(dout);
    if (rc == 0) {
        int device = 0;
        hipDeviceProp_t properties{};
        if (hipGetDevice(&device) == hipSuccess
            && hipGetDeviceProperties(&properties, device) == hipSuccess)
            std::snprintf(g_ran_on, sizeof g_ran_on, "%s", properties.name);
    }
    return rc;
}

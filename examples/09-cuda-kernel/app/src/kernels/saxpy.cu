// The island. Nothing here is visible to the module graph: nvcc does not
// accept C++20 modules, so this translation unit is never scanned and never
// produces a BMI.
//
// It uses no C++ standard library. That is a deliberate property rather than
// an accident of a small example: an island that pulls in libstdc++ links a
// second copy of the C++ runtime into a program whose own copy came from
// mcpp's toolchain, which is the failure where one is linked and the other is
// loaded.
#include "saxpy/saxpy.h"
#include <cuda_runtime.h>
#include <cstdio>

namespace {

__global__ void saxpy_kernel(float a, const float* x, const float* y,
                             float* out, unsigned n) {
    unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) out[i] = a * x[i] + y[i];
}

} // namespace

extern "C" int saxpy_device(float a, const float* x, const float* y,
                            float* out, unsigned n) {
    float *dx = nullptr, *dy = nullptr, *dout = nullptr;
    const size_t bytes = static_cast<size_t>(n) * sizeof(float);
    int rc = -1;

    if (cudaError_t e = cudaMalloc(&dx, bytes); e != cudaSuccess) {
        std::fprintf(stderr, "cudaMalloc: %s\n", cudaGetErrorString(e));
        goto done;
    }
    if (cudaMalloc(&dy,   bytes) != cudaSuccess) goto done;
    if (cudaMalloc(&dout, bytes) != cudaSuccess) goto done;
    if (cudaMemcpy(dx, x, bytes, cudaMemcpyHostToDevice) != cudaSuccess) goto done;
    if (cudaMemcpy(dy, y, bytes, cudaMemcpyHostToDevice) != cudaSuccess) goto done;

    saxpy_kernel<<<(n + 255) / 256, 256>>>(a, dx, dy, dout, n);
    // The launch is asynchronous, so its own return value reports only whether
    // the launch was accepted. A kernel compiled for an architecture this
    // device does not have fails HERE, with `no kernel image is available for
    // execution on the device` — which is the runtime failure the accelerator
    // dimension of an artifact's identity exists to turn into a build-time one.
    if (cudaError_t e = cudaGetLastError(); e != cudaSuccess) {
        std::fprintf(stderr, "launch: %s\n", cudaGetErrorString(e));
        goto done;
    }
    if (cudaDeviceSynchronize() != cudaSuccess) goto done;
    if (cudaMemcpy(out, dout, bytes, cudaMemcpyDeviceToHost) != cudaSuccess) goto done;
    rc = 0;

done:
    cudaFree(dx); cudaFree(dy); cudaFree(dout);
    return rc;
}

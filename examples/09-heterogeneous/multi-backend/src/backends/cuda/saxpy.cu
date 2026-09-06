// The CUDA island. Compiled only when the build names `cuda`, by the compiler
// mcpp.rules.cuda resolves -- never by mcpp's own.
#include <cuda_runtime.h>

__global__ void opkit_saxpy_kernel(float a, const float* x, const float* y,
                                   float* out, unsigned n) {
    unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) out[i] = a * x[i] + y[i];
}

extern "C" int opkit_cuda_saxpy(float a, const float* x, const float* y,
                                float* out, unsigned n) {
    // A backend that is compiled in may still find no device. Returning
    // non-zero is how it declines, and the dispatcher moves to the next one --
    // the reason this is a chain rather than a build-time choice.
    int count = 0;
    if (cudaGetDeviceCount(&count) != cudaSuccess || count == 0) return 1;

    float *dx = nullptr, *dy = nullptr, *dout = nullptr;
    const size_t bytes = size_t(n) * sizeof(float);
    if (cudaMalloc(&dx, bytes)   != cudaSuccess) return 1;
    if (cudaMalloc(&dy, bytes)   != cudaSuccess) { cudaFree(dx); return 1; }
    if (cudaMalloc(&dout, bytes) != cudaSuccess) { cudaFree(dx); cudaFree(dy); return 1; }

    cudaMemcpy(dx, x, bytes, cudaMemcpyHostToDevice);
    cudaMemcpy(dy, y, bytes, cudaMemcpyHostToDevice);
    opkit_saxpy_kernel<<<(n + 255) / 256, 256>>>(a, dx, dy, dout, n);
    const bool ok = cudaDeviceSynchronize() == cudaSuccess;
    if (ok) cudaMemcpy(out, dout, bytes, cudaMemcpyDeviceToHost);

    cudaFree(dx); cudaFree(dy); cudaFree(dout);
    return ok ? 0 : 1;
}

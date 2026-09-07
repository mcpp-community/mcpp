// opkit -- one operator, several device backends, one artifact.
//
// `extern "C"` and free of standard-library types, for the reason the other
// examples in this directory give: a device island is compiled by a compiler
// mcpp did not choose, so the two sides share no C++ ABI.
#ifndef MCPP_EXAMPLE_OPKIT_H
#define MCPP_EXAMPLE_OPKIT_H

#ifdef __cplusplus
extern "C" {
#endif

// out[i] = a * x[i] + y[i]. Returns 0 on success.
//
// WHICH backend answers is decided at RUN time among those this build
// compiled in, which is what makes this an operator library rather than four
// separate programs.
int opkit_saxpy(float a, const float* x, const float* y, float* out, unsigned n);

// The backend that served the last successful call, or "" before one.
//
// An operator library that computes and does not say where cannot be checked:
// every backend returns the same numbers, so the numbers alone do not
// distinguish a device run from the reference one.
const char* opkit_backend(void);

// Each backend supplies these two. A backend that is not compiled in is not
// declared, so the dispatcher's list is decided at compile time.
int opkit_cpu_saxpy(float, const float*, const float*, float*, unsigned);
#ifdef OPKIT_HAVE_CUDA
int opkit_cuda_saxpy(float, const float*, const float*, float*, unsigned);
#endif
#ifdef OPKIT_HAVE_VULKAN
int opkit_vulkan_saxpy(float, const float*, const float*, float*, unsigned);
// The device the Vulkan backend last ran on. A Vulkan build may find a
// discrete GPU, an integrated one or a CPU rasteriser, and which of those
// answered is not derivable from the numbers -- they are the same numbers.
const char* opkit_vulkan_device_name(void);
#endif

#ifdef __cplusplus
}
#endif
#endif

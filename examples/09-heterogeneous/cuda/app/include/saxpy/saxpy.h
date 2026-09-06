// The device island's interface.
//
// `extern "C"` and free of standard-library types, on purpose. The island is
// compiled by nvcc driving a host compiler that mcpp did not choose, so the two
// sides do not share a C++ ABI and must not exchange anything that depends on
// one. Keeping the boundary this narrow is also what lets the island publish a
// C-surface compatibility tag.
#ifndef MCPP_EXAMPLE_SAXPY_H
#define MCPP_EXAMPLE_SAXPY_H

#ifdef __cplusplus
extern "C" {
#endif

// out[i] = a * x[i] + y[i], computed on the device. Returns 0 on success.
int saxpy_device(float a, const float* x, const float* y, float* out, unsigned n);

// WHICH DEVICE THE LAST SUCCESSFUL `saxpy_device` RAN ON, or "" if none has.
//
// A program that computes on a device and does not say where it computed
// cannot be checked. Both implementations of this seam produce the same four
// numbers, so the numbers alone do not distinguish a device run from the CPU
// fallback -- which is exactly the confusion an example about heterogeneous
// compute must not teach. Reading it before a successful call returns "",
// because a device run that did not happen has no device to name.
const char* saxpy_device_name(void);

#ifdef __cplusplus
}
#endif
#endif

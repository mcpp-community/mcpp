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

#ifdef __cplusplus
}
#endif
#endif

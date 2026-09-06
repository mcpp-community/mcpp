// The Ascend island's interface.
//
// `extern "C"` and free of standard-library types, for the reason every island
// in this directory gives: the device half is compiled by BiSheng, a compiler
// mcpp did not choose, so the two sides share no C++ ABI.
#ifndef MCPP_EXAMPLE_ASCEND_SAXPY_H
#define MCPP_EXAMPLE_ASCEND_SAXPY_H

#ifdef __cplusplus
extern "C" {
#endif

// out[i] = a * x[i] + y[i], computed on the NPU. Returns 0 on success.
int saxpy_device(float a, const float* x, const float* y, float* out, unsigned n);

// Which device the last successful call ran on, or "" if none has. Both
// implementations of this seam produce the same numbers, so the numbers alone
// do not separate a device run from the fallback.
const char* saxpy_device_name(void);

#ifdef __cplusplus
}
#endif
#endif

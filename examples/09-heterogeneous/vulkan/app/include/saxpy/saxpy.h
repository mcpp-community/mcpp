#pragma once

// The device interface, in the one shape every device API agrees on: raw
// pointers and a count. The seam module above it turns that back into C++.
//
// Exactly one definition of this symbol is linked: the Vulkan island when the
// build names an accelerator, the CPU file when it does not.
#ifdef __cplusplus
extern "C" {
#endif

int saxpy_device(float a, const float* x, const float* y, float* out, unsigned n);

#ifdef __cplusplus
}
#endif

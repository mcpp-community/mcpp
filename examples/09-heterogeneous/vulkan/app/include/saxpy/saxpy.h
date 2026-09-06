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

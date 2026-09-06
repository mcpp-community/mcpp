// The CPU implementation behind the same seam. Compiled only when the build
// asks for no accelerator (`mcpp build --no-accel`), through the
// `cfg(not(accelerator = "sycl"))` section of the manifest; the device island
// and this file define the same symbol and are never in one link.
#include "saxpy/saxpy.h"

extern "C" int saxpy_device(float a, const float* x, const float* y,
                            float* out, unsigned n) {
    for (unsigned i = 0; i < n; ++i) out[i] = a * x[i] + y[i];
    return 0;
}

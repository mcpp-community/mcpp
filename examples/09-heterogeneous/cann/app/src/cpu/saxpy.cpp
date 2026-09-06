#include "saxpy/saxpy.h"

// The CPU variant of the same seam, selected by `cfg(accelerator = "none")`.
// This file and the Ascend host half define the same symbols and are never in
// one link.
extern "C" int saxpy_device(float a, const float* x, const float* y,
                            float* out, unsigned n) {
    for (unsigned i = 0; i < n; ++i) out[i] = a * x[i] + y[i];
    return 0;
}
extern "C" const char* saxpy_device_name(void) {
    return "cpu (this build names no accelerator)";
}

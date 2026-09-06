// The CPU implementation behind the same seam. Compiled only when the build
// asks for no accelerator (`mcpp build --no-accel`), through the
// `cfg(not(accelerator = "sycl"))` section of the manifest; the device island
// and this file define the same symbol and are never in one link.
#include "saxpy/saxpy.h"

namespace {
// Set by the call, read by the name. Empty until then, so `--no-accel` and a
// device build answer the same question the same way.
const char* g_ran_on = "";
} // namespace

extern "C" int saxpy_device(float a, const float* x, const float* y,
                            float* out, unsigned n) {
    for (unsigned i = 0; i < n; ++i) out[i] = a * x[i] + y[i];
    g_ran_on = "cpu (this build names no accelerator)";
    return 0;
}

extern "C" const char* saxpy_device_name(void) { return g_ran_on; }

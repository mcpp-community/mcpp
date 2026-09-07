// The CPU implementation behind the same seam. Compiled only when the build
// asks for no accelerator (`mcpp build --no-accel`), through the
// `cfg(not(accelerator = "cuda"))` section of the manifest; the device island
// and this file define the same symbol and are never in one link.
//
// THIS FILE INCLUDES THE GENERATED BOUNDARY AND THE ISLAND DOES NOT, and the
// asymmetry is not an oversight. The island is compiled by a driver mcpp did
// not invoke, so its rule can be handed forced-include flags for that one
// command line. This half is ordinary project C++ compiled by mcpp, and the
// only project-wide channel available would force the header into every C++
// translation unit -- including `src/app.cppm`, where declarations ahead of
// `export module` are ill-formed. One `#include` of a generated header in an
// ordinary source is the smaller thing.
//
// The header is generated from the marked declarations in BOTH files, so the
// signature below is still written once, and a signature here that disagreed
// with the island's is refused while the boundary is being generated.
#include "app.kernels.h"

namespace {
// Set by the call, read by the name. Empty until then, so `--no-accel` and a
// device build answer the same question the same way.
const char* g_ran_on = "";
} // namespace

MCPP_EXPORT_C
int saxpy_device(float a, const float* x, const float* y,
                 float* out, unsigned n) {
    for (unsigned i = 0; i < n; ++i) out[i] = a * x[i] + y[i];
    g_ran_on = "cpu (this build names no accelerator)";
    return 0;
}

MCPP_EXPORT_C
const char* saxpy_device_name(void) { return g_ran_on; }

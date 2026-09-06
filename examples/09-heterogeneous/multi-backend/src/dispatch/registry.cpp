#include "opkit/opkit.h"

// THE DISPATCHER, built only when this build names at least one device
// backend -- `cfg(not(accelerator = "none"))`.
//
// The predicate needs no enumeration of the backends, which is the whole point:
// this file must be built for cuda, for vulkan, for both, and for a backend
// that does not exist yet. Written as `not(any(accelerator = "cuda",
// accelerator = "vulkan"))` it would have to be edited every time the ecosystem
// gains a backend, and the edit that is forgotten is silent.
static const char* g_backend = "";

extern "C" const char* opkit_backend(void) { return g_backend; }

extern "C" int opkit_saxpy(float a, const float* x, const float* y,
                           float* out, unsigned n) {
    // Device backends first, in declaration order; the reference last. A
    // device that is compiled in may still be absent at run time, which is why
    // this is a fallback chain rather than a single choice made at build time.
#ifdef OPKIT_HAVE_CUDA
    if (opkit_cuda_saxpy(a, x, y, out, n) == 0)   { g_backend = "cuda";   return 0; }
#endif
#ifdef OPKIT_HAVE_VULKAN
    if (opkit_vulkan_saxpy(a, x, y, out, n) == 0) { g_backend = "vulkan"; return 0; }
#endif
    if (opkit_cpu_saxpy(a, x, y, out, n) == 0)    { g_backend = "cpu (fallback)"; return 0; }
    return 1;
}

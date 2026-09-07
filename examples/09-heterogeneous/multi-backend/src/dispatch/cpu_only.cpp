#include "opkit/opkit.h"

// The CPU-ONLY build, `cfg(accelerator = "none")`.
//
// `mcpp build` with no accelerator named produces this: no dispatch chain, no
// registry, a direct call. That is a real difference rather than a cosmetic
// one -- a consumer who wants the operator and no device machinery gets an
// artifact that contains none of it.
//
// The predicate is the reason this file can exist at all. `accelerator` is an
// OPEN vocabulary, so "this build names no backend" cannot be said by listing
// the backends it is not; `none` says it directly and keeps saying it after the
// ecosystem gains a fifth.
static const char* g_backend = "";

extern "C" const char* opkit_backend(void) { return g_backend; }

extern "C" int opkit_saxpy(float a, const float* x, const float* y,
                           float* out, unsigned n) {
    int rc = opkit_cpu_saxpy(a, x, y, out, n);
    if (rc == 0) g_backend = "cpu (only backend in this build)";
    return rc;
}

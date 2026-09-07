#include "opkit/opkit.h"

// The reference implementation, ALWAYS built.
//
// This is the difference between an operator library and the single-seam
// examples beside it: there a CPU file and a device file define the SAME
// symbol and are never in one link, so exactly one exists. Here the reference
// is a backend like any other and every build has it, which is what makes a
// runtime fallback possible on a machine whose device turns out to be absent.
extern "C" int opkit_cpu_saxpy(float a, const float* x, const float* y,
                               float* out, unsigned n) {
    for (unsigned i = 0; i < n; ++i) out[i] = a * x[i] + y[i];
    return 0;
}

#include "opkit/opkit.h"

// The Vulkan backend's host half. Kept deliberately small: what this example
// demonstrates is the BUILD shape -- which sources reach which compiler, and
// how several backends land in one artifact -- not a Vulkan tutorial. The
// sibling `examples/09-heterogeneous/vulkan` carries the full dispatch.
//
// It declines when no usable device is present, which is the contract every
// backend in the chain follows.
extern "C" int opkit_vulkan_saxpy(float a, const float* x, const float* y,
                                  float* out, unsigned n) {
    (void)a; (void)x; (void)y; (void)out; (void)n;
    return 1;   // declines here; see examples/09-heterogeneous/vulkan
}

// The seam, in the shape the `cuda` example uses, for the same reason:
// it is the one place a backend can be exchanged. Underneath is a Vulkan
// compute island on one build and a plain loop on another, and no importer of
// this module can tell.
module;
#include "saxpy/saxpy.h"
export module app.saxpy;
import std;

export namespace app {

std::optional<std::vector<float>>
saxpy(float a, std::span<const float> x, std::span<const float> y) {
    if (x.size() != y.size()) return std::nullopt;
    std::vector<float> out(x.size());
    if (saxpy_device(a, x.data(), y.data(), out.data(),
                     static_cast<unsigned>(x.size())) != 0)
        return std::nullopt;
    return out;
}

} // namespace app

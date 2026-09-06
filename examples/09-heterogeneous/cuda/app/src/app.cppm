// The seam.
//
// Its reason for existing is not that nvcc rejects modules. It is that this is
// the one place a backend can be exchanged: the island underneath can become
// HIP or a CPU fallback without a single consumer of this module changing, and
// a `cfg(accelerator = ...)` section has somewhere to apply. Remove the seam
// and every importer becomes backend-specific.
module;
#include "saxpy/saxpy.h"
export module app.saxpy;
import std;

export namespace app {

// The device interface is raw pointers and a count because it has to be. The
// seam is where that becomes a C++ interface again.
std::optional<std::vector<float>>
saxpy(float a, std::span<const float> x, std::span<const float> y) {
    if (x.size() != y.size()) return std::nullopt;
    std::vector<float> out(x.size());
    if (saxpy_device(a, x.data(), y.data(), out.data(),
                     static_cast<unsigned>(x.size())) != 0)
        return std::nullopt;
    return out;
}

// The seam answers this too, because the seam is the only place that knows
// which island was linked.
std::string_view device_name() { return saxpy_device_name(); }

} // namespace app

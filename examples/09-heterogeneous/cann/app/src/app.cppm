// The seam, as a module.
//
// Its reason for existing is not that BiSheng rejects modules. It is that this
// is the one place a backend can be exchanged: the island underneath can become
// CUDA or a CPU fallback without a single importer changing.
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

std::string_view device_name() { return saxpy_device_name(); }

} // namespace app

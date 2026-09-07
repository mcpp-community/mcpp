// The seam. Underneath is a Vulkan graphics pipeline on one build and a
// software rasteriser on another, and no importer of this module can tell.
//
// The same shape `examples/09-heterogeneous/vulkan` uses for compute. What
// differs is only what the device does: there the device answered with numbers,
// here it answers with an image.
module;
#include "render/render.h"
export module app.render;
import std;

export namespace app {

struct image {
    unsigned w = 0, h = 0;
    std::vector<unsigned char> rgba;   // w * h * 4

    std::array<unsigned char, 4> at(unsigned x, unsigned y) const {
        const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
        return { rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3] };
    }
};

std::optional<image> render(unsigned w, unsigned h) {
    image out{ w, h, std::vector<unsigned char>(static_cast<std::size_t>(w) * h * 4) };
    if (render_offscreen(w, h, out.rgba.data()) != 0) return std::nullopt;
    return out;
}

std::array<unsigned char, 4> clear_color() {
    std::array<unsigned char, 4> c{};
    render_clear_color(c.data());
    return c;
}

// Answered by the seam, because the seam is the only place that knows which
// implementation was linked.
std::string_view device_name() { return render_device_name(); }

} // namespace app

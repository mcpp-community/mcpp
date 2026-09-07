// The software rasteriser behind the same seam, compiled when the build names
// no accelerator (`mcpp build --no-accel`).
//
// It draws the SAME triangle the shaders draw, which is the point: the pixel
// assertions in `main.cpp` are the contract, and this file exists to show the
// contract is satisfiable without a GPU. What distinguishes the two builds is
// `render_device_name`, never the image.
#include "render/render.h"

// `<cstddef>` FOR `std::size_t`, AND IT IS NOT PEDANTRY.
//
// A standard header is entitled to bring in whichever others it needs, and
// which ones it brings differs between implementations. This file compiled
// against libstdc++ and then failed against libc++ on the same machine:
//
//   src/cpu/render.cpp:56:52: error: no type named 'size_t' in namespace 'std'
//
// A translation unit that names a type has to include the header that declares
// it, whatever the last implementation happened to hand it for free.
#include <cstddef>
#include <cmath>

namespace {

const char* g_ran_on = "";

// Clipspace positions and colours, byte for byte what `shaders/triangle.vert`
// declares. Two copies of one fact, and the assertion that keeps them honest is
// that both legs must satisfy the same pixel test.
constexpr float kPos[3][2] = { { 0.0f, -0.5f }, { 0.5f, 0.5f }, { -0.5f, 0.5f } };
constexpr float kCol[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };

constexpr unsigned char kClear[4] = { 16, 16, 16, 255 };

float edge(const float a[2], const float b[2], float px, float py) {
    return (px - a[0]) * (b[1] - a[1]) - (py - a[1]) * (b[0] - a[0]);
}

unsigned char quantise(float v) {
    const float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return static_cast<unsigned char>(c * 255.0f + 0.5f);
}

} // namespace

extern "C" void render_clear_color(unsigned char* rgba4) {
    for (int i = 0; i < 4; ++i) rgba4[i] = kClear[i];
}

extern "C" int render_offscreen(unsigned w, unsigned h, unsigned char* rgba) {
    if (w == 0 || h == 0 || rgba == nullptr) return 1;

    const float area = edge(kPos[0], kPos[1], kPos[2][0], kPos[2][1]);
    if (area == 0.0f) return 1;

    for (unsigned y = 0; y < h; ++y) {
        for (unsigned x = 0; x < w; ++x) {
            // Vulkan's framebuffer origin is top-left and its clip space has
            // +Y down, so the pixel centre maps straight through.
            const float px = 2.0f * (static_cast<float>(x) + 0.5f) / static_cast<float>(w) - 1.0f;
            const float py = 2.0f * (static_cast<float>(y) + 0.5f) / static_cast<float>(h) - 1.0f;

            const float w0 = edge(kPos[1], kPos[2], px, py) / area;
            const float w1 = edge(kPos[2], kPos[0], px, py) / area;
            const float w2 = edge(kPos[0], kPos[1], px, py) / area;

            unsigned char* p = rgba + (static_cast<std::size_t>(y) * w + x) * 4;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) {
                for (int i = 0; i < 4; ++i) p[i] = kClear[i];
                continue;
            }
            for (int c = 0; c < 3; ++c)
                p[c] = quantise(w0 * kCol[0][c] + w1 * kCol[1][c] + w2 * kCol[2][c]);
            p[3] = 255;
        }
    }
    g_ran_on = "cpu rasteriser (this build names no accelerator)";
    return 0;
}

extern "C" const char* render_device_name(void) { return g_ran_on; }

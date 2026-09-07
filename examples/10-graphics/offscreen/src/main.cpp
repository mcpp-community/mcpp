import std;
import app.render;

namespace {

constexpr unsigned kW = 64, kH = 64;

bool same(std::array<unsigned char, 4> a, std::array<unsigned char, 4> b) {
    return a == b;
}

std::string show(std::array<unsigned char, 4> c) {
    return std::format("({}, {}, {}, {})", c[0], c[1], c[2], c[3]);
}

} // namespace

int main() {
    auto img = app::render(kW, kH);
    if (!img) { std::println("render unavailable"); return 1; }
    const auto clear = app::clear_color();

    // The four corners are outside the triangle whichever way it is rasterised,
    // so they are the clear colour exactly. No tolerance: nothing interpolates
    // there, and a tolerance would hide an image that was never rendered into.
    const std::array<std::pair<unsigned, unsigned>, 4> corners{{
        {0, 0}, {kW - 1, 0}, {0, kH - 1}, {kW - 1, kH - 1}}};
    for (auto [x, y] : corners) {
        const auto got = img->at(x, y);
        if (!same(got, clear)) {
            std::println("corner ({}, {}) is {}, expected the clear colour {}",
                         x, y, show(got), show(clear));
            return 1;
        }
    }

    // THE CENTRE IS THE ASSERTION THIS EXAMPLE EXISTS FOR.
    //
    // It lies inside the triangle, where all three vertex colours contribute.
    // A fragment shader that ignored its input and wrote a constant would put
    // 255 in one channel and 0 in the other two; a pipeline whose vertex stage
    // never ran would leave the clear colour. Requiring all three channels to
    // be non-zero separates those from an interpolated result, and it does so
    // without depending on a rasteriser's exact rounding.
    const auto centre = img->at(kW / 2, kH / 2);
    std::println("centre pixel: {}", show(centre));
    if (same(centre, clear)) {
        std::println("the centre is the clear colour: nothing was drawn");
        return 1;
    }
    if (centre[0] == 0 || centre[1] == 0 || centre[2] == 0) {
        std::println("the centre has a zero channel: the vertex colours were not interpolated");
        return 1;
    }
    if (centre[3] != 255) {
        std::println("the centre is not opaque: alpha is {}", centre[3]);
        return 1;
    }

    std::println("device: {}", app::device_name());
    return 0;
}

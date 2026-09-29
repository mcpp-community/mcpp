// mcpp.ui.dots_screen:chomp — the chomper, its pellets and its ghost
// (.agents/docs/2026-09-30-build-output-refinement-design.md, §5.9 to §5.13).

export module mcpp.ui.dots_screen:chomp;

import std;
import :core;

namespace mcpp::ui::dots_screen {

// ── chomp: the chomper's position is the fraction, the pellets ahead the
// work left, and a ghost follows; the chomper chomps when steps finish
// (design §5.10; the first design, as the review chose in §5.13).
class Chomp final : public Animation {
public:
    void update(const Input& in) override {
        t_ += in.dt;
        failed_ = in.failed;
        x_ = in.fraction * (kWidth - 5);
        if (in.finished > 0) shut_ = !shut_;
        else if (static_cast<long>(t_ * 2) % 2) shut_ = false;   // an idle chomp
    }
    void draw(Screen& cv) const override {
        for (int x = static_cast<int>(x_) + 5; x < kWidth; x += 2) cv.set(x, 2, Colour::Grey);
        static constexpr std::array<std::string_view, 4> ghostA = {".XX.", "XXXX", "XXXX", "X.X."};
        static constexpr std::array<std::string_view, 4> ghostB = {".XX.", "XXXX", "XXXX", ".X.X"};
        static constexpr std::array<std::string_view, 4> open   = {".XXX", "XX..", "XX..", ".XXX"};
        static constexpr std::array<std::string_view, 4> shut   = {".XX.", "XXXX", "XXXX", ".XX."};
        const bool stride = static_cast<long>(t_ * 4) % 2;
        sprite(cv, failed_ ? x_ - 3 : x_ - 7, 0, stride ? ghostB : ghostA, Colour::BrightCyan);
        sprite(cv, x_, 0, (shut_ || failed_) ? shut : open, failed_ ? Colour::Red : Colour::Yellow);
    }
private:
    double t_ = 0, x_ = 0;
    bool shut_ = false, failed_ = false;
};

std::unique_ptr<Animation> make_chomp(std::uint64_t) { return std::make_unique<Chomp>(); }

} // namespace mcpp::ui::dots_screen

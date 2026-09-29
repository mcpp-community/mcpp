// mcpp.ui.dots_screen:ions — the emitter whose ions build the deposit
// (.agents/docs/2026-09-30-build-output-refinement-design.md, §5.9 to §5.13).

export module mcpp.ui.dots_screen:ions;

import std;
import :core;

namespace mcpp::ui::dots_screen {

// ── ions: an emitter on the right edge scans the four rows and fires ions
// when steps finish; they fly left and vanish at the deposit, which is the
// progress, each dot in the colour of the source whose package last started
// when it was laid (design §5.11). The deposit follows the fraction exactly;
// the ions only show the pace.
class Ions final : public Animation {
public:
    explicit Ions(std::uint64_t seed) : rnd_(seed) {}
    void package(Source s) override { current_ = colour_of(s); }
    void update(const Input& in) override {
        t_ += in.dt;
        failed_ = in.failed;
        const auto target = static_cast<std::size_t>(in.fraction * (kWidth - 4) * kHeight);
        while (units_.size() < target) units_.push_back(current_);
        muzzle_ = 1.5 + 1.5 * std::sin(t_ * 3);
        fire_ = std::max(0.0, fire_ - in.dt);
        if (in.finished > 0 && !failed_) {
            const auto n = std::min<std::size_t>(3, 1 + in.finished / 20);
            std::uniform_real_distribution<double> jitter(-0.6, 0.6);
            for (std::size_t i = 0; i < n; ++i)
                ions_.push_back({kWidth - 3.0, muzzle_ + jitter(rnd_),
                                 current_ == Colour::Grey ? Colour::White : current_});
            fire_ = 0.15;
        }
        const double front = static_cast<double>(units_.size() / kHeight);
        for (auto& ion : ions_) ion.x -= 2.5 * in.dt * 10;
        std::erase_if(ions_, [&](const Ion& ion) { return ion.x <= front + 1; });
    }
    void draw(Screen& cv) const override {
        for (std::size_t u = 0; u < units_.size(); ++u) {
            const bool breach = failed_ && u + 4 >= units_.size();
            cv.set(static_cast<double>(u / kHeight), static_cast<double>(kHeight - 1 - u % kHeight),
                   breach ? Colour::Red : units_[u]);
        }
        for (auto const& ion : ions_)
            cv.set(ion.x, std::clamp(ion.y, 0.0, static_cast<double>(kHeight - 1)), ion.colour);
        for (int y = 0; y < kHeight; ++y) cv.set(kWidth - 1, y, Colour::Blue);
        cv.set(kWidth - 2, std::clamp(std::round(muzzle_), 0.0, static_cast<double>(kHeight - 1)),
               failed_ ? Colour::Red : fire_ > 0 ? Colour::White : Colour::Blue);
    }
private:
    struct Ion { double x, y; Colour colour; };
    std::mt19937_64 rnd_;
    std::vector<Colour> units_;
    std::vector<Ion> ions_;
    Colour current_ = Colour::Grey;
    double t_ = 0, muzzle_ = 1.5, fire_ = 0;
    bool failed_ = false;
};

std::unique_ptr<Animation> make_ions(std::uint64_t seed) { return std::make_unique<Ions>(seed); }

} // namespace mcpp::ui::dots_screen

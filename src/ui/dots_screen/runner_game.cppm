// mcpp.ui.dots_screen:runner_game — the runner that jumps the cacti
// (.agents/docs/2026-09-30-build-output-refinement-design.md, §5.14).
//
// Cacti come from the right, faster as the score rises; space or up jumps.
// Each cactus cleared scores one; touching one ends the round.

export module mcpp.ui.dots_screen:runner_game;

import std;
import :core;

namespace mcpp::ui::dots_screen {

class RunnerGame final : public Game {
public:
    explicit RunnerGame(std::uint64_t seed) : rnd_(seed) { reset(); }

    void key(Key k) override {
        if ((k == Key::Space || k == Key::Up) && y_ == 0 && dead_ <= 0) vy_ = 14.0;
    }

    void update(const Input& in) override {
        t_ += in.dt;
        if (dead_ > 0) {
            dead_ -= in.dt;
            if (dead_ <= 0) reset();
            return;
        }
        const double speed = 14.0 + score_ * 0.6;   // columns a second
        for (auto& c : cacti_) c.x -= speed * in.dt;
        while (!cacti_.empty() && cacti_.front().x < -1) {
            cacti_.pop_front();
            ++score_;
            best_ = std::max(best_, score_);
        }
        if (cacti_.empty() || cacti_.back().x < kWidth - gap_) {
            cacti_.push_back({static_cast<double>(kWidth),
                              std::uniform_int_distribution<int>(1, 2)(rnd_)});
            gap_ = std::uniform_int_distribution<int>(14, 26)(rnd_);
        }
        y_ = std::max(0.0, y_ + vy_ * in.dt);
        vy_ -= 60.0 * in.dt;
        if (y_ == 0) vy_ = std::max(vy_, 0.0);
        // The runner's feet are on row 3 minus its height; a cactus of height
        // h fills rows 4 - h to 3 at its column.
        for (auto const& c : cacti_) {
            const int cx = static_cast<int>(std::lround(c.x));
            if (cx < kX || cx > kX + 3) continue;
            if (std::lround(y_) < c.height) {
                best_ = std::max(best_, score_);
                dead_ = 1.0;
                break;
            }
        }
    }

    void draw(Screen& sc) const override {
        for (int x = 0; x < kWidth; x += 3) sc.set(x, 3, Colour::Grey);
        for (auto const& c : cacti_)
            for (int h = 0; h < c.height; ++h) sc.set(c.x, 3 - h, Colour::Green);
        static constexpr std::array<std::string_view, 3> a = {"..XX", "XXX.", ".X.X"};
        static constexpr std::array<std::string_view, 3> b = {"..XX", "XXX.", "X.X."};
        const auto& body = (y_ == 0 && static_cast<long>(t_ * 8) % 2) ? b : a;
        const double top = 1 - std::round(y_);
        for (std::size_t dy = 0; dy < body.size(); ++dy)
            for (std::size_t dx = 0; dx < body[dy].size(); ++dx)
                if (body[dy][dx] == 'X')
                    sc.set(kX + static_cast<double>(dx), top + static_cast<double>(dy),
                           dead_ > 0 ? Colour::Red : Colour::White);
    }

    int score() const override { return score_; }
    int best() const override { return best_; }

private:
    struct Cactus { double x; int height; };
    static constexpr int kX = 3;

    void reset() {
        cacti_.clear();
        y_ = vy_ = 0;
        score_ = 0;
        dead_ = 0;
        gap_ = 20;
    }

    std::mt19937_64 rnd_;
    std::deque<Cactus> cacti_;
    double t_ = 0, y_ = 0, vy_ = 0, dead_ = 0;
    int gap_ = 20, score_ = 0, best_ = 0;
};

std::unique_ptr<Game> make_runner_game(std::uint64_t seed) { return std::make_unique<RunnerGame>(seed); }

} // namespace mcpp::ui::dots_screen

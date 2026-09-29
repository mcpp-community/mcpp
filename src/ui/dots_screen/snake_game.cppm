// mcpp.ui.dots_screen:snake_game — the snake, steered with the arrows
// (.agents/docs/2026-09-30-build-output-refinement-design.md, §5.14).
//
// Eight cells a second on the 48 x 4 screen, wrapping left and right and
// walled above and below. A package that starts still drops a food in its
// source's colour, and the segments grown from a meal keep that colour. Hitting
// a wall or the body ends the round; the screen turns red for a second and a
// new round starts.

export module mcpp.ui.dots_screen:snake_game;

import std;
import :core;

namespace mcpp::ui::dots_screen {

class SnakeGame final : public Game {
public:
    explicit SnakeGame(std::uint64_t seed) : rnd_(seed) { reset(); }

    void package(Source s) override { meals_.push_back({place(), colour_of(s)}); }

    void key(Key k) override {
        const Cell want = k == Key::Up ? Cell{0, -1} : k == Key::Down ? Cell{0, 1}
                        : k == Key::Left ? Cell{-1, 0} : k == Key::Right ? Cell{1, 0} : dir_;
        if (want.first != -dir_.first || want.second != -dir_.second) next_ = want;
    }

    void update(const Input& in) override {
        if (dead_ > 0) {
            dead_ -= in.dt;
            if (dead_ <= 0) reset();
            return;
        }
        acc_ += in.dt;
        while (acc_ >= kPeriod && dead_ <= 0) {
            acc_ -= kPeriod;
            step();
        }
    }

    void draw(Screen& sc) const override {
        sc.set(food_.first, food_.second, Colour::Grey);
        for (auto const& [cell, colour] : meals_) sc.set(cell.first, cell.second, colour);
        for (std::size_t i = 0; i < body_.size(); ++i) {
            const Colour c = dead_ > 0 ? Colour::Red
                           : i == 0    ? Colour::BrightGreen
                           : i < colours_.size() ? colours_[i] : Colour::Green;
            sc.set(body_[i].first, body_[i].second, c);
        }
    }

    int score() const override { return score_; }
    int best() const override { return best_; }

private:
    using Cell = std::pair<int, int>;
    static constexpr double kPeriod = 0.125;   // eight cells a second

    void reset() {
        body_.clear();
        colours_.clear();
        for (int x = 6; x >= 3; --x) { body_.push_back({x, 1}); colours_.push_back(Colour::Green); }
        dir_ = next_ = {1, 0};
        score_ = 0;
        dead_ = 0;
        acc_ = 0;
        food_ = place();
    }

    Cell place() {
        std::vector<Cell> free;
        for (int x = 0; x < kWidth; ++x)
            for (int y = 0; y < kHeight; ++y)
                if (std::ranges::find(body_, Cell{x, y}) == body_.end()) free.push_back({x, y});
        if (free.empty()) return {0, 0};
        return free[std::uniform_int_distribution<std::size_t>(0, free.size() - 1)(rnd_)];
    }

    void step() {
        dir_ = next_;
        const Cell head{(body_.front().first + dir_.first + kWidth) % kWidth,
                        body_.front().second + dir_.second};
        const bool wall = head.second < 0 || head.second >= kHeight;
        const bool bite = std::ranges::find(body_.begin(), body_.end() - 1, head) != body_.end() - 1;
        if (wall || bite) {
            best_ = std::max(best_, score_);
            dead_ = 1.0;
            return;
        }
        body_.push_front(head);
        Colour grown = colours_.empty() ? Colour::Green : colours_.front();
        bool ate = false;
        if (!meals_.empty() && head == meals_.front().first) {
            grown = meals_.front().second;
            meals_.pop_front();
            ate = true;
        } else if (head == food_) {
            food_ = place();
            ate = true;
        }
        colours_.push_front(grown);
        if (ate) {
            ++score_;
            best_ = std::max(best_, score_);
        } else {
            body_.pop_back();
            colours_.pop_back();
        }
    }

    std::mt19937_64 rnd_;
    std::deque<Cell> body_;
    std::deque<Colour> colours_;
    std::deque<std::pair<Cell, Colour>> meals_;
    Cell food_{0, 0}, dir_{1, 0}, next_{1, 0};
    double acc_ = 0, dead_ = 0;
    int score_ = 0, best_ = 0;
};

std::unique_ptr<Game> make_snake_game(std::uint64_t seed) { return std::make_unique<SnakeGame>(seed); }

} // namespace mcpp::ui::dots_screen

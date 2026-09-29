// mcpp.ui.dots_screen:snake — the snake that eats the packages the build reaches
// (.agents/docs/2026-09-30-build-output-refinement-design.md, §5.9 to §5.13).

export module mcpp.ui.dots_screen:snake;

import std;
import :core;

namespace mcpp::ui::dots_screen {

// ── snake: it takes the shortest free path to its food; a package's first
// step drops a food in the package's colour, and the segments grown after
// that meal keep its colour, so the body records the packages the build
// reached (design §5.12). It grows with the fraction.
class Snake final : public Animation {
public:
    explicit Snake(std::uint64_t seed) : rnd_(seed) {
        for (int x = 6; x >= 3; --x) { body_.push_back({x, 1}); colours_.push_back(Colour::Grey); }
        food_ = place();
    }
    void package(Source s) override {
        meals_.push_back({place(), colour_of(s)});
    }
    void update(const Input& in) override {
        failed_ = in.failed;
        length_ = 4 + static_cast<std::size_t>(in.fraction * 30);
        acc_ += in.dt;
        const double period = in.finished > 0 ? 0.05 : 0.25;
        if (failed_ || acc_ < period) return;
        acc_ = 0;
        const Cell target = meals_.empty() ? food_ : meals_.front().first;
        auto step = path_step(target);
        if (!step) {
            step = any_step();
            if (!step) { reset(); return; }
        }
        body_.push_front(*step);
        if (!meals_.empty() && *step == meals_.front().first) {
            eaten_ = meals_.front().second;
            meals_.pop_front();
        } else if (*step == food_) {
            food_ = place();
        }
        colours_.push_front(eaten_);
        while (body_.size() > length_) body_.pop_back();
        while (colours_.size() > body_.size()) colours_.pop_back();
    }
    void draw(Screen& cv) const override {
        cv.set(food_.first, food_.second, Colour::Dark);
        for (auto const& [cell, colour] : meals_) cv.set(cell.first, cell.second, colour);
        for (std::size_t i = 0; i < body_.size(); ++i) {
            const Colour c = failed_ ? Colour::Red
                           : i == 0  ? Colour::BrightGreen
                           : i < colours_.size() ? colours_[i] : Colour::Grey;
            cv.set(body_[i].first, body_[i].second, c);
        }
    }
private:
    using Cell = std::pair<int, int>;
    bool occupied(Cell c) const { return std::ranges::find(body_, c) != body_.end(); }
    Cell place() {
        std::vector<Cell> free;
        for (int x = 0; x < kWidth; ++x)
            for (int y = 0; y < kHeight; ++y)
                if (!occupied({x, y})) free.push_back({x, y});
        if (free.empty()) return {0, 0};
        return free[std::uniform_int_distribution<std::size_t>(0, free.size() - 1)(rnd_)];
    }
    static Cell moved(Cell c, int dx, int dy) { return {(c.first + dx + kWidth) % kWidth, c.second + dy}; }
    std::optional<Cell> path_step(Cell target) const {
        const Cell head = body_.front();
        std::set<Cell> blocked(body_.begin(), body_.end() - 1);
        std::map<Cell, Cell> prev{{head, head}};
        std::deque<Cell> q{head};
        while (!q.empty()) {
            const Cell cur = q.front();
            q.pop_front();
            if (cur == target) break;
            for (auto [dx, dy] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
                const Cell n = moved(cur, dx, dy);
                if (n.second < 0 || n.second >= kHeight || prev.contains(n) || blocked.contains(n))
                    continue;
                prev.emplace(n, cur);
                q.push_back(n);
            }
        }
        if (!prev.contains(target) || target == head) return std::nullopt;
        Cell step = target;
        while (prev.at(step) != head) step = prev.at(step);
        return step;
    }
    std::optional<Cell> any_step() const {
        std::set<Cell> blocked(body_.begin(), body_.end() - 1);
        for (auto [dx, dy] : {std::pair{1, 0}, {0, 1}, {0, -1}, {-1, 0}}) {
            const Cell n = moved(body_.front(), dx, dy);
            if (n.second >= 0 && n.second < kHeight && !blocked.contains(n)) return n;
        }
        return std::nullopt;
    }
    void reset() {
        body_.clear();
        colours_.clear();
        for (int x = 6; x >= 3; --x) { body_.push_back({x, 1}); colours_.push_back(Colour::Grey); }
    }
    std::mt19937_64 rnd_;
    std::deque<Cell> body_;
    std::deque<Colour> colours_;
    std::deque<std::pair<Cell, Colour>> meals_;
    Cell food_{0, 0};
    Colour eaten_ = Colour::Grey;
    std::size_t length_ = 4;
    double acc_ = 0;
    bool failed_ = false;
};

std::unique_ptr<Animation> make_snake(std::uint64_t seed) { return std::make_unique<Snake>(seed); }

} // namespace mcpp::ui::dots_screen

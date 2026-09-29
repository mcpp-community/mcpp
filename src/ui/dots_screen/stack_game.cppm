// mcpp.ui.dots_screen:stack_game — Tetris on its side, played
// (.agents/docs/2026-09-30-build-output-refinement-design.md, §5.14).
//
// Gravity pulls to the left. A piece enters at the right and falls one column
// every half second, faster as the score rises. Up and down move it between
// the four rows, left drops it, right or space turns it. A column filled from
// top to bottom clears, as a filled row does in the original, and scores one.
// A piece that cannot enter ends the round.

export module mcpp.ui.dots_screen:stack_game;

import std;
import :core;

namespace mcpp::ui::dots_screen {

class StackGame final : public Game {
public:
    explicit StackGame(std::uint64_t seed) : rnd_(seed) { spawn(); }

    void key(Key k) override {
        if (dead_ > 0 || !piece_) return;
        auto& p = *piece_;
        switch (k) {
        case Key::Up:    if (fits(p.rot, p.x, p.y - 1)) --p.y; break;
        case Key::Down:  if (fits(p.rot, p.x, p.y + 1)) ++p.y; break;
        case Key::Left:  while (fits(p.rot, p.x - 1, p.y)) --p.x; lock(); break;
        case Key::Right:
        case Key::Space: {
            const auto next = (p.rot + 1) % kinds()[p.kind].size();
            for (int dy : {0, -1, 1, -2})   // turn, nudged back onto the board
                if (fits(next, p.x, p.y + dy)) { p.rot = next; p.y += dy; break; }
            break;
        }
        }
    }

    void update(const Input& in) override {
        if (dead_ > 0) {
            dead_ -= in.dt;
            if (dead_ <= 0) { cells_.clear(); score_ = 0; spawn(); }
            return;
        }
        acc_ += in.dt;
        const double fall = std::max(0.1, 0.5 - 0.03 * score_);
        while (acc_ >= fall && piece_) {
            acc_ -= fall;
            if (fits(piece_->rot, piece_->x - 1, piece_->y)) --piece_->x;
            else lock();
        }
    }

    void draw(Screen& sc) const override {
        for (auto const& [cell, colour] : cells_)
            sc.set(cell.first, cell.second, dead_ > 0 ? Colour::Red : colour);
        if (piece_)
            for (auto [dx, dy] : kinds()[piece_->kind][piece_->rot])
                sc.set(piece_->x + dx, piece_->y + dy, colours()[piece_->kind]);
    }

    int score() const override { return score_; }
    int best() const override { return best_; }

private:
    using Cell = std::pair<int, int>;
    using Shape = std::vector<Cell>;
    struct Piece { std::size_t kind, rot; int x, y; };

    static const std::vector<std::vector<Shape>>& kinds() {
        static const std::vector<std::vector<Shape>> k = {
            {{{0,0},{1,0},{2,0},{3,0}}, {{0,0},{0,1},{0,2},{0,3}}},
            {{{0,0},{1,0},{0,1},{1,1}}},
            {{{0,0},{1,0},{2,0},{1,1}}, {{0,0},{0,1},{0,2},{1,1}},
             {{1,0},{0,1},{1,1},{2,1}}, {{1,0},{1,1},{1,2},{0,1}}},
            {{{1,0},{2,0},{0,1},{1,1}}, {{0,0},{0,1},{1,1},{1,2}}},
            {{{0,0},{1,0},{1,1},{2,1}}, {{1,0},{1,1},{0,1},{0,2}}},
            {{{0,0},{0,1},{0,2},{1,2}}, {{0,0},{1,0},{2,0},{0,1}},
             {{0,0},{1,0},{1,1},{1,2}}, {{2,0},{0,1},{1,1},{2,1}}},
            {{{1,0},{1,1},{1,2},{0,2}}, {{0,0},{0,1},{1,1},{2,1}},
             {{0,0},{1,0},{0,1},{0,2}}, {{0,0},{1,0},{2,0},{2,1}}},
        };
        return k;
    }
    static const std::vector<Colour>& colours() {
        static const std::vector<Colour> c = {Colour::BrightCyan, Colour::Yellow, Colour::Magenta,
            Colour::BrightGreen, Colour::Red, Colour::White, Colour::Blue};
        return c;
    }

    bool fits(std::size_t rot, int x, int y) const {
        for (auto [dx, dy] : kinds()[piece_->kind][rot]) {
            const int cx = x + dx, cy = y + dy;
            if (cx < 0 || cx >= kWidth || cy < 0 || cy >= kHeight) return false;
            if (cells_.contains({cx, cy})) return false;
        }
        return true;
    }

    void spawn() {
        const auto kind = std::uniform_int_distribution<std::size_t>(0, kinds().size() - 1)(rnd_);
        piece_ = Piece{kind, 0, kWidth - 4, 1};
        if (!fits(0, piece_->x, piece_->y)) piece_->y = 0;
        if (!fits(0, piece_->x, piece_->y)) {
            best_ = std::max(best_, score_);
            dead_ = 1.0;
        }
    }

    void lock() {
        if (!piece_) return;
        for (auto [dx, dy] : kinds()[piece_->kind][piece_->rot])
            cells_[{piece_->x + dx, piece_->y + dy}] = colours()[piece_->kind];
        // Full columns clear; the columns to their right close the gap.
        for (int x = kWidth - 1; x >= 0; --x) {
            bool full = true;
            for (int y = 0; y < kHeight; ++y) full = full && cells_.contains({x, y});
            if (!full) continue;
            std::map<Cell, Colour> moved;
            for (auto const& [cell, colour] : cells_) {
                if (cell.first == x) continue;
                moved[{cell.first > x ? cell.first - 1 : cell.first, cell.second}] = colour;
            }
            cells_ = std::move(moved);
            ++score_;
        }
        best_ = std::max(best_, score_);
        spawn();
    }

    std::mt19937_64 rnd_;
    std::map<Cell, Colour> cells_;
    std::optional<Piece> piece_;
    double acc_ = 0, dead_ = 0;
    int score_ = 0, best_ = 0;
};

std::unique_ptr<Game> make_stack_game(std::uint64_t seed) { return std::make_unique<StackGame>(seed); }

} // namespace mcpp::ui::dots_screen

#include <gtest/gtest.h>

import std;
import mcpp.ui.dots_screen;

// The status row's screen and its animations
// (.agents/docs/2026-09-30-build-output-refinement-design.md, §5.9 to §5.13):
// 24 braille cells, a colour per cell, and four animations that take their
// tempo from the build and are pure given a seed and their inputs.

using namespace mcpp::ui::dots_screen;

namespace {

// The code points of a rendering without colour: one braille cell each.
std::vector<char32_t> cells(std::string_view s) {
    std::vector<char32_t> out;
    for (std::size_t i = 0; i < s.size();) {
        const auto b = static_cast<unsigned char>(s[i]);
        if (b == 0x1b) {                      // skip an escape sequence
            while (i < s.size() && s[i] != 'm') ++i;
            ++i;
            continue;
        }
        EXPECT_EQ(b & 0xF0, 0xE0) << "not a three-byte code point at " << i;
        const char32_t c = (static_cast<char32_t>(b & 0x0F) << 12)
                         | (static_cast<char32_t>(s[i + 1] & 0x3F) << 6)
                         | static_cast<char32_t>(s[i + 2] & 0x3F);
        out.push_back(c);
        i += 3;
    }
    return out;
}

// Plays `frames` frames of a build that finishes `perFrame` steps a frame
// until `fraction` reaches 1, and returns the last rendering.
std::string play(Animation& a, int frames, std::size_t perFrame, bool failAt = false,
                 bool colour = false) {
    std::string last;
    for (int i = 0; i < frames; ++i) {
        Input in;
        in.dt = 0.1;
        in.finished = perFrame;
        in.fraction = std::min(1.0, (i + 1) / static_cast<double>(frames));
        in.failed = failAt && i > frames / 2;
        a.update(in);
        Screen sc;
        a.draw(sc);
        last = sc.render(colour);
    }
    return last;
}

} // namespace

TEST(DotsScreen, TheFourAnimationsAreBuiltIn) {
    auto names = mcpp::ui::dots_screen::names();
    ASSERT_EQ(names.size(), 4u);
    EXPECT_EQ(names[0], "chomp");
    EXPECT_EQ(names[1], "snake");
    EXPECT_EQ(names[2], "stack");
    EXPECT_EQ(names[3], "ions");
    for (auto n : names) EXPECT_NE(make(n, 1), nullptr) << n;
    EXPECT_EQ(make("runner", 1), nullptr);
    EXPECT_EQ(make("", 1), nullptr);
}

TEST(DotsScreen, TheScreenIsTwentyFourBrailleCells) {
    Screen sc;
    sc.set(0, 0, Colour::White);     // the first dot of the first cell
    sc.set(47, 3, Colour::Red);      // the last dot of the last cell
    sc.set(99, 0, Colour::White);    // off the screen: ignored
    const auto text = sc.render(false);
    auto c = cells(text);
    ASSERT_EQ(c.size(), 24u);
    for (auto cp : c) EXPECT_TRUE(cp >= 0x2800 && cp <= 0x28FF);
    EXPECT_EQ(c.front(), char32_t{0x2801});
    EXPECT_EQ(c.back(), char32_t{0x2880});
    EXPECT_EQ(text.find('\x1b'), std::string::npos);
    const auto coloured = sc.render(true);
    EXPECT_NE(coloured.find("\x1b[91m"), std::string::npos);
    EXPECT_TRUE(coloured.ends_with("\x1b[0m"));
}

TEST(DotsScreen, AnAnimationIsPureGivenItsSeedAndInputs) {
    for (auto n : mcpp::ui::dots_screen::names()) {
        auto a = make(n, 42), b = make(n, 42);
        EXPECT_EQ(play(*a, 60, 5), play(*b, 60, 5)) << n;
    }
}

TEST(DotsScreen, EveryFrameIsTwentyFourCellsAndAFailureTurnsRed) {
    for (auto n : mcpp::ui::dots_screen::names()) {
        auto a = make(n, 7);
        const auto plain = play(*a, 40, 3);
        EXPECT_EQ(cells(plain).size(), 24u) << n;
        EXPECT_EQ(plain.find('\n'), std::string::npos) << n;
        auto f = make(n, 7);
        const auto failed = play(*f, 40, 3, /*failAt=*/true, /*colour=*/true);
        EXPECT_NE(failed.find("\x1b[91m"), std::string::npos) << n;
    }
}

TEST(DotsScreen, TheSnakeCarriesTheColourOfThePackagesItAte) {
    auto a = make("snake", 3);
    a->package(Source::Official);
    a->package(Source::Official);
    const auto frame = play(*a, 200, 2, false, /*colour=*/true);
    EXPECT_NE(frame.find("\x1b[36m"), std::string::npos) << "no segment in the official colour";
}

TEST(DotsScreen, TheIonsDepositIsTheFraction) {
    auto a = make("ions", 5);
    Input in;
    in.dt = 0.1;
    in.fraction = 0.5;
    a->update(in);
    Screen sc;
    a->draw(sc);
    int lit = 0;
    for (int x = 0; x < kWidth - 4; ++x)
        for (int y = 0; y < kHeight; ++y)
            if (sc.at(x, y) != Colour::None) ++lit;
    EXPECT_EQ(lit, (kWidth - 4) * kHeight / 2);
}

TEST(DotsScreen, TheChomperStandsAtTheFraction) {
    auto a = make("chomp", 1);
    Input in;
    in.fraction = 1.0;
    a->update(in);
    Screen sc;
    a->draw(sc);
    // At the end the chomper has eaten every pellet: nothing lit on its right.
    bool pellet = false;
    for (int x = kWidth - 1 - 0; x < kWidth; ++x)
        if (sc.at(x, 2) == Colour::Grey) pellet = true;
    EXPECT_FALSE(pellet);
    EXPECT_EQ(sc.at(kWidth - 5, 1), Colour::Yellow);
}

// ─── The games of --play-game (design §5.14) ─────────────────────────────

namespace {
bool any_red(const Game& g) {
    Screen sc;
    g.draw(sc);
    for (int x = 0; x < kWidth; ++x)
        for (int y = 0; y < kHeight; ++y)
            if (sc.at(x, y) == Colour::Red) return true;
    return false;
}
void run(Game& g, double seconds) {
    Input in;
    in.dt = 0.05;
    for (double t = 0; t < seconds; t += in.dt) g.update(in);
}
} // namespace

TEST(DotsScreenGames, TheThreeGamesAreBuiltIn) {
    auto names = game_names();
    ASSERT_EQ(names.size(), 3u);
    EXPECT_EQ(names[0], "snake");
    EXPECT_EQ(names[1], "stack");
    EXPECT_EQ(names[2], "runner");
    for (auto n : names) EXPECT_NE(make_game(n, 1), nullptr) << n;
    EXPECT_EQ(make_game("chomp", 1), nullptr);   // an animation, not a game
}

TEST(DotsScreenGames, TheSnakeTurnsWithTheArrowsAndAWallEndsTheRound) {
    auto g = make_game("snake", 4);
    EXPECT_FALSE(any_red(*g));
    g->key(Key::Up);      // from row 1: row 0, then the wall
    run(*g, 0.3);
    EXPECT_TRUE(any_red(*g)) << "the snake went through the wall";
    run(*g, 1.2);         // a new round starts after a second
    EXPECT_FALSE(any_red(*g));
    EXPECT_EQ(g->score(), 0);
}

TEST(DotsScreenGames, TheStackPlacesPiecesAgainstTheLeftAndScoresClearedColumns) {
    auto g = make_game("stack", 9);
    for (int i = 0; i < 400 && g->score() == 0; ++i) {
        // Spread the pieces over the four rows so that columns fill.
        g->key(i % 4 == 0 ? Key::Up : i % 4 == 1 ? Key::Down : Key::Space);
        g->key(Key::Left);
        run(*g, 0.05);
    }
    Screen sc;
    g->draw(sc);
    bool left = false;
    for (int y = 0; y < kHeight; ++y) left = left || sc.at(0, y) != Colour::None;
    EXPECT_TRUE(left) << "no piece rests against the left edge";
    EXPECT_GE(g->best(), g->score());
}

TEST(DotsScreenGames, TheRunnerJumpsOnSpaceAndACactusEndsTheRound) {
    auto g = make_game("runner", 2);
    // Without a key, a cactus reaches the runner within a few seconds.
    bool ended = false;
    Input in;
    in.dt = 0.05;
    for (double t = 0; t < 6 && !ended; t += in.dt) { g->update(in); ended = any_red(*g); }
    EXPECT_TRUE(ended) << "no cactus ever touched the runner";
    run(*g, 1.1);
    // Space lifts it: its head rises above row 1.
    g->key(Key::Space);
    run(*g, 0.1);
    Screen sc;
    g->draw(sc);
    bool high = false;
    for (int x = 3; x < 7; ++x) high = high || sc.at(x, 0) == Colour::White;
    EXPECT_TRUE(high) << "the runner did not jump";
}

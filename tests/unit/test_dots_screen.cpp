
import std;
import mcpplibs.mtest;
import mcpp.ui.dots_screen;

using namespace mcpplibs::mtest;

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
        expect(eq(b & 0xF0, 0xE0)) << "not a three-byte code point at " << i;
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

UTest the_four_animations_are_built_in{"DotsScreen/TheFourAnimationsAreBuiltIn", [] {
    auto names = mcpp::ui::dots_screen::names();
    expect(fatal(eq(names.size(), 4u)));
    expect(eq(names[0], "chomp"));
    expect(eq(names[1], "snake"));
    expect(eq(names[2], "stack"));
    expect(eq(names[3], "ions"));
    for (auto n : names) expect(ne(make(n, 1), nullptr)) << n;
    expect(eq(make("runner", 1), nullptr));
    expect(eq(make("", 1), nullptr));
}};

UTest the_screen_is_twenty_four_braille_cells{"DotsScreen/TheScreenIsTwentyFourBrailleCells", [] {
    Screen sc;
    sc.set(0, 0, Colour::White);     // the first dot of the first cell
    sc.set(47, 3, Colour::Red);      // the last dot of the last cell
    sc.set(99, 0, Colour::White);    // off the screen: ignored
    const auto text = sc.render(false);
    auto c = cells(text);
    expect(fatal(eq(c.size(), 24u)));
    for (auto cp : c) expect(cp >= 0x2800 && cp <= 0x28FF);
    expect(eq(c.front(), char32_t{0x2801}));
    expect(eq(c.back(), char32_t{0x2880}));
    expect(eq(text.find('\x1b'), std::string::npos));
    const auto coloured = sc.render(true);
    expect(ne(coloured.find("\x1b[91m"), std::string::npos));
    expect(coloured.ends_with("\x1b[0m"));
}};

UTest an_animation_is_pure_given_its_seed_and_inputs{"DotsScreen/AnAnimationIsPureGivenItsSeedAndInputs", [] {
    for (auto n : mcpp::ui::dots_screen::names()) {
        auto a = make(n, 42), b = make(n, 42);
        expect(eq(play(*a, 60, 5), play(*b, 60, 5))) << n;
    }
}};

UTest every_frame_is_twenty_four_cells_and_a_failure_turns_red{"DotsScreen/EveryFrameIsTwentyFourCellsAndAFailureTurnsRed", [] {
    for (auto n : mcpp::ui::dots_screen::names()) {
        auto a = make(n, 7);
        const auto plain = play(*a, 40, 3);
        expect(eq(cells(plain).size(), 24u)) << n;
        expect(eq(plain.find('\n'), std::string::npos)) << n;
        auto f = make(n, 7);
        const auto failed = play(*f, 40, 3, /*failAt=*/true, /*colour=*/true);
        expect(ne(failed.find("\x1b[91m"), std::string::npos)) << n;
    }
}};

UTest the_snake_carries_the_colour_of_the_packages_it_ate{"DotsScreen/TheSnakeCarriesTheColourOfThePackagesItAte", [] {
    auto a = make("snake", 3);
    a->package(Source::Official);
    a->package(Source::Official);
    const auto frame = play(*a, 200, 2, false, /*colour=*/true);
    expect(ne(frame.find("\x1b[36m"), std::string::npos)) << "no segment in the official colour";
}};

UTest the_ions_deposit_is_the_fraction{"DotsScreen/TheIonsDepositIsTheFraction", [] {
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
    expect(eq(lit, (kWidth - 4) * kHeight / 2));
}};

UTest the_chomper_stands_at_the_fraction{"DotsScreen/TheChomperStandsAtTheFraction", [] {
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
    expect(!pellet);
    expect(eq(sc.at(kWidth - 5, 1), Colour::Yellow));
}};

// ─── Every frame returns (build wall-time plan, F1 and W1) ──────────────
//
// The ticker draws a frame while it holds the line lock, and the build joins
// the ticker when ninja ends: a frame that does not return hangs the build
// after it has finished. The stack animation did, in about 3% of builds that
// chose it (66 of 2000 seeded runs of this shape). The property is stated
// over seeds and input sequences, not by example, and a frame that never
// returns cannot be joined, so the watchdog ends the test binary.

namespace {

void returns_within(std::chrono::seconds limit, std::string what,
                    std::function<void()> body) {
    auto done = std::make_shared<std::promise<void>>();
    auto finished = done->get_future();
    std::thread([body = std::move(body), done] {
        body();
        done->set_value();
    }).detach();
    if (finished.wait_for(limit) != std::future_status::ready) {
        expect(false) << what << " did not return within " << limit.count() << " s";
        std::fflush(nullptr);
        std::_Exit(1);
    }
}

// A build of `total` steps that finishes in bursts at ten frames a second,
// then `tail` more frames at its end; the failure input on one seed in eight.
void play_build(Animation& a, std::uint64_t seed, std::size_t total, int tail) {
    std::mt19937_64 rnd(seed);
    std::size_t done = 0;
    const bool fails = seed % 8 == 7;
    for (int frame = 0; done < total || tail-- > 0; ++frame) {
        const std::size_t burst = rnd() % 7 == 0 ? rnd() % 12 : rnd() % 2;
        const std::size_t before = done;
        done = std::min(total, done + burst);
        Input in;
        in.dt = 0.1;
        in.finished = done - before;
        in.fraction = static_cast<double>(done) / static_cast<double>(total);
        in.failed = fails && done * 2 > total;
        a.update(in);
        if (frame % 16 == 0) a.package(static_cast<Source>(rnd() % 6));
    }
}

} // namespace

UTest every_animation_returns_from_every_frame{"DotsScreen/EveryAnimationReturnsFromEveryFrame", [] {
    for (auto name : mcpp::ui::dots_screen::names()) {
        const std::string n(name);
        returns_within(std::chrono::seconds(120), "an animation '" + n + "'", [n] {
            for (std::uint64_t seed = 0; seed < 2000; ++seed) {
                auto a = make(n, seed);
                play_build(*a, seed, 240, 120);
            }
        });
    }
}};

UTest every_game_returns_from_every_frame{"DotsScreenGames/EveryGameReturnsFromEveryFrame", [] {
    for (auto name : game_names()) {
        const std::string n(name);
        returns_within(std::chrono::seconds(120), "a game '" + n + "'", [n] {
            constexpr Key kKeys[] = {Key::Up, Key::Down, Key::Left, Key::Right, Key::Space};
            for (std::uint64_t seed = 0; seed < 500; ++seed) {
                auto g = make_game(n, seed);
                std::mt19937_64 rnd(seed);
                Input in;
                in.dt = 0.05;
                for (int frame = 0; frame < 400; ++frame) {
                    if (rnd() % 3 == 0) g->key(kKeys[rnd() % 5]);
                    in.failed = frame > 300 && seed % 4 == 3;
                    g->update(in);
                }
            }
        });
    }
}};

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

UTest the_three_games_are_built_in{"DotsScreenGames/TheThreeGamesAreBuiltIn", [] {
    auto names = game_names();
    expect(fatal(eq(names.size(), 3u)));
    expect(eq(names[0], "snake"));
    expect(eq(names[1], "stack"));
    expect(eq(names[2], "runner"));
    for (auto n : names) expect(ne(make_game(n, 1), nullptr)) << n;
    expect(eq(make_game("chomp", 1), nullptr));   // an animation, not a game
}};

UTest the_snake_turns_with_the_arrows_and_a_wall_ends_the_round{"DotsScreenGames/TheSnakeTurnsWithTheArrowsAndAWallEndsTheRound", [] {
    auto g = make_game("snake", 4);
    expect(!any_red(*g));
    g->key(Key::Up);      // from row 1: row 0, then the wall
    run(*g, 0.3);
    expect(bool(any_red(*g))) << "the snake went through the wall";
    run(*g, 1.2);         // a new round starts after a second
    expect(!any_red(*g));
    expect(eq(g->score(), 0));
}};

UTest the_stack_places_pieces_against_the_left_and_scores_cleared_columns{"DotsScreenGames/TheStackPlacesPiecesAgainstTheLeftAndScoresClearedColumns", [] {
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
    expect(bool(left)) << "no piece rests against the left edge";
    expect(ge(g->best(), g->score()));
}};

UTest the_runner_jumps_on_space_and_a_cactus_ends_the_round{"DotsScreenGames/TheRunnerJumpsOnSpaceAndACactusEndsTheRound", [] {
    auto g = make_game("runner", 2);
    // Without a key, a cactus reaches the runner within a few seconds.
    bool ended = false;
    Input in;
    in.dt = 0.05;
    for (double t = 0; t < 6 && !ended; t += in.dt) { g->update(in); ended = any_red(*g); }
    expect(bool(ended)) << "no cactus ever touched the runner";
    run(*g, 1.1);
    // Space lifts it: its head rises above row 1.
    g->key(Key::Space);
    run(*g, 0.1);
    Screen sc;
    g->draw(sc);
    bool high = false;
    for (int x = 3; x < 7; ++x) high = high || sc.at(x, 0) == Colour::White;
    expect(bool(high)) << "the runner did not jump";
}};

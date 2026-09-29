// mcpp.ui.dots_screen — the screen of the status row and the animations it plays
// (.agents/docs/2026-09-30-build-output-refinement-design.md, §5.9 to §5.13).
//
// The counts beside the screen state the progress, so the screen plays one of
// four animations, chosen per command: the chomper, the snake, the stack and
// the ions. With `--play-game` it plays one of three games instead: the
// snake, the stack and the runner, steered from the keyboard. Each lives in a
// partition of its own; this unit knows them by name. The progress model
// feeds the chosen one and asks it for its cells; nothing here touches a
// terminal or a clock.

export module mcpp.ui.dots_screen;

export import :core;
export import :chomp;
export import :snake;
export import :stack;
export import :ions;
export import :snake_game;
export import :stack_game;
export import :runner_game;

import std;

export namespace mcpp::ui::dots_screen {

// The animations built in: chomp, snake, stack, ions.
std::span<const std::string_view> names();
// The animation called `name`, seeded; nullptr for a name not in `names()`.
std::unique_ptr<Animation> make(std::string_view name, std::uint64_t seed);

// The games of `--play-game`: snake, stack, runner.
std::span<const std::string_view> game_names();
// The game called `name`, seeded; nullptr for a name not in `game_names()`.
std::unique_ptr<Game> make_game(std::string_view name, std::uint64_t seed);

} // namespace mcpp::ui::dots_screen

namespace mcpp::ui::dots_screen {

namespace {
constexpr std::array<std::string_view, 4> kNames = {"chomp", "snake", "stack", "ions"};
constexpr std::array<std::string_view, 3> kGames = {"snake", "stack", "runner"};
} // namespace

std::span<const std::string_view> names() { return kNames; }

std::unique_ptr<Animation> make(std::string_view name, std::uint64_t seed) {
    if (name == "chomp") return make_chomp(seed);
    if (name == "snake") return make_snake(seed);
    if (name == "stack") return make_stack(seed);
    if (name == "ions")  return make_ions(seed);
    return nullptr;
}

std::span<const std::string_view> game_names() { return kGames; }

std::unique_ptr<Game> make_game(std::string_view name, std::uint64_t seed) {
    if (name == "snake")  return make_snake_game(seed);
    if (name == "stack")  return make_stack_game(seed);
    if (name == "runner") return make_runner_game(seed);
    return nullptr;
}

} // namespace mcpp::ui::dots_screen

// mcpp.ui.dots_screen:core — the screen of the status row, its colours, and what
// an animation is (.agents/docs/2026-09-30-build-output-refinement-design.md,
// §5.9 to §5.13).
//
// The screen is 24 braille cells: 48 columns of four dots. A braille cell is
// U+2800 plus eight bits, one per dot, so every dot is addressable; a colour
// applies to the whole cell. Braille is East Asian narrow everywhere, so the
// screen takes one column per cell whatever the terminal does with ambiguous
// characters.

export module mcpp.ui.dots_screen:core;

import std;

export namespace mcpp::ui::dots_screen {

inline constexpr int kWidth  = 48;   // columns of dots
inline constexpr int kHeight = 4;    // rows of dots
inline constexpr int kCells  = kWidth / 2;

enum class Colour : std::uint8_t {
    None, Dark, Grey, Default, White, Cyan, BrightCyan, Magenta, Blue,
    Yellow, Red, Green, BrightGreen,
};

// Where a package's work came from (design §5.10). Each source has the colour
// its packages' names have in the package lines, so those lines are the
// screen's legend.
enum class Source : std::uint8_t { Cache, Official, Index, Git, Project, Other };
Colour colour_of(Source s);

// 48 x 4 dots, each unlit or lit in a colour.
class Screen {
public:
    // Lights the dot nearest (x, y); a dot off the screen is ignored.
    void set(double x, double y, Colour c);
    Colour at(int x, int y) const { return dots_[static_cast<std::size_t>(y * kWidth + x)]; }
    // The 24 cells. A cell takes the colour of its brightest lit dot (grey
    // and dark count least). With `colour` false no escape sequence is
    // written, and the shapes alone remain.
    std::string render(bool colour) const;
private:
    std::array<Colour, kWidth * kHeight> dots_{};
};

// What the build did since the previous frame.
struct Input {
    double      dt       = 0.0;   // seconds since the previous frame
    std::size_t finished = 0;     // steps finished since the previous frame
    double      fraction = 0.0;   // finished / planned, 0 to 1
    bool        failed   = false; // a step has failed
};

// EVERY ANIMATION TAKES ITS TEMPO FROM THE BUILD: time moves it slowly, which
// shows that mcpp is alive; finished steps move it further, which shows that
// the build is busy; a failure changes it. A wait therefore looks like a
// wait, and the counts beside the screen say why. An animation is pure: given
// a seed and a sequence of inputs it draws the same frames, which is what the
// unit tests read.
class Animation {
public:
    virtual ~Animation() = default;
    virtual void update(const Input& in) = 0;
    // A package's first step finished: its line was written.
    virtual void package(Source) {}
    virtual void draw(Screen& screen) const = 0;
};

// The keys a game reads (design §5.14).
enum class Key { Up, Down, Left, Right, Space };

// A GAME IS AN ANIMATION THE USER STEERS (`--play-game`, design §5.14). It
// runs at a speed of its own, not at the build's pace: `update` reads only the
// elapsed time and the failure; the counts beside the screen state the build.
// A round that ends starts again at once, and the best round is kept.
class Game : public Animation {
public:
    virtual void key(Key k) = 0;
    virtual int score() const = 0;   // this round
    virtual int best() const = 0;    // the best round so far
};

} // namespace mcpp::ui::dots_screen

namespace mcpp::ui::dots_screen {

// For the animations of this module: draws `rows` ('X' lit) with its top
// left corner at (x, y).
void sprite(Screen& screen, double x, double y, std::span<const std::string_view> rows, Colour c) {
    for (std::size_t dy = 0; dy < rows.size(); ++dy)
        for (std::size_t dx = 0; dx < rows[dy].size(); ++dx)
            if (rows[dy][dx] == 'X')
                screen.set(x + static_cast<double>(dx), y + static_cast<double>(dy), c);
}

namespace {

// Bit of the dot at (column within the cell, row).
constexpr std::uint8_t kBit[2][4] = {{0x01, 0x02, 0x04, 0x40}, {0x08, 0x10, 0x20, 0x80}};

std::string_view sgr(Colour c) {
    switch (c) {
    case Colour::Dark:        return "\x1b[2;90m";
    case Colour::Grey:        return "\x1b[90m";
    case Colour::Default:     return "\x1b[39m";
    case Colour::White:       return "\x1b[97m";
    case Colour::Cyan:        return "\x1b[36m";
    case Colour::BrightCyan:  return "\x1b[96m";
    case Colour::Magenta:     return "\x1b[95m";
    case Colour::Blue:        return "\x1b[94m";
    case Colour::Yellow:      return "\x1b[93m";
    case Colour::Red:         return "\x1b[91m";
    case Colour::Green:       return "\x1b[32m";
    case Colour::BrightGreen: return "\x1b[92m";
    case Colour::None:        break;
    }
    return "\x1b[90m";
}

int rank(Colour c) {
    return c == Colour::None ? -1 : (c == Colour::Grey || c == Colour::Dark) ? 0 : 1;
}

} // namespace

Colour colour_of(Source s) {
    switch (s) {
    case Source::Cache:    return Colour::Grey;
    case Source::Official: return Colour::Cyan;
    case Source::Index:    return Colour::Magenta;
    case Source::Git:      return Colour::Blue;
    case Source::Project:  return Colour::White;
    case Source::Other:    break;
    }
    return Colour::Default;
}

void Screen::set(double x, double y, Colour c) {
    const auto xi = static_cast<int>(std::lround(x));
    const auto yi = static_cast<int>(std::lround(y));
    if (xi < 0 || xi >= kWidth || yi < 0 || yi >= kHeight) return;
    dots_[static_cast<std::size_t>(yi * kWidth + xi)] = c;
}

std::string Screen::render(bool colour) const {
    std::string out;
    Colour open = Colour::None;
    for (int cell = 0; cell < kCells; ++cell) {
        std::uint8_t bits = 0;
        Colour c = Colour::None;
        for (int side = 0; side < 2; ++side)
            for (int y = 0; y < kHeight; ++y) {
                const Colour d = at(cell * 2 + side, y);
                if (d == Colour::None) continue;
                bits |= kBit[side][y];
                if (rank(d) > rank(c)) c = d;
            }
        if (colour && bits && c != open) {
            out += sgr(c);
            open = c;
        }
        // U+2800 + bits, in UTF-8: three bytes.
        const char32_t cp = 0x2800 + bits;
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    if (colour && open != Colour::None) out += "\x1b[0m";
    return out;
}

} // namespace mcpp::ui::dots_screen

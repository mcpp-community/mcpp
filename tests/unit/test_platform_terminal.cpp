#include <gtest/gtest.h>

import std;
import mcpp.platform.terminal;

// The keys a game reads from a POSIX terminal
// (.agents/docs/2026-09-30-build-output-refinement-design.md, §5.14): the
// decoder turns the bytes the terminal sent into keys, skips a sequence for
// any other key whole, and keeps an incomplete sequence for the next read.

using mcpp::platform::terminal::Key;
using mcpp::platform::terminal::decode_keys;

TEST(TerminalKeys, ArrowsInBothModesAndTheSecondSet) {
    std::string in = "\x1b[A\x1b[B\x1bOC\x1bOD wasd";
    const auto keys = decode_keys(in);
    const std::vector<Key> want = {Key::Up, Key::Down, Key::Right, Key::Left, Key::Space,
                                   Key::Up, Key::Left, Key::Down, Key::Right};
    EXPECT_EQ(keys, want);
    EXPECT_TRUE(in.empty());
}

TEST(TerminalKeys, AModifiedArrowIsOneKeyAndOtherSequencesAreSkippedWhole) {
    // Ctrl-Up, then F5 (`ESC [ 1 5 ~`), then Home in application mode.
    std::string in = "\x1b[1;5A\x1b[15~\x1bOH";
    const auto keys = decode_keys(in);
    const std::vector<Key> want = {Key::Up};
    EXPECT_EQ(keys, want) << "a parameter byte or a final byte was read as a key";
    EXPECT_TRUE(in.empty());
}

TEST(TerminalKeys, AnIncompleteSequenceWaitsForTheNextRead) {
    std::string in = "d\x1b[1;";
    EXPECT_EQ(decode_keys(in), std::vector<Key>{Key::Right});
    EXPECT_EQ(in, "\x1b[1;");
    in += "2B";
    EXPECT_EQ(decode_keys(in), std::vector<Key>{Key::Down});
    EXPECT_TRUE(in.empty());

    std::string esc = "\x1b";
    EXPECT_TRUE(decode_keys(esc).empty());
    EXPECT_EQ(esc, "\x1b");
    esc += "w";   // escape on its own, then a key
    EXPECT_EQ(decode_keys(esc), std::vector<Key>{Key::Up});
}

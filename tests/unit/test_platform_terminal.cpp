
import std;
import mcpplibs.mtest;
import mcpp.ui.terminal;

using namespace mcpplibs::mtest;

// The keys a game reads from a POSIX terminal
// (.agents/docs/2026-09-30-build-output-refinement-design.md, §5.14): the
// decoder turns the bytes the terminal sent into keys, skips a sequence for
// any other key whole, and keeps an incomplete sequence for the next read.

using mcpp::ui::terminal::Key;
using mcpp::ui::terminal::decode_keys;

UTest arrows_in_both_modes_and_the_second_set{"TerminalKeys/ArrowsInBothModesAndTheSecondSet", [] {
    std::string in = "\x1b[A\x1b[B\x1bOC\x1bOD wasd";
    const auto keys = decode_keys(in);
    const std::vector<Key> want = {Key::Up, Key::Down, Key::Right, Key::Left, Key::Space,
                                   Key::Up, Key::Left, Key::Down, Key::Right};
    expect(eq(keys, want));
    expect(in.empty());
}};

UTest a_modified_arrow_is_one_key_and_other_sequences_are_skipped_whole{"TerminalKeys/AModifiedArrowIsOneKeyAndOtherSequencesAreSkippedWhole", [] {
    // Ctrl-Up, then F5 (`ESC [ 1 5 ~`), then Home in application mode.
    std::string in = "\x1b[1;5A\x1b[15~\x1bOH";
    const auto keys = decode_keys(in);
    const std::vector<Key> want = {Key::Up};
    expect(eq(keys, want)) << "a parameter byte or a final byte was read as a key";
    expect(in.empty());
}};

UTest an_incomplete_sequence_waits_for_the_next_read{"TerminalKeys/AnIncompleteSequenceWaitsForTheNextRead", [] {
    std::string in = "d\x1b[1;";
    expect(eq(decode_keys(in), std::vector<Key>{Key::Right}));
    expect(eq(in, "\x1b[1;"));
    in += "2B";
    expect(eq(decode_keys(in), std::vector<Key>{Key::Down}));
    expect(in.empty());

    std::string esc = "\x1b";
    expect(decode_keys(esc).empty());
    expect(eq(esc, "\x1b"));
    esc += "w";   // escape on its own, then a key
    expect(eq(decode_keys(esc), std::vector<Key>{Key::Up}));
}};

// mcpp.build.cmdlimits — the command-length budget, as data.
//
// WHY THIS MODULE EXISTS
//
// Seven times now, a build has died because some command grew past a ceiling
// nobody was tracking (#247, #261 ×2, #274, #344, 2026.8.5.3, 2026.8.5.4).
// Every one of them was found the same way — by crashing, at the end of a
// build, with an error that named neither the edge nor the cause:
//
//     ninja: fatal: posix_spawn: Argument list too long
//     LNK1170: line in command file contains 135135 or more characters
//     <cmd.exe returns a bare 127 and nobody prints anything>
//
// And every one of them was TRIGGERED by an unrelated change: #344 was a cache
// correctness fix that made object paths longer, #274 changed error-report
// granularity, 2026.8.5.4 was a CI pin bump. Nothing in the code connected
// "the thing I am editing" to "how long the resulting command will be",
// because that knowledge lived only in comments.
//
// So this is not another ceiling. It is the missing contract: a table of the
// channels a command travels through and what each of them will tolerate,
// plus the SYMPTOM each produces — because with this family the expensive part
// has never been the fix, it has been recognising it. The three messages above
// have nothing in common; the next one will look different again, and the
// table is where someone should be able to find it.
//
// The primary defence is still structural (see
// .agents/docs/2026-08-06-command-length-architecture.md §2 P1): unbounded
// payloads go through response files, newline-separated, to tools that have no
// per-line limit. This module is the backstop for whatever that misses.
export module mcpp.build.cmdlimits;

import std;

export namespace mcpp::build::cmdlimits {

// A command does not meet one limit — it travels through a chain of them.
enum class Channel {
    NinjaArgv,    // ninja creates the process directly
    PosixShell,   // ninja wraps in `sh -c "<whole command>"` — ONE argv entry
    CmdWrapper,   // `cmd /c` (gone since #261; kept so a revival is legible)
    RspContent,   // a response file, in total
    RspLine,      // a response file, per line
};

struct Limit {
    Channel          channel;
    std::size_t      bytes;
    std::string_view imposedBy;
    std::string_view symptom;   // what the user actually sees
    std::string_view remedy;
};

// Sizes are the real ones, not round numbers: MAX_ARG_STRLEN is 32 pages, and
// the 2 MiB ARG_MAX everyone reaches for first is the wrong limit (#344 spent
// a release believing it).
inline constexpr std::array<Limit, 5> kTable{{
    {Channel::NinjaArgv, 32u * 1024u, "Windows CreateProcess",
     "the command does not run; ninja reports a spawn failure",
     "route the unbounded payload through a response file"},

    {Channel::PosixShell, 128u * 1024u, "POSIX MAX_ARG_STRLEN (32 pages)",
     "ninja: fatal: posix_spawn: Argument list too long — names no edge, "
     "no file, no cause",
     "route the unbounded payload through a response file; note ARG_MAX "
     "(2 MiB) is NOT the limit that applies here"},

    {Channel::CmdWrapper, 8191u, "cmd.exe",
     "a bare exit code 127: the command never ran, so neither ninja nor mcpp "
     "prints anything",
     "do not let a rule need a shell — no redirection, no `cmd /c` (#261 "
     "removed the last one)"},

    {Channel::RspContent, 0u, "no practical limit on any supported tool",
     "n/a", "n/a"},

    {Channel::RspLine, 128u * 1024u, "link.exe / lib.exe",
     "LNK1170: line in command file contains N or more characters — names no "
     "target",
     "write the response file newline-separated, AND link with lld: a driver "
     "may generate a SECOND response file we do not control, and only the "
     "final tool's parser decides whether a line length matters"},
}};

constexpr const Limit& limit_of(Channel c) {
    for (auto const& l : kTable)
        if (l.channel == c) return l;
    return kTable[0];  // unreachable: every enumerator is in the table
}

// The tightest limit a command must satisfy on this platform, given whether
// its rule needs a shell. Response-file payloads are excluded — they are the
// thing being moved OUT of the command.
constexpr std::size_t inline_budget(bool hostIsWindows, bool needsShell) {
    if (hostIsWindows)
        return needsShell ? limit_of(Channel::CmdWrapper).bytes
                          : limit_of(Channel::NinjaArgv).bytes;
    // ninja runs POSIX commands through `sh -c`, so the whole command is a
    // single argv entry whichever way you look at it.
    return limit_of(Channel::PosixShell).bytes;
}

struct Overrun {
    Channel     channel;
    std::size_t actual;
    std::size_t allowed;
};

// `text` is what will land on the command line — NOT response-file content.
constexpr std::optional<Overrun> check_inline(std::string_view text,
                                              bool hostIsWindows,
                                              bool needsShell) {
    const std::size_t allowed = inline_budget(hostIsWindows, needsShell);
    if (text.size() <= allowed) return std::nullopt;
    const Channel c = hostIsWindows
        ? (needsShell ? Channel::CmdWrapper : Channel::NinjaArgv)
        : Channel::PosixShell;
    return Overrun{c, text.size(), allowed};
}

// Deliberately verbose: this message exists so that the eighth occurrence is
// diagnosed in seconds rather than in a release. It names the edge — which is
// exactly what every underlying error fails to do — and it fires while
// generating build.ninja, before anything has been compiled, rather than at
// the end of a build that has already spent its time.
std::string explain(std::string_view what, const Overrun& o) {
    auto const& l = limit_of(o.channel);
    return std::format(
        "{}: command is {} bytes, over the {} byte limit imposed by {}.\n"
        "       Left alone this surfaces as: {}\n"
        "       Fix: {}\n"
        "       Background: .agents/docs/2026-08-06-command-length-architecture.md",
        what, o.actual, l.bytes, l.imposedBy, l.symptom, l.remedy);
}

}  // namespace mcpp::build::cmdlimits

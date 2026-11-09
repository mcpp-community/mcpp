// mcpp.cli.cmd_check — `mcpp check platform [<dir>]` (SPEC-016 §2, §3).
//
// A platform package's statement checked as the engine reads it: the rows it
// defines (their segments, that they name no built-in row, that none repeats),
// the providers (that each serves a row, and which share a row at one
// priority), and the identities it serves, each with its provider and
// priority. Errors end the command with 1; warnings do not.

export module mcpp.cli.cmd_check;

import std;
import mcpplibs.cmdline;

export namespace mcpp::cli {

int cmd_check_platform(const mcpplibs::cmdline::ParsedArgs& parsed);

}  // namespace mcpp::cli

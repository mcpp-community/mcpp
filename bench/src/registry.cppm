// bench.registry — turning `--engines` text into engine objects.
//
// Adding an engine is: write bench.engines.<name>, then add ONE line to
// `make_engine`. Nothing else in the suite — runner, protocol, scenarios, CI —
// changes.
//
// A spec is either a bare name (`cmake`) or `name=program` (`mcpp=/path/to/mcpp`).
// The second form is what makes "is the new release faster?" a normal query:
//
//     --engines mcpp=/usr/bin/mcpp,mcpp=./target/x86_64-linux-gnu/*/bin/mcpp
//
// registers two mcpp engines that label themselves from the version each binary
// reports, so the two rows never collapse into one.
export module bench.registry;

import std;
import bench.engines.engine;
import bench.engines.mcpp;
import bench.engines.cmake;
import bench.engines.xmake;
import bench.engines.bazel;

export namespace bench {

// Makes a program spec independent of the current directory.
//
// Every measured command runs with its cwd set to the project under test, so a
// relative `--engines mcpp=./mcpp-old` resolves against the FIXTURE rather than
// the shell the user typed it in. The spawn then fails with "could not start",
// which is reported per cell as `exited -1` — a whole matrix of failures whose
// cause is one missing `./`. Resolving here, once, at the only place a spec
// becomes an engine, removes the class of bug rather than documenting it.
//
// Bare names (`mcpp`, `cmake`) are left alone: those are PATH lookups, which the
// child performs itself and which cwd does not affect.
std::string anchor_program(std::string program);

// A spec may carry ENGINE OPTIONS in brackets: `mcpp[schedule=on]=/path/to/mcpp`.
//
// This exists for opt-in behaviour. mcpp's split build schedule is a key in the
// MEASURED PROJECT's manifest, and the measured projects are pinned workloads —
// one of them belongs to someone else — so the suite had no way to reach the
// largest cold-build change in the release it was benchmarking, and reported
// "no improvement" for something worth 2.29x.
//
// Bracket options become environment variables for that engine's child only, so
// both arms sit in one report against one baseline on one machine. Unbracketed
// specs are untouched, and an unknown option is an error rather than a silently
// ignored word — a benchmark that quietly measures the default when you asked
// for the option is the exact failure this is meant to remove.
std::optional<std::pair<std::string, std::string>> engine_option(
    std::string_view engine, std::string_view key, std::string_view value);

std::unique_ptr<engines::Engine> make_engine(std::string_view spec);

// The default set, used when --engines is omitted. Order is the reporting order,
// chosen for reading: mcpp first (the subject), then the others.
std::vector<std::string> default_engine_specs();

}  // namespace bench

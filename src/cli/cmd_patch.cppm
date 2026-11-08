// mcpp.cli.cmd_patch — `mcpp patch edit <dependency>` and `mcpp patch save
// <dependency>` (mcpp#911 G8; docs/05 §"Patching a dependency").
//
// `edit` copies the dependency's package root as this build reads it (its
// existing patches applied) to `target/patch/<dependency>/`, beside a copy of
// the same tree it is compared with. `save` writes the difference as the next
// patch of the edge, under `patches/`, named from the package's root, and adds
// it to the edge's `patches`; the two copies are then removed.

export module mcpp.cli.cmd_patch;

import std;
import mcpplibs.cmdline;

export namespace mcpp::cli {

int cmd_patch_edit(const mcpplibs::cmdline::ParsedArgs& parsed);
int cmd_patch_save(const mcpplibs::cmdline::ParsedArgs& parsed);

}  // namespace mcpp::cli

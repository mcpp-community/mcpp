// mcpp.effects — what this run may do, and what it did (design 2026-10-26
// §5.5, D8): offline mode, the effects a caller denies, the first refusal, and
// the network and global-cache effects a run had.
export module mcpp.effects;

import std;

export namespace mcpp::effects {

// ── Network policy: offline mode ──────────────────────────────────────────
//
// One process-wide knob answering "may mcpp reach the network at all?", read
// at each point of use rather than threaded through a dozen signatures — the
// same idiom the older MCPP_NO_AUTO_INSTALL gate already uses.
//
// It lives in a leaf module on purpose: the consumers span pm (index refresh,
// package install) and build (toolchain auto-install), and a home inside one of
// those subsystems would make the other import it across a layer boundary.
//
// Set by `--offline` (cli pre-scan writes the env var) or by the user exporting
// MCPP_OFFLINE. "0" and the empty string mean off, matching MCPP_VERBOSE.
bool offline_mode();

// THE BOOTSTRAP TOOLS THIS RUN TAKES FROM THE MACHINE'S PATH (`[build]
// allow_host_tools`, design 2026-11-21, offline and toolchain defaults §3.3):
// MCPP_ALLOW_HOST_TOOLS, a comma list (`ninja,patchelf`; `none` takes none).
bool host_tool_allowed(std::string_view tool);

// ── Keys that hold for a whole run (SPEC-004 §2) ─────────────────────────
//
// `[build] offline`, `[build] allow_host_tools` and `[xlings] archives` hold
// for the run and for the programs it starts, so each is stated once, as its
// variable (MCPP_OFFLINE, MCPP_ALLOW_HOST_TOOLS, MCPP_ARCHIVES): the variable
// as the caller gave it (the override tier), else the project's statement,
// else the machine's. `state_key` writes a tier's statement unless a higher
// tier made one; `key_origin` names the tier that answered, so a message says
// where the value is changed. Every reader reads the variable alone.
enum class Tier { None, Machine, Project, Override };
struct KeyOrigin {
    Tier        tier = Tier::None;
    std::string where;   // "mcpp.toml [build] offline", "MCPP_OFFLINE", ...
};
void state_key(std::string_view var, Tier tier, std::string value, std::string where);
KeyOrigin key_origin(std::string_view var);

// How this run came to be offline, as the way back: "run without --offline
// (or unset MCPP_OFFLINE)", "set `offline = false` in mcpp.toml [build]", ...
std::string offline_remedy();

// THE ARCHIVE DIRECTORIES OF THIS RUN (`[xlings] archives`, MCPP_ARCHIVES;
// design 2026-11-21 §3.2): directories whose archives xlings takes by sha256,
// before the network and in its place offline.
std::vector<std::filesystem::path> archive_dirs();

// WHERE A PAYLOAD THE HOME LACKS MAY COME FROM. `Network`: every source, the
// local ones first. `Local`: offline, with archive directories -- the download
// cache and those directories, never the network. `None`: offline without an
// archive directory, where a run installs nothing, as offline always meant.
enum class InstallSources { Network, Local, None };
InstallSources install_sources();

// WHAT THIS RUN TOOK FROM OUTSIDE THE PROJECT (design 2026-11-21 §3.2): each
// payload it used, `<index>:<name>@<version>`, and each git clone. `mcpp fetch
// --to` writes them into a bundle; a payload's own dependencies are its
// provider's to close.
struct GitInput {
    std::string           url;
    std::string           commit;
    std::filesystem::path clone;
};
void note_package_used(std::string ref);
void note_git_used(GitInput input);
std::vector<std::string> packages_used();
std::vector<GitInput>    git_used();

// THE EFFECTS THIS RUN MAY NOT HAVE (design 2026-10-26 §5.5, D8).
//
// `--deny <effect,…>` and MCPP_DENY_EFFECTS name effects of docs/50 §4 the
// caller does not allow: `network`, `write-project`, `write-global-cache`,
// `exec-build-script`, `init-mcpp-home` (`read-project` cannot be denied).
// The two are a union, and nothing a project declares widens them: the
// constraint is the caller's. `--offline` is `--deny network`, and both
// spellings answer `offline_mode()`. A run that stops before a denied effect
// records it here, and the process exits 5 (SPEC-003): a caller tells "my
// constraint stopped it" from "it failed" by the status alone.
bool effect_denied(std::string_view effect);
// The deniable effects, for the CLI's validation of `--deny`.
const std::vector<std::string_view>& deniable_effects();
void note_effect_refused(std::string_view effect, std::string_view what);
// "network: install xim:gcc@15.1.0" -- the first refusal of this run, if any.
std::optional<std::string> effect_refused();

// WHETHER THIS RUN WRITES THE GLOBAL CACHE. Answers kept under the home (a
// compiler's probe, the provider's handshake) are read by every command and
// written only by a run whose effects include `write-global-cache`: one that
// plans or builds (it says so with `note_writes_global_cache`) and was not
// denied it. A query -- `self doctor`, `self env`, `toolchain list` -- declares
// no such effect, so it leaves the home exactly as it found it.
void note_writes_global_cache();
bool writes_global_cache();

// The older, narrower spelling: MCPP_NO_AUTO_INSTALL gates ONLY the toolchain
// auto-install, nothing else. Kept working (CI and existing setups export it)
// and kept next to offline_mode() so the two network knobs cannot drift apart.
bool no_auto_install();

// WHAT THIS RUN DID WITH THE NETWORK, AS OPPOSED TO WHAT IT MAY DO.
//
// docs/50 defines an envelope's `effects` as what running the command did,
// and `--protocol-version` as what a command may do. The per-run list used to
// be assembled from constants, so a plan that refreshed the index reported no
// `network` (#648 A4). A launcher of a network-bound child (an index refresh,
// an install, a git remote operation) calls `note_network_access` before it
// starts the child; an attempt that fails or times out still counts, because
// the effect is the access. Offline, those launchers return before this call.
// Kept beside offline_mode() for the reason stated above: every layer can reach
// this module, and the record has writers in several of them.
void note_network_access();
bool network_accessed();

}  // namespace mcpp::effects

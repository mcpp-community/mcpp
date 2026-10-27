// retarget.cpp -- telling a retargetable driver what it targets (phase 2 of
// prepare_build): the target triple, the sysroot and the multilib directory a
// clang driver is given when the toolchain resolved for a cross target. Moved
// out of toolchain.cpp whole; declared in :state.

module mcpp.build.prepare;
import :state;
import mcpp.build.prepare_inputs;
import std;
import mcpp.build.engine_package;
import mcpp.diag;
import mcpp.build.refusal;
import mcpp.build.version_floor;
import mcpplibs.os.platform;
import mcpp.toolchain.platform;
import mcpp.toolchain.apple_sdk;
import mcpp.effects;
import mcpp.build.command_line;
import mcpp.manifest;
import mcpp.source_kind;
import mcpp.toolchain.hostflags;   // the compile-token producer the package std module reuses
import mcpp.toolchain.detect;
import mcpp.toolchain.dialect;
import mcpp.toolchain.fingerprint;
import mcpp.toolchain.msvc;
import mcpp.toolchain.registry;
import mcpp.toolchain.linkmodel;
import mcpp.toolchain.lifecycle;
import mcpp.toolchain.stdmod;
import mcpp.freestanding.target;   // the target sysroot layout (libdir)
import mcpp.freestanding.linkline; // the ISA profile, for the std module command
import mcpp.toolchain.post_install;
import mcpp.toolchain.abi;
import mcpp.toolchain.triple;
import mcpp.toolchain.base;
import mcpp.toolchain.base_probe;
import mcpp.home;
import mcpp.build.plan;
import mcpp.build.flags;          // compute_flags — the per-role contracts (#418)
import mcpp.build.graph_shape;  // #407: the graph says which mode wrote it
import mcpp.pack.abi_tag;      // the tag a prebuilt dependency is checked against
import mcpp.pack.prebuilt;     // …and the check itself
import mcpp.pack.stage_tree;   // where `${mcpp.stage_dir}` points, and its manifest
import mcpp.build.build_program;
import mcpp.build.backend;      // BuildOptions for the tool sub-build
import mcpp.build.ninja;        // make_ninja_backend — driving that sub-build
import mcpp.config;
import mcpp.provider;
import mcpp.provider.select;
import mcpp.provider.runtime_selection;
import mcpp.runtime.binding;
import mcpp.toolchain.post_install;
import mcpp.fetcher;
import mcpp.fetcher.progress;
import mcpp.ui;
import mcpp.invocation;

namespace mcpp::build {

std::expected<void, std::string>
step2_retarget_for_retargetable_driver(PrepareState& state) {
      // ── A retargetable driver has to be TOLD what it is targeting ────────
      //
      // `tc.targetTriple` comes from `-dumpmachine`, and for every cross target
      // that worked before this it was right for a reason that does not
      // generalise: those targets use a DISTINCT compiler binary
      // (`x86_64-w64-mingw32-g++`, `aarch64-linux-musl-g++`), whose own
      // -dumpmachine reports the cross triple. Clang is ONE binary that emits
      // every target it was built with, so -dumpmachine always answers with the
      // host — and nothing downstream ever learns otherwise.
      //
      // Measured before this line existed:
      //
      //   $ mcpp build --target riscv64-none-elf
      //       Resolved llvm@22.1.8 → riscv64-none-elf → …/bin/clang++
      //       Finished dev [unoptimized + debuginfo] in 0.47s
      //   $ ls target/
      //       x86_64-linux-gnu/          ← an ELF for the host, reported as riscv64
      //
      // That is E1: success reported, host artifact produced. The output
      // directory, the fingerprint, the cache key and the flag layer all read
      // `tc.targetTriple`, so correcting it here corrects all of them at once —
      // which is the point of there being one field rather than five answers.
      //
      // THIS USED TO BE SCOPED TO FREESTANDING, WITH THIS REASON:
      //
      //     The hosted cross targets already resolve a per-target binary, and
      //     overwriting their probed triple would replace a measured fact with
      //     an assumed one for no gain.
      //
      // That was true while every hosted cross was served by a payload. It
      // stops being true when the TARGET SIDE comes from the dependency graph:
      // the C library, the C++ runtime and the platform's own implementation are
      // then packages built from source, and the compiler is an ordinary clang —
      // whose `-dumpmachine` answers the host, exactly as the paragraph above
      // describes for freestanding.
      //
      // Measured 2026-08-23, with an explicit `[target.aarch64-macos]
      // toolchain = "llvm@…"`. The manifest's cfg evaluation used the REQUESTED
      // target, so the C library's aarch64 headers were on the command line; the
      // toolchain's own triple was still the host's, so code generation was
      // x86_64. Two answers to one question, in one command:
      //
      //     okm_float_assert.c: the C library and the compiler disagree about
      //     LDBL_DIG  ('33 == 18')          33 = aarch64 binary128, 18 = x87
      //
      // ⇒ The condition is now the property the first paragraph of this comment
      // already names: a RETARGETABLE driver has to be told. gcc is not one — a
      // gcc payload IS its target — so the mingw and musl-gcc crosses keep
      // answering from `-dumpmachine`, which for them remains a measured fact.
      if (!state.overrides.target_triple.empty()) {
          if (auto want = mcpp::toolchain::triple::parse(state.overrides.target_triple);
              want && (want->is_freestanding()
                       || state.tc->compiler == mcpp::toolchain::CompilerId::Clang))
          {
              state.tc->targetTriple = want->str();

              // AND THE GATE THAT ALREADY EXISTS FOR THIS, APPLIED WHERE THE
              // ANSWER IS KNOWN.
              //
              // `discover_link_runtime_dirs` refuses to report these
              // directories for a target that carries its own sysroot, and the
              // refusal never fired: that function runs during DETECTION,
              // before this line, when `targetTriple` is still the HOST's. The
              // gate read a host triple and answered correctly about it.
              //
              // The artefact is what showed it. An Android link line carried
              //
              //     -L <ndk>/toolchains/llvm/prebuilt/linux-x86_64/lib/
              //        x86_64-unknown-linux-gnu
              //
              // whose last component is this machine's triple, produced by
              // `root / "lib" / targetTriple` -- so the string names the
              // question that was asked. Those are the compiler's own host
              // runtime directories; an Android artefact must resolve libc++,
              // the crt objects and the loader from the NDK's sysroot, and the
              // hermetic check reported exactly that failure with six host
              // objects.
              //
              // Cleared rather than re-derived. Re-running the discovery with
              // the final triple would also change what every OTHER clang cross
              // target gets, and those are measured as they stand; the claim
              // being made here is only the one the gate already states.
              if (want->has_own_sysroot()) state.tc->linkRuntimeDirs.clear();

              // And the flag that says it to the driver — for a HOSTED target
              // only. Freestanding already emits its own `--target`, together
              // with the ISA flags that must accompany it
              // (freestanding/target.cppm); a second one here would be the same
              // decision in two places.
              if (!want->is_freestanding()
                  && state.tc->compiler == mcpp::toolchain::CompilerId::Clang) {
                  state.tc->crossTargetFlag =
                      "--target=" + want->llvm_triple(
                          min_platform_version(*state.m, *want, state.tc->binaryPath));

                  // AND THE SAME FLAG ON THE std MODULE'S OWN COMMANDS, FOR A
                  // PAYLOAD THAT SERVES MORE THAN ONE TARGET.
                  //
                  // The std module is built by its own command assembly
                  // (clang.cppm), not by the compile flags, so a decision made
                  // only here reaches every translation unit and not that. For
                  // most toolchains the omission cannot be seen: a payload
                  // whose compiler IS its target finds its own headers, and a
                  // package-provided module carries the target inside
                  // `stdModuleFlags`.
                  //
                  // ONE NDK SERVES BOTH ANDROID ARCHES, which is the property
                  // that makes this necessary and is stated in the row's own
                  // pin: `android-ndk@<v>` names no arch, so `--target` is the
                  // only thing that says which. Without it the precompile
                  // resolved libc++'s `#include <__config>` against the
                  // building machine and stopped there.
                  //
                  // NOT `has_own_sysroot()`, though both rows that answer true
                  // to it are SDKs with their own sysroot. Emscripten's `em++`
                  // serves exactly one target and needs no flag -- the verified
                  // wasm loop is measured without it -- so widening the gate to
                  // the predicate would add a flag to a command that does not
                  // want one. The property here is "one payload, several
                  // targets", and Android is the only row that has it; a future
                  // row brings its own measurement.
                  if (want->is_android()) {
                      state.tc->stdModuleTargetFlags = " " + state.tc->crossTargetFlag;
                      // BIONIC'S ctype HEADER AND A MODULE'S EXPORT RULES.
                      //
                      // bionic declares `isalnum` and its neighbours
                      // `static inline`, and libc++'s module surface exports
                      // them with `using std::isalnum`. A using-declaration
                      // cannot export a name with internal linkage, so the
                      // precompile fails on 14 names at once. Defining the
                      // macro empty makes those declarations extern, which is
                      // what every other C library this engine compiles
                      // against already does.
                      //
                      // Scoped to the std module and not to every unit: the
                      // rule being satisfied is about exporting from a module,
                      // and a translation unit that includes <ctype.h>
                      // directly is entitled to bionic's inline definitions.
                      // `xim:android-ndk`'s own install-time self-test reaches
                      // the identical conclusion from the other direction.
                      //
                      // AND THE PAYLOAD MAY SAY SO ITSELF. The recipe applies
                      // this same define in that self-test, so it is a fact
                      // the payload already holds; `std_module_defines` in
                      // `.mcpp-toolchain.json` is the channel for it, and the
                      // define below is what a payload that ships no
                      // descriptor still gets. The two are not added
                      // together: a descriptor that names defines is the
                      // payload's complete answer for this channel, and
                      // appending to it would mean a payload could not
                      // withdraw a define this engine once needed.
                      auto stdDefines = [&]() -> std::vector<std::string> {
                          auto desc =
                              mcpp::toolchain::payload_descriptor_for_compiler(
                                  state.tc->binaryPath);
                          if (desc && *desc && !(*desc)->stdModuleDefines.empty())
                              return (*desc)->stdModuleDefines;
                          return { "__BIONIC_CTYPE_INLINE=" };
                      }();
                      for (auto const& def : stdDefines)
                          state.tc->stdModuleTargetFlags += " -D" + def;
                  }

                  // ── iOS: THE COMPILER IS OURS, THE SDK IS THE MACHINE'S ──
                  //
                  // The three iOS rows pin `llvm@23.1.3` -- any sufficiently
                  // new clang emits arm64 Mach-O for an iOS deployment target
                  // -- and take their headers and stub libraries from the
                  // machine's Xcode, which is where the whole item shrinks to
                  // a located sysroot. `aarch64-macos` is verified on exactly
                  // this split and is the precedent.
                  //
                  // LOCATED HERE, ONCE. Three later sites need the answer (the
                  // compile flags, the link line, and the std module's own
                  // command), and a function that probes the machine is the
                  // wrong thing to call three times: `xcrun` shells out, and
                  // three answers can differ if the developer directory
                  // changes mid-build.
                  //
                  // AND ITS ABSENCE IS A REFUSAL THAT NAMES THE SDK. The
                  // recorded host-surface rule is that a host dependency must
                  // be minimal, named, and never a fallthrough; the iOS SDK
                  // and `simctl` are the two this platform adds, both in the
                  // "proprietary runtime that exists only on its own OS"
                  // category. A build that continued without the SDK would
                  // fail in the driver's header search, naming a file rather
                  // than the thing that is missing.
                  if (want->is_ios()) {
                      // READ, NOT RE-DERIVED. The refusal above located it
                      // before any payload was resolved, and that is the one
                      // `xcrun` call this build makes.
                      //
                      // An empty answer here cannot happen through the
                      // `--target` path, and a line that prints when it does
                      // is cheaper than a branch that pretends it cannot: the
                      // row could be reached one day by a route that skipped
                      // the gate, and an iOS build with no `-isysroot` is a
                      // macOS artefact with an iOS triple on it.
                      if (!state.appleSdkLocated) {
                          return std::unexpected(std::format(
                              "internal: target {} reached toolchain "
                              "resolution without its SDK being located; the "
                              "gate that locates it did not run for this "
                              "request", want->str()));
                      }
                      state.tc->base.apple.root = *state.appleSdkLocated;
                      if (state.appleSdkFound) {
                          state.tc->base.apple.name    = state.appleSdkFound->name;
                          state.tc->base.apple.version = state.appleSdkFound->version;
                      }
                      auto sdk = state.appleSdkLocated;
                      // AND THE std MODULE'S OWN COMMAND, WHICH IS A SEPARATE
                      // CHANNEL. Same reason the Android rows set it: the
                      // module is precompiled by `clang.cppm`'s own assembly
                      // rather than by the compile flags, so a decision made
                      // only in the flag builder reaches every translation
                      // unit and not the module they all import. Without the
                      // SDK here the precompile resolves libc++'s
                      // `#include <__config>` against the macOS SDK and the
                      // module is built for the wrong platform.
                      //
                      // QUOTED, as every path this string carries is (see the
                      // package-provided producer, which uses `shq` for each
                      // `-isystem`). The string is spliced into a shell
                      // command, and an Xcode installed as `Xcode 16.app` is
                      // a path with a space in it.
                      state.tc->stdModuleTargetFlags =
                          " " + state.tc->crossTargetFlag
                          + " -isysroot " + mcpp::build::command_line::quote(sdk->string());
                  }
              }
          }
          if (auto want = mcpp::toolchain::triple::parse(state.overrides.target_triple);
              want && want->is_freestanding())
          {
              // `import std` is structurally hosted, and turning it off is the
              // SAME fact as the line above, not a second policy: libc++'s
              // std.cppm is one module over the whole library, including the
              // parts that are threads, filesystem and iostreams. There is no
              // subset of it to precompile.
              //
              // Left on, the failure is neither early nor legible — measured:
              //
              //   error: std module precompile failed (rc=1):
              //   .../include/c++/v1/__config:13:10: fatal error:
              //       '__config_site' file not found
              //
              // which reads as a broken toolchain payload and says nothing about
              // the target. The freestanding std subset a user actually wants is
              // an ordinary package (`mcpplibs.std.freestanding`), so mcpp's job
              // here is to stop pretending the hosted one exists and to say
              // where the other one is.
              state.tc->clear_std_modules();

              // ── The target's C library, resolved like its compiler ─────────
              //
              // The row in kKnownTargets names it, exactly as it names the
              // toolchain pin, and it is installed through the same channel a
              // project's `[xlings] deps` use (see the materialization above).
              // Resolved HERE because the config is already open; the flag
              // builder only reads the result.
              //
              // Absent is not an error at this point: the install happens
              // earlier in this function and may legitimately not have run yet
              // on a first pass. What follows would then simply not add the
              // paths, and the link fails naming the missing libc — which is the
              // truthful message either way.
              const std::string want_sysroot =
                  mcpp::toolchain::triple::effective_sysroot(
                      *want, base_statement_text(state, *want));
              // A C library named by path (design 2026-10-23 §3.1): the same
              // `<root>/{include,lib}/<libdir>` layout, nothing to install.
              if (want_sysroot.starts_with("path:")) {
                  const std::filesystem::path dir = want_sysroot.substr(5);
                  std::error_code ec2;
                  if (!std::filesystem::is_directory(dir, ec2)) {
                      refusal::record(refusal::Code::TargetBase);
                      return std::unexpected(std::format(
                          "[target.{}].sysroot = {{ path = \"{}\" }} does not exist", want->str(),
                          dir.generic_string()));
                  }
                  if (auto spec = mcpp::freestanding::resolve(*want)) {
                      state.tc->base.multilibRoot = dir;
                      state.tc->base.package      = dir.filename().string();
                      const auto inc = dir / "include" / std::string(spec->libdir);
                      const auto lib = dir / "lib" / std::string(spec->libdir);
                      if (std::filesystem::is_directory(inc, ec2)) state.tc->base.multilibInclude = inc;
                      if (std::filesystem::is_directory(lib, ec2)) state.tc->base.multilibLib = lib;
                  }
              } else if (!want_sysroot.empty()) {
                  if (auto cfg3 = state.get_cfg(true); cfg3) {
                      auto ref = mcpp::provider::parse_ref(want_sysroot);
                      auto xl  = mcpp::config::make_xlings_env(**cfg3);
                      // INSTALLED, NOT MERELY LOOKED UP — THE SAME CHANNEL
                      // THE ROW'S TOOLCHAIN PIN GOES THROUGH.
                      //
                      // The row names two things and only one of them used to
                      // be made to exist: `pin` went through
                      // `resolve_xpkg_path(…, PayloadIntent::Use, …)` while
                      // `sysroot` was a pure lookup that returned nullopt and
                      // let the whole block below be skipped without a word.
                      //
                      // Measured 2026-08-26 in a clean environment (an empty
                      // home, so mcpp's registry starts fresh):
                      //
                      //     Target riscv64-none-elf
                      //            c-abi  picolibc-riscv (…, prebuilt)
                      //     error: 'stdio.h' file not found
                      //
                      // The report named the C library and the build could not
                      // find its headers. mcpp's own bare-metal CI installs it
                      // by hand, which is why no test ever saw this — every
                      // bare-metal e2e runs on a machine where the gap has
                      // already been papered over.
                      //
                      // OFFLINE AND `MCPP_NO_AUTO_INSTALL` ARE THE FETCHER'S
                      // DECISION, not re-derived here. One question, one place
                      // that answers it — asking it twice is the shape this
                      // whole release exists to remove.
                      mcpp::fetcher::Fetcher srFetcher(**cfg3);
                      mcpp::fetcher::InstallProgressHandler srProgress;
                      std::optional<std::filesystem::path> dir;
                      if (auto p = srFetcher.resolve_xpkg_path(
                              want_sysroot, mcpp::fetcher::PayloadIntent::Use, &srProgress))
                          dir = p->root;
                      else
                          dir = mcpp::provider::payload(*mcpp::provider::open(xl), ref);
                      if (dir) {
                          if (auto spec = mcpp::freestanding::resolve(*want)) {
                              const auto inc =
                                  *dir / "include" / std::string(spec->libdir);
                              const auto lib =
                                  *dir / "lib"     / std::string(spec->libdir);
                              std::error_code ec2;
                              state.tc->base.multilibRoot = *dir;
                              state.tc->base.package  = ref.name;
                              if (std::filesystem::is_directory(inc, ec2))
                                  state.tc->base.multilibInclude = inc;
                              if (std::filesystem::is_directory(lib, ec2))
                                  state.tc->base.multilibLib = lib;
                          }
                      }
                  }
              }
          }
      }
      return {};
}

} // namespace mcpp::build

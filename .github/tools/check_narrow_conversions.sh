#!/usr/bin/env bash
#
# Guard: a path that came out of a directory walk is never narrowed directly.
#
# WHY
#
# On Windows `std::filesystem::path::string()` converts the native (wide) name
# through the process ANSI code page and THROWS std::system_error when a
# character has no spelling there — "No mapping for the Unicode character
# exists in the target multi-byte code page". Off Windows the same call is a
# copy that cannot fail, so nothing on Linux or macOS — including their tests —
# can see the hazard.
#
# It has cost two incidents, each wearing a different mask:
#
#   #230  a walked index tree held a CJK-named issue template; the throw
#         escaped to std::terminate → __fastfail → git-bash reported a bare
#         exit 127, which reads as "command not found".
#   #516  cpp-httplib ships test/www/<CJK>Dir/ and the `include_dirs = { "*" }`
#         convention walks the whole extracted tarball; the throw escaped to
#         main()'s catch as `internal: unhandled exception`, which reads as an
#         extraction/encoding bug in the downloader.
#
# #231 hardened three call sites and missed a fourth — `is_excluded_walk_dir`,
# which runs ONE LINE EARLIER in the same walk loop. A fifth site is what this
# script exists to make expensive.
#
# THE RULE
#
#   * Comparing against ASCII literals?      Compare as `path`. Do not narrow.
#   * Need a stable identity (hash, key)?    `u8string()` — UTF-8 everywhere,
#                                            never touches the code page.
#   * Need a build-facing string (compiler
#     argument, ninja file, CDB)?            `mcpp::modgraph::try_narrow()`,
#                                            and handle the nullopt.
#
# WHAT THIS DOES AND DOES NOT CATCH
#
# It greps the leaf layers that walk trees mcpp does not control. It catches a
# NEW direct narrowing written there. It does NOT catch a path narrowed after
# being passed out to another layer — that is what the try_narrow convention is
# for, and no grep can enforce it. Do not read a pass here as "audited".
#
# `.extension()` is deliberately NOT matched: an extension is ASCII in every
# case that reaches these predicates, so matching it would produce only noise —
# and noise is how a gate gets suppressed.
#
# Escape hatch: `// NARROW-OK: <reason>` on the line itself or within the two
# lines above it. Use it when the input provably cannot carry an unspellable
# name, and say why — a bare marker with no argument is worse than no gate,
# because it reads as "someone checked".
#
# Usage: bash .github/tools/check_narrow_conversions.sh [repo_dir]

set -uo pipefail

REPO_DIR="${1:-$(pwd)}"
cd "$REPO_DIR" || { echo "FAIL: cannot cd to $REPO_DIR" >&2; exit 1; }

# SCOPE, and why it is this narrow.
#
# The hazard needs a path from a tree MCPP DOES NOT CONTROL. Two directories
# qualify: src/modgraph walks arbitrary package and project trees, and
# src/scaffold enumerates third-party template providers.
#
# The first draft of this guard also covered src/pack and src/manifest and
# produced 22 hits, ~20 of them false: src/pack narrows names MCPP ITSELF
# produced (staging roots, built binaries, strip artifacts — all derived from
# validated ASCII package/target names), and src/manifest only ever narrows an
# `.extension()`. A gate with twenty false positives is a gate that gets
# suppressed within a month, and the suppression then becomes the only record
# that a rule existed. The real hazards in those two directories were fixed by
# hand instead (pack/digest.cppm, which feeds on an unfiltered
# recursive_directory_iterator over a published package).
#
# So: a pass here does NOT mean "the tree is audited". It means no NEW direct
# narrowing was written where this class originates.
#
# ⚠️ `modules/manifest/src/glob.cppm` is in scope even though the rest of that
# package is not. It is the glob walker itself -- the file this guard's
# background note names -- and it moved out of `src/modgraph/` in the subsystem
# split. A scope written as directory names shrinks silently when a file moves,
# and the pass keeps reading the same either way, so the file is named
# directly rather than inferred from where it currently sits.
SCAN_DIRS="src/modgraph src/scaffold"
SCAN_FILES="modules/manifest/src/glob.cppm"

PATTERN='\.(filename|stem)\(\)\.(generic_)?string\(\)'

fail=0
found=0

scan_one() {
  file="$1"
    # Strip // line comments before matching: several of these files DESCRIBE
    # the forbidden call in prose (that is the point of the comments), and a
    # guard that trips on its own documentation gets deleted.
    while IFS=: read -r lineno text; do
      [ -n "${lineno:-}" ] || continue
      found=1
      # NARROW-OK on the line itself, or on either of the two lines above it.
      ctx=$(sed -n "$(( lineno > 2 ? lineno - 2 : 1 )),${lineno}p" "$file")
      case "$ctx" in
        *NARROW-OK:*) continue ;;
      esac
      echo "FAIL: $file:$lineno narrows a path directly:" >&2
      echo "        ${text# }" >&2
      fail=1
    done < <(sed 's://.*::' "$file" | grep -nE "$PATTERN")
}

for dir in $SCAN_DIRS; do
  [ -d "$dir" ] || { echo "FAIL: $dir does not exist — this guard has gone stale" >&2; exit 1; }
  while IFS= read -r f; do scan_one "$f"; done \
    < <(find "$dir" -type f \( -name '*.cppm' -o -name '*.cpp' -o -name '*.hpp' \) | sort)
done

for f in $SCAN_FILES; do
  # Named files are asserted to EXIST. A moved file that silently drops out of
  # scope is the failure this list was added to prevent, so its absence has to
  # be louder than its presence.
  [ -f "$f" ] || { echo "FAIL: $f does not exist — this guard has gone stale" >&2; exit 1; }
  scan_one "$f"
done

if [ "$fail" = 1 ]; then
  cat >&2 <<'EOF'

  Use one of:
    - compare as std::filesystem::path            (ASCII literals; no narrowing)
    - p.u8string()                                (stable identity: hashes, keys)
    - mcpp::modgraph::try_narrow(p)               (build-facing; handle nullopt)
  or annotate with `// NARROW-OK: <why this input cannot carry such a name>`.

  Background: mcpp#516, mcpp#230, modules/manifest/src/glob.cppm.
EOF
  exit 1
fi

if [ "$found" = 0 ]; then
  echo "ok: no direct path narrowing in $SCAN_DIRS $SCAN_FILES"
else
  echo "ok: every direct narrowing in $SCAN_DIRS $SCAN_FILES carries a NARROW-OK rationale"
fi
exit 0

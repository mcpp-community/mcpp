#!/usr/bin/env bash
# check_docs_style.sh — the mechanically checkable half of .agents/skills/mcpp-docs-style.
#
# A style rule nobody can check is a suggestion. This covers the three rules
# that are decidable from the text alone:
#
#   1. headings are not questions and not conversational fragments
#   2. reference docs do not address the reader in the second person
#      (tutorials do — they are listed below, not inferred)
#   3. docs/X.md and docs/zh/X.md have the same heading structure
#
# What it deliberately does NOT check: whether a claim's strength matches its
# evidence. That is the most important rule in the skill and it needs a reader.
#
# Usage: bash .github/tools/check_docs_style.sh
set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1

fail=0
bad() { echo "FAIL: $*"; fail=1; }

# Tutorials address the reader on purpose: the reader is following along.
TUTORIALS="00-getting-started.md 01-examples.md 04-build-from-source.md"

# Headings outside fenced code blocks. `# …` inside a ```sh block is a shell
# comment, and counting it made the first version of this script report a
# parity gap in docs/10 that did not exist.
headings() {
  awk '
    /^```/ { infence = !infence; next }
    !infence && /^#{1,6} / { print }
  ' "$1"
}

for f in docs/*.md docs/zh/*.md; do
  base="$(basename "$f")"

  # ── 1. heading register ───────────────────────────────────────────────
  while IFS= read -r h; do
    case "$h" in
      *"?"*|*"吗"*|*"呢"*)
        bad "$f: question heading — use a noun phrase: $h" ;;
    esac
    case "$h" in
      *"一段话"*|*"讲完"*|*"姊妹篇"*|*"干活"*|*"怎么"*|*"会怎样"*|*"不许"*)
        bad "$f: conversational heading: $h" ;;
      *"The whole idea"*|*"in one paragraph"*|*"Consuming one"*|*"the thing that"*)
        bad "$f: conversational heading: $h" ;;
    esac
  done < <(headings "$f")

  # ── 2. second person in reference docs ────────────────────────────────
  case " $TUTORIALS " in
    *" $base "*) ;;
    *)
      # Prose only: quoted program output keeps its own wording ("your
      # toolchain : …" comes out of mcpp and must be reproduced verbatim), so
      # fenced blocks and lines that are clearly transcript are skipped.
      hits=$(awk '
        /^```/ { infence = !infence; next }
        infence { next }
        {
          # Inline code spans are quoted material — mcpp prints
          # `did you mean ...?` and `your toolchain : ...`, and reproducing
          # those verbatim is required, not a style lapse. Blank them before
          # matching rather than exempting whole lines, so prose on the same
          # line is still checked.
          line = $0
          gsub(/`[^`]*`/, "", line)
          if (line ~ /\<you\>|\<your\>|\<yours\>/ || line ~ /你/)
            print FILENAME ":" FNR ": " $0
        }
      ' "$f")
      if [[ -n "$hits" ]]; then
        while IFS= read -r line; do
          bad "$f: second person in a reference doc: ${line#*: }"
        done <<< "$hits"
      fi ;;
  esac
done

# ── 3. bilingual heading parity ───────────────────────────────────────────
for f in docs/*.md; do
  z="docs/zh/$(basename "$f")"
  [[ -f "$z" ]] || continue
  # The LEVEL SEQUENCE, not just the count: two documents can have the same
  # number of headings and still disagree about which are sections and which
  # are subsections. Comparing counts would call that identical.
  levels() { headings "$1" | sed -E 's/^(#+).*/\1/' | awk '{print length($0)}'; }
  if ! diff -q <(levels "$f") <(levels "$z") >/dev/null; then
    ne=$(headings "$f" | wc -l); nz=$(headings "$z" | wc -l)
    bad "$(basename "$f"): heading structure differs (en=$ne zh=$nz headings); first divergence:"
    diff <(levels "$f") <(levels "$z") | head -4 | sed 's/^/       /'
  fi
done

if [[ "$fail" -eq 0 ]]; then
  echo "OK: docs style checks pass"
fi
exit "$fail"

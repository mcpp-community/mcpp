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
#   4. a table's header cells follow the same register as a heading
#
# Rule 4 exists because rule 1 read only lines beginning with `#`, and a column
# header is a heading by every property that matters: it names a topic, it is
# read out of order, and it is what a reader scans. `| 部分 | 大致相当于谁的活 |`
# passed every check in this file while being the plainest register violation
# in the tree.
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
TUTORIALS="01-getting-started.md 03-examples.md 90-build-from-source.md"

# Headings outside fenced code blocks. `# …` inside a ```sh block is a shell
# comment, and counting it made the first version of this script report a
# parity gap in docs/10 that did not exist.
headings() {
  awk '
    /^```/ { infence = !infence; next }
    !infence && /^#{1,6} / { print }
  ' "$1"
}

# THE REGISTER RULES COVER docs/specs/ TOO, AND USED NOT TO.
#
# The glob was `docs/*.md docs/zh/*.md`, which does not descend, so the four
# specifications were exempt from rules 1 and 2 by accident rather than by
# decision. They are in scope now. The PARITY loop below still is not: the
# specifications are 简体中文 with no docs/zh/ mirror, and whether that changes
# is an open question rather than a defect this script should assert.
for f in docs/*.md docs/zh/*.md docs/specs/*.md; do
  base="$(basename "$f")"

  # ── 1. heading register ───────────────────────────────────────────────
  #
  # An interrogative WORD is the test, not a question mark. `谁在为这个落差付账`
  # and `打什么由谁决定` carry no `?` and are questions all the same, and the
  # first version of this rule matched `?`/`吗`/`呢` and passed both.
  #
  # THE ONE EXEMPTION IS BY NAME. `# 00 —— mcpp 是什么` mirrors the English
  # `What mcpp Is`, which is a noun clause rather than a question, and a chapter
  # title is the chapter's registered name: it appears in three indexes and in
  # every citation of the chapter. Exempting it here states the decision;
  # narrowing the rule to `##` and deeper would have hidden it.
  while IFS= read -r h; do
    if [[ "$f" == "docs/zh/00-what-mcpp-is.md" && "$h" == "# 00 —— mcpp 是什么" ]]; then
      continue
    fi
    # Inline code is quoted material: `cxx_stdlib` and `mcpp.why.toolchain`
    # must not be read for register.
    bare="$(sed -E 's/`[^`]*`//g' <<< "$h")"
    case "$bare" in
      *"?"*|*"？"*|*"吗"*|*"呢"*)
        bad "$f: question heading — use a noun phrase: $h" ;;
      *"谁"*|*"哪"*|*"如何"*|*"为何"*|*"什么"*|*"多少"*|*"怎样"*|*"怎么"*)
        bad "$f: interrogative heading — use a noun phrase (…的原因 / …的依据 / …的范围): $h" ;;
    esac
    case "$bare" in
      *"一段话"*|*"讲完"*|*"姊妹篇"*|*"干活"*|*"会怎样"*|*"不许"*|*"跟上"*|*"付账"*|*"长什么样"*)
        bad "$f: conversational heading: $h" ;;
      *"The whole idea"*|*"in one paragraph"*|*"Consuming one"*|*"the thing that"*)
        bad "$f: conversational heading: $h" ;;
    esac
  done < <(headings "$f")

  # ── 4. table header register ──────────────────────────────────────────
  #
  # A header row is the row directly above the `|---|---|` separator, so the
  # separator is what identifies it; matching every `|` line would read data.
  while IFS= read -r cell; do
    case "$cell" in
      *"?"*|*"？"*|*"吗"*|*"呢"*)
        bad "$f: question in a table header — use a noun phrase: $cell" ;;
      *"谁"*|*"哪"*|*"如何"*|*"为何"*|*"什么"*|*"多少"*|*"怎样"*|*"怎么"*)
        bad "$f: interrogative table header — use a noun phrase: $cell" ;;
    esac
    shopt -s nocasematch
    if [[ "$cell" =~ ^(what|which|who|how|why|where|whether)([[:space:]]|$) ]]; then
      bad "$f: interrogative table header — use a noun phrase: $cell"
    fi
    shopt -u nocasematch
  done < <(awk '
    /^```/ { infence = !infence; prev = ""; next }
    infence { next }
    /^\|[[:space:]:|-]+\|[[:space:]]*$/ && prev ~ /^\|/ {
      line = prev
      gsub(/`[^`]*`/, "", line)          # quoted material, as above
      n = split(line, cells, "|")
      for (i = 2; i < n; i++) {
        c = cells[i]
        gsub(/^[[:space:]]+|[[:space:]]+$/, "", c)
        if (c != "") print c
      }
    }
    { prev = $0 }
  ' "$f")

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

#!/usr/bin/env bash
# check_docs_structure.sh — the rules of .agents/skills/mcpp-docs-style that are
# about WHERE a document lives rather than how its prose reads.
#
# check_docs_style.sh covers the register. This covers the architecture:
#
#   1. no user chapter cites a design record
#   2. a specification cites one only in its metadata table
#   3. every docs/*.md path named outside .agents/ resolves
#   4. every specification is listed in all three indexes
#   5. every specification has a metadata table and a change record
#   6. no emoji under docs/ or in a top-level README
#   7. the generated design-record index is current
#   8. a new design record declares its subject and status
#
# What it deliberately does NOT check: whether a chapter documents what is
# implemented, whether an assertion's strength matches its evidence, or whether
# a surface's coverage has a denominator. Those need a reader, and they are the
# three most important rules in the skill.
#
# Usage: bash .github/tools/check_docs_structure.sh
set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1

fail=0
bad() { echo "FAIL: $*"; fail=1; }

# ── 1. no user chapter cites a design record ──────────────────────────────
#
# The two READMEs are exempt BY DECISION, not by accident: they are where the
# three-tree architecture is published, so they name `.agents/docs/` in order to
# say what it holds and that a chapter must not send a reader there. Every other
# file under docs/ that names it has delegated a question it should have
# answered -- see "引用方向是规则" in the skill.
ARCHITECTURE_PAGES="docs/README.md docs/zh/README.md"
for f in docs/*.md docs/zh/*.md; do
  case " $ARCHITECTURE_PAGES " in *" $f "*) continue ;; esac
  if grep -n '\.agents/' "$f" >/dev/null 2>&1; then
    while IFS= read -r hit; do
      bad "$f: a user chapter cites a design record: ${hit}"
    done < <(grep -n '\.agents/' "$f")
  fi
done

# ── 2. a specification cites a design record only in its metadata table ───
#
# Provenance belongs in the metadata row. A citation in the body is the same
# delegation rule 1 refuses, one tree over.
for f in docs/specs/*.md; do
  [ "$(basename "$f")" = "README.md" ] && continue
  # The metadata table is the leading block: everything before the first `##`.
  body_start=$(grep -n '^## ' "$f" | head -1 | cut -d: -f1)
  [ -z "$body_start" ] && body_start=1
  if awk -v s="$body_start" 'NR >= s && /\.agents\//' "$f" | grep -q .; then
    bad "$f: cites a design record outside its metadata table"
  fi
done

# ── 3. every docs/*.md path named outside .agents/ resolves ───────────────
#
# A comment in src/ naming a chapter is a citation. Six of them named chapters
# that had not existed since an earlier numbering, and nothing reported it.
while IFS= read -r p; do
  [ -f "$p" ] || bad "a document names \`$p\`, which does not exist"
done < <(git grep -ohE 'docs/[0-9]{2}-[a-z0-9-]+\.md' -- ':!.agents' | sort -u)

# ── 4. every specification is listed in all three indexes ────────────────
for f in docs/specs/*.md; do
  base="$(basename "$f")"
  [ "$base" = "README.md" ] && continue
  grep -q "$base" docs/README.md        || bad "docs/README.md does not list docs/specs/$base"
  grep -q "$base" docs/zh/README.md     || bad "docs/zh/README.md does not list docs/specs/$base"
  grep -q "$base" docs/specs/README.md  || bad "docs/specs/README.md does not list $base"
done

# ── 5. every specification has a metadata table and a change record ──────
#
# docs/specs/README.md has required both since the directory existed. Nothing
# checked, so the requirement held only for specs whose author read the README.
#
# Four rows are required rather than "a table", and the fourth is why: a
# specification whose implementation version is not stated cannot be judged
# stale by anyone. Two spellings are in use for it and both are accepted.
for f in docs/specs/*.md; do
  base="$(basename "$f")"
  [ "$base" = "README.md" ] && continue
  meta="$(head -30 "$f")"
  for row in '规范编号' '状态' '最后修改'; do
    printf '%s' "$meta" | grep -q "$row" \
      || bad "$f: metadata table has no \`$row\` row"
  done
  printf '%s' "$meta" | grep -qE '对应实现|最低实现版本' \
    || bad "$f: metadata table states no implementation version, so nothing can decide whether it is stale"
  grep -qE '^#{2,3} .*(变更记录|Change record|Change log)' "$f" \
    || bad "$f: no change record"
done

# ── 6. no emoji under docs/ or in a top-level README ─────────────────────
#
# Status is a word: 已实现 / 未实现, yes / no, verified / not verified. A symbol
# needs a legend and a word does not. `.agents/docs/` is NOT checked: its
# records carry four thousand of them and are immutable once their change
# lands, so normalising them would edit documents whose value is that they are
# not edited. New records follow the rule by review.
EMOJI='[✅❌⚠⭐🎉🚀💡🔥👍✨📦🔧]'
for f in docs/*.md docs/zh/*.md docs/specs/*.md README.md README.zh-CN.md; do
  [ -f "$f" ] || continue
  if grep -nP "$EMOJI" "$f" >/dev/null 2>&1; then
    while IFS= read -r hit; do
      bad "$f: emoji — state the status as a word: ${hit%%:*}: $(echo "$hit" | cut -d: -f2- | cut -c1-60)"
    done < <(grep -nP "$EMOJI" "$f")
  fi
done

# ── 7. the design-record index is current ────────────────────────────────
#
# 269 records and the index was one heading. It is generated now, so it cannot
# drift -- and a generated file that is checked in must be compared against the
# generator or it drifts anyway.
python3 .github/tools/gen_agents_index.py --check || fail=1

# ── 8. a new design record declares its subject and status ───────────────
#
# From the date the convention starts. The 268 records that predate it are not
# rewritten: a record describes the moment its change was made, and a pass that
# added a field nobody chose would edit documents whose value is that they are
# not edited.
CONVENTION_FROM="2026-09-08"
for f in .agents/docs/[0-9]*.md; do
  [ -f "$f" ] || continue
  d="$(basename "$f" | cut -c1-10)"
  [[ "$d" < "$CONVENTION_FROM" ]] && continue
  head -1 "$f" | grep -q '^---$' \
    || { bad "$f: a record dated $CONVENTION_FROM or later has no front matter"; continue; }
  fmblock="$(awk 'NR>1 && /^---$/ {exit} NR>1' "$f")"
  printf '%s' "$fmblock" | grep -qE '^subject: *[a-z]' \
    || bad "$f: front matter declares no \`subject\`"
  printf '%s' "$fmblock" | grep -qE '^status: *(active|landed|superseded|abandoned) *$' \
    || bad "$f: front matter declares no valid \`status\` (active | landed | superseded | abandoned)"
done

if [[ "$fail" -eq 0 ]]; then
  echo "OK: docs structure checks pass"
fi
exit "$fail"

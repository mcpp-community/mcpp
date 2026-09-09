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
#   9. every relative link in docs/ and examples/ resolves
#  10. a translation carries the same tables and code blocks
#  11. every chapter states its reader, its question and its exclusions
#  12. a citation naming a section lands in the chapter that contains it
#  13. every table the manifest reference documents is in the lookup index
#  14. a link labelled with a chapter number points at that chapter
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

# ── 9. every relative link in docs/, examples/ and the READMEs resolves ──────
#
# Rule 3 catches `docs/NN-*.md` named anywhere, including from source comments.
# This is the other half: a Markdown link in a document that points at a file
# which is not there. Both halves are needed -- a chapter moved in this batch
# would satisfy one and break the other.
#
# THE TWO TOP-LEVEL READMEs ARE IN THE SET. They are the entry point to every
# tree below them and they carry more relative links than most chapters, and
# until they were added here nothing checked one of those links at all.
#
# THE FRAGMENT IS PART OF THE LINK. The first version of this rule discarded
# it (`(?:#[^)]*)?`), so a link to a section that had been renamed resolved to
# the file and was reported correct. Renaming 100 headings for register in one
# batch is exactly the change that produces those, and two hand-written anchors
# in chapter 30 were already wrong before the renames began.
python3 - <<'PYCHECK' || fail=1
import re, pathlib, sys, unicodedata

def slug(heading):
    """GitHub's heading slug: lowercase, drop punctuation and symbols, spaces to hyphens.

    Category P* and S* covers what github-slugger removes -- ASCII punctuation,
    the em dash, the backticks around inline code, and the full-width comma and
    colon the Chinese chapters use -- while `-` and `_` are kept because an
    anchor is allowed to contain them.
    """
    t = re.sub(r"^#+\s+", "", heading).strip().lower()
    keep = []
    for ch in t:
        if ch in "-_":
            keep.append(ch)
        elif ch.isspace():
            keep.append(" ")
        elif unicodedata.category(ch)[0] in "PS":
            continue
        else:
            keep.append(ch)
    return "".join(keep).replace(" ", "-")

def anchors_of(path):
    """Every anchor the file defines, with GitHub's -1/-2 suffix for repeats."""
    seen, out, infence = {}, set(), False
    for line in path.read_text(errors="ignore").splitlines():
        if line.startswith("```"):
            infence = not infence
            continue
        if infence or not re.match(r"^#{1,6} ", line):
            continue
        s = slug(line)
        n = seen.get(s, 0)
        seen[s] = n + 1
        out.add(s if n == 0 else f"{s}-{n}")
    return out

files = (list(pathlib.Path("docs").rglob("*.md"))
         + list(pathlib.Path("examples").rglob("*.md"))
         + [pathlib.Path("README.md"), pathlib.Path("README.zh-CN.md")])
cache = {}
bad = 0
for f in files:
    for m in re.finditer(r"\]\(([^)\s]*?)(?:#([^)\s]+))?\)", f.read_text(errors="ignore")):
        t, frag = m.group(1), m.group(2)
        if t.startswith(("http", "mailto:")):
            continue
        target = (f.parent / t) if t else f
        if t and not target.exists():
            print(f"FAIL: {f}: link to `{t}` does not resolve")
            bad += 1
            continue
        if not frag or target.suffix != ".md" or not target.is_file():
            continue
        key = target.resolve()
        if key not in cache:
            cache[key] = anchors_of(target)
        if frag not in cache[key]:
            print(f"FAIL: {f}: `#{frag}` is not a heading in {target.as_posix()}")
            bad += 1
sys.exit(1 if bad else 0)
PYCHECK

# ── 10. a translation carries the same tables and code blocks ────────────
#
# check_docs_style.sh compares HEADING STRUCTURE, which is what catches a page
# that has fallen a section behind. It does not see a table row or a code block
# that never made it across, and two of those were sitting in the tree: the
# 简体中文 `[features]` section had no body at all, and 简体中文 §2.11 was
# missing the `identity` verdict table. Both predate this check and both are
# invisible to every other one.
#
# THE PAIR AT THE ROOT IS CHECKED TOO, AND IT IS WHERE THE COST WAS HIGHEST.
# `README.zh-CN.md` carried 14 target rows against the English 21: the seven it
# lacked were every bare-metal row, so a reader of the 简体中文 README saw a
# tool with no freestanding support at all. Heading count, code-block count and
# `<details>` count were all equal, which is why every other check was green.
python3 - <<'PYPARITY' || fail=1
import pathlib, sys, re
bad = 0
pairs = [(en, pathlib.Path("docs/zh") / en.name)
         for en in sorted(pathlib.Path("docs").glob("*.md"))]
pairs.append((pathlib.Path("README.md"), pathlib.Path("README.zh-CN.md")))
for en, zh in pairs:
    if not zh.exists():
        continue
    def count(f):
        rows = blocks = 0
        infence = False
        for line in f.read_text(errors="ignore").split("\n"):
            if line.startswith("```"):
                if not infence:
                    blocks += 1
                infence = not infence
                continue
            if infence:
                continue
            if line.startswith("|"):
                rows += 1
        return rows, blocks
    er, eb = count(en)
    zr, zb = count(zh)
    if er != zr:
        print(f"FAIL: {en.name}: {er} table rows in English, {zr} in 简体中文")
        bad += 1
    if eb != zb:
        print(f"FAIL: {en.name}: {eb} code blocks in English, {zb} in 简体中文")
        bad += 1
sys.exit(1 if bad else 0)
PYPARITY

# ── 11. every chapter states its reader and its question ─────────────────
#
# `.agents/skills/mcpp-docs-style` R3: reader, the one question, and the
# EXCLUSIONS, in the first fifteen lines. The exclusions are the load-bearing
# half -- they are the gate that stops a chapter re-absorbing a topic another
# chapter owns. Five of 24 chapters had this before the design; a rule nothing
# checks is a rule that decays back to five.
# Chapter 00 is exempt: it is the book's front door, and a metadata block is a
# reference-chapter device. It has no "not here" to declare, because everything
# else IS elsewhere -- which is what its closing paragraph says instead.
for f in docs/[0-9]*.md docs/zh/[0-9]*.md; do
  [ -f "$f" ] || continue
  case "$(basename "$f")" in 00-*) continue ;; esac
  head -18 "$f" | grep -qE '^\*\*(Reader|读者)' \
    || bad "$f: no designed opening — the first lines must name the reader"
  head -18 "$f" | grep -qE '(question this chapter answers|本章回答的那一个问题)' \
    || bad "$f: the opening names no question"
  head -22 "$f" | grep -qE '^\*\*(Not here|不在这里)' \
    || bad "$f: the opening states no exclusions"
done

# ── 12. a citation that names a section lands in the chapter that has it ─
#
# `See *One package, one version* in [04](04-mcpp-toml.md)` survived a split
# that moved the section to chapter 23, in five places and two languages. Rule 3
# could not see it -- the path resolved; it was the wrong chapter. A citation
# that names a section by TITLE is checkable against that chapter's headings.
python3 - <<'PYCITE' || fail=1
import re, pathlib, sys
EN = re.compile(r"See \*([^*]{3,60})\* in \[\d{2}[^\]]*\]\((\d{2}-[a-z0-9-]+)\.md\)")
ZH = re.compile(r"见\s*\[\d{2}[^\]]*\]\((\d{2}-[a-z0-9-]+)\.md\)\s*的\*([^*]{2,40})\*")
bad = 0
for f in list(pathlib.Path("docs").glob("[0-9]*.md")) + list(pathlib.Path("docs/zh").glob("[0-9]*.md")):
    text = f.read_text(errors="ignore")
    for m in EN.finditer(text):
        title, chap = m.group(1), m.group(2)
        t = (f.parent / f"{chap}.md")
        if not t.exists() or title.lower() not in t.read_text(errors="ignore").lower():
            print(f"FAIL: {f}: cites *{title}* in {chap}, which does not contain it"); bad += 1
    for m in ZH.finditer(text):
        chap, title = m.group(1), m.group(2)
        t = (f.parent / f"{chap}.md")
        if not t.exists() or title not in t.read_text(errors="ignore"):
            print(f"FAIL: {f}: cites *{title}* in {chap}, which does not contain it"); bad += 1
sys.exit(1 if bad else 0)
PYCITE

# ── 13. every table the manifest reference documents is in the lookup index ─
#
# docs/README.md carries a reverse index -- a key in front of a reader to the
# chapter that owns it -- and it is hand-built. A key added to the reference and
# not indexed is invisible: the reader concludes it is undocumented. The
# denominator is the reference chapter's own `###` headings, and the comparison
# is on the KEY NAME rather than on its spelling, because the two documents
# legitimately write `[targets.<name>]` and `[targets.<n>]`.
python3 - <<'PYLOOKUP' || fail=1
import re, pathlib, sys
ref = pathlib.Path("docs/04-mcpp-toml.md").read_text(errors="ignore")
idx = pathlib.Path("docs/README.md").read_text(errors="ignore")
keys = set()
for m in re.finditer(r"^### [0-9.b]+ `([^`]+)`", ref, re.M):
    tok = m.group(1)
    name = re.sub(r"^\[|\].*$", "", tok)          # [targets.<name>] -> targets.<name>
    name = name.split(".")[0].split(" ")[-1]      # -> targets ; "[package] platforms" -> platforms
    keys.add(name)
missing = sorted(k for k in keys if k not in idx)
for k in missing:
    print(f"FAIL: docs/README.md lookup index does not mention `{k}`, which docs/04 documents")
sys.exit(1 if missing else 0)
PYLOOKUP

# ── 14. a link labelled with a chapter number points at that chapter ─────────
#
# Rule 3 checks that a named path exists and rule 9 that a link resolves. Both
# passed on `[docs/13 -- Bare-Metal and Freestanding Targets](docs/40-baremetal.md)`
# in README.md: the renumbering rewrote the path and left the label, so the
# README told its reader to read chapter 13 for eleven of the twenty-one rows
# in its own target table. A label that names a number is an assertion about
# where the reader is being sent, and it is checkable against the path.
python3 - <<'PYLABEL' || fail=1
import re, pathlib, sys
LINK = re.compile(r"\[([^\]]+)\]\((?!https?:|mailto:)([^)\s#]+)(?:#[^)\s]+)?\)")
NUM  = re.compile(r"(?:docs/|^|[^0-9a-zA-Z])(\d{2})(?:\s*(?:--|—|-|\s)|$)")
files = (list(pathlib.Path("docs").rglob("*.md"))
         + [pathlib.Path("README.md"), pathlib.Path("README.zh-CN.md")])
bad = 0
for f in files:
    for m in LINK.finditer(f.read_text(errors="ignore")):
        label, path = m.group(1), m.group(2)
        base = pathlib.Path(path).name
        target_no = re.match(r"(\d{2})-", base)
        label_no = NUM.match(label.strip())
        if not target_no or not label_no:
            continue
        if target_no.group(1) != label_no.group(1):
            print(f"FAIL: {f}: label `{label}` names chapter "
                  f"{label_no.group(1)}, the link goes to {base}")
            bad += 1
sys.exit(1 if bad else 0)
PYLABEL

if [[ "$fail" -eq 0 ]]; then
  echo "OK: docs structure checks pass"
fi
exit "$fail"

#!/usr/bin/env python3
"""Generate .agents/docs/README.md from the records themselves.

The design tree holds 268 records and its index was one heading. This writes an
index that cannot drift, because `check_docs_structure.sh` compares the file on
disk against what this produces.

WHAT IT DERIVES AND WHAT IT READS.

The date comes from the filename, the title from the first `#` heading, and both
are mechanical. `subject` and `status` are read from YAML front matter when a
record declares it, and are shown only then -- a subject inferred from filename
keywords would misfile records, which is worse for a reader than no grouping.
New records declare it; the 268 that predate the convention are listed by date
with their titles, which is what they can support without anyone rewriting them.

Usage:
    python3 .github/tools/gen_agents_index.py            # write
    python3 .github/tools/gen_agents_index.py --check    # exit 1 if stale
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DOCS = ROOT / ".agents" / "docs"
INDEX = DOCS / "README.md"

DATED = re.compile(r"^(\d{4})-(\d{2})-(\d{2})-(.+)\.md$")
STATUSES = {"active", "landed", "superseded", "abandoned"}


def front_matter(text):
    """Return the YAML-ish front matter as a dict, or {} when there is none."""
    if not text.startswith("---\n"):
        return {}
    end = text.find("\n---", 4)
    if end < 0:
        return {}
    out = {}
    for line in text[4:end].split("\n"):
        if ":" not in line or line.startswith(" "):
            continue
        k, v = line.split(":", 1)
        out[k.strip()] = v.strip()
    return out


def title_of(text, fallback):
    for line in text.split("\n"):
        if line.startswith("# "):
            return line[2:].strip()
    return fallback


def collect():
    dated, undated, todos = [], [], []
    for p in sorted(DOCS.rglob("*.md")):
        if p == INDEX:
            continue
        rel = p.relative_to(DOCS).as_posix()
        text = p.read_text(errors="ignore")
        fm = front_matter(text)
        entry = {
            "rel": rel,
            "title": title_of(text, p.stem),
            "status": fm.get("status", ""),
            "subject": fm.get("subject", ""),
            "superseded_by": fm.get("superseded_by", ""),
        }
        m = DATED.match(p.name)
        if rel.startswith("todos/"):
            todos.append(entry)
        elif m:
            entry["date"] = f"{m.group(1)}-{m.group(2)}-{m.group(3)}"
            dated.append(entry)
        else:
            undated.append(entry)
    dated.sort(key=lambda e: (e["date"], e["rel"]), reverse=True)
    return dated, undated, todos


def row(e):
    note = ""
    if e["status"]:
        note = f" — {e['status']}"
        if e["status"] == "superseded" and e["superseded_by"]:
            note += f" by [{e['superseded_by']}]({e['superseded_by']})"
    return f"- [{e['title']}]({e['rel']}){note}"


def render():
    dated, undated, todos = collect()
    out = [
        "# Design records",
        "",
        "The reasoning behind changes to mcpp: what was measured, what was decided,",
        "and what a later measurement refuted. A record describes the moment its",
        "change was made and is not edited afterwards, so **nothing here is a",
        "statement about the present**. What mcpp does today is in",
        "[docs/](../../docs/README.md); what is guaranteed is in",
        "[docs/specs/](../../docs/specs/README.md).",
        "",
        "**This file is generated** by `.github/tools/gen_agents_index.py` and is",
        "checked in CI. A new record declares front matter:",
        "",
        "```yaml",
        "---",
        "subject: heterogeneous            # a short, reused word",
        "status: landed                    # active | landed | superseded | abandoned",
        "superseded_by: 2026-09-07-....md  # when status is superseded",
        "---",
        "```",
        "",
        f"{len(dated) + len(undated) + len(todos)} records.",
        "",
    ]

    by_subject = {}
    for e in dated + undated + todos:
        if e["subject"]:
            by_subject.setdefault(e["subject"], []).append(e)
    if by_subject:
        out += ["## By subject", "",
                "Records that declare one. Everything else is listed by date below.", ""]
        for subject in sorted(by_subject):
            out.append(f"### {subject}")
            out.append("")
            out += [row(e) for e in by_subject[subject]]
            out.append("")

    out += ["## By date", ""]
    month = None
    for e in dated:
        m = e["date"][:7]
        if m != month:
            month = m
            out += [f"### {month}", ""]
        out.append(row(e))
    out.append("")

    if undated:
        out += ["## Undated", "",
                "Records written before the date prefix was the convention.", ""]
        out += [row(e) for e in undated]
        out.append("")

    if todos:
        out += ["## todos/", "",
                "Work items rather than records of a decision.", ""]
        out += [row(e) for e in todos]
        out.append("")

    return "\n".join(out)


def main():
    want = render()
    if "--check" in sys.argv:
        have = INDEX.read_text() if INDEX.exists() else ""
        if have != want:
            print("FAIL: .agents/docs/README.md is stale — run "
                  "`python3 .github/tools/gen_agents_index.py`")
            return 1
        print("OK: .agents/docs index is current")
        return 0
    INDEX.write_text(want)
    print(f"wrote {INDEX.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

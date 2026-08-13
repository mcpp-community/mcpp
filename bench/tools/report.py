#!/usr/bin/env python3
"""Turn one or more bench report JSONs into the markdown table they describe.

WHY THIS EXISTS. The published tables in bench/results/, bench/README.md and the
root README were transcribed by hand from harness output. Transcription is
exactly the failure this whole suite is built to remove — a benchmark whose
headline number was mistyped is indistinguishable from one that was measured,
and there is no test that can catch it. So the tables come from the JSON.

    bench/tools/report.py run.json [more.json ...] [--baseline NAME]

Rows are scenarios, columns are engines, ordered as the report file lists them.
Each cell is `<median>s · <ratio>x`, the ratio against `--baseline` within the
same (variant, scenario) group — the same grouping the harness's own summary
uses, because a ratio across source forms or perturbations is not a ratio.

A non-`ok` cell renders as its status in italics, never as a number and never as
a blank: protocol invariant 1 says a failure must not be able to look like a
measurement, and a table is where that invariant is most easily lost.

The PERTURBATION FORM is carried through into a footnote when a group has more
than one, because `edit-comment` means two different things depending on whether
the target unit had a function body (see SPEC.md §4).
"""
import json
import sys
from collections import OrderedDict


def load(paths):
    cells, hosts = [], []
    for p in paths:
        d = json.load(open(p, encoding="utf-8"))
        cells += d["cells"]
        hosts.append(d.get("host", {}))
    return cells, hosts


def form_of(note):
    marker = "perturbation: "
    return note.split(marker, 1)[1].strip() if marker in note else ""


def render(cells, baseline):
    engines = list(OrderedDict.fromkeys(c["engine"] for c in cells))
    groups = list(OrderedDict.fromkeys((c["variant"], c["scenario"]) for c in cells))
    # Group by variant so a table never mixes source forms.
    variants = list(OrderedDict.fromkeys(v for v, _ in groups))
    out, notes = [], []

    for variant in variants:
        out.append(f"\n**`{variant}`**\n")
        out.append("| scenario | " + " | ".join(f"`{e}`" for e in engines) + " |")
        out.append("|---" * (len(engines) + 1) + "|")
        for v, scenario in groups:
            if v != variant:
                continue
            row = [f"`{scenario}`"]
            here = {c["engine"]: c for c in cells
                    if c["variant"] == v and c["scenario"] == scenario}
            base = next((c for e, c in here.items()
                         if baseline in e and c["status"] == "ok"), None)
            forms = {form_of(c.get("note", "")) for c in here.values()} - {""}
            if len(forms) > 1:
                notes.append(f"`{variant}`/`{scenario}` mixes perturbation forms "
                             f"({', '.join(sorted(forms))}) — those are different "
                             f"questions; see SPEC.md §4")
            for e in engines:
                c = here.get(e)
                if c is None:
                    row.append("—")
                elif c["status"] != "ok":
                    row.append(f"_{c['status']}_")
                elif base and base.get("median_s"):
                    ratio = c["median_s"] / base["median_s"]
                    mark = "  ← baseline" if c is base else ""
                    row.append(f"{c['median_s']:.2f}s · {ratio:.2f}x{mark}")
                else:
                    row.append(f"{c['median_s']:.2f}s")
            out.append("| " + " | ".join(row) + " |")
    for n in OrderedDict.fromkeys(notes):
        out.append(f"\n> ⚠️ {n}")
    return "\n".join(out)


def main(argv):
    baseline = "cmake"
    paths = []
    it = iter(argv)
    for a in it:
        if a == "--baseline":
            baseline = next(it, "cmake")
        else:
            paths.append(a)
    if not paths:
        print(__doc__)
        return 2
    cells, hosts = load(paths)
    if not cells:
        print("no cells in those reports", file=sys.stderr)
        return 1
    h = hosts[0]
    print(f"host: {h.get('os')} {h.get('arch')} · {h.get('cpu_model')} · "
          f"{h.get('logical_cores')} logical / {h.get('physical_cores')} physical"
          f"{' (heterogeneous)' if h.get('heterogeneous') else ''}")
    print(f"baseline: {baseline}")
    print(render(cells, baseline))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

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

A cell that has no measurement renders as `-`, and one that HAS a measurement
which did not succeed renders as its status in italics. Those two are opposite
claims and must never collapse into the same mark:

    -               not measured — no data exists for this combination
    _failed_        the engine ran and produced no artifact — a FINDING
    _unavailable_   the engine is not installed, or cannot express this cell

Never a blank, and never `0.00s`: protocol invariant 1 says a failure must not be
able to look like a measurement, and a table is where that invariant is most
easily lost — the shell harness this suite replaces formatted three failed cells
as `0.000 s` and they were published as the fastest builds ever recorded.

The PERTURBATION FORM is carried through into a footnote when a group has more
than one, because `edit-comment` means two different things depending on whether
the target unit had a function body (see SPEC.md §4).
"""
import json
import sys
from collections import OrderedDict


def load_journal(path):
    """Reduce a `.mbench/<fingerprint>/journal.jsonl` to report cells.

    ⚠️ WHY THIS EXISTS. The journal records one line per measured SAMPLE, but a
    report JSON is only written when a whole CELL finishes — so an interrupted
    run left its samples on disk with no way to look at them. 42 measured points
    of an in-flight cell were invisible to every table in the repository while
    sitting in a file. The design says the journal is the source of truth and the
    report is derived from it; until this function, that was only half true.

    The reduction is deliberately the same one the harness does: group by
    (project, variant, scenario, engine), median/min/max over the samples
    present. A partial group is reported with the count it actually has, never
    padded — `runs` in the output is how many samples exist, not how many were
    planned.
    """
    groups = OrderedDict()
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        if not line.startswith("{") or not line.endswith("}"):
            continue          # a half-written last line is expected after a kill
        try:
            e = json.loads(line)
        except json.JSONDecodeError:
            continue
        key = (e.get("project", ""), e.get("variant", ""),
               e.get("scenario", ""), e.get("engine", ""))
        groups.setdefault(key, []).append(e)

    cells = []
    for (project, variant, scenario, engine), samples in groups.items():
        walls = sorted(s["wall_s"] for s in samples)
        n = len(walls)
        failed = [s for s in samples if s.get("exit", 0) != 0]
        cell = {
            "engine": engine, "compiler": "", "profile": "",
            "scenario": scenario, "fixture": project, "variant": variant,
            "runs": n,
            "note": "" if not failed else f"{len(failed)} of {n} samples exited non-zero",
            "status": "ok" if not failed else "failed",
            "samples": [{"wall_s": w} for w in walls],
        }
        if not failed:
            cell["median_s"] = walls[n // 2] if n % 2 else (walls[n // 2 - 1] + walls[n // 2]) / 2
            cell["min_s"] = walls[0]
            cell["max_s"] = walls[-1]
        cells.append(cell)
    return cells


def load(paths):
    cells, hosts = [], []
    for p in paths:
        # A journal and a report describe the same thing at different stages;
        # accepting both means a table can be drawn at any moment, not only
        # after a cell completes.
        if p.endswith(".jsonl"):
            cells += load_journal(p)
            hosts.append({})
            continue
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
                    row.append("-")        # not measured — see the legend above
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


# ── the root README's headline table ───────────────────────────────────────
#
# That table was the last hand-transcribed thing in the repository, and it was
# also the WORST place for one: it is the first table a reader sees, and it was
# stitched from three separate runs because no single run measured every column.
# The standard set now does, so it can be generated — and once it is generated,
# `233_bench_matrix.sh` can check it against ONE file instead of a hard-coded
# list of three.
#
# ⚠️ The wording is deliberately not translated field-by-field. A benchmark table
# that says different things in two languages is two claims, and only one of them
# can be checked against the data.
ALLOW_SUSPECT = [False]
HEADLINE_SCENARIOS = ["cold", "noop", "touch-hub", "edit-body", "edit-comment"]
HEADLINE_WHAT = {
    "en": {
        "cold":         "nothing built yet",
        "noop":         "nothing at all",
        "touch-hub":    "mtime only, content unchanged",
        "edit-body":    "a real edit inside a function body",
        "edit-comment": "a comment added to a hub interface",
        "scenario": "scenario", "what": "what changed",
    },
    "zh": {
        "cold":         "还没编过",
        "noop":         "什么都没改",
        "touch-hub":    "只碰 mtime,内容不变",
        "edit-body":    "真的改了一个函数体",
        "edit-comment": "在 hub 接口里加一行注释",
        "scenario": "场景", "what": "改了什么",
    },
}


def engine_order(engines):
    """mcpp arms first, newest version first, each default before its opt-in arm.

    Reading order matters more than it looks: the build under test and its
    `+schedule=on` arm answer one question ("what does the key buy?") and the
    released reference answers a different one ("did this get faster?"). Sorting
    the raw strings interleaved them — reference, opt-in, default — so neither
    pair sat together and every comparison was two columns apart.
    """
    def version_key(e):
        head = e.split("+", 1)[0]
        ver = head.split("@", 1)[1] if "@" in head else ""
        parts = []
        for piece in ver.split("."):
            parts.append(int(piece) if piece.isdigit() else 0)
        return parts + [0] * (4 - len(parts))

    def key(e):
        if not e.startswith("mcpp@"):
            return (1, [], 0, e)          # other engines after every mcpp arm
        # negated version → newest first; base arm before its own option arm
        return (0, [-p for p in version_key(e)], 1 if "+" in e else 0, e)
    return sorted(engines, key=key)


# Short COLUMN names. The engine labels are the truth — `mcpp@2026.8.13.1`,
# `mcpp@2026.8.13.1+schedule=on`, `mcpp@2026.8.11.3` — but three of them side by
# side make the table wider than a README renders, so it arrives collapsed with a
# horizontal scrollbar and the reader sees two columns of a five-column
# comparison. The identity moves to the footnote, where it is read once.
#
# ⚠️ THE MAPPING IS EMITTED, NOT REPEATED. A short name in the table and a long
# one in the data is exactly the drift this tool exists to remove, so the header
# carries a machine-readable `<!-- columns: ... -->` line and the guard reads the
# mapping from there instead of keeping a second copy.
def short_name(engine, newest):
    if not engine.startswith("mcpp@"):
        return engine
    base, _, opt = engine.partition("+")
    if opt:
        return "mcpp +schedule"
    return "mcpp" if base == newest else "mcpp (released)"


def columns_legend(short, engines, lang):
    """The sentence that has to accompany short column names.

    Shortening a header is only safe if the identity it dropped is still stated
    somewhere the reader will see. This is that somewhere, generated from the
    same mapping the table uses so the two cannot disagree."""
    parts = []
    for e in engines:
        name = short[e]
        if not e.startswith("mcpp@"):
            continue
        if name == "mcpp":
            parts.append(f"`mcpp` = {e}, the build under test"
                         if lang == "en" else f"`mcpp` = {e},被测的这一版")
        elif name == "mcpp +schedule":
            parts.append("`mcpp +schedule` = the same binary with "
                         "`[build] bmi_schedule = \"on\"`"
                         if lang == "en" else
                         "`mcpp +schedule` = 同一个二进制,开了 "
                         "`[build] bmi_schedule = \"on\"`")
        else:
            parts.append(f"`mcpp (released)` = {e}, the published release"
                         if lang == "en" else f"`mcpp (released)` = {e},已发布版")
    return " · ".join(parts)


def headline(cells, baseline, lang):
    w = HEADLINE_WHAT[lang]
    # The real workload only: a headline table that silently mixed a generated
    # fixture into it would be comparing two different questions.
    #
    # ⚠️ THE DISCRIMINATOR IS THE FIXTURE NAME, NOT THE VARIANT. `variant` is
    # whatever the cell declared, and the same real project is labelled `modules`
    # in one published run and `native` in another — so filtering on `native`
    # silently dropped every cmake and xmake column and the table rendered with
    # two columns and no ratios at all.
    cells = [c for c in cells if not c["fixture"].startswith("synth-")]
    projects = {c["fixture"] for c in cells}
    if len(projects) > 1:
        raise SystemExit(f"headline: {len(projects)} projects in these reports "
                         f"({', '.join(sorted(projects))}) — one table, one workload; "
                         f"pass the report for a single project")
    if not cells:
        raise SystemExit("headline: no real-workload cells in these reports "
                         "(only generated fixtures)")
    engines = engine_order({c["engine"] for c in cells})
    newest = next((e.split("+", 1)[0] for e in engines if e.startswith("mcpp@")), "")
    short = {e: short_name(e, newest) for e in engines}

    # ⚠️ A COLD BUILD THAT IS NOT SLOWER THAN A NO-OP DID NOT BUILD ANYTHING.
    #
    # Not a hypothetical: rendering the previously published files produced
    #     xmake  cold  0.60s · 153.1x
    # next to cmake's 92s — xmake was resolving `--buildir` relative to `-P` and
    # configuring into a directory that was already populated, so it exited
    # having compiled nothing. The number was real, the measurement was not, and
    # in a headline table it reads as xmake being 153x faster than cmake.
    #
    # This is the whole failure mode of the suite in one cell, so it BLOCKS
    # rather than warns: a table nobody can publish is better than one that is
    # wrong in the reader's favour.
    suspect = []
    for e in engines:
        cold = next((c for c in cells if c["engine"] == e and c["scenario"] == "cold"
                     and c["status"] == "ok"), None)
        noop = next((c for c in cells if c["engine"] == e and c["scenario"] == "noop"
                     and c["status"] == "ok"), None)
        if cold and noop and noop["median_s"] > 0 and cold["median_s"] < noop["median_s"] * 5:
            suspect.append(f"{e}: cold {cold['median_s']:.2f}s vs noop "
                           f"{noop['median_s']:.2f}s — a cold build that is not at least "
                           f"5x its own no-op did not build the project")
    if suspect and not ALLOW_SUSPECT[0]:
        raise SystemExit("headline: refusing to render — "
                         + "; ".join(suspect)
                         + "\n(pass --allow-suspect to see it anyway)")

    rows = ["<!-- columns: " + "; ".join(f"{short[e]}={e}" for e in engines) + " -->",
            f"| {w['scenario']} | {w['what']} | " + " | ".join(f"`{short[e]}`" for e in engines) + " |",
            "|---" * (len(engines) + 2) + "|"]
    for sc in HEADLINE_SCENARIOS:
        here = {c["engine"]: c for c in cells if c["scenario"] == sc}
        if not here:
            continue
        base = next((c for e, c in here.items()
                     if baseline in e and c["status"] == "ok"), None)
        ok = [c for c in here.values() if c["status"] == "ok" and c.get("median_s") is not None]
        best = min((c["median_s"] for c in ok), default=None)
        cellsr = []
        for e in engines:
            c = here.get(e)
            if c is None:
                cellsr.append("-")         # not measured; see the legend above
            elif c["status"] != "ok":
                cellsr.append(f"_{c['status']}_")
            else:
                txt = f"{c['median_s']:.2f}s"
                if base and base.get("median_s"):
                    txt += f" · {base['median_s'] / c['median_s']:.1f}x"
                # Bold the fastest arm in the row, so the table reads without
                # the reader dividing anything in their head.
                cellsr.append(f"**{txt}**" if c["median_s"] == best else txt)
        rows.append(f"| `{sc}` | {w[sc]} | " + " | ".join(cellsr) + " |")
    return "\n".join(rows)


def main(argv):
    baseline = "cmake"
    mode = "table"
    lang = "en"
    paths = []
    it = iter(argv)
    for a in it:
        if a == "--baseline":
            baseline = next(it, "cmake")
        elif a == "--headline":
            mode = "headline"
        elif a == "--allow-suspect":
            ALLOW_SUSPECT[0] = True
        elif a == "--lang":
            lang = next(it, "en")
        else:
            paths.append(a)
    if not paths:
        print(__doc__)
        return 2
    if mode == "headline":
        cells, hosts = load(paths)
        h = hosts[0]
        print(headline(cells, baseline, lang))
        print()
        _engines = engine_order({c["engine"] for c in cells
                                 if not c["fixture"].startswith("synth-")})
        _newest = next((e.split("+", 1)[0] for e in _engines if e.startswith("mcpp@")), "")
        print("COLUMNS: " + columns_legend({e: short_name(e, _newest) for e in _engines},
                                           _engines, lang))
        # The toolchain is recorded as the driver PATH; a footnote wants the
        # compiler, not where this machine happens to keep it.
        tc = str(h.get("toolchain", ""))
        for part in tc.replace("\\", "/").split("/"):
            if any(ch.isdigit() for ch in part) and "." in part:
                tc = part
        print(f"<sub>{h.get('os')} {h.get('arch')} · {h.get('cpu_model')} · "
              f"{tc} · n={max((len(c.get('samples', [])) for c in cells), default=0)}"
              f"</sub>")
        return 0
    cells, hosts = load(paths)
    if not cells:
        print("no cells in those reports", file=sys.stderr)
        return 1
    h = next((x for x in hosts if x), {})
    if h:
        print(f"host: {h.get('os')} {h.get('arch')} · {h.get('cpu_model')} · "
              f"{h.get('logical_cores')} logical / {h.get('physical_cores')} physical"
              f"{' (heterogeneous)' if h.get('heterogeneous') else ''}")
    else:
        # A journal records samples, not the machine — that is in the report the
        # run writes at the end. Say so rather than printing a row of `None`,
        # which reads like the host detection failed.
        print("host: not recorded in a journal (see the run's report JSON)")
    print(f"baseline: {baseline}")
    incomplete = sorted({c["runs"] for c in cells})
    if len(incomplete) > 1:
        print(f"⚠️  sample counts differ across cells ({incomplete}) — this is a "
              f"PARTIAL run; a cell with fewer samples has less dispersion, not less variance")
    print(render(cells, baseline))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

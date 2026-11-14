#!/usr/bin/env python3
"""The run store `mcpp test` reads and writes (docs/08 "The order of a run").

    test_history.py take <store> <from>...   the runs of each <from> store taken
                                              into <store>, each run once
    test_history.py own <store> <group>      the runs of <store> that are not of
                                              <group> (a workflow run) removed

A store is the mtest store's form: `index.ndjson`, a line per run, and
`runs/<id>/`. `take` gives a job the measured history of main's last run, so
that its `mcpp test` orders the tests longest first and assigns its shards by
duration; `own` leaves the job's own runs before the store is kept, so that a
store does not carry the history it was given into the next run's.
"""
import json
import pathlib
import shutil
import sys


def runs(store):
    index = store / "index.ndjson"
    if not index.is_file():
        return []
    out = []
    for line in index.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            run = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(run, dict) and run.get("id"):
            out.append((run, line))
    return out


def take(store, sources):
    store.mkdir(parents=True, exist_ok=True)
    have = {run["id"] for run, _ in runs(store)}
    taken = 0
    with (store / "index.ndjson").open("a", encoding="utf-8") as index:
        for source in sources:
            for run, line in runs(source):
                if run["id"] in have:
                    continue
                src = source / "runs" / run["id"]
                if not src.is_dir():
                    continue
                shutil.copytree(src, store / "runs" / run["id"], dirs_exist_ok=True)
                index.write(line + "\n")
                have.add(run["id"])
                taken += 1
    print(f"{taken} run(s) taken into {store}")


def own(store, group):
    kept, dropped = [], 0
    for run, line in runs(store):
        if str(run.get("group", "")) == group:
            kept.append(line)
            continue
        shutil.rmtree(store / "runs" / run["id"], ignore_errors=True)
        dropped += 1
    if (store / "index.ndjson").is_file():
        (store / "index.ndjson").write_text("".join(l + "\n" for l in kept), encoding="utf-8")
    print(f"{len(kept)} run(s) of group {group} kept, {dropped} removed from {store}")


def main(argv):
    if len(argv) >= 3 and argv[1] == "take":
        take(pathlib.Path(argv[2]), [pathlib.Path(p) for p in argv[3:]])
        return 0
    if len(argv) == 4 and argv[1] == "own":
        own(pathlib.Path(argv[2]), argv[3])
        return 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))

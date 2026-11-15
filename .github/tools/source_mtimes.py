#!/usr/bin/env python3
"""The sources' times for an incremental build restored from a cache.

A checkout writes every file with the time of the checkout, so a build directory
restored from a cache is older than every source and ninja rebuilds all of it.
The cache records each tracked file's blob beside the build (`record`); after a
checkout, a file whose blob is the one recorded is given a time older than any
output (`restore`), and a changed or new file keeps the checkout's, so exactly
what changed is rebuilt.

    source_mtimes.py record  <build-dir>
    source_mtimes.py restore <build-dir>
"""
import os
import subprocess
import sys

STAMP = ".source-blobs"
OLD = 946684800  # 2000-01-01: older than every output a build writes


def blobs():
    """Each tracked file's blob; a file the working tree changes has none."""
    git = lambda *a: subprocess.run(["git", *a], check=True, capture_output=True).stdout
    edited = {p.decode() for p in git("ls-files", "-m", "-z").split(b"\0") if p}
    out = git("ls-files", "-s", "-z")
    table = {}
    for entry in out.split(b"\0"):
        if not entry:
            continue
        meta, path = entry.split(b"\t", 1)
        if path.decode() not in edited:
            table[path.decode()] = meta.split()[1].decode()
    return table


def main(argv):
    if len(argv) != 3 or argv[1] not in ("record", "restore"):
        print(__doc__.strip(), file=sys.stderr)
        return 2
    stamp = os.path.join(argv[2], STAMP)
    if argv[1] == "record":
        os.makedirs(argv[2], exist_ok=True)
        with open(stamp, "w", encoding="utf-8") as f:
            for path, blob in sorted(blobs().items()):
                f.write(f"{blob} {path}\n")
        return 0
    if not os.path.isfile(stamp):
        print(f"no {stamp}: the build directory is rebuilt whole")
        return 0
    recorded = {}
    with open(stamp, encoding="utf-8") as f:
        for line in f:
            blob, path = line.rstrip("\n").split(" ", 1)
            recorded[path] = blob
    same = changed = 0
    current = blobs()
    for path in subprocess.run(["git", "ls-files", "-z"], check=True,
                               capture_output=True).stdout.decode().split("\0"):
        if not path:
            continue
        blob = current.get(path)
        if blob is not None and recorded.get(path) == blob and os.path.isfile(path):
            os.utime(path, (OLD, OLD), follow_symlinks=False)
            same += 1
        else:
            changed += 1
    print(f"{same} file(s) as recorded, {changed} changed or new")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

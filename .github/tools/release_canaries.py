#!/usr/bin/env python3
"""The release canaries (.github/release-canaries.toml, WS10 of the 2026-09-28
ecosystem design), read by .github/workflows/release-canaries.yml.

    release_canaries.py matrix              the GitHub Actions matrix, as `matrix=<json>`
    release_canaries.py unpin <checkout>    remove the project's own mcpp pin from its checkout
    release_canaries.py run <name>          run a canary's commands in the current directory
    release_canaries.py steps [<dir>]       the longest build steps of the builds under <dir>

`run` reads the candidate from `$MCPP`. Each command runs under bash with
`set -eo pipefail`, so a failure is the command's own; a command listed under
`expect` must also print the given text. The whole list runs, and the exit
status says whether every command held.

The bash is `$CANARY_BASH` when the workflow names one. On Windows it must: a
Windows program that starts `bash` by name gets `System32\bash.exe`, the WSL
launcher, because the loader searches the system directory before PATH (the
first release run of these canaries printed "Windows Subsystem for Linux has
no installed distributions" for every command).

After its commands, `run` prints the longest steps (compile or link, and the
output) of every build the canary made, read from each `.ninja_log` beneath the
current directory: a canary's time is mostly its project's build, and the steps
say where (design 2026-10-30 part 2 §11).
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LIST = ROOT / ".github" / "release-canaries.toml"


def canaries() -> list[dict]:
    with LIST.open("rb") as f:
        data = tomllib.load(f)
    out = data.get("canary", [])
    names = [c["name"] for c in out]
    if len(set(names)) != len(names):
        raise SystemExit(f"FAIL: duplicate canary names in {LIST}: {names}")
    for c in out:
        for key in ("name", "repo", "ref", "os", "commands"):
            if key not in c:
                raise SystemExit(f"FAIL: canary {c.get('name', '?')} has no `{key}`")
    return out


def cmd_matrix() -> int:
    include = [{
        "name": c["name"], "repo": c["repo"], "ref": c["ref"], "os": c["os"],
        "timeout": int(c.get("timeout", 60)),
        "submodules": bool(c.get("submodules", False)),
        "cache": "\n".join(c.get("cache", [])),
    } for c in canaries()]
    print("matrix=" + json.dumps({"include": include}, separators=(",", ":")))
    return 0


def cmd_unpin(checkout: str) -> int:
    """A project pins the mcpp it builds with in `.xlings.json`
    (`workspace.mcpp`); the canary builds with the candidate instead, so the
    pin is removed in the checkout and nothing installs the released mcpp."""
    path = Path(checkout) / ".xlings.json"
    if not path.is_file():
        print(f"{path}: no .xlings.json, nothing to unpin")
        return 0
    data = json.loads(path.read_text(encoding="utf-8"))
    ws = data.get("workspace", {})
    if "mcpp" in ws:
        print(f"{path}: removing workspace.mcpp = {ws.pop('mcpp')}")
        path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    else:
        print(f"{path}: no workspace.mcpp pin")
    return 0


def shell_argv(command: str) -> list[str]:
    """The argv that runs one canary command: the named bash, else `bash`."""
    shell = os.environ.get("CANARY_BASH") or "bash"
    return [shell, "-c", f"set -eo pipefail\n{command}"]


def longest_steps(root: Path, count: int = 20) -> list[tuple[int, str, str]]:
    """(milliseconds, build directory, output) of the longest steps in every
    `.ninja_log` beneath `root`. A log holds every build of its directory; the
    last line for an output is its latest step."""
    steps: list[tuple[int, str, str]] = []
    for log in root.rglob(".ninja_log"):
        latest: dict[str, int] = {}
        try:
            lines = log.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            continue
        for line in lines:
            if line.startswith("#"):
                continue
            fields = line.split("\t")
            if len(fields) < 4 or not fields[0].isdigit() or not fields[1].isdigit():
                continue
            latest[fields[3]] = int(fields[1]) - int(fields[0])
        where = str(log.parent.relative_to(root)) if log.parent != root else "."
        steps += [(ms, where, out) for out, ms in latest.items()]
    steps.sort(key=lambda s: s[0], reverse=True)
    return steps[:count]


def print_steps(root: Path) -> None:
    steps = longest_steps(root)
    if not steps:
        print(f"no .ninja_log beneath {root}")
        return
    print("::group::the longest build steps", flush=True)
    for ms, where, out in steps:
        print(f"{ms / 1000:9.1f} s  {where}  {out}")
    print("::endgroup::", flush=True)


def cmd_steps(root: str) -> int:
    print_steps(Path(root).resolve())
    return 0


def cmd_run(name: str) -> int:
    mcpp = os.environ.get("MCPP", "")
    if not mcpp:
        raise SystemExit("FAIL: $MCPP names no candidate")
    matches = [c for c in canaries() if c["name"] == name]
    if not matches:
        raise SystemExit(f"FAIL: no canary named {name}")
    c = matches[0]
    expect = c.get("expect", {})
    failed = []
    for command in c["commands"]:
        print(f"::group::{command}", flush=True)
        proc = subprocess.run(shell_argv(command),
                              capture_output=True, text=True, check=False,
                              env={**os.environ, "MCPP": mcpp})
        sys.stdout.write(proc.stdout)
        sys.stderr.write(proc.stderr)
        print("::endgroup::", flush=True)
        if proc.returncode != 0:
            failed.append(f"`{command}` exited {proc.returncode}")
            continue
        want = expect.get(command)
        if want and want not in proc.stdout + proc.stderr:
            failed.append(f"`{command}` did not print `{want}`")
    print_steps(Path.cwd())
    for f in failed:
        print(f"::error::canary {name}: {f}")
    print(f"canary {name}: {len(c['commands']) - len(failed)} of {len(c['commands'])} command(s) held")
    return 1 if failed else 0


def main(argv: list[str]) -> int:
    if not argv:
        print(__doc__)
        return 2
    if argv[0] == "matrix":
        return cmd_matrix()
    if argv[0] == "unpin" and len(argv) == 2:
        return cmd_unpin(argv[1])
    if argv[0] == "run" and len(argv) == 2:
        return cmd_run(argv[1])
    if argv[0] == "steps" and len(argv) <= 2:
        return cmd_steps(argv[1] if len(argv) == 2 else ".")
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

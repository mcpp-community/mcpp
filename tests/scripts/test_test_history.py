"""The run store tool of the unit jobs (.github/tools/test_history.py)."""
import json
import pathlib
import subprocess
import sys
import tempfile

TOOL = pathlib.Path(__file__).resolve().parents[2] / ".github" / "tools" / "test_history.py"


def store(root, *runs):
    root.mkdir(parents=True, exist_ok=True)
    with (root / "index.ndjson").open("a") as index:
        for rid, group in runs:
            (root / "runs" / rid).mkdir(parents=True)
            (root / "runs" / rid / "programs.ndjson").write_text("{}\n")
            index.write(json.dumps({"id": rid, "group": group}) + "\n")
    return root


def ids(root):
    return [json.loads(l)["id"] for l in (root / "index.ndjson").read_text().splitlines() if l.strip()]


def tool(*args):
    subprocess.run([sys.executable, str(TOOL), *map(str, args)], check=True, capture_output=True)


with tempfile.TemporaryDirectory() as tmp:
    tmp = pathlib.Path(tmp)
    main1 = store(tmp / "a", ("r1", "100"), ("r2", "100"))
    main2 = store(tmp / "b", ("r2", "100"), ("r3", "100"))
    dest = tmp / "dest"
    # Each run is taken once, with its directory.
    tool("take", dest, main1, main2)
    assert ids(dest) == ["r1", "r2", "r3"], ids(dest)
    assert (dest / "runs" / "r3" / "programs.ndjson").is_file()
    tool("take", dest, main1)
    assert ids(dest) == ["r1", "r2", "r3"], ids(dest)
    # The job's own run is kept, the history it was given is not.
    store(dest, ("mine", "200"))
    tool("own", dest, "200")
    assert ids(dest) == ["mine"], ids(dest)
    assert not (dest / "runs" / "r1").exists()
    # A store with no index takes nothing and keeps nothing.
    tool("take", tmp / "empty", tmp / "none")
    assert ids(tmp / "empty") == [] if (tmp / "empty" / "index.ndjson").exists() else True
print("test_history: ok")

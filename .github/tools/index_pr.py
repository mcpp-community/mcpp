#!/usr/bin/env python3
"""THE INDEX ENTRY OF A RELEASE, PROPOSED DIRECTLY (design 2026-11-14
release-publication §1.2 B2).

    index_pr.py --manifest mcpp-release.json [--gitcode pending|absent] [--dry-run]
    index_pr.py --manifest mcpp-release.json --edit <recipe.lua>     # edit a file in place, no PR

Reads the sealed release manifest (`mcpp-release.json`: version, and each
platform archive with its sha256), appends the version to the recipe's
`linux`, `macosx` and `windows` tables of openxlings/xim-pkgindex
`pkgs/<p>/<project>.lua` -- the GLOBAL url on github.com/xlings-res, the CN url
on gitcode.com/xlings-res -- moves each table's `latest` to it, and opens a pull
request from the branch `auto/bump-xim-<project>`.

The digests are the sealed manifest's: nothing is downloaded or computed
again. The pull request body starts with the index updater's marker
(`xpkgindex-update`), by which that updater knows the proposal exists and makes
none of its own. A pull request with the marker, open or merged, ends the
script with nothing done, so a run started again changes nothing. It used to
ask the updater to propose (`bump_index.sh`), which requires both mirrors and
so waited on the GitCode upload; v2026.11.13.1's updater run failed.

Auth: PKGINDEX_TOKEN (Contents and Pull requests on openxlings/xim-pkgindex).
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

INDEX_REPO = os.environ.get("INDEX_REPO", "openxlings/xim-pkgindex")
RES_OWNER = "xlings-res"
# The manifest's platform and architecture words, as the recipe's tables name them.
TABLE = {"linux": "linux", "macos": "macosx", "windows": "windows"}
ARCH = {"arm64": "aarch64"}


def entries(manifest: dict) -> dict[str, list[tuple[str, str, str]]]:
    """Each recipe table's (arch, asset, sha256), in the manifest's order."""
    out: dict[str, list[tuple[str, str, str]]] = {}
    for a in manifest["assets"]:
        table = TABLE[a["platform"]]
        out.setdefault(table, []).append((ARCH.get(a["arch"], a["arch"]), a["name"], a["sha256"]))
    return out


def version_block(project: str, version: str, rows: list[tuple[str, str, str]]) -> str:
    lines = [f'            ["{version}"] = {{']
    for arch, asset, sha in rows:
        lines += [
            f"                {arch} = {{",
            "                    url = {",
            f'                        GLOBAL = "https://github.com/{RES_OWNER}/{project}/releases/download/{version}/{asset}",',
            f'                        CN = "https://gitcode.com/{RES_OWNER}/{project}/releases/download/{version}/{asset}",',
            "                    },",
            f'                    sha256 = "{sha}",',
            "                },",
        ]
    lines.append("            },")
    return "\n".join(lines) + "\n"


def edit(text: str, project: str, version: str, tables: dict[str, list[tuple[str, str, str]]]) -> str:
    """The recipe with the version appended to each table and `latest` moved to it.
    A table that already holds the version is left as it is."""
    for table, rows in tables.items():
        start = re.search(rf"^        {table} = \{{\n", text, re.M)
        if not start:
            raise SystemExit(f"index_pr: the recipe has no `{table}` table")
        end = text.index("\n        },\n", start.end()) + 1
        body = text[start.end():end]
        if f'["{version}"] = {{' in body:
            continue
        body = re.sub(r'\["latest"\] = \{ ref = "[^"]*" \}', f'["latest"] = {{ ref = "{version}" }}', body, count=1)
        body += version_block(project, version, rows)
        text = text[:start.end()] + body + text[end:]
    return text


def marker(project: str, version: str) -> str:
    return f'<!-- xpkgindex-update: {{"id":"xim:{project}","candidate":"{version}"}} -->'


def sh(*args: str, cwd: Path | None = None, capture: bool = False) -> str:
    r = subprocess.run(args, cwd=cwd, check=True, text=True,
                       stdout=subprocess.PIPE if capture else None)
    return r.stdout or ""


def existing_pr(project: str, version: str) -> str:
    out = sh("gh", "pr", "list", "-R", INDEX_REPO, "--state", "all", "--limit", "50",
             "--json", "url,body,state", capture=True)
    for pr in json.loads(out):
        if pr["state"] != "CLOSED" and pr["body"].startswith(marker(project, version)):
            return pr["url"]
    return ""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--manifest", required=True, type=Path)
    ap.add_argument("--project", default="mcpp")
    # `pending`: the release run mirrors GitCode after this pull request.
    ap.add_argument("--gitcode", choices=["pending", "absent"], default="absent")
    ap.add_argument("--edit", type=Path, help="edit this recipe in place and open no pull request")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    manifest = json.loads(a.manifest.read_text())
    version, tables = manifest["version"], entries(manifest)
    if a.edit:
        a.edit.write_text(edit(a.edit.read_text(), a.project, version, tables))
        return 0

    if url := existing_pr(a.project, version):
        print(f"[index] proposed already: {url}")
        return 0
    recipe = f"pkgs/{a.project[0]}/{a.project}.lua"
    branch = f"auto/bump-xim-{a.project}"
    with tempfile.TemporaryDirectory() as tmp:
        repo = Path(tmp) / "index"
        sh("gh", "repo", "clone", INDEX_REPO, str(repo), "--", "--depth", "1", "--quiet")
        path = repo / recipe
        path.write_text(edit(path.read_text(), a.project, version, tables))
        if not sh("git", "status", "--porcelain", cwd=repo, capture=True).strip():
            print(f"[index] {recipe} holds {version} already")
            return 0
        title = f"{a.project} {version}; latest"
        cn = ("The release run mirrors GitCode after this pull request: merge once its "
              "`publish-ecosystem` job is green." if a.gitcode == "pending" else
              "The release run did not mirror GitCode (the `gitcode` input): the CN urls are "
              "served once the archives are uploaded there (`MIRROR_HOSTS=gtc .github/tools/mirror_res.sh`). "
              "Merge after that.")
        body = "\n".join([
            marker(a.project, version),
            f"Append `xim:{a.project}@{version}` to the linux, macosx and windows tables and move "
            "`latest` to it. Existing versions are unchanged.",
            "",
            f"The digests are those of the sealed release manifest (`mcpp-release.json` of "
            f"{manifest.get('tag', version)}, commit {manifest.get('commit', '?')}).",
            "",
            cn,
        ])
        if a.dry_run:
            print(sh("git", "diff", cwd=repo, capture=True))
            print(body)
            return 0
        sh("git", "switch", "-c", branch, cwd=repo)
        sh("git", "-c", "user.name=mcpp-release", "-c", "user.email=mcpp-release@users.noreply.github.com",
           "commit", "-q", "-am", title, cwd=repo)
        sh("git", "push", "-q", "-f", "origin", branch, cwd=repo)
        # An open pull request of an earlier version from the same branch now
        # carries this commit: it is retitled, not duplicated.
        stale = json.loads(sh("gh", "pr", "list", "-R", INDEX_REPO, "--head", branch, "--state", "open",
                              "--json", "url", capture=True))
        if stale:
            url = stale[0]["url"]
            sh("gh", "pr", "edit", url, "--title", title, "--body", body)
            print(f"[index] updated {url}")
        else:
            url = sh("gh", "pr", "create", "-R", INDEX_REPO, "--base", "main", "--head", branch,
                     "--title", title, "--body", body, capture=True).strip()
            print(f"[index] opened {url}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

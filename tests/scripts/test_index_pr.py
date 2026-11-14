#!/usr/bin/env python3
"""The index entry a release proposes (.github/tools/index_pr.py), and the order
in which the release publishes (release.yml `publish-ecosystem`); offline, no
pull request."""

from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / ".github/tools/index_pr.py"
spec = importlib.util.spec_from_file_location("index_pr", SCRIPT)
index_pr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(index_pr)

RECIPE = '''package = {
    xpm = {
        linux = {
            ["latest"] = { ref = "2026.11.12.1" },
            ["2026.11.12.1"] = {
                x86_64 = {
                    url = { GLOBAL = "g", CN = "c" },
                    sha256 = "old",
                },
            },
        },
        macosx = {
            ["latest"] = { ref = "2026.11.12.1" },
        },
        windows = {
            ["latest"] = { ref = "2026.11.12.1" },
        },
    },
}
'''

MANIFEST = {
    "version": "2026.11.13.1",
    "tag": "v2026.11.13.1",
    "assets": [
        {"platform": "linux", "arch": "aarch64", "name": "mcpp-2026.11.13.1-linux-aarch64.tar.gz", "sha256": "a" * 64},
        {"platform": "linux", "arch": "x86_64", "name": "mcpp-2026.11.13.1-linux-x86_64.tar.gz", "sha256": "b" * 64},
        {"platform": "macos", "arch": "arm64", "name": "mcpp-2026.11.13.1-macosx-arm64.tar.gz", "sha256": "c" * 64},
        {"platform": "windows", "arch": "x86_64", "name": "mcpp-2026.11.13.1-windows-x86_64.zip", "sha256": "d" * 64},
    ],
}


class IndexEntry(unittest.TestCase):
    def edited(self) -> str:
        return index_pr.edit(RECIPE, "mcpp", MANIFEST["version"], index_pr.entries(MANIFEST))

    def test_each_table_gains_the_version_and_its_latest(self):
        text = self.edited()
        self.assertEqual(text.count('["2026.11.13.1"] = {'), 3)
        self.assertEqual(text.count('["latest"] = { ref = "2026.11.13.1" }'), 3)
        self.assertNotIn('["latest"] = { ref = "2026.11.12.1" }', text)
        self.assertIn('sha256 = "old"', text)  # an existing version is unchanged

    def test_the_urls_name_both_mirrors_and_the_digest_is_the_manifests(self):
        text = self.edited()
        self.assertIn('GLOBAL = "https://github.com/xlings-res/mcpp/releases/download/2026.11.13.1/'
                      'mcpp-2026.11.13.1-macosx-arm64.tar.gz"', text)
        self.assertIn('CN = "https://gitcode.com/xlings-res/mcpp/releases/download/2026.11.13.1/'
                      'mcpp-2026.11.13.1-windows-x86_64.zip"', text)
        self.assertIn('sha256 = "' + "c" * 64 + '"', text)

    def test_the_manifests_words_are_the_recipes(self):
        tables = index_pr.entries(MANIFEST)
        self.assertEqual(sorted(tables), ["linux", "macosx", "windows"])
        self.assertEqual(tables["macosx"][0][0], "aarch64")
        macos = self.edited().split("        macosx = {")[1].split("        windows = {")[0]
        self.assertIn("                aarch64 = {", macos)

    def test_a_second_edit_changes_nothing(self):
        once = self.edited()
        self.assertEqual(index_pr.edit(once, "mcpp", MANIFEST["version"], index_pr.entries(MANIFEST)), once)

    def test_the_marker_is_the_updaters(self):
        self.assertEqual(index_pr.marker("mcpp", "2026.11.13.1"),
                         '<!-- xpkgindex-update: {"id":"xim:mcpp","candidate":"2026.11.13.1"} -->')


class Publication(unittest.TestCase):
    """release.yml: GitHub mirror, then the index pull request, then GitCode only when asked."""

    def job(self) -> str:
        text = (ROOT / ".github/workflows/release.yml").read_text()
        return text.split("\n  publish-ecosystem:\n")[1]

    def steps(self) -> list[str]:
        return self.job().split("\n      - name: ")[1:]

    def test_gitcode_is_an_input_off_by_default(self):
        text = (ROOT / ".github/workflows/release.yml").read_text()
        block = text.split("\n      gitcode:\n")[1].split("\n      tag:")[0].split("\njobs:")[0]
        self.assertIn("type: boolean", block)
        self.assertIn("default: false", block)
        self.assertIn("WITH_GITCODE: ${{ github.event_name == 'workflow_dispatch' && inputs.gitcode }}", self.job())

    def test_the_order_is_github_index_gitcode(self):
        names = [s.splitlines()[0] for s in self.steps()]
        github = names.index("Mirror the archives to xlings-res/mcpp on GitHub")
        index = names.index("Open the index pull request (xim-pkgindex)")
        gitcode = names.index("Mirror the archives to GitCode")
        self.assertLess(github, index)
        self.assertLess(index, gitcode)

    def test_only_the_gitcode_step_reaches_gitcode(self):
        for step in self.steps():
            name = step.splitlines()[0]
            if name == "Mirror the archives to GitCode":
                self.assertIn("env.WITH_GITCODE == 'true'", step)
                self.assertIn("MIRROR_HOSTS: gtc", step)
            else:
                self.assertNotIn(".github/tools/gtc", step, name)
                if "mirror_res.sh" in step:
                    self.assertIn("MIRROR_HOSTS: gh", step, name)

    def test_the_index_reads_the_sealed_manifest(self):
        step = next(s for s in self.steps() if s.startswith("Open the index pull request"))
        self.assertIn("index_pr.py --manifest sealed/mcpp-release.json", step)
        self.assertNotIn("bump_index.sh", self.job())


if __name__ == "__main__":
    unittest.main()

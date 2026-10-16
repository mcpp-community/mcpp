#!/usr/bin/env python3
"""Fixture tests for .github/tools/check_provider_boundary.py.

The check passes on the repository's src/, and on a tree that names the
provider's layout only inside the adapter, in comments, or with a stated
exception; it fails on an import of the adapter outside the composition root,
on a layout spelling in code, and on a payload canonicalised or removed.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / ".github" / "tools" / "check_provider_boundary.py"


def run(root: Path) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(SCRIPT), "--root", str(root)],
                          capture_output=True, text=True, check=False)


def tree(files: dict[str, str]) -> tempfile.TemporaryDirectory:
    d = tempfile.TemporaryDirectory()
    for rel, text in files.items():
        p = Path(d.name) / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8")
    return d


CLEAN = {
    "src/provider/select.cppm": "export module mcpp.provider.select;\nimport mcpp.provider.xlings;\n",
    "src/provider/xlings/xlings.cppm": 'auto p = home / "xpkgs" / "xim-x-gcc";\n',
    "src/build/a.cppm": ('// the payload under xpkgs/xim-x-gcc is the provider\'s\n'
                         'import mcpp.provider.select;\n'
                         'auto q = std::filesystem::canonical(sourceDir);\n'
                         'auto r = dir / "subos"; // provider-boundary-ok: a fixture\'s reason\n'),
}


class ProviderBoundary(unittest.TestCase):
    def test_the_repository_is_clean(self) -> None:
        r = run(REPO_ROOT)
        self.assertEqual(r.returncode, 0, r.stdout)

    def test_the_adapter_comments_and_stated_exceptions_pass(self) -> None:
        with tree(CLEAN) as d:
            r = run(Path(d))
            self.assertEqual(r.returncode, 0, r.stdout)

    def test_each_crossing_is_found(self) -> None:
        cases = {
            "import": "import mcpp.provider.xlings.subos_info;\n",
            "layout": 'auto p = home / "xpkgs";\n',
            "marker": 'auto ok = dir / ".mcpp_ok";\n',
            "payload": "auto p = std::filesystem::canonical(payloadDir);\n",
            "removal": "std::filesystem::remove_all(payload);\n",
        }
        for name, line in cases.items():
            with self.subTest(case=name), tree({**CLEAN, "src/build/b.cppm": line}) as d:
                r = run(Path(d))
                self.assertEqual(r.returncode, 1, r.stdout)
                self.assertIn("src/build/b.cppm:1:", r.stdout)


if __name__ == "__main__":
    unittest.main()

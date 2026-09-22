#!/usr/bin/env python3
"""Contract tests for scripts/pypi/build_wheels.py.

Offline: a synthetic release (manifest + four payloads with the real archive
layout) is built into wheels, and each wheel is read back the way pip reads it.
"""
import base64
import hashlib
import io
import json
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BUILDER = ROOT / "scripts" / "pypi" / "build_wheels.py"
VERSION = "2026.1.2.3"

ROWS = [
    ("linux", "aarch64", f"mcpp-{VERSION}-linux-aarch64.tar.gz"),
    ("linux", "x86_64", f"mcpp-{VERSION}-linux-x86_64.tar.gz"),
    ("macos", "arm64", f"mcpp-{VERSION}-macosx-arm64.tar.gz"),
    ("windows", "x86_64", f"mcpp-{VERSION}-windows-x86_64.zip"),
]


def make_payload(path: Path) -> None:
    root = path.name.removesuffix(".tar.gz").removesuffix(".zip")
    exe = ".exe" if path.name.endswith(".zip") else ""
    files = {
        f"{root}/bin/mcpp{exe}": (b"MCPP-" + path.name.encode(), 0o755),
        f"{root}/registry/bin/xlings{exe}": (b"XLINGS", 0o755),
        f"{root}/LICENSE": (b"Apache-2.0", 0o644),
        f"{root}/README.md": (b"readme", 0o644),
        f"{root}/mcpp": (b"#!/bin/sh\n", 0o755),
    }
    if exe:
        with zipfile.ZipFile(path, "w") as z:
            for name, (data, _) in files.items():
                z.writestr(name, data)
    else:
        with tarfile.open(path, "w:gz") as t:
            for name, (data, mode) in files.items():
                info = tarfile.TarInfo(name)
                info.size, info.mode = len(data), mode
                t.addfile(info, io.BytesIO(data))


def make_release(d: Path, corrupt: str | None = None) -> None:
    assets = []
    for platform, arch, name in ROWS:
        make_payload(d / name)
        digest = hashlib.sha256((d / name).read_bytes()).hexdigest()
        if name == corrupt:
            digest = "0" * 64
        assets.append({"platform": platform, "arch": arch, "name": name, "sha256": digest})
    (d / "mcpp-release.json").write_text(json.dumps(
        {"schema": 1, "version": VERSION, "tag": f"v{VERSION}", "assets": assets}))


def build(rel: Path, out: Path) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(BUILDER), "--release-dir", str(rel),
                           "--out", str(out)], capture_output=True, text=True)


class WheelContract(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.rel = Path(self.tmp.name) / "rel"
        self.out = Path(self.tmp.name) / "dist"
        self.rel.mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def test_one_wheel_per_manifest_row_with_its_platform_tag(self):
        make_release(self.rel)
        r = build(self.rel, self.out)
        self.assertEqual(r.returncode, 0, r.stderr)
        tags = sorted(p.name.split("-", 2)[2].removesuffix(".whl") for p in self.out.glob("*.whl"))
        self.assertEqual(tags, sorted([
            "py3-none-manylinux_2_17_aarch64.manylinux2014_aarch64.musllinux_1_1_aarch64",
            "py3-none-manylinux_2_17_x86_64.manylinux2014_x86_64.musllinux_1_1_x86_64",
            "py3-none-macosx_14_0_arm64",
            "py3-none-win_amd64",
        ]))

    def test_wheel_contents_record_and_modes(self):
        make_release(self.rel)
        self.assertEqual(build(self.rel, self.out).returncode, 0)
        for whl in self.out.glob("*.whl"):
            exe = ".exe" if "win_amd64" in whl.name else ""
            with zipfile.ZipFile(whl) as z:
                names = set(z.namelist())
                di = f"mcpp_bin-{VERSION}.dist-info"
                self.assertEqual(names, {
                    "mcpp_bin/__init__.py", "mcpp_bin/__main__.py",
                    f"mcpp_bin/bin/mcpp{exe}", f"mcpp_bin/registry/bin/xlings{exe}",
                    f"{di}/LICENSE", f"{di}/METADATA", f"{di}/WHEEL",
                    f"{di}/entry_points.txt", f"{di}/RECORD",
                }, whl.name)
                # The payload's bytes, not the bundle's root convenience script.
                self.assertTrue(z.read(f"mcpp_bin/bin/mcpp{exe}").startswith(b"MCPP-"))
                mode = z.getinfo(f"mcpp_bin/bin/mcpp{exe}").external_attr >> 16
                self.assertTrue(mode & 0o111, f"{whl.name}: binary is not executable")
                self.assertIn("mcpp = mcpp_bin:main", z.read(f"{di}/entry_points.txt").decode())
                meta = z.read(f"{di}/METADATA").decode()
                self.assertIn("Name: mcpp-bin\n", meta)
                self.assertIn(f"Version: {VERSION}\n", meta)
                for line in z.read(f"{di}/RECORD").decode().splitlines():
                    name, digest, size = line.split(",")
                    if name.endswith("RECORD"):
                        continue
                    data = z.read(name)
                    want = "sha256=" + base64.urlsafe_b64encode(
                        hashlib.sha256(data).digest()).rstrip(b"=").decode()
                    self.assertEqual((digest, int(size)), (want, len(data)), name)

    def test_payload_whose_hash_disagrees_with_the_manifest_is_refused(self):
        make_release(self.rel, corrupt=ROWS[1][2])
        r = build(self.rel, self.out)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("sha256", r.stderr)

    def test_manifest_missing_a_platform_is_refused(self):
        make_release(self.rel)
        m = json.loads((self.rel / "mcpp-release.json").read_text())
        m["assets"] = [a for a in m["assets"] if a["platform"] != "windows"]
        (self.rel / "mcpp-release.json").write_text(json.dumps(m))
        r = build(self.rel, self.out)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("windows", r.stderr)


if __name__ == "__main__":
    unittest.main()

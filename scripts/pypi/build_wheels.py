#!/usr/bin/env python3
"""Build the `mcpp-bin` PyPI wheels from one published mcpp release.

Each wheel carries the prebuilt release payload for one platform, the same
bytes `install.sh`, Homebrew and the AUR `mcpp-bin` package install:

    mcpp_bin/__init__.py            launcher (the `mcpp` console script)
    mcpp_bin/bin/mcpp[.exe]         the release binary
    mcpp_bin/registry/bin/xlings    the bundled xlings

The launcher pins MCPP_HOME to the per-user home and MCPP_VENDORED_XLINGS to
the bundled xlings, for the reason scripts/aur/README.md gives: mcpp resolves
its home from the real path of its binary, and site-packages is not a place a
per-user sandbox may be written into.

INPUT IS THE RELEASE MANIFEST, NOT THE ASSET LISTING. `mcpp-release.json` is
the release workflow's statement of which payloads are complete (docs/92). A
wheel is built only for a row it names, and only from bytes whose sha256
matches the row.

The wheels are written by hand with the standard library. A wheel is a zip
with three metadata files, and a build backend would add a dependency without
adding anything this script needs.

Usage:
  build_wheels.py --tag v2026.9.21.3 --out dist/
  build_wheels.py --release-dir DIR --out dist/     # offline, from local files
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import io
import json
import shutil
import sys
import tarfile
import tempfile
import urllib.request
import zipfile
from pathlib import Path

REPO = "mcpp-community/mcpp"
DIST_NAME = "mcpp-bin"
WHEEL_NAME = "mcpp_bin"
SUMMARY = "Modern C++ module-first build tool (prebuilt mcpp binary)"

# (platform, arch) from mcpp-release.json -> wheel platform tag.
#
# The Linux payloads are fully static musl binaries, so one wheel serves both
# glibc and musl installers. The glibc floor is the lowest manylinux tag pip
# on the target arch accepts; the binary itself needs none.
PLATFORM_TAGS = {
    ("linux", "x86_64"):
        "manylinux_2_17_x86_64.manylinux2014_x86_64.musllinux_1_1_x86_64",
    ("linux", "aarch64"):
        "manylinux_2_17_aarch64.manylinux2014_aarch64.musllinux_1_1_aarch64",
    # The Homebrew formula states the same floor: Apple silicon + macOS 14.
    ("macos", "arm64"): "macosx_14_0_arm64",
    ("windows", "x86_64"): "win_amd64",
}

LAUNCHER = '''\
"""mcpp launcher installed by the `mcpp-bin` PyPI package.

mcpp writes its registry sandbox, caches and downloaded toolchains into
MCPP_HOME. The package directory is shared and may be read-only, so the
launcher pins the per-user home that install.sh uses, and points mcpp at the
bundled xlings, which mcpp copies into that home on first run. A value the
user already exported is kept.
"""
import os
import subprocess
import sys
from pathlib import Path

__version__ = "{version}"

_HERE = Path(__file__).resolve().parent
_EXE = ".exe" if os.name == "nt" else ""


def binary() -> Path:
    return _HERE / "bin" / ("mcpp" + _EXE)


def main() -> None:
    exe = binary()
    xlings = _HERE / "registry" / "bin" / ("xlings" + _EXE)
    env = os.environ
    env.setdefault("MCPP_HOME", str(Path.home() / ".mcpp"))
    env.setdefault("MCPP_VENDORED_XLINGS", str(xlings))
    argv = [str(exe), *sys.argv[1:]]
    if os.name == "nt":
        # No exec on Windows: wait for the child and forward its status.
        # Ctrl+C reaches the child through the shared console; the launcher
        # only has to outlive it.
        try:
            rc = subprocess.call(argv)
        except KeyboardInterrupt:
            rc = 130
        sys.exit(rc)
    os.execv(str(exe), argv)


if __name__ == "__main__":
    main()
'''

MAIN = "from mcpp_bin import main\n\nmain()\n"


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch(url: str, dest: Path) -> None:
    for attempt in range(4):
        try:
            with urllib.request.urlopen(url, timeout=120) as r, dest.open("wb") as f:
                shutil.copyfileobj(r, f)
            return
        except OSError as e:
            if attempt == 3:
                raise
            print(f"retry {attempt + 1}: {url}: {e}", file=sys.stderr)


def payload_members(archive: Path) -> dict[str, tuple[bytes, bool]]:
    """Return {relative path: (bytes, executable)} for the files a wheel carries.

    Only bin/mcpp, registry/bin/xlings and LICENSE are taken, matching the AUR
    package: the bundle's root `mcpp` / `mcpp.bat` convenience entry and its
    README are not part of the runtime.
    """
    wanted = {
        "bin/mcpp", "bin/mcpp.exe",
        "registry/bin/xlings", "registry/bin/xlings.exe",
        "LICENSE",
    }
    out: dict[str, tuple[bytes, bool]] = {}
    if archive.name.endswith(".zip"):
        with zipfile.ZipFile(archive) as z:
            for info in z.infolist():
                rel = info.filename.split("/", 1)[-1] if "/" in info.filename else ""
                if rel in wanted:
                    out[rel] = (z.read(info), rel.endswith(".exe"))
    else:
        with tarfile.open(archive, "r:gz") as t:
            for m in t.getmembers():
                rel = m.name.split("/", 1)[-1] if "/" in m.name else ""
                if rel in wanted and m.isfile():
                    data = t.extractfile(m).read()
                    out[rel] = (data, bool(m.mode & 0o111))
    exe = ".exe" if archive.name.endswith(".zip") else ""
    for need in (f"bin/mcpp{exe}", f"registry/bin/xlings{exe}", "LICENSE"):
        if need not in out:
            raise SystemExit(f"{archive.name}: payload has no {need}")
    return out


def record_hash(data: bytes) -> str:
    digest = hashlib.sha256(data).digest()
    return "sha256=" + base64.urlsafe_b64encode(digest).rstrip(b"=").decode()


def metadata(version: str, readme: str) -> str:
    return "\n".join([
        "Metadata-Version: 2.1",
        f"Name: {DIST_NAME}",
        f"Version: {version}",
        f"Summary: {SUMMARY}",
        f"Home-page: https://github.com/{REPO}",
        "License: Apache-2.0",
        f"Project-URL: Source, https://github.com/{REPO}",
        f"Project-URL: Documentation, https://github.com/{REPO}/tree/main/docs",
        f"Project-URL: Changelog, https://github.com/{REPO}/blob/main/CHANGELOG.md",
        "Classifier: License :: OSI Approved :: Apache Software License",
        "Classifier: Programming Language :: C++",
        "Classifier: Topic :: Software Development :: Build Tools",
        "Classifier: Operating System :: POSIX :: Linux",
        "Classifier: Operating System :: MacOS",
        "Classifier: Operating System :: Microsoft :: Windows",
        "Requires-Python: >=3.8",
        "Description-Content-Type: text/markdown",
        "",
        readme,
    ])


def build_wheel(version: str, platform_tag: str, members: dict[str, tuple[bytes, bool]],
                readme: str, out_dir: Path) -> Path:
    tag = f"py3-none-{platform_tag}"
    dist_info = f"{WHEEL_NAME}-{version}.dist-info"
    files: list[tuple[str, bytes, bool]] = [
        (f"{WHEEL_NAME}/__init__.py", LAUNCHER.format(version=version).encode(), False),
        (f"{WHEEL_NAME}/__main__.py", MAIN.encode(), False),
    ]
    for rel, (data, _) in sorted(members.items()):
        if rel == "LICENSE":
            files.append((f"{dist_info}/LICENSE", data, False))
        else:
            files.append((f"{WHEEL_NAME}/{rel}", data, True))
    files += [
        (f"{dist_info}/METADATA", metadata(version, readme).encode(), False),
        (f"{dist_info}/WHEEL", (
            "Wheel-Version: 1.0\n"
            "Generator: mcpp scripts/pypi/build_wheels.py\n"
            "Root-Is-Purelib: false\n"
            + "".join(f"Tag: py3-none-{p}\n" for p in platform_tag.split("."))
        ).encode(), False),
        (f"{dist_info}/entry_points.txt",
         b"[console_scripts]\nmcpp = mcpp_bin:main\n", False),
    ]
    record_name = f"{dist_info}/RECORD"
    record = io.StringIO()
    for name, data, _ in files:
        record.write(f"{name},{record_hash(data)},{len(data)}\n")
    record.write(f"{record_name},,\n")
    files.append((record_name, record.getvalue().encode(), False))

    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / f"{WHEEL_NAME}-{version}-{tag}.whl"
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data, is_exec in files:
            info = zipfile.ZipInfo(name, date_time=(2020, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            # pip applies the executable bits it finds in the entry's mode.
            info.external_attr = ((0o755 if is_exec else 0o644) | 0o100000) << 16
            z.writestr(info, data)
    return path


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--tag", help="release tag, e.g. v2026.9.21.3")
    src.add_argument("--release-dir", type=Path,
                     help="directory holding mcpp-release.json and its payloads")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--readme", type=Path,
                    default=Path(__file__).with_name("README.pypi.md"))
    args = ap.parse_args(argv)

    with tempfile.TemporaryDirectory() as tmp:
        rel_dir = args.release_dir or Path(tmp)
        base = f"https://github.com/{REPO}/releases/download/{args.tag}"
        manifest_path = rel_dir / "mcpp-release.json"
        if args.tag:
            fetch(f"{base}/mcpp-release.json", manifest_path)
        manifest = json.loads(manifest_path.read_text())
        version = manifest["version"]
        if args.tag and manifest.get("tag") != args.tag:
            raise SystemExit(f"manifest names {manifest.get('tag')}, expected {args.tag}")
        readme = args.readme.read_text().replace("{version}", version)

        built = []
        for row in manifest["assets"]:
            key = (row["platform"], row["arch"])
            if key not in PLATFORM_TAGS:
                print(f"skip {row['name']}: no wheel platform for {key}")
                continue
            archive = rel_dir / row["name"]
            if args.tag:
                fetch(f"{base}/{row['name']}", archive)
            got = sha256_file(archive)
            if got != row["sha256"]:
                raise SystemExit(f"{row['name']}: sha256 {got} != manifest {row['sha256']}")
            wheel = build_wheel(version, PLATFORM_TAGS[key], payload_members(archive),
                                readme, args.out)
            built.append(wheel)
            print(f"built {wheel.name} ({wheel.stat().st_size} bytes)")

        missing = set(PLATFORM_TAGS) - {(r["platform"], r["arch"]) for r in manifest["assets"]}
        if missing:
            raise SystemExit(f"manifest has no payload for {sorted(missing)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

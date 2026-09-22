# PyPI packaging (`mcpp-bin`)

`pip install mcpp-bin` installs the prebuilt release binary and puts the `mcpp`
command on the environment's `PATH`. The PyPI name is `mcpp-bin` because `mcpp`
on PyPI is an unrelated project, which is the same situation as on the AUR and
Homebrew.

## Wheels

One wheel per payload in the release's `mcpp-release.json`:

| Payload | Wheel platform tag |
| --- | --- |
| `linux-x86_64` | `manylinux_2_17_x86_64.manylinux2014_x86_64.musllinux_1_1_x86_64` |
| `linux-aarch64` | `manylinux_2_17_aarch64.manylinux2014_aarch64.musllinux_1_1_aarch64` |
| `macosx-arm64` | `macosx_14_0_arm64` |
| `windows-x86_64` | `win_amd64` |

The Linux payloads are fully static musl binaries, so one wheel carries both
the glibc and the musl tag. The macOS floor is the one the Homebrew formula
states. There is no sdist; on any other platform, pip reports that no
distribution matches.

Each wheel holds the same two binaries the AUR package ships, plus a launcher:

| Path in site-packages | Contents |
| --- | --- |
| `mcpp_bin/bin/mcpp[.exe]` | the release binary |
| `mcpp_bin/registry/bin/xlings[.exe]` | the bundled xlings |
| `mcpp_bin/__init__.py` | the launcher behind the `mcpp` console script |

## The launcher

mcpp writes its registry sandbox, caches and toolchains into `MCPP_HOME`, and
it resolves that home from the real path of its binary. Resolved from
site-packages, the home would sit inside the Python environment, which may be
shared or read-only and is removed by `pip uninstall`. The launcher therefore
sets `MCPP_HOME=~/.mcpp` (`%USERPROFILE%\.mcpp` on Windows) and
`MCPP_VENDORED_XLINGS=<site-packages>/mcpp_bin/registry/bin/xlings`, the same
two variables `scripts/aur/mcpp-bin/mcpp.sh` sets. A value the user already
exported is kept. On POSIX the launcher `exec`s the binary; on Windows it runs
the binary as a child process and exits with its status.

## Publishing

[`.github/workflows/pypi-publish.yml`](../../.github/workflows/pypi-publish.yml):

1. runs [`tests/scripts/test_pypi_wheels.py`](../../tests/scripts/test_pypi_wheels.py);
2. builds the four wheels with [`build_wheels.py`](build_wheels.py) from the
   release manifest, checking each payload's sha256 against it;
3. runs `twine check --strict`;
4. installs from the directory of all four wheels on Linux x86_64, Linux
   aarch64, macOS arm64 and Windows x86_64, so pip picks the wheel. It then
   checks the version, that `MCPP_HOME` is outside the Python environment, and,
   on POSIX, that the bundled xlings was seeded into the home;
5. uploads to PyPI through Trusted Publishing, when publishing is enabled.

| Trigger | Publishes |
| --- | --- |
| `release` completed | only if the repository variable `PYPI_AUTOPUBLISH` is `true` |
| `workflow_dispatch` | only if its `publish` input is checked |
| `pull_request` touching the packaging | never; builds and installs the latest release |

A version already on PyPI is never uploaded again. PyPI refuses to replace a
file, so such a run reports "already on PyPI" and succeeds.

To build locally without uploading:

```bash
python3 scripts/pypi/build_wheels.py --tag v2026.9.21.3 --out dist/
python3 -m venv /tmp/v && /tmp/v/bin/pip install --no-index --find-links dist mcpp-bin
```

## One-time setup

No token or secret is stored in the repository.

1. **PyPI.** Signed in to PyPI, open
   <https://pypi.org/manage/account/publishing/> and add a *pending publisher*:

   | Field | Value |
   | --- | --- |
   | PyPI Project Name | `mcpp-bin` |
   | Owner | `mcpp-community` |
   | Repository name | `mcpp` |
   | Workflow name | `pypi-publish.yml` |
   | Environment name | `pypi` |

   The first successful upload creates the project, and the pending publisher
   becomes its trusted publisher.
2. **GitHub.** In the repository's *Settings → Environments*, create an
   environment named `pypi`. Required reviewers are optional; with them, each
   upload waits for an approval.
3. **First publish.** Run the workflow by hand with `publish` checked (*Actions →
   pypi-publish → Run workflow*), and confirm that
   <https://pypi.org/project/mcpp-bin/> shows the version.
4. **Arming.** Set the repository variable `PYPI_AUTOPUBLISH` to `true`
   (*Settings → Secrets and variables → Actions → Variables*). From then on,
   every completed release publishes its wheels.

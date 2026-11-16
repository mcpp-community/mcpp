"""Prove home and runner-cache paths under a non-UTF-8 Windows ACP."""
import os
from pathlib import Path
import re
import subprocess
import sys

if os.name != 'nt':
    raise SystemExit('This check requires Windows')
root = Path(__file__).resolve().parents[2]
manifest = root / 'mcpp.toml'
original = manifest.read_bytes()
marker = b'windows_code_page = "utf-8"'
assert original.count(marker) == 1, 'identify the test-only code-page setting explicitly'


# The profile the job built the tests with (MCPP_TEST_PROFILE; empty: the
# manifest's default). Another profile builds every test again, which outlasted
# the timeout on the Windows row (run 43636471152).
profile = os.environ.get('MCPP_TEST_PROFILE', '')


def run_case(passed_case):
    # The case by its identifier (mtest's): it runs in the process of its file.
    command = [os.environ['MCPP'], '--offline', 'test', '--case', passed_case]
    if profile:
        command += ['--profile', profile]
    result = subprocess.run(command, cwd=root,
                            capture_output=True, text=True, encoding='utf-8', errors='replace',
                            timeout=600)
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    assert result.returncode == 0, result.returncode
    output = re.sub(r'\x1b\[[0-9;]*m', '', result.stdout + result.stderr)
    assert re.search(r'^\s*ok\s+' + re.escape(passed_case) + r'\s*$', output, re.M), \
        'the legacy ACP case did not pass; a UTF-8-only skip is not proof'


try:
    manifest.write_bytes(original.replace(marker, b'windows_code_page = "legacy"', 1))
    run_case('HomeSelection/UnicodeLocatorRoundTripDoesNotNarrowTheSelectedPath')
    run_case('BMICacheMaintenance/RunnerCachePathDoesNotUseTheWindowsAnsiCodePage')
finally:
    manifest.write_bytes(original)
print('PASS: home and runner-cache paths ran under legacy Windows ACP; manifest restored')

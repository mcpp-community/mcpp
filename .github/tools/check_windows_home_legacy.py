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


def run_case(pattern, passed_case):
    result = subprocess.run([os.environ['MCPP'], '--offline', 'test', pattern], cwd=root,
                            capture_output=True, text=True, encoding='utf-8', errors='replace',
                            timeout=600)
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    assert result.returncode == 0, result.returncode
    output = re.sub(r'\x1b\[[0-9;]*m', '', result.stdout + result.stderr)
    assert '[       OK ] ' + passed_case in output, \
        'the legacy ACP case did not pass; a UTF-8-only skip is not proof'


try:
    manifest.write_bytes(original.replace(marker, b'windows_code_page = "legacy"', 1))
    run_case('home', 'HomeSelection.UnicodeLocatorRoundTripDoesNotNarrowTheSelectedPath')
    run_case('bmi_cache', 'BMICacheMaintenance.RunnerCachePathDoesNotUseTheWindowsAnsiCodePage')
finally:
    manifest.write_bytes(original)
print('PASS: home and runner-cache paths ran under legacy Windows ACP; manifest restored')
